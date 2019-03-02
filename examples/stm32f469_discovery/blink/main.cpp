/*
 * Copyright (c) 2011, Fabian Greif
 * Copyright (c) 2013, Kevin Läufer
 * Copyright (c) 2013-2017, Niklas Hauser
 * Copyright (c) 2014, Sascha Schade
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------

#include <modm/board.hpp>
#include <modm/math.hpp>
#include <type_traits>
using namespace modm::literals;

using namespace Board;

// namespace std
// {
// 	template<>
// 	struct is_arithmetic<modm::Uart::Baudrate>
// 	{
// 		static constexpr bool value = true;
// 	};
// }

// ----------------------------------------------------------------------------
int
main()
{
	Board::initialize();
	LedD13::setOutput(modm::Gpio::Low);

	// Use the logging streams to print some messages.
	// Change MODM_LOG_LEVEL above to enable or disable these messages
	MODM_LOG_DEBUG   << "debug"   << modm::endl;
	MODM_LOG_INFO    << "info"    << modm::endl;
	MODM_LOG_WARNING << "warning" << modm::endl;
	MODM_LOG_ERROR   << "error"   << modm::endl;

	uint32_t counter(0);

	// static_assert(std::is_arithmetic<modm::Uart::Baudrate>::value, "not is_arithmetic");
	int32_t baudrate = 115'200_Baud;

	// stlink::Uart::initialize<systemClock, 115'200_Baud>();
	// ft6::I2cMaster::initialize<systemClock, 360_kBaud>();

	while (1)
	{
		LedGreen::toggle();
		modm::delayMilliseconds(Button::read() ? 125 : 500);

		LedOrange::toggle();
		modm::delayMilliseconds(Button::read() ? 125 : 500);

		LedRed::toggle();
		modm::delayMilliseconds(Button::read() ? 125 : 500);

		LedBlue::toggle();
		modm::delayMilliseconds(Button::read() ? 125 : 500);
		LedD13::toggle();



		MODM_LOG_INFO << "loop: " << counter++ << modm::endl;
	}

	return 0;
}
