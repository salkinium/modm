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
#include <modm/architecture/interface/delay.hpp>

namespace modm::platform
{

I2cMasterCommon::Attach
I2cMasterCommon::attach(I2cTransaction *t, ConfigurationHandler handler)
{
#ifdef MODM_I2C_TRANSFER_ONLY
	(void) handler;
	t->detaching(DetachCause::FailedToAttach);	// this build runs transfers only
	return Attach::Refused;
#endif
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
	seg = nullptr;
	return Attach::Now;
}

// the next client: transfers and transactions alternate while both wait
bool
I2cMasterCommon::takeNext()
{
	MODM_I2C_PROFILE_SCOPE(TakeNext);
	if (transaction or client) return false;	// a detaching() started one already
#ifdef MODM_I2C_TRANSFER_ONLY
	I2cTransferBase *const x = xferHead;
	if (not x) return false;
	xferHead = x->queueNext;
	if (not xferHead) xferTail = nullptr;
	if (x->configuration and x->configuration != configuration) { configuration = x->configuration; configuration(); }
	client = x;
	prefetched = served = false;
	seg = nullptr;
	return true;
#endif
	const bool useTransfer = xferHead and (preferTransfer or not queueHead);
	if (useTransfer)
	{
		I2cTransferBase *const n = xferHead;
		xferHead = n->queueNext;
		if (not xferHead) xferTail = nullptr;
		if (n->configuration and n->configuration != configuration) { configuration = n->configuration; configuration(); }
		client = n;
		prefetched = served = false;
		seg = nullptr;
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
	MODM_I2C_PROFILE_SCOPE(Finish);
	active = false;
	if (cause == DetachCause::ErrorCondition and error != Error::AddressNack)
		recorder.freeze();
#ifndef MODM_I2C_TRANSFER_ONLY
	if (client)
#endif
	{
		if (continueTransfer(cause)) return true;	// the same client goes on (p, polling)
		client = nullptr;
		return takeNext();
	}
	I2cTransaction *t = transaction;
	transaction = nullptr;
	if (t) { MODM_I2C_PROFILE_SCOPE(TDetaching); t->detaching(cause); }	// may call start() again
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
			if (served and seg) xfer::Interpreter::dataDone(x->t, *seg, xfer::Error::Aborted, uint16_t(errorIndex));
			if (not x->t.result.done) xfer::Interpreter::fail(x->t, xfer::Error::Aborted, seg ? seg->pos : 0);
		}
	}
}

// ---- a client's segments ----------------------------------------------------------------

// the next data segment, running the callbacks before it; false: the client has ended,
// or a STOP comes first (p after a callback that held the bus)
// the rest of nextData(): a segment other than data, and the interpreter after a STOP
[[gnu::noinline]] bool
I2cMasterCommon::nextOther(const xfer::Segment *s, bool busHeld)
{
	xfer::Transfer &t = client->t;
	for (;;)
	{
		switch (s->kind)
		{
			case xfer::Segment::Kind::Data:
				seg = s; served = false;
				return true;
			case xfer::Segment::Kind::Stop:		// release the bus first, the rest after a fresh START
				if (busHeld) return false;
				break;
			case xfer::Segment::Kind::Delay:		// d: busy-wait, the bus stays as it is (SCL low between data tokens)
#ifdef I2C_SR2_BUSY
				// v1 has no STOP interrupt: after p the STOP may still be under way, so wait for it
				// and the bus is free for the whole delay (a stuck SCL ends this wait after 1 ms)
				if (not busHeld)
					for (int i = 0; i < 1000 and (regs()->CR1 & I2C_CR1_STOP); i++) modm::delay_us(1);
#endif
				modm::delay_us(s->length);
				break;
			case xfer::Segment::Kind::Pause:		// not supported
				xfer::Interpreter::fail(t, xfer::Error::Internal, s->pos);
				return false;
			case xfer::Segment::Kind::Finished:	// a callback ended the transfer, its completion ran
				return false;
			default:	// Done: the end of the string
			{
				MODM_I2C_PROFILE_SCOPE(InterpFinish);
				xfer::Interpreter::finish(t);
			}
				return false;
		}
		s = [&] { MODM_I2C_PROFILE_SCOPE(NextData); return xfer::Interpreter::nextData(t, scratch); }();
	}
}

