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
#include "i2c.hpp"
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
 * The bus driver runs the interpreter itself: it asks for one segment at a time, runs the
 * callbacks between segments with the bus held, and reports how each segment ended. The
 * result names the error, the token and the byte. Transfers wait in a queue of their own
 * next to the transactions; while both kinds wait, the driver alternates between them.
 *
 * - After a read on the STM32 v1 peripheral the repeated START is programmed before the
 *   last byte arrives, so a callback there that ends the transfer costs one address byte
 *   (the device is addressed for writing once more before the STOP).
 * - `C` (pause) ends the transfer with Error::Internal, `t` (timeout) is ignored.
 *
 * @ingroup modm_architecture_i2c
 */
class I2cTransferBase
{
public:
	using Error = xfer::Error;
	using Result = xfer::Result;

	const Result&
	result() const
	{ return t.result; }

	/// The transfer has ended (successfully or not) and the object can be reused.
	/// Reads the interpreter's volatile busy flag (Result::done is a plain bool).
	bool
	isDone() const
	{ return not t.busy; }

	/// Waits in a fiber until the transfer has ended.
	const Result&
	wait() const
	{
		modm::this_fiber::poll([this] { return isDone(); });
		return t.result;
	}

	/// Called in interrupt context once, when the transfer has ended.
	void
	setCompletion(xfer::Completion completion, void *context = nullptr)
	{ t.completion = completion; t.context = context; }

	/// The configuration handler the bus applies before this transfer (I2cDevice).
	void
	setConfiguration(I2c::ConfigurationHandler handler)
	{ configuration = handler; }

	/// The interpreter's transfer object, for callbacks.
	xfer::Transfer&
	state()
	{ return t; }

	/// Used by the bus drivers: the queue link and the configuration handler.
	/// @{
	xfer::Transfer &t;
	I2cTransferBase *queueNext{nullptr};
	I2c::ConfigurationHandler configuration{nullptr};
	/// @}

protected:
	explicit I2cTransferBase(xfer::Transfer &transfer) : t(transfer) {}

private:
	friend class I2cBus;

	template< class D, class... Args >
	bool
	run(I2cBus &bus, D descriptor, Args... args);

#ifndef XFER_NO_STRING
	bool
	runParsed(I2cBus &bus, const char *descriptor, const uintptr_t *args, size_t count);
#endif
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
