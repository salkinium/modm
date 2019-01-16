/*
 * Copyright (c) 2010-2012, Fabian Greif
 * Copyright (c) 2011, Georgi Grinshpun
 * Copyright (c) 2012-2017, Niklas Hauser
 * Copyright (c) 2013, David Hebbeker
 * Copyright (c) 2013, Kevin Läufer
 * Copyright (c) 2014, Sascha Schade
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------

#ifndef MODM_SOFTWARE_BITBANG_I2C_HPP
#	error	"Don't include this file directly, use 'bitbang_i2c_master.hpp' instead!"
#endif

// debugging for serious dummies
//*
#include <modm/debug.hpp>
#define DEBUG_STREAM(x) MODM_LOG_DEBUG << x
#define DEBUG_STREAM_N(x) MODM_LOG_DEBUG << x << '\n'
/*/
#define DEBUG_STREAM(x)
#define DEBUG_STREAM_N(x)
//*/

template <class Scl, class Sda>
uint16_t modm::platform::BitBangI2cMaster<Scl, Sda>::delayTime{3};
template <class Scl, class Sda>
modm::I2cMaster::Error modm::platform::BitBangI2cMaster<Scl, Sda>::errorState{Error::NoError};
template <class Scl, class Sda>
const char* modm::platform::BitBangI2cMaster<Scl, Sda>::description{nullptr};
template <class Scl, class Sda>
void* modm::platform::BitBangI2cMaster<Scl, Sda>::arguments[20];
template <class Scl, class Sda>
size_t modm::platform::BitBangI2cMaster<Scl, Sda>::length{0};


template <class Scl, class Sda>
template< class SystemClock, uint32_t baudrate, uint16_t tolerance >
void
modm::platform::BitBangI2cMaster<Scl, Sda>::initialize()
{
	delayTime = uint32_t(250000000) / baudrate;
	if (delayTime == 0) delayTime = 1;

	SCL::set();
	SDA::set();
}

template <class Scl, class Sda>
template< template<modm::platform::Peripheral _> class... Signals, modm::I2cMaster::ResetDevices reset >
void
modm::platform::BitBangI2cMaster<Scl, Sda>::connect(PullUps pullups)
{
	using Connector = GpioConnector<Peripheral::BitBang, Signals...>;
	static_assert(sizeof...(Signals) == 2, "BitBangI2cMaster<Scl, Sda>::connect() requires one Scl and one Sda signal!");
	static_assert(Connector::template Contains<SCL> and Connector::template Contains<SDA>,
				  "BitBangI2cMaster<Scl, Sda> can only connect to the same Scl and Sda signals of the declaration!");
	const Gpio::InputType input =
		(pullups == PullUps::Internal) ? Gpio::InputType::PullUp : Gpio::InputType::Floating;

	Connector::disconnect();
	SCL::configure(input);
	SDA::configure(input);
	SCL::setOutput(SCL::OutputType::OpenDrain);
	SDA::setOutput(SDA::OutputType::OpenDrain);
	if (reset != ResetDevices::NoReset) resetDevices<SCL, uint32_t(reset)>();
	Connector::connect();
}

template <class Scl, class Sda>
modm::I2cMaster::Error
modm::platform::BitBangI2cMaster<Scl, Sda>::start_transfer()
{
	// reset error state
	errorState = Error::NoError;
	uint8_t address{0};
	void **arg = arguments;

	const auto hex2bin = [](const char **fmt) -> uint8_t
	{
		const auto char2bin = [](char d) -> uint8_t
		{
			d &= 0x1F;
			if (d < 16) return d + 9;
			return d - 16;
		};
		char one = *(*fmt)++;
		char two = *(*fmt)++;
		return (char2bin(one) << 4) | char2bin(two);
	};


	char current{'-'};
	while(*description)
	{
		switch(*description++)
		{
			case 'I':
				address = (hex2bin(&description) << 1) & 0xfe;
				DEBUG_STREAM_N("address=" << address);
				break;

			case 'W':
				if (current != 'w') startCondition();
				current = 'w';
				write(address);
				while(*description and *description != '-' and *description != ' ') {
					uint8_t val = hex2bin(&description);
					DEBUG_STREAM_N("write=" << val);
					write(val);
				}
				break;

			case 's':
				current = 's';
				break;

			case 'r':
				if (current != 'r') startCondition();
				current = 'r';
				write(address | modm::I2c::Read);
				{
					size_t length = hex2bin(&description);
					uint8_t *buffer = (uint8_t *) *arg++;
					DEBUG_STREAM_N("length=" << length);
					DEBUG_STREAM_N("buffer=" << buffer);
					while(length-- > 1) {
						read(*buffer, ACK);
						DEBUG_STREAM_N("data=" << *buffer);
						buffer++;
					}
					read(*buffer, NACK);
					DEBUG_STREAM_N("data=" << *buffer);
				}
				break;

			default:
				break;
		}
	}
	if (current != '-') stopCondition();

	return errorState;
}

// ----------------------------------------------------------------------------
// MARK: - error handling
template <class Scl, class Sda>
void
modm::platform::BitBangI2cMaster<Scl, Sda>::error(Error error)
{
	static bool ignore{false};
	if (ignore) return;
	DEBUG_STREAM('E');
	DEBUG_STREAM('0' + static_cast<uint8_t>(error));
	delay2();
	SCL::reset();
	SDA::reset();
	delay2();
	// attempt a stop condition, if it fails there is nothing else we can do
	ignore = true;
	if (!stopCondition()) {
		errorState = error;
		ignore = false;
		return;
	}

	// release both lines
	SCL::set();
	SDA::set();
	// save error state
	errorState = error;
}

