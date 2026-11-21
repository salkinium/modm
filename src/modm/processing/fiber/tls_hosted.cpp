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

#include "scheduler.hpp"
#include <modm/architecture/interface/assert.hpp>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>

// With emulated TLS, the compiler calls __emutls_get_address() with a control
// object for every thread_local variable. This replaces the implementation of
// the compiler runtime so that each fiber has its own copy: Each variable gets
// an index on first access and each context has an array of pointers to its
// copies, which are allocated on first access. Fibers store this array in a
// pointer at the top of their stack, other contexts per thread.

namespace
{

// Same layout as in the compiler runtime
struct EmutlsObject
{
	size_t size;
	size_t align;
	std::atomic<size_t> index;
	const void *templ;
};

struct Storage
{
	size_t size;
	void *data[];
};

std::atomic<size_t> tls_count;
pthread_key_t tls_key;
pthread_once_t tls_once = PTHREAD_ONCE_INIT;

void
tls_free(void *pointer)
{
	if (auto *storage = (Storage*) pointer)
	{
		for (size_t ii = 0; ii < storage->size; ii++) std::free(storage->data[ii]);
		std::free(storage);
	}
}

void
tls_create_key()
{
	pthread_key_create(&tls_key, tls_free);
}

}

extern "C"
{

void*
__emutls_get_address(void *pointer)
{
	auto *object = (EmutlsObject*) pointer;
	size_t index = object->index.load(std::memory_order_acquire);
	if (not index)
	{
		size_t expected{0};
		index = ++tls_count;
		if (not object->index.compare_exchange_strong(expected, index))
			index = expected;
	}

	// The context is the first member of the task, see Task::tls_reserve()
	const auto *task = (const modm_context_t*) modm::fiber::Scheduler::instance().current;
	if (not task) pthread_once(&tls_once, tls_create_key);
	Storage *storage = task ? *(Storage**) task->top : (Storage*) pthread_getspecific(tls_key);
	if (not storage or storage->size < index)
	{
		const size_t old_size = storage ? storage->size : 0;
		const size_t size = index + 8;
		storage = (Storage*) std::realloc(storage, sizeof(Storage) + size * sizeof(void*));
		std::memset(storage->data + old_size, 0, (size - old_size) * sizeof(void*));
		storage->size = size;
		if (task) *(Storage**) task->top = storage;
		else pthread_setspecific(tls_key, storage);
	}

	void *&data = storage->data[index - 1];
	if (not data)
	{
		const size_t align = std::max(object->align, sizeof(void*));
		data = std::aligned_alloc(align, (object->size + align - 1) & ~(align - 1));
		if (object->templ) std::memcpy(data, object->templ, object->size);
		else std::memset(data, 0, object->size);
	}
	return data;
}

uintptr_t*
modm_context_tls_init(uintptr_t *bottom, uintptr_t *top)
{
	// Reserve one aligned pointer for the storage of this fiber
	top -= modm::fiber::StackAlignment / sizeof(uintptr_t);
	*top = 0;
	modm_assert(uintptr_t(top) - uintptr_t(bottom) >= modm::fiber::StackSizeMinimum, "fbr.tls",
			"Stack size must be larger than minimum stack size + thread-local storage!");
	return top;
}

void
modm_context_tls_reset(uintptr_t *top)
{
	tls_free((void*) *top);
	*top = 0;
}

}

// The destructors of thread_local objects are not called in fibers. The system
// would call them when the thread exits, long after the copies of the fiber
// were freed, so their registration is ignored. Outside of fibers it is
// forwarded to the system, which destroys the objects of a thread at its exit.
template< typename Function >
static Function
system_function(const char *name)
{
	return reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
}

extern "C" int
__cxa_thread_atexit(void (*function)(void*), void *object, void *dso)
{
	if (modm::this_fiber::get_id()) return 0;
	using Function = int(*)(void(*)(void*), void*, void*);
	static const auto system = system_function<Function>("__cxa_thread_atexit");
	return system(function, object, dso);
}

#ifdef __APPLE__
// Clang registers thread_local destructors with this function on Darwin
extern "C" void
_tlv_atexit(void (*function)(void*), void *object)
{
	if (modm::this_fiber::get_id()) return;
	using Function = void(*)(void(*)(void*), void*);
	static const auto system = system_function<Function>("_tlv_atexit");
	system(function, object);
}
#endif
