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
#include <utility>
#include "waitable.hpp"

namespace modm::fiber
{

/**
 * A Channel allows message passing between fibers.
 *
 * The channel can have three states:
 *
 * 1. Empty: Polling fibers will wait for messages to become available.
 * 2. Full: Pushing threads will wait for buffer space to become available.
 * 3. Ready: There are no waiting fibers. All operations are non-blocking.
 *
 * @author  Erik Henriksson
 * @ingroup modm_processing_fiber
 */
template<typename Data_t>
class Channel : Waitable
{
public:
	bool
	empty()
	{
		return size == 0;
	}

	bool
	full()
	{
		return size > buffer_size;
	}

	/**
	 * Send data to the channel.
	 *
	 * This method will be non-blocking if the channel is in the ready state, otherwise it will
	 * yield execution.
	 *
	 *  @param data Data to add to the channel
	 */
	void
	send(const Data_t& data)
	{
		if (full()) wait();
		// Now we are in empty or ready state.
		// modm_assert(!full(), "Channel::push.full", "Channel should not be full");
		if (empty())
		{
			this->data = std::move(data);
		} else
		{
			buffer[size - 1] = std::move(data);
		}
		++size;
		wake();
	}

	/**
	 * Receive data from the channel.
	 *
	 * This method will be non-blocking if the channel is in the ready state, otherwise it will
	 * yield execution.
	 *
	 *  @return The data from the channel.
	 */
	Data_t
	receive()
	{
		if (empty()) wait();
		// Now we are in full or ready state.
		// modm_assert(!empty(), "Channel::recv.empty", "Channel should not be empty");
		Data_t result;
		if (--size)
		{
			result = std::move(buffer[size]);
		} else
		{
			result = std::move(data);
		}
		wake();
		return std::move(result);
	}

private:
	Data_t data;
	Data_t* buffer{nullptr};
	uint16_t buffer_size{0};
	uint16_t size{0};
};

}  // namespace modm::fiber
