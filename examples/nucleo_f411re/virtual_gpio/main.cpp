/*
 * Copyright (c) 2019, Niklas Hauser
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------

#include <modm/board.hpp>
#include "virtual_gpio_wrapper.hpp"

using namespace Board;

VirtualGpioWrapper<LedD13> gpioLED;


void fancyToggle(VirtualGpio *gpio)
{
	gpio->setOutput(modm::Gpio::Low);
	for (uint16_t delay=100; delay < 1000; delay += 100)
	{
		modm::delayMilliseconds(delay);
		gpio->set();
		modm::delayMilliseconds(delay);
		gpio->reset();
	}
}


int
main()
{
	Board::initialize();

	while (true)
	{
		modm::delayMilliseconds(Button::read() ? 100 : 500);

		fancyToggle(&gpioLED);

		static uint8_t counter(0);
		MODM_LOG_INFO << "loop: " << counter++ << modm::endl;
	}

	return 0;
}
