/*
 * Copyright (c) 2020, Erik Henriksson
 * Copyright (c) 2024, Niklas Hauser
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------

#include "fiber_test.hpp"

#include <array>
#include <modm/debug/logger.hpp>
#include <modm/processing/fiber.hpp>
#include <modm-test/mock/clock.hpp>

using namespace std::chrono_literals;
using test_clock = modm_test::chrono::milli_clock;

enum State
{
	INVALID,
	F1_START,
	F1_END,
	F2_START,
	F2_END,
	F3_START,
	F3_END,
	F4_START,
	F4_END,
	F5_START,
	F5_INCR10,
	F5_INCR20,
	F5_INCR30,
	F5_END,
	F6_START,
	F6_SLEEP1,
	F6_SLEEP2,
	F6_END,
	SUBROUTINE_START,
	SUBROUTINE_END,
	CONSUMER_START,
	CONSUMER_END,
	PRODUCER_START,
	PRODUCER_END,
};

static std::array<State, 10> states = {};
static size_t states_pos = 0;
static modm::fiber::Stack<1024> stack1, stack2;
#define ADD_STATE(state) states[states_pos++] = state;

static void
f1()
{
	ADD_STATE(F1_START);
	modm::this_fiber::yield();
	ADD_STATE(F1_END);
}

static void
f2()
{
	ADD_STATE(F2_START);
	modm::this_fiber::yield();
	ADD_STATE(F2_END);
}

void
FiberTest::testOneFiber()
{
	states_pos = 0;
	modm::fiber::Task fiber(stack1, f1);
	modm::fiber::Scheduler::run();
	TEST_ASSERT_EQUALS(states_pos, 2u);
	TEST_ASSERT_EQUALS(states[0], F1_START);
	TEST_ASSERT_EQUALS(states[1], F1_END);
}

void
FiberTest::testTwoFibers()
{
	states_pos = 0;
	modm::fiber::Task fiber1(stack1, f1), fiber2(stack2, f2);
	modm::fiber::Scheduler::run();
	TEST_ASSERT_EQUALS(states_pos, 4u);
	TEST_ASSERT_EQUALS(states[0], F1_START);
	TEST_ASSERT_EQUALS(states[1], F2_START);
	TEST_ASSERT_EQUALS(states[2], F1_END);
	TEST_ASSERT_EQUALS(states[3], F2_END);
}

static __attribute__((noinline)) void
subroutine()
{
	ADD_STATE(SUBROUTINE_START);
	modm::this_fiber::yield();
	ADD_STATE(SUBROUTINE_END);
}

static void
f3()
{
	ADD_STATE(F3_START);
	subroutine();
	ADD_STATE(F3_END);
}

void
FiberTest::testYieldFromSubroutine()
{
	states_pos = 0;
	modm::fiber::Task fiber1(stack1, f1), fiber2(stack2, f3);
	modm::fiber::Scheduler::run();
	TEST_ASSERT_EQUALS(states_pos, 6u);
	TEST_ASSERT_EQUALS(states[0], F1_START);
	TEST_ASSERT_EQUALS(states[1], F3_START);
	TEST_ASSERT_EQUALS(states[2], SUBROUTINE_START);
	TEST_ASSERT_EQUALS(states[3], F1_END);
	TEST_ASSERT_EQUALS(states[4], SUBROUTINE_END);
	TEST_ASSERT_EQUALS(states[5], F3_END);
}


void
FiberTest::testPollFor()
{
	test_clock::setTime(1251);
	TEST_ASSERT_TRUE(modm::this_fiber::poll_for(20ms, []{ return true; }));
	// timeout is tested in the SleepFor() test
}


void
FiberTest::testPollUntil()
{
	test_clock::setTime(451250);
	TEST_ASSERT_TRUE(modm::this_fiber::poll_until(modm::Clock::now() + 20ms, []{ return true; }));
	TEST_ASSERT_TRUE(modm::this_fiber::poll_until(modm::Clock::now() - 20ms, []{ return true; }));
	// timeout is tested in the SleepUntil() tests
}


static void
f4()
{
	ADD_STATE(F4_START);
	modm::this_fiber::sleep_for(50ms);
	ADD_STATE(F4_END);
}

static void
f5()
{
	ADD_STATE(F5_START);
	test_clock::increment(10);
	ADD_STATE(F5_INCR10);
	modm::this_fiber::yield();

	test_clock::increment(20);
	ADD_STATE(F5_INCR20);
	modm::this_fiber::yield();

	test_clock::increment(30);
	ADD_STATE(F5_INCR30);
	modm::this_fiber::yield();

	ADD_STATE(F5_END);
}

static void
runSleepFor(uint32_t startTime)
{
	test_clock::setTime(startTime);
	states_pos = 0;
	modm::fiber::Task fiber1(stack1, f4), fiber2(stack2, f5);
	modm::fiber::Scheduler::run();

	TEST_ASSERT_EQUALS(states_pos, 7u);
	TEST_ASSERT_EQUALS(states[0], F4_START);
	TEST_ASSERT_EQUALS(states[1], F5_START);
	TEST_ASSERT_EQUALS(states[2], F5_INCR10);
	TEST_ASSERT_EQUALS(states[3], F5_INCR20);
	TEST_ASSERT_EQUALS(states[4], F5_INCR30);
	TEST_ASSERT_EQUALS(states[5], F4_END);
	TEST_ASSERT_EQUALS(states[6], F5_END);
}


void
FiberTest::testSleepFor()
{
	runSleepFor(16203);
	runSleepFor(0xffff'ffff - 30);
}

static void
f6()
{
	ADD_STATE(F6_START);
	ADD_STATE(F6_SLEEP1);
	modm::this_fiber::sleep_until(modm::Clock::now() + 50ms);
	ADD_STATE(F6_SLEEP2);
	modm::this_fiber::sleep_until(modm::Clock::now() - 50ms);
	ADD_STATE(F6_END);
}

static void
runSleepUntil(uint32_t startTime)
{
	test_clock::setTime(startTime);
	states_pos = 0;
	modm::fiber::Task fiber1(stack1, f6), fiber2(stack2, f5);
	modm::fiber::Scheduler::run();

	TEST_ASSERT_EQUALS(states_pos, 9u);
	TEST_ASSERT_EQUALS(states[0], F6_START);
	TEST_ASSERT_EQUALS(states[1], F6_SLEEP1);
	TEST_ASSERT_EQUALS(states[2], F5_START);
	TEST_ASSERT_EQUALS(states[3], F5_INCR10);
	TEST_ASSERT_EQUALS(states[4], F5_INCR20);
	TEST_ASSERT_EQUALS(states[5], F5_INCR30);
	TEST_ASSERT_EQUALS(states[6], F6_SLEEP2);
	TEST_ASSERT_EQUALS(states[7], F5_END);
	TEST_ASSERT_EQUALS(states[8], F6_END);
}

void
FiberTest::testSleepUntil()
{
	runSleepUntil(1502);
	runSleepUntil(0xffff'ffff - 30);
}
