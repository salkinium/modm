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

#include <unittest/testsuite.hpp>

/// @ingroup modm_test_test_architecture
class LatchBarrierTest : public unittest::TestSuite
{
public:
	void
	setUp();

	void
	testLatch0();

	void
	testLatch1();

	void
	testLatch2();

	void
	testLatch10();

	void
	testLatchWait();

	void
	testBarrier();

	void
	testBarrierWait();
};
