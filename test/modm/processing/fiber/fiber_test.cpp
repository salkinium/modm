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
	F1_YIELD1,
	F1_YIELD2,
	F1_YIELD3,
	F1_END,
	F2_START,
	F2_JOIN,
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
	F7_START,
	F7_YIELD,
	F7_END,
	F8_START,
	F8_REQUEST_STOP,
	F8_END,
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

void
FiberTest::testOneFiber()
{
	states_pos = 0;
	modm::fiber::Task fiber(stack1, f1);
	TEST_ASSERT_DIFFERS(fiber.get_id(), modm::fiber::id(0));
	TEST_ASSERT_TRUE(fiber.joinable());
	modm::fiber::Scheduler::run();

	TEST_ASSERT_FALSE(fiber.joinable());
	TEST_ASSERT_EQUALS(states_pos, 2u);
	TEST_ASSERT_EQUALS(states[0], F1_START);
	TEST_ASSERT_EQUALS(states[1], F1_END);
}

void
FiberTest::testTwoFibers()
{
	states_pos = 0;
	modm::fiber::Task fiber1(stack1, [&]
	{
		ADD_STATE(F1_START);
		modm::this_fiber::yield();
		ADD_STATE(F1_YIELD1);
		modm::this_fiber::yield();
		ADD_STATE(F1_YIELD2);
		modm::this_fiber::yield();
		ADD_STATE(F1_YIELD3);
		modm::this_fiber::yield();
		ADD_STATE(F1_END);
	});
	modm::fiber::Task fiber2(stack2, [&]
	{
		ADD_STATE(F2_START);
		TEST_ASSERT_TRUE(fiber1.joinable());
		TEST_ASSERT_FALSE(fiber2.joinable());
		fiber1.join(); // should wait
		ADD_STATE(F2_JOIN);
		fiber2.join(); // should not hang
		ADD_STATE(F2_END);
	});
	modm::fiber::Scheduler::run();
	TEST_ASSERT_EQUALS(states_pos, 8u);
	TEST_ASSERT_EQUALS(states[0], F1_START);
	TEST_ASSERT_EQUALS(states[1], F2_START);
	TEST_ASSERT_EQUALS(states[2], F1_YIELD1);
	TEST_ASSERT_EQUALS(states[3], F1_YIELD2);
	TEST_ASSERT_EQUALS(states[4], F1_YIELD3);
	TEST_ASSERT_EQUALS(states[5], F1_END);
	TEST_ASSERT_EQUALS(states[6], F2_JOIN);
	TEST_ASSERT_EQUALS(states[7], F2_END);
}

static __attribute__((noinline)) void
subroutine()
{
	ADD_STATE(SUBROUTINE_START);
	modm::this_fiber::yield();
	ADD_STATE(SUBROUTINE_END);
}

void
FiberTest::testYieldFromSubroutine()
{
	states_pos = 0;
	modm::fiber::Task fiber1(stack1, f1), fiber2(stack2, [&]
	{
		ADD_STATE(F3_START);
		TEST_ASSERT_TRUE(fiber1.joinable());
		subroutine();
		TEST_ASSERT_FALSE(fiber1.joinable());
		ADD_STATE(F3_END);
	});
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

static void
f7(modm::fiber::stop_token stoken)
{
	ADD_STATE(F7_START);
	while(not stoken.stop_requested())
	{
		ADD_STATE(F7_YIELD);
		modm::this_fiber::yield();
	}
	ADD_STATE(F7_END);
}

void
FiberTest::testStopToken()
{
	states_pos = 0;
	modm::fiber::Task fiber1(stack1, f7);
	modm::fiber::Task fiber2(stack2, [&](modm::fiber::stop_token stoken)
	{
		ADD_STATE(F8_START);
		modm::this_fiber::yield();
		ADD_STATE(F8_REQUEST_STOP);
		fiber1.request_stop();
		modm::this_fiber::yield();
		ADD_STATE(F8_END);
	});
	modm::fiber::Scheduler::run();

	TEST_ASSERT_EQUALS(states_pos, 7u);
	TEST_ASSERT_EQUALS(states[0], F7_START);
	TEST_ASSERT_EQUALS(states[1], F7_YIELD);
	TEST_ASSERT_EQUALS(states[2], F8_START);
	TEST_ASSERT_EQUALS(states[3], F7_YIELD);
	TEST_ASSERT_EQUALS(states[4], F8_REQUEST_STOP);
	TEST_ASSERT_EQUALS(states[5], F7_END);
	TEST_ASSERT_EQUALS(states[6], F8_END);
}
