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
#include "i2c_transaction.hpp"
#include "transfer.hpp"
#include <modm/architecture/interface/fiber.hpp>

namespace modm
{

class I2cBus;

/**
 * A transfer described by a string of tokens (see transfer.hpp and
 * stm32-i2c-fsm/docs/transfer_language.md), run on an I2cBus:
 *
 * @code
 * modm::I2cTransfer<1> xfer;
 * bus.transfer(xfer, "I19 W20 r06"_desc, buffer);   // compile-time checked arguments
 * const auto& result = xfer.wait();                  // in a fiber
 * @endcode
 *
 * It is an I2cTransaction, so every I2C driver that runs transactions runs it: the
 * interpreter yields one segment at a time and this class hands them to the driver as
 * starting()/writing()/reading(). The result names the error, the token and the byte.
 *
 * - A callback between segments (`c`) runs when the driver asks for the next segment,
 *   with the bus held. If it ends the transfer, the driver stops (OperationAfterStart::End);
 *   after a read on the STM32 v1 peripheral the repeated START is already programmed, so the
 *   device is addressed for writing once more before the STOP.
 * - `p` (STOP inside the transfer) and an address NACK that a callback polls end the
 *   driver's transaction; this object attaches itself again from detaching(), before the
 *   driver takes the next queued transaction, so the bus stays with it.
 * - `C` (pause) and `t` (timeout) are not supported yet: `C` ends the transfer with
 *   Error::Internal, `t` is ignored.
 *
 * @ingroup modm_architecture_i2c
 */
class I2cTransferBase : public I2cTransaction
{
public:
	using Error = xfer::Error;
	using Result = xfer::Result;

	const Result&
	result() const
	{ return t.result; }

	/// The transfer has ended (successfully or not) and the object can be reused.
	bool
	isDone() const
	{ return t.result.done; }

	/// Waits in a fiber until the transfer has ended.
	const Result&
	wait() const
	{
		modm::this_fiber::poll([this] { return t.result.done; });
		return t.result;
	}

	/// Called in interrupt context once, when the transfer has ended.
	void
	setCompletion(xfer::Completion completion, void *context = nullptr)
	{ t.completion = completion; t.context = context; }

	/// The configuration handler the bus applies before this transfer (I2cDevice).
	void
	setConfiguration(ConfigurationHandler handler)
	{ configuration = handler; }

	/// The interpreter's transfer object, for callbacks.
	xfer::Transfer&
	state()
	{ return t; }

protected:
	explicit I2cTransferBase(xfer::Transfer &transfer) : I2cTransaction(0), t(transfer) {}

	Starting
	starting() override;

	Writing
	writing() override;

	Reading
	reading() override;

	void
	detaching(DetachCause cause) override;

private:
	friend class I2cBus;

	template< class D, class... Args >
	bool
	run(I2cBus &bus, D descriptor, Args... args);

	bool start();
	bool nextData(bool busHeld);
	void doneData();
	static Error map(I2cMaster::Error error);

	xfer::Transfer &t;
	I2cBus *bus{nullptr};
	ConfigurationHandler configuration{nullptr};
	xfer::Segment seg{};
	bool prefetched{false};	///< seg was fetched by detaching() for the next attach
	bool served{false};		///< seg's buffer was handed to the driver
	bool stopFirst{false};	///< a p after a held bus: STOP now, the rest after re-attaching
};

/// A transfer with `Slots` argument slots (the number the descriptor takes).
/// @ingroup modm_architecture_i2c
template< size_t Slots >
class I2cTransfer : public I2cTransferBase
{
public:
	I2cTransfer() : I2cTransferBase(storage) {}

private:
	xfer::TransferN<Slots> storage;
};

}	// namespace modm
