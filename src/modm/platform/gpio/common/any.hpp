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

#include "runtime.hpp"
#include <modm/architecture/interface/gpio.hpp>
#include <concepts>

namespace modm
{

/**
 * Runtime GPIO pin of this device, of an external GPIO expander, or unused.
 *
 * A value type of 8 bytes that can be copied and stored in arrays and, for
 * pins of this device and unused pins, in constexpr tables. Pins of this device
 * take the inlined fast path of `modm::platform::GpioPin` plus one test and
 * branch, pins on expanders call the expander through
 * `modm::GpioExpanderInterface` and block until the bus transfer is done. A
 * default constructed pin is unused like `GpioUnused`: all functions do
 * nothing and `read()` returns false.
 *
 * @code
 * modm::Pca8574<I2cMaster1> expander;
 * modm::GpioExpanderAdapter expander_pins{expander};
 *
 * std::array<modm::AnyGpio, 3> leds{GpioA5{}, GpioUnused{}, {expander_pins, modm::pca8574::Pin::P3}};
 * for (auto led : leds) led.toggle();
 * @endcode
 *
 * Copy the pin into a local variable in hot loops: the compiler can then hoist
 * the test out of the loop and the pin is as fast as `platform::GpioPin`.
 *
 * The functions that take an `InputType` or `OutputType` depend on the hardware
 * of the pin: on an expander they leave the pin unchanged and return false.
 *
 * @warning Pins on expanders must not be used in an interrupt.
 * @ingroup modm_platform_gpio
 */
class AnyGpio : protected platform::GpioPin
{
	// Bit 0 of the address tags an expander: GPIO ports are at least 1 KiB
	// aligned and GpioExpanderInterface objects at least 2 bytes aligned.
	// An unused pin is a tagged null pointer.
	static constexpr uintptr_t Unused{1};

	constexpr bool internal() const { return not (addr & 1u); }

	static GpioExpanderInterface*
	expander(uintptr_t addr) { return reinterpret_cast<GpioExpanderInterface*>(addr - 1u); }

public:
	using InputType = platform::Gpio::InputType;
	using OutputType = platform::Gpio::OutputType;
	using OutputSpeed = platform::Gpio::OutputSpeed;
	using Port = platform::Gpio::Port;

	using platform::GpioPin::GpioPin;

	constexpr AnyGpio() : platform::GpioPin(Unused, 0) {}

	// A template, so that classes derived from AnyGpio are not ambiguous
	template< std::same_as<platform::GpioPin> Pin >
	constexpr AnyGpio(Pin pin) : platform::GpioPin(pin) {}

	template< class Pin > requires std::same_as<typename Pin::Data, platform::detail::DataUnused>
	constexpr AnyGpio(Pin) : AnyGpio() {}

	AnyGpio(GpioExpanderInterface &expander, uint32_t mask)
	:	platform::GpioPin(reinterpret_cast<uintptr_t>(&expander) | 1u, mask) {}

	/// Pin of a `modm::GpioExpanderAdapter`.
	template< class Adapter > requires std::derived_from<Adapter, GpioExpanderInterface>
	AnyGpio(Adapter &adapter, typename Adapter::Pin pin)
	:	AnyGpio(static_cast<GpioExpanderInterface&>(adapter), uint32_t(pin)) {}

	/// @return true if the pin is on an external expander.
	constexpr bool isExternal() const { return not internal() and addr != Unused; }

	/// @return true if the pin does nothing, like `GpioUnused`.
	constexpr bool isUnused() const { return addr == Unused; }

	// The internal path comes first and the expander path is in cold functions:
	// GCC -Os ignores [[unlikely]] for the block placement. The cold functions
	// are static, so that `this` does not escape and a local copy of the pin
	// stays in registers.
	void
	setOutput() const { if (internal()) [[likely]] GpioPin::setOutput(); else x_setOutput(addr, setBits); }

	void
	setOutput(bool value) const { set(value); setOutput(); }

	/// @return false if the pin is on an expander and was left unchanged.
	bool
	setOutput(OutputType type, OutputSpeed speed = OutputSpeed::MHz50) const
	{
		if (internal()) [[likely]] GpioPin::setOutput(type, speed);
		return not isExternal();
	}

	/// @return false if the pin is on an expander and was left unchanged.
	bool
	configure(OutputType type, OutputSpeed speed = OutputSpeed::MHz50) const
	{
		if (internal()) [[likely]] GpioPin::configure(type, speed);
		return not isExternal();
	}

	void
	setInput() const { if (internal()) [[likely]] GpioPin::setInput(); else x_setInput(addr, setBits); }

	/// @return false if the pin is on an expander and was left unchanged.
	bool
	setInput(InputType type) const
	{
		if (internal()) [[likely]] GpioPin::setInput(type);
		return not isExternal();
	}

	/// @return false if the pin is on an expander and was left unchanged.
	bool
	configure(InputType type) const
	{
		if (internal()) [[likely]] GpioPin::configure(type);
		return not isExternal();
	}

