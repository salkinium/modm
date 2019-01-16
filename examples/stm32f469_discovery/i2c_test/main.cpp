/*
 * Copyright (c) 2015, Kevin Läufer
 * Copyright (c) 2015-2018, Niklas Hauser
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------

#include <modm/board.hpp>

#include "bitbang_i2c_master.hpp"

using namespace Board;

using Scl = GpioB8;
using Sda = GpioB9;
using Master = BitBangI2cMaster<Scl, Sda>;

int main()
{
	Board::initialize();

	Master::connect<Scl::BitBang, Sda::BitBang>();
	Master::initialize<Board::systemClock, 100'000>();

	// 3x inline writes to device 0x2A
	// Configure FT6x06 for polling mode, 60Hz active rate, 25Hz monitor rate
	Master::transfer("-I2a -WA400-s -W883C-s -W8919");

	while (1)
	{
		modm::delayMilliseconds(2000);

		// read touch information with 14 bytes into buffer
		uint8_t read_buffer[14];
		Master::transfer("-I2a-W01-r0E", read_buffer);

		MODM_LOG_INFO << "\n\n";
		for (uint8_t data : read_buffer)
		{ MODM_LOG_INFO << modm::hex << data << modm::ascii << " "; }
		MODM_LOG_INFO << modm::endl;
	}

	return 0;
}
