/*
 * Copyright (c) 2019, 2023, Raphael Lehmann
 * Copyright (c) 2021, Christopher Durand
 * Copyright (c) 2022, Rasmus Kleist Hørlyck Sørensen
 * Copyright (c) 2024, Kaelin Laundry
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------

#pragma once

/// @cond

namespace modm::platform::fdcan
{

constexpr MessageRamConfig defaultMessageRamConfig
{
	.filterCountStandard		= 8,
	.filterCountExtended		= 8,
	.rxFifo0Elements			= 32,
	.rxFifo0ElementSizeBytes	= 2*4 + modm::can::Message::capacity,
	.rxFifo1Elements			= 32,
	.rxFifo1ElementSizeBytes	= 2*4 + modm::can::Message::capacity,
	.rxBufferElements			= 0,
	.rxBufferElementSizeBytes	= 2*4 + modm::can::Message::capacity,
	.txEventFifoElements		= 0,
	.txFifoElements				= 32,
	.txFifoElementSizeBytes		= 2*4 + modm::can::Message::capacity,
};

}
