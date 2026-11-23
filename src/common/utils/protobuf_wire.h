#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// Bounded protobuf wire-format reader for native provider records. It only splits a message into
// fields; callers keep their own field-lookup rules (first match or unique match) and error types.
namespace uam::protobuf_wire
{
	/// <summary>One decoded field; fixed-width and length-delimited payloads are views into the input.</summary>
	struct Field
	{
		std::uint32_t number = 0;
		unsigned wire = 0;
		std::uint64_t integer = 0;
		std::string_view bytes;
	};

	enum class Error
	{
		TruncatedVarint,
		OverflowingVarint,
		NoncanonicalVarint,
		InvalidTag,
		TooManyFields,
		UnsupportedWireType,
		TruncatedField,
	};

	struct Policy
	{
		/// <summary>More fields than this refuse the whole message.</summary>
		std::size_t max_fields = 0;
		/// <summary>Refuses a multi-byte varint whose last byte is zero, so each value has one encoding.</summary>
		bool reject_noncanonical_varints = false;
	};

	/// <summary>Reads one varint at position; make_error(Error) returns the exception object to throw.</summary>
	template <typename MakeError>
	std::uint64_t ReadVarint(std::string_view bytes, std::size_t& position, const Policy& policy, const MakeError& make_error)
	{
		std::uint64_t value = 0;
		for (unsigned shift = 0; shift < 70; shift += 7)
		{
			if (position >= bytes.size()) throw make_error(Error::TruncatedVarint);
			const unsigned char part = static_cast<unsigned char>(bytes[position++]);
			if (shift == 63 && part > 1) throw make_error(Error::OverflowingVarint);
			value |= static_cast<std::uint64_t>(part & 127) << shift;
			if (part < 128)
			{
				if (policy.reject_noncanonical_varints && shift != 0 && part == 0) throw make_error(Error::NoncanonicalVarint);
				return value;
			}
		}
		throw make_error(Error::OverflowingVarint);
	}

	/// <summary>Splits bytes into fields in wire order. Accepts varint, 64-bit, length-delimited and 32-bit fields; groups are refused.</summary>
	template <typename MakeError>
	std::vector<Field> ReadFields(std::string_view bytes, const Policy& policy, const MakeError& make_error)
	{
		std::vector<Field> fields;
		for (std::size_t position = 0; position < bytes.size();)
		{
			if (fields.size() >= policy.max_fields) throw make_error(Error::TooManyFields);
			const std::uint64_t tag = ReadVarint(bytes, position, policy, make_error);
			if ((tag >> 3) == 0 || (tag >> 3) > 536870911) throw make_error(Error::InvalidTag);
			Field field{static_cast<std::uint32_t>(tag >> 3), static_cast<unsigned>(tag & 7)};
			if (field.wire == 0)
			{
				field.integer = ReadVarint(bytes, position, policy, make_error);
			}
			else
			{
				if (field.wire != 1 && field.wire != 2 && field.wire != 5) throw make_error(Error::UnsupportedWireType);
				const std::uint64_t size = field.wire == 2 ? ReadVarint(bytes, position, policy, make_error) : field.wire == 1 ? 8U : 4U;
				if (size > bytes.size() - position) throw make_error(Error::TruncatedField);
				field.bytes = bytes.substr(position, static_cast<std::size_t>(size));
				position += static_cast<std::size_t>(size);
			}
			fields.push_back(field);
		}
		return fields;
	}
}
