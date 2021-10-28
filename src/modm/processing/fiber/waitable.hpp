/*
 * Copyright (c) 2020, Erik Henriksson
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------

#pragma once

#include "context.h"
#include "fiber.hpp"
#include "scheduler.hpp"

namespace modm::fiber
{

/**
 * Waitable captures the low level fiber support for synchronization.
 *
 * Other higher level concepts such as channels, semaphores and mutexes are
 * built on top of this.
 *
 * @author Erik Henriksson
 * @ingroup    modm_processing_fiber
 */
class Waitable
{
protected:
	/**
	 * Adds current fiber to the waitlist and yields execution.
	 */
	inline void
	wait()
	{
		pushWaiter(Scheduler::removeCurrent());
		Scheduler::current->jump(*Scheduler::last->next);
	}

	/**
	 * Resumes next fiber in the waitlist (yields execution of current fiber).
	 *
	 * This will push the waiting fiber to the front of the ready queue, the
	 * reason for this is that it allows a more efficient implementation of
	 * message passing (the receiver is guaranteed to consume the sent message
	 * immediately).
	 */
	inline void
	wake()
	{
		Fiber* waiter = popWaiter();
		if (waiter)
		{
			Scheduler::runNext(waiter);
			yield();
		}
	}

private:
	inline Fiber*
	popWaiter()
	{
		if (not lastWaiter) return nullptr;

		Fiber* first = lastWaiter->next;

		if (first == lastWaiter) lastWaiter = nullptr;
		else lastWaiter->next = first->next;

		first->next = nullptr;
		return first;
	}

	inline void
	pushWaiter(Fiber* waiter)
	{
		if (lastWaiter)
		{
			waiter->next = lastWaiter->next;
			lastWaiter->next = waiter;
		}
		else waiter->next = waiter;
		lastWaiter = waiter;
	}

private:
	Fiber* lastWaiter{nullptr};
};

}  // namespace modm
