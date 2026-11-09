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
#include "i2c_bus.hpp"
#include <modm/architecture/interface/clock.hpp>
#include <chrono>

namespace modm
{

/**
 * Base of device drivers that run on an I2cBus object (compiled once for all buses).
 *
 * The driver describes each transaction with a string and runs it with run():
 *
 * @code
 * bool readRegisters(uint8_t reg, uint8_t *buffer, size_t length)
 * {
 *     this->reg = reg;
 *     return run([&]{ return bus.transfer(xfer, "i w1 r*"_desc, address, &this->reg, buffer, length); });
 * }
 * @endcode
 *
 * run() waits in a fiber until the transfer has ended or the timeout expired (then it
 * resets the bus, as I2cDevice does). result() has the error with its token and byte.
 *
 * @ingroup modm_architecture_i2c_device
 */
class I2cBusDevice
{
public:
	I2cBusDevice(I2cBus &bus, uint8_t address) : bus(bus), address(address) {}

	void
	setAddress(uint8_t address)
	{ this->address = address; }

	uint8_t
	getAddress() const
	{ return address; }

	/// The time a transfer may take including the wait for the bus, zero waits forever.
	void
	setTimeout(std::chrono::milliseconds timeout)
	{ this->timeout = timeout; }

	void
	attachConfigurationHandler(I2c::ConfigurationHandler handler)
	{ xfer.setConfiguration(handler); }

	/// The result of the last transfer.
	const xfer::Result&
	result() const
	{ return xfer.result(); }

	/// The device answers its address.
	[[nodiscard]] bool
	ping()
	{
		using xfer::operator""_desc;
		return run([&]{ return bus.transfer(xfer, "i"_desc, address); });
	}

protected:
	/// Starts a transfer with `start()` (until the bus takes it) and waits for its end.
	template< class Start >
	bool
	run(Start &&start)
	{
		const auto begin = modm::Clock::now();
		const auto expired = [&] { return timeout.count() and modm::Clock::now() - begin > timeout; };
		bool started{false};
		modm::this_fiber::poll([&]{ return (started = start()) or expired(); });
		if (started) modm::this_fiber::poll([&]{ return xfer.isDone() or expired(); });
		if (not started or not xfer.isDone())
		{
			bus.reset();	// the bus is stuck: detach all transfers, reset and clear it
			return false;
		}
		return xfer.result().error == xfer::Error::None;
	}

	I2cBus &bus;
	uint8_t address;
	std::chrono::milliseconds timeout{100};
	I2cTransfer<6> xfer;
};

}	// namespace modm
