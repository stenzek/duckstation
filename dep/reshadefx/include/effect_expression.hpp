/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "effect_token.hpp"
#include <cstdint>

namespace reshadefx
{
	/// <summary>
	/// Structure which encapsulates a parsed value type
	/// </summary>
	struct type
	{
		enum datatype : uint32_t
		{
			t_void,
			t_bool,
			t_min16int,
			t_int,
			t_min16uint,
			t_uint,
			t_min16float,
			t_float,
			t_string,
			t_struct,
			t_texture1d,
			t_texture2d,
			t_texture3d,
			t_sampler1d_int,
			t_sampler2d_int,
			t_sampler3d_int,
			t_sampler1d_uint,
			t_sampler2d_uint,
			t_sampler3d_uint,
			t_sampler1d_float,
			t_sampler2d_float,
			t_sampler3d_float,
			t_storage1d_int,
			t_storage2d_int,
			t_storage3d_int,
			t_storage1d_uint,
			t_storage2d_uint,
			t_storage3d_uint,
			t_storage1d_float,
			t_storage2d_float,
			t_storage3d_float,
			t_function,
		};
		enum qualifier : uint32_t
		{
			q_extern = 1 << 0,
			q_static = 1 << 1,
			q_uniform = 1 << 2,
			q_volatile = 1 << 3,
			q_precise = 1 << 4,
			q_groupshared = 1 << 14,
			q_in = 1 << 5,
			q_out = 1 << 6,
			q_inout = q_in | q_out,
			q_const = 1 << 8,
			q_linear = 1 << 10,
			q_noperspective = 1 << 11,
			q_centroid = 1 << 12,
			q_nointerpolation = 1 << 13,
		};

		/// <summary>
		/// Gets the result type of an operation involving the two input types.
		/// </summary>
		static type merge(const type &lhs, const type &rhs);

		/// <summary>
		/// Calculates the ranking between two types which can be used to select the best matching function overload. The higher the rank, the better the match. A value of zero indicates that the types are not compatible.
		/// </summary>
		static unsigned int rank(const type &src, const type &dst);

		/// <summary>
		/// Returns a human-readable description of this type definition.
		/// </summary>
		std::string description() const;

		bool has(qualifier x) const { return (qualifiers & x) == x; }

		bool is_void() const { return base == t_void; }
		bool is_boolean() const { return base == t_bool; }
		bool is_numeric() const { return base >= t_bool && base <= t_float; }
		bool is_integral() const { return (base >= t_bool && base <= t_uint) || (base >= t_sampler1d_int && base <= t_sampler3d_uint) || (base >= t_storage1d_int && base <= t_storage3d_uint); }
		bool is_floating_point() const { return base == t_min16float || base == t_float || (base >= t_sampler1d_float && base <= t_sampler3d_float) || (base >= t_storage1d_float && base <= t_storage3d_float); }
		bool is_signed() const { return base == t_min16int || base == t_int || (base >= t_sampler1d_int && base <= t_sampler3d_int) || (base >= t_storage1d_int && base <= t_storage3d_int) || is_floating_point(); }
		bool is_unsigned() const { return base == t_min16uint || base == t_uint || (base >= t_sampler1d_uint && base <= t_sampler3d_uint) || (base >= t_storage1d_uint && base <= t_storage3d_uint); }

		bool is_struct() const { return base == t_struct; }
		bool is_object() const { return is_texture() || is_sampler() || is_storage(); }
		bool is_texture() const { return base >= t_texture1d && base <= t_texture3d; }
		bool is_sampler() const { return base >= t_sampler1d_int && base <= t_sampler3d_float; }
		bool is_storage() const { return base >= t_storage1d_int && base <= t_storage3d_float; }
		bool is_function() const { return base == t_function; }

		bool is_array() const { return array_length != 0; }
		bool is_bounded_array() const { return is_array() && array_length != 0xFFFFFFFF; }
		bool is_unbounded_array() const { return array_length == 0xFFFFFFFF; }
		bool is_scalar() const { return is_numeric() && !is_matrix() && !is_vector() && !is_array(); }
		bool is_vector() const { return is_numeric() && rows > 1 && cols == 1; }
		bool is_matrix() const { return is_numeric() && rows >= 1 && cols > 1; }

		unsigned int precision() const { return base == t_min16int || base == t_min16uint || base == t_min16float ? 16 : 32; }
		unsigned int components() const { return rows * cols; }
		unsigned int texture_dimension() const { return base >= t_texture1d && base <= t_storage3d_float ? ((base - t_texture1d) % 3) + 1 : 0; }

		friend bool operator==(const type &lhs, const type &rhs)
		{
			return lhs.base == rhs.base && lhs.rows == rhs.rows && lhs.cols == rhs.cols && lhs.array_length == rhs.array_length && lhs.struct_definition == rhs.struct_definition;
		}
		friend bool operator!=(const type &lhs, const type &rhs)
		{
			return !operator==(lhs, rhs);
		}

