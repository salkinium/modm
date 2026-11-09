/*
 * Copyright (c) 2014-2015, Niklas Hauser
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------

#ifndef MODM_INTERFACE_I2C_DEVICE_HPP
#define MODM_INTERFACE_I2C_DEVICE_HPP

#include "i2c.hpp"
#include "i2c_master.hpp"
#include "i2c_transaction.hpp"
#include "i2c_bus.hpp"
#include <modm/architecture/interface/fiber.hpp>
#include <modm/architecture/interface/clock.hpp>
#include <modm/architecture/interface/atomic_lock.hpp>
#include <type_traits>

namespace modm
{

#ifdef MODM_I2C_TRANSFER_POOL
/**
 * Transfer objects shared by the I2cDevices of one I2cMaster (prototype): with the
 * cppdefine `MODM_I2C_TRANSFER_POOL=N` an I2cDevice holds no transfer of its own but takes
 * one of N for each call and waits in its fiber while all are in use. N is the number of
 * calls that may be in flight on the bus at once (the number of fibers using it).
 */
template< class I2cMaster, size_t N = MODM_I2C_TRANSFER_POOL >
class I2cTransferPool
{
	static_assert(N > 0 and N <= 32);
	static inline I2cTransfer<5> slots[N];
	static inline volatile uint32_t used{0};
public:
	/// calls that found all transfers in use and waited (statistics of the prototype)
	static inline volatile uint32_t waits{0};
	static I2cTransferBase *
	acquire()
	{
		modm::atomic::Lock lock;
		for (size_t ii = 0; ii < N; ii++)
			if (not (used & (1ul << ii))) { used = used | (1ul << ii); return &slots[ii]; }
		return nullptr;
	}
	static void
	release(I2cTransferBase *x)
	{
		modm::atomic::Lock lock;
		for (size_t ii = 0; ii < N; ii++)
			if (x == &slots[ii]) used = used & ~(1ul << ii);
	}
};
#endif

/**
 * Base class of an I2C Device.
 *
 * This class provides functions for configuring the adapter and peripheral and starting transactions.
 * It also provides a basic `ping()` resumable function, which checks if the device responds to its address.
 *
 * You need to provide the number of required nesting levels for your driver here.
 * By default, 10 levels are chosen, which should be more than enough in any case.
 *
 * You can choose to use your own custom I2c Transaction Adapter, by providing a class subclassed from
 * an existing I2cAdapter or which implements the I2cTransaction interface manually.
 *
 * A transaction that does not finish within the timeout, for example because a
 * device holds the bus, fails and resets the I2C master with `I2cMaster::reset()`.
 * This also detaches the transactions of the other devices on the same bus.
 *
 * @tparam	I2cMaster		an I2cMaster conforming to the I2cMaster interface.
 * @tparam	NestingLevels	number of nesting levels required for your driver
 * @tparam	Transaction		a class conforming to the I2cTransaction interface.
 *
 * @author	Georgi Grinshpun
 * @author	Niklas Hauser
 * @ingroup modm_architecture_i2c_device
 */
template < class I2cMaster, uint8_t NestingLevels = 10, class Transaction = I2cWriteReadTransaction >
class I2cDevice
{
public:
	///	@param	address	the slave address not yet shifted left (address < 128).
	constexpr I2cDevice(uint8_t address)
	:	transaction(address), configuration(nullptr), deviceAddress(address)
	{
	}

	/// Sets a new address of the slave device.
	///	@param	address	the slave address not yet shifted left (address < 128).
	void
	setAddress(uint8_t address)
	{
		transaction.setAddress(address);
		deviceAddress = address;
	}

	/// Attaches a configuration handler, which is called before a transaction,
	/// whenever the configuration has to be changed.
	void inline
	attachConfigurationHandler(I2c::ConfigurationHandler handler)
	{
		configuration = handler;
	}

	/// Sets the time a transaction may take including the wait for the bus,
	/// zero waits forever. Devices that stretch the clock may need longer.
	void inline
	setTimeout(std::chrono::milliseconds timeout)
	{
		this->timeout = timeout;
	}

	/// @retval true	device responds to address
	/// @retval false	no device with address found
	[[nodiscard]] bool
	ping()
	{
		if constexpr (HasBus) return runTransfer([&](I2cTransferBase &x){ return transferPing(x); });
		else
		{
			modm::this_fiber::poll([&]{ return transaction.configurePing(); });
			return runTransaction();
		}
	}

	/// Starts a write-read transaction and waits until finished.
	[[nodiscard]] bool
	writeRead(const uint8_t *writeBuffer, std::size_t writeSize,
			  uint8_t *readBuffer, std::size_t readSize)
	{
		if constexpr (HasBus) return runTransfer([&](I2cTransferBase &x)
		{
			using xfer::operator""_desc;
			if (writeSize and readSize)
				return I2cMaster::bus().transfer(x, "s w* r*"_desc, deviceAddress, writeBuffer, writeSize, readBuffer, readSize);
			if (writeSize) return transferWrite(x, writeBuffer, writeSize);
			if (readSize) return transferRead(x, readBuffer, readSize);
			return transferPing(x);
		});
		else
		{
			modm::this_fiber::poll([&]{ return transaction.configureWriteRead(writeBuffer, writeSize, readBuffer, readSize); });
			return runTransaction();
		}
	}

	/// Starts a write transaction and waits until finished.
	[[nodiscard]] bool
	write(const uint8_t *buffer, std::size_t size)
	{
		if constexpr (HasBus) return runTransfer([&](I2cTransferBase &x){ return size ? transferWrite(x, buffer, size) : transferPing(x); });
		else
		{
			modm::this_fiber::poll([&]{ return transaction.configureWrite(buffer, size); });
			return runTransaction();
		}
	}