// ----------------------------------------------------------------------------
// MARK: - bus condition operations
template <class Scl, class Sda>
bool
modm::platform::BitBangI2cMaster<Scl, Sda>::startCondition()
{
	DEBUG_STREAM('\n');
	DEBUG_STREAM('s');
	// release data line
	SDA::set();
	delay4();
	if(SDA::read() == modm::Gpio::Low)
	{
		// could not release data line
		error(Error::BusBusy);
		return false;
	}
	// release the clock line
	if (!sclSetAndWait())
	{
		// could not release clock line
		error(Error::BusBusy);
		return false;
	}
	// now both pins are High, ready for start

	// pull down data line
	SDA::reset();
	delay2();
	// pull down clock line
	SCL::reset();
	delay2();

	// start condition generated
	return true;
}

template <class Scl, class Sda>
bool
modm::platform::BitBangI2cMaster<Scl, Sda>::stopCondition()
{
	DEBUG_STREAM('S');
	// pull down both lines
	SCL::reset();
	SDA::reset();

	// first release clock line
	if (!sclSetAndWait())
	{
		// could not release clock line
		error(Error::BusCondition);
		return false;
	}
	delay2();
	// release data line
	SDA::set();
	delay4();

	if (SDA::read() == modm::Gpio::Low)
	{
		// could not release data line
		error(Error::BusCondition);
		return false;
	}
	return true;
}

template <class Scl, class Sda>
bool
modm::platform::BitBangI2cMaster<Scl, Sda>::sclSetAndWait()
{
	SCL::set();
	// wait for clock stretching by slave
	// only wait a maximum of 250 half clock cycles
	uint_fast8_t deadlockPreventer = 250;
	while (SCL::read() == modm::Gpio::Low && deadlockPreventer)
	{
		delay4();
		deadlockPreventer--;
		// double the read amount
		if (SCL::read() == modm::Gpio::High) return true;
		delay4();
	}
	// if extreme clock stretching occurs, then there might be an external error
	return deadlockPreventer > 0;
}

// ----------------------------------------------------------------------------
// MARK: - byte operations
template <class Scl, class Sda>
bool
modm::platform::BitBangI2cMaster<Scl, Sda>::write(uint8_t data)
{
	DEBUG_STREAM('W');
	// shift through all 8 bits
	for(uint_fast8_t i = 0; i < 8; ++i)
	{
		if (!writeBit(data & 0x80))
		{
			// arbitration error
			error(Error::ArbitrationLost);
			return false;
		}
		data <<= 1;
	}

	// release sda
	SDA::set();
	delay2();

	// rising clock edge for acknowledge bit
	if (!sclSetAndWait())
	{
		// the slave is allowed to stretch the clock, but not unreasonably long!
		error(Error::BusCondition);
		return false;
	}
	// sample the data line for acknowledge bit
	if(SDA::read() == modm::Gpio::High)
	{
		DEBUG_STREAM('n');
		// we have not received an ACK
		// depending on context, this could be AddressNack or DataNack
		error(data ? Error::AddressNack : Error::DataNack);
		return false;
	}
	DEBUG_STREAM('a');
	delay2();
	// falling clock edge
	SCL::reset();

	return true;
}

template <class Scl, class Sda>
bool
modm::platform::BitBangI2cMaster<Scl, Sda>::read(uint8_t &data, bool ack)
{
	DEBUG_STREAM('R');
	// release data line
	SDA::set();

	// shift through the 8 bits
	data = 0;
	for(uint_fast8_t i = 0; i < 8; ++i)
	{
		data <<= 1;
		if (!readBit(data))
		{
			// slaves don't stretch the clock here
			// this must be arbitration.
			error(Error::ArbitrationLost);
			return false;
		}
	}

	DEBUG_STREAM( (ack ? 'A' : 'N') );
	// generate acknowledge bit
	if (!writeBit(!ack))
	{
		// arbitration too
		error(Error::ArbitrationLost);
		return false;
	}
	// release data line
	SDA::set();
	return true;
}

// ----------------------------------------------------------------------------
// MARK: - bit operations
template <class Scl, class Sda>
bool
modm::platform::BitBangI2cMaster<Scl, Sda>::writeBit(bool bit)
{
	// set the data pin
	SDA::set(bit);
	delay2();

	// rising clock edge, the slave samples the data line now
	if ((SDA::read() == bit) && sclSetAndWait())
	{
		delay2();
		// falling clock edge
		SCL::reset();
		// everything ok
		return true;
	}
	// too much clock stretching
	return false;
}

template <class Scl, class Sda>
bool
modm::platform::BitBangI2cMaster<Scl, Sda>::readBit(uint8_t &data)
{
	// slave sets data line
	delay2();
	// rising clock edge, the master samples the data line now
	if (sclSetAndWait())
	{
		// copy bit into data
		if(SDA::read() == modm::Gpio::High)
			data |= 0x01;

		delay2();
		// falling clock edge
		SCL::reset();
		return true;
	}
	// too much clock stretching
	return false;
}
