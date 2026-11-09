/*
 * Copyright (c) 2026, Niklas Hauser
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------

#include "i2c_master_common.hpp"

namespace modm::platform
{

I2cMasterCommon::Attach
I2cMasterCommon::attach(I2cTransaction *t, ConfigurationHandler handler)
{
	if (not t->attaching())
	{
		t->detaching(DetachCause::FailedToAttach);
		return Attach::Refused;
	}
	if (transaction or client or active)
	{
		t->queueNext = nullptr;
		t->queueConfiguration = handler;
		if (queueTail) queueTail->queueNext = t; else queueHead = t;
		queueTail = t;
		return Attach::Queued;
	}
	if (handler and handler != configuration) { configuration = handler; handler(); }
	transaction = t;
	return Attach::Now;
}

I2cMasterCommon::Attach
I2cMasterCommon::attach(I2cTransferBase &x)
{
	if (transaction or client or active)
	{
		x.queueNext = nullptr;
		if (xferTail) xferTail->queueNext = &x; else xferHead = &x;
		xferTail = &x;
		return Attach::Queued;
	}
	if (x.configuration and x.configuration != configuration) { configuration = x.configuration; configuration(); }
	client = &x;
	prefetched = served = false;
	return Attach::Now;
}

// the next client: transfers and transactions alternate while both wait
bool
I2cMasterCommon::takeNext()
{
	if (transaction or client) return false;	// a detaching() started one already
	const bool useTransfer = xferHead and (preferTransfer or not queueHead);
	if (useTransfer)
	{
		I2cTransferBase *const n = xferHead;
		xferHead = n->queueNext;
		if (not xferHead) xferTail = nullptr;
		if (n->configuration and n->configuration != configuration) { configuration = n->configuration; configuration(); }
		client = n;
		prefetched = served = false;
		preferTransfer = false;
		return true;
	}
	if (not queueHead) return false;
	I2cTransaction *const n = queueHead;
	queueHead = n->queueNext;
	if (not queueHead) queueTail = nullptr;
	if (n->queueConfiguration and n->queueConfiguration != configuration)
		{ configuration = n->queueConfiguration; configuration(); }
	transaction = n;
	preferTransfer = true;
	return true;
}

bool
I2cMasterCommon::finish(DetachCause cause)
{
	active = false;
	if (cause == DetachCause::ErrorCondition and error != Error::AddressNack)
		recorder.freeze();
	if (client)
	{
		if (continueTransfer(cause)) return true;	// the same client goes on (p, polling)
		client = nullptr;
		return takeNext();
	}
	I2cTransaction *t = transaction;
	transaction = nullptr;
	if (t) t->detaching(cause);	// may call start() again
	return takeNext();
}

void
I2cMasterCommon::detachAll()
{
	fail(Error::SoftwareReset);
	recorder.freeze();
	while (I2cTransaction *const n = queueHead)
	{
		queueHead = n->queueNext;
		if (not queueHead) queueTail = nullptr;
		n->detaching(DetachCause::ErrorCondition);
	}
	while (I2cTransferBase *const n = xferHead)
	{
		xferHead = n->queueNext;
		if (not xferHead) xferTail = nullptr;
		xfer::Interpreter::fail(n->t, xfer::Error::Aborted);
	}
	active = false;
	if (I2cTransaction *t = transaction)
	{
		transaction = nullptr;
		t->detaching(DetachCause::ErrorCondition);
	}
	if (I2cTransferBase *x = client)
	{
		client = nullptr;
		if (not x->t.result.done)
		{
			if (served) xfer::Interpreter::dataDone(x->t, seg, xfer::Error::Aborted, uint16_t(errorIndex));
			if (not x->t.result.done) xfer::Interpreter::fail(x->t, xfer::Error::Aborted, seg.pos);
		}
	}
}

// ---- a client's segments ----------------------------------------------------------------

// the next data segment, running the callbacks before it; false: the client has ended,
// or a STOP comes first (p after a callback that held the bus)
bool
I2cMasterCommon::nextData(bool busHeld)
{
	xfer::Transfer &t = client->t;
	for (;;)
	{
		const xfer::Segment s = xfer::Interpreter::next(t);
		switch (s.kind)
		{
			case xfer::Segment::Kind::Data:
				seg = s; served = false;
				return true;
			case xfer::Segment::Kind::Stop:		// release the bus first, the rest after a fresh START
				if (busHeld) return false;
				break;
			case xfer::Segment::Kind::Callback:
				if (not xfer::Interpreter::act(t, s.callback(t))) return false;
				break;
			case xfer::Segment::Kind::Pause:		// not supported
				xfer::Interpreter::fail(t, xfer::Error::Internal, s.pos);
				return false;
			case xfer::Segment::Kind::Done:
				xfer::Interpreter::finish(t);
				return false;
		}
	}
}

// the segment handed to the state machine ended without an error
void
I2cMasterCommon::doneData()
{
	if (served) { xfer::Interpreter::dataDone(client->t, seg, xfer::Error::None, 0); served = false; }
}

// the state machine ended the client's attach: report, and go on if the client has more
bool
I2cMasterCommon::continueTransfer(DetachCause cause)
{
	xfer::Transfer &t = client->t;
	if (t.result.done) return false;	// ended between segments (OperationAfterStart::End)
	bool more;
	if (cause == DetachCause::NormalStop)
	{
		doneData();
		more = nextData(false);	// after p: the next segment starts with a fresh START
	}
	else
	{
		served = false;
		more = xfer::Interpreter::dataDone(t, seg, map(error), uint16_t(errorIndex))
			   and nextData(false);	// a callback polls an address NACK
	}
	prefetched = more;
	return more;
}

I2cTransaction::Starting
I2cMasterCommon::starting()
{
	if (not client) return transaction->starting();
	if (prefetched) prefetched = false;
	else
	{
		doneData();
		if (client->t.result.done or not nextData(true))
			return I2cTransaction::Starting(0, OperationAfterStart::End);
	}
	if (not seg.read and seg.length == 0)	// a ping: START, address, STOP
	{
		served = true;
		return I2cTransaction::Starting(seg.address << 1, OperationAfterStart::Stop);
	}
	return I2cTransaction::Starting(seg.address << 1, seg.read ? OperationAfterStart::Read : OperationAfterStart::Write);
}

xfer::Error
I2cMasterCommon::map(Error e)
{
	switch (e)
	{
		case Error::NoError:			return xfer::Error::None;
		case Error::AddressNack:		return xfer::Error::AddressNack;
		case Error::DataNack:			return xfer::Error::DataNack;
		case Error::ArbitrationLost:	return xfer::Error::ArbitrationLost;
		case Error::BusCondition:		return xfer::Error::BusError;
		case Error::BusBusy:			return xfer::Error::BusBusy;
		case Error::SoftwareReset:		return xfer::Error::Aborted;
		default:						return xfer::Error::Internal;
	}
}

bool
I2cMasterCommon::fetch(Operation op)
{
	index = 0;
	if (client)
	{
		using A = xfer::Segment::After;
		if (op == Operation::Write and served)	// a joined write: the next buffer of the same segment chain
		{
			doneData();
			if (client->t.result.done or not nextData(true))
				{ reading = false; wbuf = nullptr; length = 0; next = Operation::Stop; return true; }
		}
		if (op == Operation::Stop) { reading = false; wbuf = nullptr; length = 0; next = Operation::Stop; return true; }
		served = true;
		reading = seg.read; length = seg.length;
		if (reading) rbuf = seg.rbuf; else wbuf = seg.wbuf;
		next = (seg.after == A::Join) ? Operation::Write : (seg.after == A::Stop) ? Operation::Stop : Operation::Restart;
		return not reading or length > 0;
	}
	if (op == Operation::Read)
	{
		const auto r = transaction->reading();
		reading = true; rbuf = r.buffer; length = r.length; next = Operation(r.next);
		return length > 0;
	}
	reading = false;
	if (op == Operation::Write)
	{
		const auto w = transaction->writing();
		wbuf = w.buffer; length = w.length; next = Operation(w.next);
	}
	else { wbuf = nullptr; length = 0; next = Operation::Stop; }
	return true;
}

} // namespace modm::platform