	/// Starts a write transaction and waits until finished.
	[[nodiscard]] bool
	read(uint8_t *buffer, std::size_t size)
	{
		if constexpr (HasBus) return runTransfer([&](I2cTransferBase &x){ return size ? transferRead(x, buffer, size) : transferPing(x); });
		else
		{
			modm::this_fiber::poll([&]{ return transaction.configureRead(buffer, size); });
			return runTransaction();
		}
	}

	/// The result of the last ping(), write(), read() or writeRead() on a platform whose
	/// I2cMaster has a bus(): the error, the token and the byte where it happened.
	const xfer::Result&
	transferResult() const
#ifdef MODM_I2C_TRANSFER_POOL
	{ return lastResult; }
#else
	{ return xfer.result(); }
#endif

protected:
	/// Starts the transaction with our own transaction object.
	bool inline
	startTransaction()
	{
		return startTransaction(&this->transaction);
	}

	/// Starts the transaction with a separate transaction object.
	bool inline
	startTransaction(modm::I2cTransaction *transaction)
	{
		return I2cMaster::start(transaction, configuration);
	}

	/// @returns `true` when transaction is busy.
	bool inline
	isTransactionRunning()
	{
		return transaction.isBusy();
	}

	/// @returns `true` when transaction did not return an error.
	bool inline
	wasTransactionSuccessful()
	{
		return (transaction.getState() != modm::I2c::TransactionState::Error);
	}

	/// Starts our own transaction and waits until finished or the timeout expired.
	bool
	runTransaction()
	{
		// let the master time the bus access out as well
		I2cMaster::setTransactionTimeout(timeout);
		const auto start = modm::Clock::now();
		const auto expired = [&] { return timeout.count() and modm::Clock::now() - start > timeout; };
		bool started{false};
		modm::this_fiber::poll([&]{ return (started = startTransaction()) or expired(); });
		if (started) modm::this_fiber::poll([&]{ return not isTransactionRunning() or expired(); });
		if (not started or isTransactionRunning())
		{
			// The bus is stuck: detach all transactions and reset the master
			I2cMaster::reset();
			return false;
		}
		return wasTransactionSuccessful();
	}

protected:
#ifdef MODM_I2C_DEVICE_NO_TRANSACTION
	/// Prototype: on a master with bus() the default transaction is never started by
	/// ping/read/write/writeRead, so it is left out; drivers that start it themselves
	/// (startTransaction(), runTransaction()) do not compile in this build.
	struct NoTransaction
	{
		constexpr NoTransaction(uint8_t) {}
		void setAddress(uint8_t) {}
	};
	static constexpr bool DropTransaction = requires { I2cMaster::bus(); } and
			std::is_same_v<Transaction, I2cWriteReadTransaction>;
	[[no_unique_address]] std::conditional_t<DropTransaction, NoTransaction, Transaction> transaction;
#else
	Transaction transaction;
#endif
private:
	// ping(), write(), read() and writeRead() run as transfers where the I2cMaster is an
	// I2cBus object (I2cMaster::bus()); elsewhere through the transaction as before
	static constexpr bool HasBus = requires { I2cMaster::bus(); };

	bool
	transferPing(I2cTransferBase &x)
	{
		using xfer::operator""_desc;
		return I2cMaster::bus().transfer(x, "s"_desc, deviceAddress);
	}
	bool
	transferWrite(I2cTransferBase &x, const uint8_t *buffer, std::size_t size)
	{
		using xfer::operator""_desc;
		return I2cMaster::bus().transfer(x, "s w*"_desc, deviceAddress, buffer, size);
	}
	bool
	transferRead(I2cTransferBase &x, uint8_t *buffer, std::size_t size)
	{
		using xfer::operator""_desc;
		return I2cMaster::bus().transfer(x, "s r*"_desc, deviceAddress, buffer, size);
	}

	/// Starts the transfer and waits until finished or the timeout expired.
	template< class Start >
	bool
	runTransfer(Start&& start)
	{
		I2cMaster::setTransactionTimeout(timeout);
		const auto begin = modm::Clock::now();
		const auto expired = [&] { return timeout.count() and modm::Clock::now() - begin > timeout; };
#ifdef MODM_I2C_TRANSFER_POOL
		I2cTransferBase *xp{nullptr};
		bool waited{false};
		modm::this_fiber::poll([&]{ if (not (xp = Pool::acquire())) waited = true; return xp or expired(); });
		if (waited) Pool::waits = Pool::waits + 1;
		if (not xp) { lastResult = {}; lastResult.error = xfer::Error::Aborted; lastResult.done = true; return false; }
		I2cTransferBase &x = *xp;
#else
		I2cTransferBase &x = xfer;
#endif
		x.setConfiguration(configuration);
		bool started{false};
		modm::this_fiber::poll([&]{ return (started = start(x)) or expired(); });
		if (started) modm::this_fiber::poll([&]{ return x.isDone() or expired(); });
		const bool stuck = not started or not x.isDone();
		// The bus is stuck: detach all transactions and reset the master
		if (stuck) I2cMaster::reset();
		const bool ok = not stuck and x.result().error == xfer::Error::None;
#ifdef MODM_I2C_TRANSFER_POOL
		lastResult = x.result();
		Pool::release(xp);
#endif
		return ok;
	}

	I2c::ConfigurationHandler configuration;
	std::chrono::milliseconds timeout{100};
	uint8_t deviceAddress;
#ifdef MODM_I2C_TRANSFER_POOL
	using Pool = I2cTransferPool<I2cMaster>;
	xfer::Result lastResult{};
#else
	I2cTransfer<5> xfer;
#endif
};

}	// namespace modm

#endif // MODM_INTERFACE_I2C_DEVICE_HPP
