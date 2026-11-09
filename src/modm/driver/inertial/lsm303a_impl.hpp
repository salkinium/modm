/*
 * Copyright (c) 2015, Niklas Hauser
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------

#ifndef MODM_LSM303A_HPP
#	error  "Don't include this file directly, use 'lsm303a.hpp' instead!"
#endif

#include <cstring>

// MARK: register access
inline bool
modm::Lsm303a::write(uint8_t reg, uint8_t value)
{
	using xfer::operator""_desc;
	command[0] = reg; command[1] = value;
	return run([&]{ return bus.transfer(xfer, "i w2"_desc, address, command); });
}

inline bool
modm::Lsm303a::read(uint8_t reg, uint8_t *buffer, uint8_t length)
{
	using xfer::operator""_desc;
	command[0] = reg;
	return run([&]{ return bus.transfer(xfer, "i w1 r*"_desc, address, command, buffer, size_t(length)); });
}

// ----------------------------------------------------------------------------
// MARK: LIS302 DRIVER
inline modm::Lsm303a::Lsm303a(I2cBus &bus, Data &data, uint8_t address)
:	I2cBusDevice(bus, address), data(data), rawBuffer{7,0,0,0,0,0, 0,0, 0,0,0,0,0,0, 0,0}
{
}

inline bool
modm::Lsm303a::configure(Scale scale, MeasurementRate rate)
{
	// MeasurementRate must be set in Control1
	rawBuffer[0] = i(rate) | 0x07;
	// Scale must be set in Control4
	rawBuffer[3] = (r(scale) | Control4::HR | Control4::BLE).value;

	if (scale == Scale::G2) data.scale = 1;
	else if (scale == Scale::G4) data.scale = 2;
	else if (scale == Scale::G8) data.scale = 4;
	else data.scale = 12;

	if ( this->write(i(Register::CTRL1), rawBuffer[0]) )
	{
		return this->write(i(Register::CTRL4), rawBuffer[3]);
	}
	return false;
}

inline bool
modm::Lsm303a::updateControlRegister(uint8_t index, Control_t setMask, Control_t clearMask)
{
	rawBuffer[index] = (rawBuffer[index] & ~clearMask.value) | setMask.value;
	// update the scale in the data object, if we update CTRL_REG4 (index 3)
	if (index == 3)
	{
		Scale scale = Scale((Control4_t(rawBuffer[3]) & (Control4::FS1 | Control4::FS0)).value);
		if (scale == Scale::G2) data.scale = 1;
		else if (scale == Scale::G4) data.scale = 2;
		else if (scale == Scale::G8) data.scale = 4;
		else data.scale = 12;
	}

	return this->write(0x20 + index, rawBuffer[index]);
}

inline bool
modm::Lsm303a::readAcceleration()
{
	if (this->read(i(Register::STATUS) | 0x80, rawBuffer + 7, 9))
	{
		// copy the memory
		std::memcpy(data.data, rawBuffer + 7, 6);
		return true;
	}

	return false;
}

// ----------------------------------------------------------------------------
inline bool
modm::Lsm303a::updateRegister(uint8_t reg, uint8_t setMask, uint8_t clearMask)
{
	if (this->read(reg, rawBuffer[8]))
	{
		rawBuffer[8] = (rawBuffer[8] & ~clearMask) | setMask;
		return this->write(reg, rawBuffer[8]);
	}

	return false;
}
