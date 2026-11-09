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

#pragma once
#include "i2c_master.hpp"
#include "i2c_transaction.hpp"
#include "i2c_transfer.hpp"
#include <cstddef>

namespace modm
{

/**
 * An I2C controller as an object: device drivers that take an `I2cBus&` are compiled once
 * for all instances, instead of once per `I2cMaster` class. The static `I2cMasterN`
 * classes of a platform give theirs with `I2cMasterN::bus()`.
 *
 * Transactions wait in an intrusive queue (a link in each I2cTransaction), so start() never
 * refuses one for lack of space; it refuses only a transaction that is still attached.
 *
 * @ingroup modm_architecture_i2c
 */
class I2cBus : public ::modm::I2c
{
public:
	using Error = ::modm::I2cMaster::Error;

	/// Attaches the transaction and starts it, or queues it while the bus is in use.
	virtual bool
	start(I2cTransaction *transaction, ConfigurationHandler handler = nullptr) = 0;

	/// Detaches all transactions and resets the controller. Not from an interrupt.
	virtual void
	reset() = 0;

	/// The error of the last transfer that ended with one.
	virtual Error
	getErrorState() const = 0;

	/// The byte position of that error in its buffer: for a data NACK the byte the target
	/// refused, for a read the number of bytes that arrived.
	virtual size_t
	getErrorIndex() const = 0;

	/// Runs a transfer described by a string: `bus.transfer(xfer, "I19 W20 r06"_desc, buf)`.
	/// The number and kinds of the arguments are checked against the descriptor at compile
	/// time. Returns false if the transfer object is still in use (result: Refused).
	template< class D, class... Args >
		requires requires { D::slots; D::role; }
	bool
	transfer(I2cTransferBase &xfer, D descriptor, Args... args)
	{ return xfer.run(*this, descriptor, args...); }

	/// The same for a descriptor known only at run time (the C API, replays): the string is
	/// parsed now, `args` are the pointer-sized argument slots in descriptor order. A wrong
	/// string or count ends the transfer at once with Error::Descriptor and its position.
	bool
	transfer(I2cTransferBase &xfer, const char *descriptor, const uintptr_t *args, size_t count)
	{ return xfer.runParsed(*this, descriptor, args, count); }

protected:
	constexpr I2cBus() = default;
	~I2cBus() = default;
};

// ---- I2cTransferBase: the interpreter as a source of segments ---------------------------

template< class D, class... Args >
bool
I2cTransferBase::run(I2cBus &b, D descriptor, Args... args)
{
	if (isBusy() or t.busy) return false;	// do not touch the result of a transfer in flight
	xfer::bind(t, descriptor, args...);
	// checked at compile time: no parse at run time
	if (not xfer::Interpreter::begin(t, descriptor)) return false;
	bus = &b;
	prefetched = served = false;
	return start();
}

inline bool
I2cTransferBase::runParsed(I2cBus &b, const char *descriptor, const uintptr_t *args, size_t count)
{
	if (isBusy() or t.busy) return false;
	if (count > t.slotCount)
	{
		t.result = {}; t.result.error = Error::Descriptor; t.result.done = true;
		return false;
	}
	for (size_t ii = 0; ii < count; ii++) t.args[ii] = args[ii];
	if (not xfer::Interpreter::begin(t, descriptor, xfer::Role::I2cController, count)) return false;
	bus = &b;
	prefetched = served = false;
	return start();
}

inline bool
I2cTransferBase::start()
{
	if (bus->start(this, configuration)) return true;
	return false;	// detaching(FailedToAttach) ended the transfer
}

// the next data segment, running the callbacks before it; false: the transfer has ended,
// or a STOP comes first (p after a callback that held the bus: `stopFirst`)
inline bool
I2cTransferBase::nextData(bool busHeld)
{
	for (;;)
	{
		const xfer::Segment s = xfer::Interpreter::next(t);
		switch (s.kind)
		{
			case xfer::Segment::Kind::Data:
				seg = s; served = false;
				return true;
			case xfer::Segment::Kind::Stop:	// release the bus first, the rest after a fresh START
				if (busHeld) { stopFirst = true; return false; }
				break;
			case xfer::Segment::Kind::Callback:
				if (not xfer::Interpreter::act(t, s.callback(t))) return false;
				break;
			case xfer::Segment::Kind::Pause:	// not supported yet
				xfer::Interpreter::fail(t, Error::Internal, s.pos);
				return false;
			case xfer::Segment::Kind::Done:
				xfer::Interpreter::finish(t);
				return false;
		}
	}
}

// the segment handed to the driver has ended without an error
inline void
I2cTransferBase::doneData()
{
	if (served) { xfer::Interpreter::dataDone(t, seg, Error::None, 0); served = false; }
}

inline I2cTransaction::Starting
I2cTransferBase::starting()
{
	if (prefetched) prefetched = false;
	else
	{
		doneData();
		if (t.result.done or not nextData(true)) return Starting(0, OperationAfterStart::End);
	}
	if (not seg.read and seg.length == 0)	// a ping: START, address, STOP
	{
		served = true;
		return Starting(seg.address << 1, OperationAfterStart::Stop);
	}
	return Starting(seg.address << 1, seg.read ? OperationAfterStart::Read : OperationAfterStart::Write);
}

inline I2cTransaction::Writing
I2cTransferBase::writing()
{
	if (served)	// a joined write: the previous buffer is done, the next one follows
	{
		doneData();
		if (t.result.done or not nextData(true)) return Writing(nullptr, 0, OperationAfterWrite::Stop);
	}
	served = true;
	using A = xfer::Segment::After;
	const auto next = (seg.after == A::Join) ? OperationAfterWrite::Write :
					  (seg.after == A::Stop) ? OperationAfterWrite::Stop : OperationAfterWrite::Restart;
	return Writing(seg.wbuf, seg.length, next);
}

inline I2cTransaction::Reading
I2cTransferBase::reading()
{
	served = true;
	return Reading(seg.rbuf, seg.length, (seg.after == xfer::Segment::After::Stop) ?
				   OperationAfterRead::Stop : OperationAfterRead::Restart);
}

inline xfer::Error
I2cTransferBase::map(I2cMaster::Error error)
{
	using E = I2cMaster::Error;
	switch (error)
	{
		case E::NoError:			return Error::None;
		case E::AddressNack:		return Error::AddressNack;
		case E::DataNack:			return Error::DataNack;
		case E::ArbitrationLost:	return Error::ArbitrationLost;
		case E::BusCondition:		return Error::BusError;
		case E::BusBusy:			return Error::BusBusy;
		case E::SoftwareReset:		return Error::Aborted;
		default:					return Error::Internal;
	}
}

inline void
I2cTransferBase::detaching(DetachCause cause)
{
	I2cTransaction::detaching(cause);
	if (t.result.done) return;	// ended between segments (OperationAfterStart::End)
	bool more;
	if (cause == DetachCause::NormalStop)
	{
		doneData();
		stopFirst = false;
		more = nextData(false);	// after p: the next segment starts with a fresh START
	}
	else if (cause == DetachCause::FailedToAttach)
		more = xfer::Interpreter::fail(t, Error::Refused);
	else
	{
		served = false;
		more = xfer::Interpreter::dataDone(t, seg, map(bus->getErrorState()), uint16_t(bus->getErrorIndex()))
			   and nextData(false);	// a callback polls an address NACK
	}
	// keep the bus: attached again before the driver takes the next queued transaction
	if (more) { prefetched = true; start(); }
}

}	// namespace modm