	::modm::Gpio::Direction
	getDirection() const
	{
		if (internal()) [[likely]] return GpioPin::getDirection();
		return x_getDirection(addr, setBits);
	}

	/// Back to a floating input, detached from any peripheral signal
	void
	disconnect() const { if (internal()) [[likely]] GpioPin::disconnect(); else x_setInput(addr, setBits); }

	modm_always_inline void
	set() const { if (internal()) [[likely]] GpioPin::set(); else x_set(addr, setBits); }

	modm_always_inline void
	reset() const { if (internal()) [[likely]] GpioPin::reset(); else x_reset(addr, setBits); }

	modm_always_inline void
	set(bool value) const { if (internal()) [[likely]] GpioPin::set(value); else x_set(addr, setBits, value); }

	modm_always_inline void
	toggle() const { if (internal()) [[likely]] GpioPin::toggle(); else x_toggle(addr, setBits); }

	modm_always_inline bool
	isSet() const { if (internal()) [[likely]] return GpioPin::isSet(); return x_isSet(addr, setBits); }

	modm_always_inline bool
	read() const { if (internal()) [[likely]] return GpioPin::read(); return x_read(addr, setBits); }

private:
	[[gnu::cold, gnu::noinline]] static void
	x_setOutput(uintptr_t a, uint32_t m) { if (a != Unused) expander(a)->setOutput(m); }
	[[gnu::cold, gnu::noinline]] static void
	x_setInput(uintptr_t a, uint32_t m) { if (a != Unused) expander(a)->setInput(m); }
	[[gnu::cold, gnu::noinline]] static void
	x_set(uintptr_t a, uint32_t m) { if (a != Unused) expander(a)->set(m); }
	[[gnu::cold, gnu::noinline]] static void
	x_reset(uintptr_t a, uint32_t m) { if (a != Unused) expander(a)->reset(m); }
	[[gnu::cold, gnu::noinline]] static void
	x_set(uintptr_t a, uint32_t m, bool v) { v ? x_set(a, m) : x_reset(a, m); }
	[[gnu::cold, gnu::noinline]] static void
	x_toggle(uintptr_t a, uint32_t m) { if (a != Unused) expander(a)->toggle(m); }
	[[gnu::cold, gnu::noinline]] static bool
	x_isSet(uintptr_t a, uint32_t m) { return a != Unused and expander(a)->isSet(m); }
	[[gnu::cold, gnu::noinline]] static bool
	x_read(uintptr_t a, uint32_t m) { return a != Unused and expander(a)->read(m); }
	[[gnu::cold, gnu::noinline]] static ::modm::Gpio::Direction
	x_getDirection(uintptr_t a, uint32_t m)
	{ return a != Unused ? expander(a)->getDirection(m) : ::modm::Gpio::Direction::In; }
};

/**
 * Static GPIO class that forwards to the `modm::AnyGpio` of `Source::get()`.
 *
 * A bridge for classes that take their pins as template arguments, like
 * `BitBangI2cMaster`, when the pin is only known at runtime. `Source::get()`
 * can return a pin attached at runtime or an entry of a lookup table.
 *
 * @code
 * struct SclSource
 * {
 *     static inline modm::AnyGpio pin{};
 *     static modm::AnyGpio get() { return pin; }
 * };
 * using Scl = modm::AnyGpioAdapter<SclSource>;
 * @endcode
 *
 * There is no compile-time pin behind it, so it has no peripheral signals and
 * cannot be used with `connect()` or `SoftwareGpioPort`.
 *
 * @ingroup modm_platform_gpio
 */
template< class Source >
struct AnyGpioAdapter : public GpioIO
{
	using InputType = AnyGpio::InputType;
	using OutputType = AnyGpio::OutputType;
	using OutputSpeed = AnyGpio::OutputSpeed;

	/// The runtime pin behind this class
	static AnyGpio get() { return Source::get(); }

	static void setOutput() { Source::get().setOutput(); }
	static void setOutput(bool value) { Source::get().setOutput(value); }
	static void setOutput(OutputType type, OutputSpeed speed = OutputSpeed::MHz50)
	{ Source::get().setOutput(type, speed); }
	static void configure(OutputType type, OutputSpeed speed = OutputSpeed::MHz50)
	{ Source::get().configure(type, speed); }
	static void set() { Source::get().set(); }
	static void set(bool value) { Source::get().set(value); }
	static void reset() { Source::get().reset(); }
	static void toggle() { Source::get().toggle(); }
	static bool isSet() { return Source::get().isSet(); }
	static void setInput() { Source::get().setInput(); }
	static void setInput(InputType type) { Source::get().setInput(type); }
	static void configure(InputType type) { Source::get().configure(type); }
	static bool read() { return Source::get().read(); }
	static Direction getDirection() { return Source::get().getDirection(); }
	static void disconnect() { Source::get().disconnect(); }
};

static_assert(GpioIOPin<platform::GpioPin>);
static_assert(GpioIOPin<AnyGpio>);
static_assert(sizeof(platform::GpioPin) == 2 * sizeof(uint32_t));
static_assert(sizeof(AnyGpio) == 2 * sizeof(uint32_t));

} // namespace modm
