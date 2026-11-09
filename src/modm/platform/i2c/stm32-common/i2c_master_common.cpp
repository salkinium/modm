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

#include "i2c_master_common.hpp"

namespace modm::platform
{

I2cMasterCommon::Attach
I2cMasterCommon::attach(I2cTransaction *t, ConfigurationHandler handler)
{
	if (not t->attaching())
	{
		t->detaching(DetachCause::FailedToAttach);
		return Attach::Refused;
	}
	if (transaction or active)
	{
		t->queueNext = nullptr;
		t->queueConfiguration = handler;
		if (queueTail) queueTail->queueNext = t; else queueHead = t;
		queueTail = t;
		return Attach::Queued;
	}
	if (handler and handler != configuration) { configuration = handler; handler(); }
	transaction = t;
	return Attach::Now;
}

bool
I2cMasterCommon::finish(DetachCause cause)
{
	active = false;
	if (cause == DetachCause::ErrorCondition and error != Error::AddressNack)
		recorder.freeze();
	I2cTransaction *t = transaction;
	transaction = nullptr;
	if (t) t->detaching(cause);	// may call start() again
	if (transaction or not queueHead) return false;
	I2cTransaction *const n = queueHead;
	queueHead = n->queueNext;
	if (not queueHead) queueTail = nullptr;
	if (n->queueConfiguration and n->queueConfiguration != configuration)
		{ configuration = n->queueConfiguration; configuration(); }
	transaction = n;
	return true;
}

void
I2cMasterCommon::detachAll()
{
	fail(Error::SoftwareReset);
	recorder.freeze();
	while (I2cTransaction *const n = queueHead)
	{
		queueHead = n->queueNext;
		if (not queueHead) queueTail = nullptr;
		n->detaching(DetachCause::ErrorCondition);
	}
	active = false;
	if (I2cTransaction *t = transaction)
	{
		transaction = nullptr;
		t->detaching(DetachCause::ErrorCondition);
	}
}

bool
I2cMasterCommon::fetch(Operation op)
{
	index = 0;
	if (op == Operation::Read)
	{
		const auto r = transaction->reading();
		reading = true; rbuf = r.buffer; length = r.length; next = Operation(r.next);
		return length > 0;
	}
	reading = false;
	if (op == Operation::Write)
	{
		const auto w = transaction->writing();
		wbuf = w.buffer; length = w.length; next = Operation(w.next);
	}
	else { wbuf = nullptr; length = 0; next = Operation::Stop; }
	return true;
}

} // namespace modm::platform
