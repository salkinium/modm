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
#include <modm/architecture/interface/i2c_bus_device.hpp>

namespace modm
{

/**
 * CS43L22 audio DAC: register access over its I2C control port.
 *
 * The control port takes a memory address pointer (MAP) byte, bit 7 set increments it
 * for each byte. A read sets the MAP with a write first; the datasheet shows a STOP and a
 * new START between the two (4.4.2), which the descriptor says with `p`.
 * Only the control port: no I2S setup. The device answers only with RESET high.
 *
 * @ingroup modm_driver_cs43l22
 */
class Cs43l22 : public modm::I2cBusDevice
{
public:
	static constexpr uint8_t ChipIdRegister = 0x01;	///< reads 0xE0 | revision (0xE3 for B1)
	static constexpr uint8_t Increment = 0x80;

	Cs43l22(I2cBus &bus, uint8_t address = 0x4A) : I2cBusDevice(bus, address) {}

	bool
	writeRegister(uint8_t reg, uint8_t value)
	{
		using xfer::operator""_desc;
		map[0] = reg; map[1] = value;
		return run([&]{ return bus.transfer(xfer, "s w2"_desc, address, map); });
	}

	/// Several registers from `reg` on: the MAP byte, then the payload joined to it.
	bool
	writeRegisters(uint8_t reg, const uint8_t *values, size_t count)
	{
		using xfer::operator""_desc;
		map[0] = reg | (count > 1 ? Increment : 0);
		return run([&]{ return bus.transfer(xfer, "s w1 +w*"_desc, address, map, values, count); });
	}

	/// Registers from `reg` on: MAP write, STOP, START, read.
	bool
	readRegisters(uint8_t reg, uint8_t *values, size_t count)
	{
		using xfer::operator""_desc;
		map[0] = reg | (count > 1 ? Increment : 0);
		return run([&]{ return bus.transfer(xfer, "s w1 p s r*"_desc, address, map, address, values, count); });
	}

	bool
	readChipId(uint8_t &id)
	{ return readRegisters(ChipIdRegister, &id, 1); }

private:
	uint8_t map[2];
};

}	// namespace modm
