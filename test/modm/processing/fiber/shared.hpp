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

#pragma once
#include <array>
#include <modm/processing/fiber.hpp>

// shared objects to reduce memory consumption
[[maybe_unused]] static inline modm::fiber::Stack<> stack1, stack2;
[[maybe_unused]] static inline uint8_t state;