		// Underlying base type ('int', 'float', ...)
		datatype base : 8;
		// Number of rows if this is a vector type
		uint32_t rows : 4;
		// Number of columns if this is a matrix type
		uint32_t cols : 4;
		// Bit mask of all the qualifiers decorating the type
		uint32_t qualifiers : 16;
		// Number of elements if this is an array type, 0xFFFFFFFF if it is an unsized array
		uint32_t array_length;
		// ID of the matching struct if this is a struct type
		uint32_t struct_definition;
	};

	/// <summary>
	/// Structure which encapsulates a parsed constant value
	/// </summary>
	struct constant
	{
		union
		{
			float as_float[16];
			int32_t as_int[16];
			uint32_t as_uint[16];
		};

		// Optional string associated with this constant
		std::string string_data;
		// Optional additional elements if this is an array constant
		std::vector<constant> array_data;
	};

	/// <summary>
	/// Structures which keeps track of the access chain of an expression
	/// </summary>
	struct expression
	{
		struct operation
		{
			enum op_type
			{
				op_cast,
				op_member,
				op_dynamic_index,
				op_constant_index,
				op_swizzle,
			};

			op_type op;
			reshadefx::type from, to;
			uint32_t index;
			signed char swizzle[4];
		};

		uint32_t base = 0;
		reshadefx::type type = {};
		reshadefx::constant constant = {};
		bool is_lvalue = false;
		bool is_constant = false;
		reshadefx::location location;
		std::vector<operation> chain;

		/// <summary>
		/// Initializes the expression to a l-value.
		/// </summary>
		/// <param name="loc">Code location of the expression.</param>
		/// <param name="base">SSA ID of the l-value.</param>
		/// <param name="type">Value type of the expression result.</param>
		void reset_to_lvalue(const reshadefx::location &loc, uint32_t base, const reshadefx::type &type);
		/// <summary>
		/// Initializes the expression to a r-value.
		/// </summary>
		/// <param name="loc">Code location of the expression.</param>
		/// <param name="base">SSA ID of the r-value.</param>
		/// <param name="type">Value type of the expression result.</param>
		void reset_to_rvalue(const reshadefx::location &loc, uint32_t base, const reshadefx::type &type);

		/// <summary>
		/// Initializes the expression to a constant value.
		/// </summary>
		/// <param name="loc">Code location of the constant expression.</param>
		/// <param name="data">Constant value to initialize to.</param>
		void reset_to_rvalue_constant(const reshadefx::location &loc, bool data);
		void reset_to_rvalue_constant(const reshadefx::location &loc, float data);
		void reset_to_rvalue_constant(const reshadefx::location &loc, int32_t data);
		void reset_to_rvalue_constant(const reshadefx::location &loc, uint32_t data);
		void reset_to_rvalue_constant(const reshadefx::location &loc, std::string data);
		void reset_to_rvalue_constant(const reshadefx::location &loc, reshadefx::constant data, const reshadefx::type &type);

		/// <summary>
		/// Adds a cast operation to the current access chain.
		/// </summary>
		/// <param name="type">Type to cast the expression to.</param>
		void add_cast_operation(const reshadefx::type &type);
		/// <summary>
		/// Adds a structure member lookup to the current access chain.
		/// </summary>
		/// <param name="index">Index of the member to dereference.</param>
		/// <param name="type">Value type of the member.</param>
		void add_member_access(unsigned int index, const reshadefx::type &type);
		/// <summary>
		/// Adds an index operation to the current access chain.
		/// </summary>
		/// <param name="index_expression">SSA ID of the indexing value.</param>
		void add_dynamic_index_access(uint32_t index_expression);
		/// <summary>
		/// Adds an constant index operation to the current access chain.
		/// </summary>
		/// <param name="index">Constant indexing value.</param>
		void add_constant_index_access(unsigned int index);
		/// <summary>
		/// Adds a swizzle operation to the current access chain.
		/// </summary>
		/// <param name="swizzle">Swizzle for each component. -1 = unused, 0 = x, 1 = y, 2 = z, 3 = w.</param>
		/// <param name="length">Number of components in the swizzle. The maximum is 4.</param>
		void add_swizzle_access(const signed char swizzle[4], unsigned int length);

		/// <summary>
		/// Applies an unary operation to this constant expression.
		/// </summary>
		/// <param name="op">Unary operator to apply.</param>
		bool evaluate_constant_expression(reshadefx::tokenid op);
		/// <summary>
		/// Applies a binary operation to this constant expression.
		/// </summary>
		/// <param name="op">Binary operator to apply.</param>
		/// <param name="rhs">Constant value to use as right-hand side of the binary operation.</param>
		bool evaluate_constant_expression(reshadefx::tokenid op, const reshadefx::constant &rhs);
	};
}
