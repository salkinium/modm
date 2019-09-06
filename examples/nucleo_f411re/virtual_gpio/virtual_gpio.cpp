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

#include "virtual_gpio.hpp"
#include <modm/debug/logger.hpp>

#undef	MODM_LOG_LEVEL
#define	MODM_LOG_LEVEL modm::log::INFO

using namespace modm::platform;

VirtualGpio::VirtualGpio(Gpio::Port port, uint8_t pin, bool inverted):
	isInverted(inverted), port('A' + uint8_t(port)), pin(pin)
{
}

VirtualGpio::~VirtualGpio()
{
}

void
VirtualGpio::setInput(InputType type)
{
	configure(type);
	setInput();
}

void
VirtualGpio::setOutput(bool value)
{
	MODM_LOG_DEBUG << "Gpio" << port << pin << ".setOutput(" << value << ")" << modm::endl;
	set(value);
	setOutput();
}

void
VirtualGpio::setOutput(OutputType type, OutputSpeed speed)
{
	configure(type, speed);
	setOutput();
}

void
VirtualGpio::set(bool value)
{
	MODM_LOG_DEBUG << "Gpio" << port << pin << ".set(" << (value ? "High)" : "Low)") << modm::endl;
	if (value) set();
	else reset();
}

void
VirtualGpio::toggle()
{
	MODM_LOG_DEBUG << "Gpio" << port << pin << ".toggle()" << modm::endl;
	if (isSet()) reset();
	else set();
}

void
VirtualGpio::setInverted(bool inverted)
{
	MODM_LOG_DEBUG << "Gpio" << port << pin << ".setInverted(" << inverted << ")" << modm::endl;
	isInverted = inverted;
}
