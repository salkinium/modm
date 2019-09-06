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

#pragma once

#include "virtual_gpio.hpp"
#include <modm/debug/logger.hpp>

#pragma push_macro("MODM_LOG_LEVEL")
#undef	MODM_LOG_LEVEL
#define	MODM_LOG_LEVEL modm::log::INFO

template< class Pin >
class VirtualGpioWrapper : public VirtualGpio
{
public:
	VirtualGpioWrapper() :
		VirtualGpio(Pin::port, Pin::pin, Pin::isInverted)
	{}

	~VirtualGpioWrapper() {}

	void
	configure(OutputType type, OutputSpeed speed = OutputSpeed::MHz50) override
	{
		MODM_LOG_DEBUG << "Gpio" << port << pin << ".configure(OutputType::" << uint8_t(type) << ", OutputSpeed::" << uint8_t(speed) << ")" << modm::endl;
		Pin::configure(type, speed);
	}

	void
	configure(InputType type) override
	{
		MODM_LOG_DEBUG << "Gpio" << port << pin << ".configure(InputType::" << uint8_t(type) << ")" << modm::endl;
		Pin::configure(type);
	}

public:
	using VirtualGpio::setInput;

	void
	setInput() override
	{
		MODM_LOG_DEBUG << "Gpio" << port << pin << ".setInput()" << modm::endl;
		Pin::setInput();
	}

	bool
	read() override
	{
		bool value =  Pin::read();
		// this can flood your debug stream, only use when you know what you're doing
//		MODM_LOG_DEBUG << "Gpio" << port << pin << ".read() ~> " << (isInverted ? !value : value) << modm::endl;
		return isInverted ? !value : value;
	}

public:
	using VirtualGpio::setOutput;
	using VirtualGpio::set;
	using VirtualGpio::toggle;

	void
	setOutput() override
	{
		MODM_LOG_DEBUG << "Gpio" << port << pin << ".setOutput()" << modm::endl;
		Pin::setOutput();
	}

	void
	set() override
	{
		// "normal" = !(isInverted == false) -> true
		MODM_LOG_DEBUG << "Gpio" << port << pin << ".set(" << (isInverted ? "Inverted)" : ")") << modm::endl;
		Pin::set(!isInverted);
	}

	void
	reset() override
	{
		// "normal" = (isInverted == false) -> false
		MODM_LOG_DEBUG << "Gpio" << port << pin << ".reset(" << (isInverted ? "Inverted)" : ")") << modm::endl;
		Pin::set(isInverted);
	}

	bool
	isSet() override
	{
		bool value = Pin::isSet();
		MODM_LOG_DEBUG << "Gpio" << port << pin << ".isSet() ~> " << (isInverted ? !value : value) << modm::endl;
		return isInverted ? !value : value;
	}

public:
	Direction
	getDirection() override
	{
		Direction direction = Pin::getDirection();
		MODM_LOG_DEBUG << "Gpio" << port << pin << ".getDirection() ~> " << uint8_t(direction) << modm::endl;
		return direction;
	}

	using VirtualGpio::setInverted;
};

#pragma pop_macro("MODM_LOG_LEVEL")
