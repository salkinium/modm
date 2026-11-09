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

protected:
	constexpr I2cBus() = default;
	~I2cBus() = default;
};

}	// namespace modm
