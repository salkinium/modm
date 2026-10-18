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

// Every fiber exercises one construct of the static stack usage analysis and
// yields at its deepest point, so that the watermark of the fiber stack shows
// the stack usage including the context switch. The main stack is watermarked
// as well. compare.py compares these measurements with `scons stack`.

#include <modm/board.hpp>
#include <modm/debug/logger.hpp>
#include <modm/processing.hpp>

#include "../integration_test.hpp"

#ifdef __AVR__
constexpr size_t StackSize = 768;
#elif defined(__ARM_ARCH_6M__)
constexpr size_t StackSize = 1024;
#else
constexpr size_t StackSize = 1536;
#endif

// Uses a frame of at least `size` bytes, which the compiler cannot remove
#define FRAME(size) volatile uint8_t frame[size]; frame[0] = 1; frame[size - 1] = frame[0]

// ------------------------------------------------------------------------------------------------
// A chain of direct calls, which use their frame afterwards
modm_noinline static void chain_leaf() { FRAME(32); modm::this_fiber::yield(); frame[1] = 0; }
modm_noinline static void chain_middle() { FRAME(64); chain_leaf(); frame[1] = 0; }
modm_noinline static void chain_top() { FRAME(96); chain_middle(); frame[1] = 0; }

// The same chain as tail calls, which remove their frame before the call
modm_noinline static void tail_leaf() { FRAME(32); modm::this_fiber::yield(); }
modm_noinline static void tail_middle() { FRAME(64); tail_leaf(); }
modm_noinline static void tail_top() { FRAME(96); tail_middle(); }

// A chain that is deeper than its context switch, where an interrupt occurs
modm_noinline static void interrupt_leaf()
{
	FRAME(160);
#ifndef __AVR__
	// The SysTick exception pushes its frame onto the fiber stack
	SCB->ICSR = SCB_ICSR_PENDSTSET_Msk;
	__DSB(); __ISB();
#endif
	frame[1] = 0;
}
modm_noinline static void interrupt_middle() { FRAME(64); interrupt_leaf(); frame[1] = 0; }
modm_noinline static void interrupt_top() { FRAME(96); modm::this_fiber::yield(); interrupt_middle(); frame[1] = 0; }

// ------------------------------------------------------------------------------------------------
// Virtual calls through a base class, only the large implementation yields
struct Shape
{
	virtual void draw() = 0;
};
struct Small : Shape
{
	modm_noinline void draw() override { FRAME(16); }
} small;
struct Large : Shape
{
	modm_noinline void draw() override { FRAME(160); modm::this_fiber::yield(); }
} large;
// An unrelated class with the same vtable slot, which the analysis must exclude
struct Unrelated
{
	virtual void paint() = 0;
};
struct Huge : Unrelated
{
	modm_noinline void paint() override { FRAME(400); }
} huge;
Shape* volatile shapes[] = {&small, &large};
Unrelated* volatile unrelated = &huge;

// ------------------------------------------------------------------------------------------------
// Function pointers: a constant table and a pointer that is stored at runtime
modm_noinline static void callback_small(uint8_t value) { FRAME(16); frame[1] = value; }
modm_noinline static void callback_large(uint8_t value) { FRAME(128); frame[1] = value; modm::this_fiber::yield(); }
modm_noinline static void callback_other(uint8_t value) { FRAME(48); frame[1] = value; }
static void (* const table[])(uint8_t) = {callback_small, callback_large};
void (* volatile hook)(uint8_t) = callback_other;

// ------------------------------------------------------------------------------------------------
// Recursion of a depth that the analysis cannot know
volatile uint8_t recursion_depth = 4;
modm_noinline static void recurse(uint8_t depth)
{
	FRAME(32);
	if (depth) recurse(depth - 1);
	else modm::this_fiber::yield();
	frame[1] = depth;
}

// ------------------------------------------------------------------------------------------------
modm::Fiber<StackSize> fiber_chain([] { chain_top(); });
modm::Fiber<StackSize> fiber_tail([] { tail_top(); });
modm::Fiber<StackSize> fiber_interrupt([] { interrupt_top(); });
modm::Fiber<StackSize> fiber_virtual([] { for (auto* shape : shapes) shape->draw(); });
modm::Fiber<StackSize> fiber_unrelated([] { unrelated->paint(); modm::this_fiber::yield(); });
modm::Fiber<StackSize> fiber_pointer([]
{
	for (auto* callback : table) callback(1);
	hook(2);
});
modm::Fiber<StackSize> fiber_recursion([] { recurse(recursion_depth); });
modm::Fiber<StackSize> fiber_logger([]
{
	MODM_LOG_INFO << "logger " << uint32_t(1234567) << " " << int16_t(-42) << modm::endl;
	MODM_LOG_INFO.printf("printf %u %s\n", 42u, "text");
	modm::this_fiber::yield();
});
modm::Fiber<StackSize> fiber_idle([] { modm::this_fiber::yield(); });

// ------------------------------------------------------------------------------------------------
// The main stack is watermarked below the current stack pointer
constexpr uint8_t Watermark = 0xa5;
#ifdef __AVR__
extern uint8_t __heap_start[];
static uint8_t* const main_bottom = __heap_start;
static uint8_t* const main_top = reinterpret_cast<uint8_t*>(RAMEND + 1);
#else
extern uint8_t __main_stack_bottom[], __main_stack_top[];
static uint8_t* const main_bottom = __main_stack_bottom;
static uint8_t* const main_top = __main_stack_top;
#endif

modm_noinline static void
main_watermark()
{
	uint8_t* sp = reinterpret_cast<uint8_t*>(__builtin_frame_address(0)) - 64;
	for (uint8_t* byte = main_bottom; byte < sp; byte++) *byte = Watermark;
}

static size_t
main_usage()
{
	uint8_t* byte = main_bottom;
	while (byte < main_top and *byte == Watermark) byte++;
	return main_top - byte;
}

int
main()
{
	Board::initialize();
	MODM_LOG_INFO << "Stack usage" << modm::endl;
	main_watermark();

	modm::fiber::Scheduler::run(modm::fiber::Scheduler::AutoWatermark);

	MODM_LOG_INFO << "stack fiber_chain " << fiber_chain.stack_usage() << modm::endl;
	MODM_LOG_INFO << "stack fiber_tail " << fiber_tail.stack_usage() << modm::endl;
	MODM_LOG_INFO << "stack fiber_interrupt " << fiber_interrupt.stack_usage() << modm::endl;
	MODM_LOG_INFO << "stack fiber_virtual " << fiber_virtual.stack_usage() << modm::endl;
	MODM_LOG_INFO << "stack fiber_unrelated " << fiber_unrelated.stack_usage() << modm::endl;
	MODM_LOG_INFO << "stack fiber_pointer " << fiber_pointer.stack_usage() << modm::endl;
	MODM_LOG_INFO << "stack fiber_recursion " << fiber_recursion.stack_usage() << modm::endl;
	MODM_LOG_INFO << "stack fiber_logger " << fiber_logger.stack_usage() << modm::endl;
	MODM_LOG_INFO << "stack fiber_idle " << fiber_idle.stack_usage() << modm::endl;
	// The main stack is measured last, after printing integers
	MODM_LOG_INFO << "stack main " << main_usage() << modm::endl;
	return finishTest(true);
}
