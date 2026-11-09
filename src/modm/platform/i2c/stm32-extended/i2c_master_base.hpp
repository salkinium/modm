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

#pragma once
#include "i2c_master_common.hpp"

namespace modm::platform
{

/**
 * The I2C controller driver shared by all instances of the NBYTES/RELOAD/AUTOEND peripheral (I2C v2).
 *
 * It implements modm::I2cMaster on the peripheral at `peripheral`, the static
 * `I2cMasterN` classes forward to one object of it each. The state machine was
 * derived from the reference manual and errata models and stress tested on
 * hardware, the states carry the names of these models.
 *
 * @ingroup	modm_platform_i2c
 */
class I2cMasterBase : public I2cMasterCommon
{
public:
	using I2cMasterCommon::I2cMasterCommon;

	/// Attaches the transaction and starts it, or queues it while the bus is in use.
	bool
	start(I2cTransaction *transaction, ConfigurationHandler handler = nullptr) final;

	/// Starts the transfer (bound and begun by I2cTransferBase), or queues it.
	bool
	start(I2cTransferBase &transfer) final;

	/// Ends the transfer with a STOP after the current byte, detaches the current and all
	/// queued transactions, resets the peripheral and clears the bus with `recoverBus`
	/// if a device still holds it. Busy waits up to 4 ms; not from an interrupt.
	void
	reset() final;

	/// The interrupt handler of the event and error interrupts.
	void
	isr();

	/// Disables the peripheral and sets the timing register.
	void
	configure(uint32_t timingRegister);

#ifdef I2C_TIMEOUTR_TIMOUTEN
	/// Sets the SMBus bus timeout to `ticks` of 2048 I2CCLK (1..4096), 0 disables it.
	void
	setTimeout(uint16_t ticks);
#endif

	/// Bytes per NBYTES chunk (1..255), smaller values force RELOAD. For tests only.
	uint8_t maxChunk = 255;

	// the state machine, with the names of the peripheral model
	enum class
	State : uint8_t
	{ Idle, Addr, Tx, TxWait, RxAck, Rx, TcWait, ReloadTx, ReloadRx, Stopping, Disabled };
	enum
	Flag : uint8_t
	{ TXIS = 1, RXNE = 2, NACKF = 4, STOPF = 5, TC = 6, TCR = 7, BERR = 8, ARLO = 9, TIMEOUT = 12 };
	enum class
	Action : uint8_t
	{ None, Start, Write, Read, Next, Reload, Done, Nack, Berr, Arlo, Reset, Unexpected, Stop };

private:
	void begin();
	bool segment(I2cTransaction::Starting starting);
	void unexpected(uint8_t flag);
	void refuse();
	void detach(DetachCause cause);
	void hwReset();
	bool busBusy() const;
	bool fetch(Operation op);
	uint32_t chunk();
	void stop(State before, uint8_t flag);

	void
	go(State before, uint8_t flag, Action action, State after)
	{
		trace(uint8_t(before), flag, uint8_t(action), uint8_t(after));
		state = after;
	}

	volatile State state{State::Idle};
	size_t planned{0};	///< bytes of the buffer programmed into NBYTES so far
	uint32_t timingr{0};
};

} // namespace modm::platform