// the next data segment (callbacks run in the interpreter); a prepared one is taken inline
[[gnu::always_inline]] inline bool
I2cMasterCommon::nextData(bool busHeld)
{
	MODM_I2C_PROFILE_SCOPE(NextGlue);
	const xfer::Segment *s = [&] { MODM_I2C_PROFILE_SCOPE(NextData); return xfer::Interpreter::nextData(client->t, scratch); }();
	if (s->kind == xfer::Segment::Kind::Data) [[likely]] { seg = s; served = false; return true; }
	return nextOther(s, busHeld);
}

// the segment handed to the state machine ended without an error (the last one finishes the transfer)
void
I2cMasterCommon::doneData()
{
	if (not served) return;
	served = false;
	MODM_I2C_PROFILE_SCOPE(DataDone);
	xfer::Interpreter::dataDone(client->t, *seg, xfer::Error::None, 0);
}

// the state machine ended the client's attach: report, and go on if the client has more
bool
I2cMasterCommon::continueTransfer(DetachCause cause)
{
	MODM_I2C_PROFILE_SCOPE(Continue);
	xfer::Transfer &t = client->t;
	if (t.result.done) return false;	// ended between segments (OperationAfterStart::End)
	bool more;
	if (cause == DetachCause::NormalStop)
	{
		if (served)	// doneData(), inlined
		{
			served = false;
			MODM_I2C_PROFILE_SCOPE(DataDone);
			xfer::Interpreter::dataDone(t, *seg, xfer::Error::None, 0);	// the last one finishes the transfer
			if (seg->last) return prefetched = false;
		}
		more = not t.result.done and nextData(false);	// after p: the next segment starts with a fresh START
	}
	else
	{
		served = false;
		more = [&] { MODM_I2C_PROFILE_SCOPE(DataDone); return xfer::Interpreter::dataDone(t, *seg, map(error), uint16_t(errorIndex)); }()
			   and nextData(false);	// a callback polls an address NACK
	}
	prefetched = more;
	return more;
}

I2cTransaction::Starting
I2cMasterCommon::starting()
{
	MODM_I2C_PROFILE_SCOPE(Starting);
#ifndef MODM_I2C_TRANSFER_ONLY
	if (not client) { MODM_I2C_PROFILE_SCOPE(TStarting); return transaction->starting(); }
#endif
	if (prefetched) prefetched = false;
	else
	{
		xfer::Transfer &t = client->t;
		if (served)	// doneData(), inlined (the core's good case is inline too)
		{
			served = false;
			MODM_I2C_PROFILE_SCOPE(DataDone);
			xfer::Interpreter::dataDone(t, *seg, xfer::Error::None, 0);
		}
		if (t.result.done or not nextData(true))
			return I2cTransaction::Starting(0, OperationAfterStart::End);
	}
	const xfer::Segment &s = *seg;
	if (not s.read and s.length == 0)	// a ping: START, address, STOP
	{
		served = true;
		return I2cTransaction::Starting(s.address << 1, OperationAfterStart::Stop);
	}
	return I2cTransaction::Starting(s.address << 1, s.read ? OperationAfterStart::Read : OperationAfterStart::Write);
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
	MODM_I2C_PROFILE_SCOPE(Fetch);
	index = 0;
#ifndef MODM_I2C_TRANSFER_ONLY
	if (client)
#endif
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
		const xfer::Segment &s = *seg;
		reading = s.read; length = s.length;
		if (reading) rbuf = s.rbuf; else wbuf = s.wbuf;	// one union: only the one for the direction
		next = (s.after == A::Join) ? Operation::Write : (s.after == A::Stop) ? Operation::Stop : Operation::Restart;
		return not reading or length > 0;
	}
	if (op == Operation::Read)
	{
		const auto r = [&] { MODM_I2C_PROFILE_SCOPE(TReading); return transaction->reading(); }();
		reading = true; rbuf = r.buffer; length = r.length; next = Operation(r.next);
		return length > 0;
	}
	reading = false;
	if (op == Operation::Write)
	{
		const auto w = [&] { MODM_I2C_PROFILE_SCOPE(TWriting); return transaction->writing(); }();
		wbuf = w.buffer; length = w.length; next = Operation(w.next);
	}
	else { wbuf = nullptr; length = 0; next = Operation::Stop; }
	return true;
}

} // namespace modm::platform
