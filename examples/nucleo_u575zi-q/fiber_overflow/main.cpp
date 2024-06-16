/*
 * Copyright (c) 2024, Niklas Hauser
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------

#include <modm/board.hpp>
#include <modm/processing.hpp>

using namespace Board;
using namespace std::chrono_literals;

modm::Fiber<> blinky([]
{
	while(1)
	{
		Board::LedBlue::toggle();
		modm::this_fiber::sleep_for(0.5s);
	}
});

modm::Fiber<> bad_fiber([]
{

	MODM_LOG_INFO << "\nReboot!\nPush the button to overflow the stack!" << modm::endl;

	while(1)
	{
		// cause stack overflow on button push
		if (Button::read()) asm volatile ("push {r0-r7}");
		modm::this_fiber::yield();
	}
});

// On fiber stack overflow this handler will be called
extern "C" void
UsageFault_Handler()
{
	// check if the fault is a stack overflow *from the fiber stack*
	if (SCB->CFSR & SCB_CFSR_STKOF_Msk and __get_PSP() == __get_PSPLIM())
	{
		// lower the priority of the usage fault to allow the UART interrupts to work
		NVIC_SetPriority(UsageFault_IRQn, (1ul << __NVIC_PRIO_BITS) - 1ul);
		// raise an assertion to report this overflow
		modm_assert(false, "fbr.stkof", "Fiber stack overflow", modm::this_fiber::get_id());
	}
	else HardFault_Handler();
}

int
main()
{
	Board::initialize();
	// Enable the UsageFault handler
	SCB->SHCSR |= SCB_SHCSR_USGFAULTENA_Msk;

	modm::fiber::Scheduler::run();

	return 0;
}
