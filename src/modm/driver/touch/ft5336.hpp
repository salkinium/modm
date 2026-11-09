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
#include <cstring>

namespace modm
{

/**
 * FT5336 capacitive touch controller: registers and the touch points.
 *
 * Register access with a repeated START between the register address and the read.
 *
 * @ingroup modm_driver_ft5336
 */
class Ft5336 : public modm::I2cBusDevice
{
public:
	static constexpr uint8_t TouchStatus = 0x02;	///< TD_STATUS, then 6 bytes per point
	static constexpr uint8_t ChipId = 0xA8;		///< vendor ID 0x51
	static constexpr uint8_t Mode = 0xA4;			///< G_MODE

	struct Point { uint16_t x, y; uint8_t event, id; };

	Ft5336(I2cBus &bus, uint8_t address = 0x38) : I2cBusDevice(bus, address) {}

	bool
	readRegisters(uint8_t reg, uint8_t *values, size_t count)
	{
		using xfer::operator""_desc;
		command[0] = reg;
		return run([&]{ return bus.transfer(xfer, "i w1 r*"_desc, address, command, values, count); });
	}

	bool
	writeRegister(uint8_t reg, uint8_t value)
	{
		using xfer::operator""_desc;
		command[0] = reg; command[1] = value;
		return run([&]{ return bus.transfer(xfer, "i w2"_desc, address, command); });
	}

	/// The number of touches (0..5) and the first two points.
	bool
	readTouches(uint8_t &touches, Point *points)
	{
		if (not readRegisters(TouchStatus, buffer, sizeof(buffer))) return false;
		touches = buffer[0] & 0x0f;
		for (uint8_t ii = 0; ii < 2; ii++)
		{
			const uint8_t *b = buffer + 1 + 6 * ii;
			points[ii] = {uint16_t((b[0] & 0x0f) << 8 | b[1]), uint16_t((b[2] & 0x0f) << 8 | b[3]),
						  uint8_t(b[0] >> 6), uint8_t(b[2] >> 4)};
		}
		return true;
	}

private:
	uint8_t command[2];
	uint8_t buffer[13];
};

}	// namespace modm
