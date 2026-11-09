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

	/// Starts the transfer (its descriptor and arguments bound already), or queues it.
	/// Use transfer() instead.
	virtual bool
	start(I2cTransferBase &transfer) = 0;

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
#ifndef XFER_NO_STRING
	bool
	transfer(I2cTransferBase &xfer, const char *descriptor, const uintptr_t *args, size_t count)
	{ return xfer.runParsed(*this, descriptor, args, count); }
#endif

protected:
	constexpr I2cBus() = default;
	~I2cBus() = default;
};

// ---- I2cTransferBase: binding and starting -----------------------------------------------

template< class D, class... Args >
bool
I2cTransferBase::run(I2cBus &b, D descriptor, Args... args)
{
	if (t.busy) return false;	// do not touch the result of a transfer in flight
	xfer::bind(t, descriptor, args...);
	// checked at compile time: no parse at run time
	if (not xfer::Interpreter::begin(t, descriptor)) return false;
	return b.start(*this);
}

#ifndef XFER_NO_STRING
inline bool
I2cTransferBase::runParsed(I2cBus &b, const char *descriptor, const uintptr_t *args, size_t count)
{
	if (t.busy) return false;
	if (count > t.slotCount)
	{
		t.result = {}; t.result.error = Error::Descriptor; t.result.done = true;
		return false;
	}
	for (size_t ii = 0; ii < count; ii++) t.args[ii] = args[ii];
	if (not xfer::Interpreter::begin(t, descriptor, xfer::Role::I2cController, count)) return false;
	return b.start(*this);
}
#endif

}	// namespace modm
