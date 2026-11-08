# General Purpose Input/Output

GPIOs are modeled as individual classes with an interface consisting out of a
common set of static methods.

## Using GPIOs

These classes are used to create architecture independent
definitions for hardware pins which then can be used as template
parameters for miscellaneous device drivers.

Example of a platform-independent blinking light:

<!--
.. compile::
    :target: *
-->

```cpp
#include <modm/platform.hpp>

using Led = GpioOutputB0;

Led::setOutput();
Led::set();

while (true)
{
    Led::toggle();
    modm::delay(500ms);
}
```

You can use the common definitions of `modm::Gpio` to express your intention
for setting a pin. However, you can also use anything that casts to `bool`.

<!--
.. compile::
    :target: *
    :prefix: |
    	using Led = GpioOutputB0;
-->

```cpp
Led::set(modm::Gpio::High); // Turns Led on
Led::set(modm::Gpio::Low); // Turns Led off
Led::set(true);
Led::set(false);
Led::set(1);
Led::set(0);
Led::set();
Led::reset();
```

!!! warning "Initialize your GPIOs"
    Call `Gpio::setInput()` or `Gpio::setOutput()` before the first use,
    otherwise the GPIO state may be undefined!

See the `modm:platform:gpio` module for platform-specific GPIO behaviors,
especially signal connections to peripherals.


## Runtime GPIOs

The static GPIO classes are as fast as possible, but every pin is a separate
type. When a pin is only known at runtime, for example because the wiring
depends on the hardware revision, or the same code is used with several pins,
use one of the runtime pins. They are values of 8 bytes that can be copied and
stored in arrays and `constexpr` tables:

- `modm::platform::GpioPin`: a pin of this device, also an inverted one, as
  port address and BSRR bits, almost as fast as the static classes.
- `modm::AnyGpio`: a pin of this device, of an external GPIO expander, or
  unused. Device pins take the inlined fast path plus one test and branch,
  expander pins call the expander driver through `modm::GpioExpanderInterface`,
  and unused pins do nothing like `GpioUnused`, also when default constructed.

```cpp
GpioPin led = GpioInverted<GpioA5>{};
static constexpr modm::AnyGpio pins[]{GpioA0{}, GpioPin{Gpio::Port::B, 3}, GpioUnused{}};
pins[index].toggle();

modm::Pca8574<I2cMaster1> expander;
modm::GpioExpanderAdapter expander_pins{expander};
modm::AnyGpio relay{expander_pins, modm::pca8574::Pin::P3};
std::array<modm::AnyGpio, 2> outputs{led, relay};
for (auto output : outputs) output.set(); // a register write and an I2C transfer
```

Copy an `AnyGpio` into a local variable in hot loops: the compiler can then move
the test out of the loop and the pin is as fast as `GpioPin`. Expander pins
block until the bus transfer is done, so they must not be used in interrupts.

The functions that take an `InputType` or `OutputType` depend on the hardware of
the pin. On an expander pin `modm::AnyGpio` leaves the pin unchanged and returns
`false`, since for example an open-drain request must not silently drive a bus.

`modm::AnyGpioAdapter<Source>` is a static GPIO class that forwards to the
`AnyGpio` returned by `Source::get()`, for classes that take their pins as
template arguments, like `BitBangI2cMaster`:

```cpp
struct SclSource
{
    static modm::AnyGpio get() { return table[hardware_revision()]; }
};
using Scl = modm::AnyGpioAdapter<SclSource>;
```

Runtime pins are only implemented for STM32 so far.


## Code for Static and Runtime Pins

The concepts `modm::GpioOutputPin`, `modm::GpioInputPin` and `modm::GpioIOPin`
call the GPIO functions on an *instance*. Static GPIO classes satisfy them too,
since static functions can be called through an object. Code that stores a pin
as a member therefore works with static and with runtime pins, and costs
nothing extra with a static pin:

```cpp
template< modm::GpioOutputPin Pin >
struct Pulse
{
    [[no_unique_address]] Pin pin;
    void operator()() { pin.set(); pin.reset(); }
};

Pulse<GpioA4> pulse1;                // no storage, static register access
Pulse<modm::AnyGpio> pulse2{relay};  // one instance for all runtime pins
```

Code that is not speed critical, like the chip select of a SPI device, can
simply take a `modm::AnyGpio` instead of a template parameter.
