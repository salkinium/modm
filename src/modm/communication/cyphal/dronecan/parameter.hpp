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

#include <uavcan.protocol.param.GetSet.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace modm::dronecan
{

/**
 * A configuration parameter of a DroneCAN node, which the
 * uavcan.protocol.param.GetSet service reads and writes by name or index.
 *
 * Integer and real parameters have a range, sets outside of it are refused.
 * The application reads the value with integer(), real() or boolean().
 */
class Parameter
{
public:
	enum class
	Type : uint8_t
	{
		Integer,
		Real,
		Boolean,
	};

	/// @param name at most 92 characters, by convention lowercase with dots
	template< std::integral T > requires (not std::same_as<T, bool>)
	constexpr Parameter(const char* name, T value, int64_t min, int64_t max)
	:	name_{name}, type_{Type::Integer}, value_{.integer = value}, default_{.integer = value},
		min_{.integer = min}, max_{.integer = max} {}

	constexpr Parameter(const char* name, float value, float min, float max)
	:	name_{name}, type_{Type::Real}, value_{.real = value}, default_{.real = value},
		min_{.real = min}, max_{.real = max} {}

	constexpr Parameter(const char* name, bool value)
	:	name_{name}, type_{Type::Boolean}, value_{.boolean = value}, default_{.boolean = value} {}

	Parameter(const Parameter&) = delete;
	Parameter& operator=(const Parameter&) = delete;

	std::string_view name() const { return name_; }
	Type type() const { return type_; }

	int64_t integer() const { return value_.integer; }
	float real() const { return value_.real; }
	bool boolean() const { return value_.boolean; }

	/// Sets the value, an integer also of a real parameter.
	/// @return false if the type does not match or the value is out of range
	template< class T >
	bool
	set(T value)
	{
		if constexpr (std::same_as<T, bool>)
		{
			if (type_ != Type::Boolean) return false;
			value_.boolean = value;
			return true;
		}
		else if constexpr (std::integral<T>)
		{
			if (type_ == Type::Real) return set(float(value));
			if constexpr (std::unsigned_integral<T>) if (value > uint64_t(INT64_MAX)) return false;
			if (type_ != Type::Integer or int64_t(value) < min_.integer or int64_t(value) > max_.integer)
				return false;
			value_.integer = value;
			return true;
		}
		else
		{
			if (type_ != Type::Real or std::isnan(value) or
				value < min_.real or value > max_.real) return false;
			value_.real = value;
			return true;
		}
	}

	/// Sets the default value.
	void
	reset()
	{
		value_ = default_;
	}

	bool
	isDefault() const
	{
		return raw(value_) == raw(default_);
	}

	/// Sets the value of a GetSet request: an integer, real or boolean value
	/// as the type of the parameter, or an integer also for real and boolean.
	/// @return false if the type does not match or the value is out of range
	bool
	set(const uavcan_protocol_param_Value& value)
	{
		switch (value.union_tag)
		{
			case UAVCAN_PROTOCOL_PARAM_VALUE_INTEGER_VALUE:
				if (type_ == Type::Boolean)
				{
					if (value.integer_value != 0 and value.integer_value != 1) return false;
					return set(value.integer_value == 1);
				}
				return set(value.integer_value);
			case UAVCAN_PROTOCOL_PARAM_VALUE_REAL_VALUE:
				return set(value.real_value);
			case UAVCAN_PROTOCOL_PARAM_VALUE_BOOLEAN_VALUE:
				return set(value.boolean_value != 0);
			default:
				return false;
		}
	}

	/// Fills the value, default, range and name of a GetSet response.
	void
	get(uavcan_protocol_param_GetSetResponse& response) const
	{
		response = {};
		response.value = value(value_);
		response.default_value = value(default_);
		if (type_ == Type::Integer)
		{
			response.min_value.union_tag = UAVCAN_PROTOCOL_PARAM_NUMERICVALUE_INTEGER_VALUE;
			response.min_value.integer_value = min_.integer;
			response.max_value.union_tag = UAVCAN_PROTOCOL_PARAM_NUMERICVALUE_INTEGER_VALUE;
			response.max_value.integer_value = max_.integer;
		}
		else if (type_ == Type::Real)
		{
			response.min_value.union_tag = UAVCAN_PROTOCOL_PARAM_NUMERICVALUE_REAL_VALUE;
			response.min_value.real_value = min_.real;
			response.max_value.union_tag = UAVCAN_PROTOCOL_PARAM_NUMERICVALUE_REAL_VALUE;
			response.max_value.real_value = max_.real;
		}
		const size_t length = std::min(name().size(), sizeof(response.name.data));
		std::memcpy(response.name.data, name_, length);
		response.name.len = length;
	}

	/// Key of the parameter in a storage: a hash of the name and the type, so
	/// that changing the order of the parameters keeps the stored values.
	uint32_t
	key() const
	{
		// FNV-1a
		uint32_t hash = 2166136261u;
		for (const char c : name()) hash = (hash ^ uint8_t(c)) * 16777619u;
		hash = (hash ^ uint8_t(type_)) * 16777619u;
		// ZMS reserves the ID 0xffff'ffff
		return hash == 0xffff'ffff ? 0 : hash;
	}

	/// The value as 8 bytes for a storage
	uint64_t
	stored() const
	{
		return raw(value_);
	}

	/// Sets a value read from a storage
	/// @return false if it is out of range
	bool
	load(uint64_t data)
	{
		switch (type_)
		{
			case Type::Integer: return set(int64_t(data));
			case Type::Real: return data <= 0xffff'ffff and set(std::bit_cast<float>(uint32_t(data)));
			case Type::Boolean: return data <= 1 and set(data == 1);
		}
		return false;
	}

private:
	union Data
	{
		int64_t integer;
		float real;
		bool boolean;
	};

	uint64_t
	raw(Data data) const
	{
		switch (type_)
		{
			case Type::Integer: return uint64_t(data.integer);
			case Type::Real: return std::bit_cast<uint32_t>(data.real);
			case Type::Boolean: return data.boolean;
		}
		return 0;
	}

	uavcan_protocol_param_Value
	value(Data data) const
	{
		uavcan_protocol_param_Value value{};
		switch (type_)
		{
			case Type::Integer:
				value.union_tag = UAVCAN_PROTOCOL_PARAM_VALUE_INTEGER_VALUE;
				value.integer_value = data.integer;
				break;
			case Type::Real:
				value.union_tag = UAVCAN_PROTOCOL_PARAM_VALUE_REAL_VALUE;
				value.real_value = data.real;
				break;
			case Type::Boolean:
				value.union_tag = UAVCAN_PROTOCOL_PARAM_VALUE_BOOLEAN_VALUE;
				value.boolean_value = data.boolean;
				break;
		}
		return value;
	}

	const char* const name_;
	const Type type_;
	Data value_;
	const Data default_;
	const Data min_{};
	const Data max_{};
};

/// Persistent storage of parameters, e.g. modm::dronecan::ZmsStorage
class ParameterStorage
{
public:
	/// @return false if there is no stored value
	virtual bool read(uint32_t key, uint64_t& data) = 0;
	virtual bool write(uint32_t key, uint64_t data) = 0;
	/// Removes a stored value, also succeeds if there is none.
	virtual bool remove(uint32_t key) = 0;

protected:
	~ParameterStorage() = default;
};

}	// namespace modm::dronecan
