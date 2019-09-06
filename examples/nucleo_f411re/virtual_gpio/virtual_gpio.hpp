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

#include <modm/architecture/interface/gpio.hpp>
#include <modm/platform/gpio/base.hpp>

class VirtualGpio : public  modm::platform::Gpio, public modm::GpioIO
{
public:
	VirtualGpio(const modm::platform::Gpio::Port port, const uint8_t pin,
	            bool inverted=false);

	virtual
	~VirtualGpio();

	/// configure pin as input
	virtual void
	setInput() = 0;

	void
	setInput(InputType type);

	/// read input
	virtual bool
	read() = 0;

public:
	/// configure pin as output
	virtual void
	setOutput() = 0;

	/// configure pin as output and set high or low
	void
	setOutput(bool value);

	void
	setOutput(modm::platform::Gpio::OutputType type,
	          modm::platform::Gpio::OutputSpeed speed = modm::platform::Gpio::OutputSpeed::MHz50);

	/// set output to high level
	virtual void
	set() = 0;

	/// set output to high or low level
	void
	set(bool value);

	/// set output to low level
	virtual void
	reset() = 0;

	/// returns if the pin is logically set
	virtual bool
	isSet() = 0;

	/// toggle output level
	void
	toggle();

public:
	/// Configure output pin settings
	virtual void
	configure(modm::platform::Gpio::OutputType type,
	          modm::platform::Gpio::OutputSpeed speed = modm::platform::Gpio::OutputSpeed::MHz50) = 0;

	/// Configure input pin settings
	virtual void
	configure(modm::platform::Gpio::InputType type) = 0;

public:
	virtual modm::Gpio::Direction
	getDirection() = 0;

	void
	setInverted(bool inverted = true);

protected:
	bool isInverted;
public:
	const char port;
	const uint8_t pin;
};
