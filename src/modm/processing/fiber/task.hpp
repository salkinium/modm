/*
 * Copyright (c) 2020, Erik Henriksson
 * Copyright (c) 2021, 2023, Niklas Hauser
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------
#pragma once

#include "stack.hpp"
#include "context.h"
#include "stop_token.hpp"
#include <type_traits>

// forward declaration
namespace modm::this_fiber { void yield(); }

namespace modm::fiber
{

// forward declaration
class Scheduler;

/// The Fiber scheduling policy.
/// @ingroup modm_processing_fiber
enum class
Start
{
	Now,	// Automatically add the fiber to the active scheduler.
	Later,	// Manually add the fiber to a scheduler.
};

/// Identifier of a fiber task.
/// @ingroup modm_processing_fiber
using id = uintptr_t;

/**
 * The fiber task connects the callable fiber object with the fiber context and
 * scheduler. It constructs the fiber function on the stack if necessary, and
 * adds the contexts to the scheduler. If the fiber function returns, the task
 * is removed from the scheduler. Tasks can then be restarted, which will call
 * the fiber function from the beginning again
 *
 * Note that a task contains no stack, only the control structures necessary for
 * managing a fiber. You may therefore place objects of this class in fast
 * core-local memory, while the stack must remain in DMA-able memory!
 *
 * @see https://en.cppreference.com/w/cpp/thread/jthread
 * @author Erik Henriksson
 * @author Niklas Hauser
 * @ingroup modm_processing_fiber
 */
class Task
{
	Task() = delete;
	Task(const Task&) = delete;
	Task& operator=(const Task&) = delete;
	friend class Scheduler;

	// Make sure that Task and Fiber use a callable constructor, otherwise they
	// may get placed in the .data section including the whole stack!!!
	modm_context_t ctx;
	Task* next;
	Scheduler *scheduler{nullptr};
	stop_state stop{};

public:
	/// @param stack	A stack object that is *NOT* shared with other tasks.
	/// @param closure	A callable object of signature `void()`.
	/// @param start	When to start this task.
	template<size_t Size, class Callable>
	Task(Stack<Size>& stack, Callable&& closure, Start start=Start::Now);

	inline
	~Task()
	{
		request_stop();
		join();
	}

	/// Returns the number of concurrent threads supported by the implementation.
	[[nodiscard]] static constexpr unsigned int
	hardware_concurrency();

	/// Returns a value of std::thread::id identifying the thread associated
	/// with `*this`.
	/// @note This function can be called from an interrupt.
	[[nodiscard]] modm::fiber::id inline
	get_id() const
	{
		return reinterpret_cast<id>(this);
	}

	/// Checks if the Task object identifies an active fiber of execution.
	[[nodiscard]] bool
	joinable() const;

	/// Blocks the current fiber until the fiber identified by `*this`
	/// finishes its execution. Returns immediately if the thread is not joinable.
	void inline
	join()
	{
		if (joinable()) while(isRunning()) modm::this_fiber::yield();
	}

	[[nodiscard]]
	stop_source inline
	get_stop_source()
	{
		return stop.get_source();
	}

	[[nodiscard]]
	stop_token inline
	get_stop_token()
	{
		return stop.get_token();
	}

	/// @note This function can be called from an interrupt.
	bool inline
	request_stop()
	{
		return stop.request_stop();
	}


	/// Watermarks the stack to measure `stack_usage()` later.
	/// @see `modm_context_watermark()`.
	void inline
	watermark_stack()
	{
		modm_context_watermark(&ctx);
	}

	/// @returns the stack usage as measured by a watermark level.
	/// @see `modm_context_stack_usage()`.
	[[nodiscard]] size_t inline
	stack_usage() const
	{
		return modm_context_stack_usage(&ctx);
	}

	/// @returns if the bottom word on the stack has been overwritten.
	/// @see `modm_context_stack_overflow()`.
	[[nodiscard]] bool inline
	stack_overflow() const
	{
		return modm_context_stack_overflow(&ctx);
	}

	/// Adds the task to the currently active scheduler, if not already running.
	/// @returns if the fiber has been scheduled.
	bool
	start();

	/// @returns if the fiber is attached to a scheduler.
	[[nodiscard]] bool inline
	isRunning() const
	{
		return scheduler;
	}
};

}	// namespace modm::fiber

#include "scheduler.hpp"
#include <memory>

/// @cond
namespace modm::fiber
{

template<size_t Size, class T>
Task::Task(Stack<Size>& stack, T&& closure, Start start)
{
	constexpr bool with_stop_token = std::is_invocable_r_v<void, T, stop_token>;
	if constexpr (std::is_convertible_v<T, void(*)()> or
				  std::is_convertible_v<T, void(*)(stop_token)>)
	{
		// A plain function without closure
		using Callable = std::conditional_t<with_stop_token, void(*)(stop_token), void(*)()>;
		auto caller = (uintptr_t) +[](Callable fn)
		{
			if constexpr (with_stop_token) {
				fn(fiber::Scheduler::instance().current->get_stop_token());
			} else fn();
			fiber::Scheduler::instance().unschedule();
		};
		modm_context_init(&ctx, stack.memory, stack.memory + stack.words,
						  caller, (uintptr_t) static_cast<Callable>(closure));
	}
	else
	{
		// lambda functions with a closure must be allocated on the stack ALIGNED!
		constexpr size_t align_mask = std::max(StackAlignment, alignof(std::decay_t<T>)) - 1u;
		constexpr size_t closure_size = (sizeof(std::decay_t<T>) + align_mask) & ~align_mask;
		static_assert(Size >= closure_size + StackSizeMinimum,
				"Stack size must be larger than minimum stack size + aligned sizeof(closure))!");
		// Find a suitable aligned area at the top of stack to allocate the closure
		const uintptr_t ptr = uintptr_t(stack.memory + stack.words) - closure_size;
		// construct closure in place
		::new ((void*)ptr) std::decay_t<T>{std::forward<T>(closure)};
		// Encapsulate the proper ABI function call into a simpler function
		auto caller = (uintptr_t) +[](std::decay_t<T>* closure)
		{
			if constexpr (with_stop_token) {
				(*closure)(fiber::Scheduler::instance().current->get_stop_token());
			} else (*closure)();
			fiber::Scheduler::instance().unschedule();
		};
		// initialize the stack below the allocated closure
		modm_context_init(&ctx, stack.memory, (uintptr_t*)ptr, caller, ptr);
	}
	if (start == Start::Now) this->start();
}

inline bool
Task::start()
{
	if (isRunning()) return false;
	modm_context_reset(&ctx);
	Scheduler::instance().add(*this);
	return true;
}

constexpr unsigned int
Task::hardware_concurrency()
{
	return Scheduler::hardware_concurrency();
}

bool inline
Task::joinable() const
{
	if (not isRunning()) return false;
	if (Scheduler::isInsideInterrupt()) return false;
	return get_id() != Scheduler::instance().get_id();
}

}
/// @endcond
