/*
 * Copyright (c) 2020, Erik Henriksson
 * Copyright (c) 2021, Niklas Hauser
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
 * A Mutex is a Semaphore which allows only one fiber to enter at a time.
 *
 * @author	Niklas Hauser
 * @ingroup	modm_processing_fiber
 */
class Mutex : Waitable
{
public:
	void
	acquire()
	{
		if (guard) wait();
		guard = true;
		wake();
	}

	void
	release()
	{
		if (not guard) wait();
		guard = false;
		wake();
	}

private:
	bool guard{false};
};

/**
 * A MutexLock is a helper class that acquires/releases the Mutex via RAII.
 *
 * @author	Erik Henriksson
 * @ingroup	modm_processing_fiber
 */
class MutexLock
{
public:
	MutexLock(Mutex* m) : mutex(m)
	{
		mutex->acquire();
	}

	~MutexLock()
	{
		mutex->release();
	}

private:
	Mutex* mutex;
};

}  // namespace modm::fiber
