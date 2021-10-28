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
#include "waitable.hpp"

namespace modm::fiber
{

/**
 * A Semaphore is a lightweight synchronization primitive that can control access to a shared
 * resource. It allows up to MaxValue fibers to access the resource concurrently.
 *
 * @author	Erik Henriksson
 * @ingroup	modm_processing_fiber
 */
class Semaphore : Waitable
{
public:
	using Type = uint8_t;

	constexpr Semaphore(Type maximum=1)
	:	maximum(maximum)
	{}

	Type
	max()
	{
		return maximum;
	}

	/**
	 * Decrements the internal counter and unblocks releasers.
	 *
	 * Yields execution if the counter is zero.
	 */
	void
	acquire()
	{
		if (counter >= maximum) wait();
		++counter;
		wake();
	}

	/**
	 * Increments the internal counter and unblocks acquirers.
	 *
	 * Yields execution if the counter is equal to MaxValue.
	 */
	void
	release()
	{
		if (counter == 0) wait();
		--counter;
		wake();
	}

private:
	Type maximum;
	Type counter{0};
};

}  // namespace modm::fiber
