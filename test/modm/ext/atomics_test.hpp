/*
 * Copyright (c) 2023, Niklas Hauser
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------

#include <unittest/testsuite.hpp>

/// @ingroup modm_test_test_utils
class AtomicsTest : public unittest::TestSuite
{
public:
	void
	testAtomic8();

	void
	testAtomic16();

	void
	testAtomic32();

	void
	testAtomic64();

	void
	testAtomicArray3();

	void
	testAtomicArray();

	void
	testAtomicFlag();
};
