/*
 * Copyright (c) 2017, Sascha Schade
 * Copyright (c) 2017-2019, Niklas Hauser
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------

#ifndef MODM_I2C_MASTER_HAL_HPP
#define MODM_I2C_MASTER_HAL_HPP

#include <modm/platform/device.hpp>
#include <modm/architecture/interface/i2c_master.hpp>
#include <modm/container.hpp>

namespace modm::platform
{

struct I2cMasterHal
{
	I2C_TypeDef *const instance;

	// parameter advice
	modm::I2c::Operation nextOperation{modm::I2c::Operation::Stop};

	// transaction queue management
	struct ConfiguredTransaction
	{
		inline ConfiguredTransaction()
		:	transaction(nullptr), configuration(nullptr) {}

		inline ConfiguredTransaction(modm::I2cTransaction *transaction, modm::I2c::ConfigurationHandler configuration)
		:	transaction(transaction), configuration(configuration) {}

		modm::I2cTransaction *transaction;
		modm::I2c::ConfigurationHandler configuration;
	};

	modm::BoundedQueue<ConfiguredTransaction, 8> queue{};
	modm::I2c::ConfigurationHandler configuration{nullptr};

	// delegating
	modm::I2cTransaction *transaction{nullptr};
	modm::I2cMaster::Error error{modm::I2cMaster::Error::NoError};

	// buffer management
	modm::I2cTransaction::Starting starting{0, modm::I2c::OperationAfterStart::Stop};
	modm::I2cTransaction::Writing writing{nullptr, 0, modm::I2c::OperationAfterWrite::Stop};
	modm::I2cTransaction::Reading reading{nullptr, 0, modm::I2c::OperationAfterRead::Stop};

	// helper functions
	void
	callStarting(const bool startCondition);

	void
	callWriteOperation(const bool startCondition);

	void
	callReadOperation(const bool startCondition);

	void
	callStarting();

	void
	callNextTransaction();

	// ----------------------------------------------------------------------------
	void
	isr_event();

	void
	isr_error();

	// ----------------------------------------------------------------------------
	void
	initializeWithPrescaler(uint32_t timingRegisterValue);

	void
	reset();

	// MARK: - ownership
	bool
	start(modm::I2cTransaction *transaction, modm::I2cMaster::ConfigurationHandler handler);
};

}

#endif // MODM_I2C_MASTER_HAL_HPP