/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "effect_lexer.hpp"
#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include <cassert>

#define RESHADEFX_SHORT_CIRCUIT 0

reshadefx::parser::parser()
{
}
reshadefx::parser::~parser()
{
}

void reshadefx::parser::error(const location &location, unsigned int code, const std::string &message)
{
	_errors += location.source;
	_errors += '(' + std::to_string(location.line) + ", " + std::to_string(location.column) + ')' + ": error";
	_errors += (code == 0) ? ": " : " X" + std::to_string(code) + ": ";
	_errors += message;
	_errors += '\n';
}
void reshadefx::parser::warning(const location &location, unsigned int code, const std::string &message)
{
	_errors += location.source;
	_errors += '(' + std::to_string(location.line) + ", " + std::to_string(location.column) + ')' + ": warning";
	_errors += (code == 0) ? ": " : " X" + std::to_string(code) + ": ";
	_errors += message;
	_errors += '\n';
}

void reshadefx::parser::backup()
{
	_token_backup = _token_next;
	_lexer_backup_offset = _lexer->input_offset();
}
void reshadefx::parser::restore()
{
	_lexer->reset_to_offset(_lexer_backup_offset);
	_token_next = _token_backup; // Copy instead of move here, since restore may be called twice (from 'accept_type_class' and then again from 'parse_expression_unary')
}

void reshadefx::parser::consume()
{
	_token = std::move(_token_next);
	_token_next = _lexer->lex();
}
void reshadefx::parser::consume_until(tokenid tokid)
{
	while (!accept(tokid) && !peek(tokenid::end_of_file))
	{
		consume();
	}
}

bool reshadefx::parser::accept(tokenid tokid)
{
	if (peek(tokid))
	{
		consume();
		return true;
	}

	return false;
}
bool reshadefx::parser::expect(tokenid tokid)
{
	if (!accept(tokid))
	{
		error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + "', expected '" + token::id_to_name(tokid) + '\'');
		return false;
	}

	return true;
}

bool reshadefx::parser::accept_symbol(std::string &identifier, scoped_symbol &symbol)
{
	// Starting an identifier with '::' restricts the symbol search to the global namespace level
	const bool exclusive = accept(tokenid::colon_colon);

	if (exclusive ? !expect(tokenid::identifier) : !accept(tokenid::identifier))
	{
		// No token should come through here, since all possible prefix expressions should have been handled above, so this is an error in the syntax
		if (!exclusive)
			error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + '\'');
		return false;
	}

	identifier = std::move(_token.literal_as_string);

	// Can concatenate multiple '::' to force symbol search for a specific namespace level
	while (accept(tokenid::colon_colon))
	{
		if (!expect(tokenid::identifier))
			return false;
		identifier += "::" + std::move(_token.literal_as_string);
	}

	// Figure out which scope to start searching in
	struct scope scope = { "::", 0, 0 };
	if (!exclusive) scope = current_scope();

	// Lookup name in the symbol table
	symbol = find_symbol(identifier, scope, exclusive);

	return true;
}
bool reshadefx::parser::accept_type_class(type &type)
{
	type.rows = type.cols = 0;

	if (peek(tokenid::identifier) || peek(tokenid::colon_colon))
	{
		type.base = type::t_struct;

		backup(); // Need to restore if this identifier does not turn out to be a structure

		std::string identifier;
		scoped_symbol symbol;
		if (accept_symbol(identifier, symbol))
		{
			if (symbol.id && symbol.op == symbol_type::structure)
			{
				type.definition = symbol.id;
				return true;
			}
		}

		restore();

		return false;
	}

	if (accept(tokenid::vector))
	{
		type.base = type::t_float; // Default to float4 unless a type is specified (see below)
		type.rows = 4, type.cols = 1;

		if (accept('<'))
		{
			if (!accept_type_class(type)) // This overwrites the base type again
				return error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + "', expected vector element type"), false;
			else if (!type.is_scalar())
				return error(_token.location, 3122, "vector element type must be a scalar type"), false;

			if (!expect(',') || !expect(tokenid::int_literal))
				return false;
			else if (_token.literal_as_int < 1 || _token.literal_as_int > 4)
				return error(_token.location, 3052, "vector dimension must be between 1 and 4"), false;

			type.rows = static_cast<unsigned int>(_token.literal_as_int);

			if (!expect('>'))
				return false;
		}

		return true;
	}
	if (accept(tokenid::matrix))
	{
		type.base = type::t_float; // Default to float4x4 unless a type is specified (see below)
		type.rows = 4, type.cols = 4;

		if (accept('<'))
		{
			if (!accept_type_class(type)) // This overwrites the base type again
				return error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + "', expected matrix element type"), false;
			else if (!type.is_scalar())
				return error(_token.location, 3123, "matrix element type must be a scalar type"), false;

			if (!expect(',') || !expect(tokenid::int_literal))
				return false;
			else if (_token.literal_as_int < 1 || _token.literal_as_int > 4)
				return error(_token.location, 3053, "matrix dimensions must be between 1 and 4"), false;

			type.rows = static_cast<unsigned int>(_token.literal_as_int);

			if (!expect(',') || !expect(tokenid::int_literal))
				return false;
			else if (_token.literal_as_int < 1 || _token.literal_as_int > 4)
				return error(_token.location, 3053, "matrix dimensions must be between 1 and 4"), false;

			type.cols = static_cast<unsigned int>(_token.literal_as_int);

			if (!expect('>'))
				return false;
		}

		return true;
	}

	if (accept(tokenid::sampler1d) || accept(tokenid::sampler2d) || accept(tokenid::sampler3d))
	{
		const unsigned int texture_dimension = static_cast<unsigned int>(_token.id) - static_cast<unsigned int>(tokenid::sampler1d);

		if (accept('<'))
		{
			if (!accept_type_class(type))
				return error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + "', expected sampler element type"), false;
			if (type.is_object())
				return error(_token.location, 3124, "object element type cannot be an object type"), false;
			if (!type.is_numeric() || type.is_matrix())
				return error(_token.location, 3521, "sampler element type must fit in four 32-bit quantities"), false;

			if (type.is_integral() && type.is_signed())
				type.base = static_cast<type::datatype>(type::t_sampler1d_int + texture_dimension);
			else if (type.is_integral() && type.is_unsigned())
				type.base = static_cast<type::datatype>(type::t_sampler1d_uint + texture_dimension);
			else
				type.base = static_cast<type::datatype>(type::t_sampler1d_float + texture_dimension);

			if (!expect('>'))
				return false;
		}
		else
		{
			type.base = static_cast<type::datatype>(type::t_sampler1d_float + texture_dimension);
			type.rows = 4;
			type.cols = 1;
		}

		return true;
	}
	if (accept(tokenid::storage1d) || accept(tokenid::storage2d) || accept(tokenid::storage3d))
	{
		const unsigned int texture_dimension = static_cast<unsigned int>(_token.id) - static_cast<unsigned int>(tokenid::storage1d);

		if (accept('<'))
		{
			if (!accept_type_class(type))
				return error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + "', expected storage element type"), false;
			if (type.is_object())
				return error(_token.location, 3124, "object element type cannot be an object type"), false;
			if (!type.is_numeric() || type.is_matrix())
				return error(_token.location, 3521, "storage element type must fit in four 32-bit quantities"), false;

			if (type.is_integral() && type.is_signed())
				type.base = static_cast<type::datatype>(type::t_storage1d_int + texture_dimension);
			else if (type.is_integral() && type.is_unsigned())
				type.base = static_cast<type::datatype>(type::t_storage1d_uint + texture_dimension);
			else
				type.base = static_cast<type::datatype>(type::t_storage1d_float + texture_dimension);

			if (!expect('>'))
				return false;
		}
		else
		{
			type.base = static_cast<type::datatype>(type::t_storage1d_float + texture_dimension);
			type.rows = 4;
			type.cols = 1;
		}

		return true;
	}

	switch (_token_next.id)
	{
	case tokenid::void_:
		type.base = type::t_void;
		break;
	case tokenid::bool_:
	case tokenid::bool2:
	case tokenid::bool3:
	case tokenid::bool4:
		type.base = type::t_bool;
		type.rows = 1 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::bool_));
		type.cols = 1;
		break;
	case tokenid::bool2x2:
	case tokenid::bool2x3:
	case tokenid::bool2x4:
	case tokenid::bool3x2:
	case tokenid::bool3x3:
	case tokenid::bool3x4:
	case tokenid::bool4x2:
	case tokenid::bool4x3:
	case tokenid::bool4x4:
		type.base = type::t_bool;
		type.rows = 2 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::bool2x2)) / 3;
		type.cols = 2 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::bool2x2)) % 3;
		break;
	case tokenid::int_:
	case tokenid::int2:
	case tokenid::int3:
	case tokenid::int4:
		type.base = type::t_int;
		type.rows = 1 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::int_));
		type.cols = 1;
		break;
	case tokenid::int2x2:
	case tokenid::int2x3:
	case tokenid::int2x4:
	case tokenid::int3x2:
	case tokenid::int3x3:
	case tokenid::int3x4:
	case tokenid::int4x2:
	case tokenid::int4x3:
	case tokenid::int4x4:
		type.base = type::t_int;
		type.rows = 2 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::int2x2)) / 3;
		type.cols = 2 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::int2x2)) % 3;
		break;
	case tokenid::min16int:
	case tokenid::min16int2:
	case tokenid::min16int3:
	case tokenid::min16int4:
		type.base = type::t_min16int;
		type.rows = 1 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::min16int));
		type.cols = 1;
		break;
	case tokenid::uint_:
	case tokenid::uint2:
	case tokenid::uint3:
	case tokenid::uint4:
		type.base = type::t_uint;
		type.rows = 1 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::uint_));
		type.cols = 1;
		break;
	case tokenid::uint2x2:
	case tokenid::uint2x3:
	case tokenid::uint2x4:
	case tokenid::uint3x2:
	case tokenid::uint3x3:
	case tokenid::uint3x4:
	case tokenid::uint4x2:
	case tokenid::uint4x3:
	case tokenid::uint4x4:
		type.base = type::t_uint;
		type.rows = 2 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::uint2x2)) / 3;
		type.cols = 2 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::uint2x2)) % 3;
		break;
	case tokenid::min16uint:
	case tokenid::min16uint2:
	case tokenid::min16uint3:
	case tokenid::min16uint4:
		type.base = type::t_min16uint;
		type.rows = 1 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::min16uint));
		type.cols = 1;
		break;
	case tokenid::float_:
	case tokenid::float2:
	case tokenid::float3:
	case tokenid::float4:
		type.base = type::t_float;
		type.rows = 1 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::float_));
		type.cols = 1;
		break;
	case tokenid::float2x2:
	case tokenid::float2x3:
	case tokenid::float2x4:
	case tokenid::float3x2:
	case tokenid::float3x3:
	case tokenid::float3x4:
	case tokenid::float4x2:
	case tokenid::float4x3:
	case tokenid::float4x4:
		type.base = type::t_float;
		type.rows = 2 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::float2x2)) / 3;
		type.cols = 2 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::float2x2)) % 3;
		break;
	case tokenid::min16float:
	case tokenid::min16float2:
	case tokenid::min16float3:
	case tokenid::min16float4:
		type.base = type::t_min16float;
		type.rows = 1 + (static_cast<unsigned int>(_token_next.id) - static_cast<unsigned int>(tokenid::min16float));
		type.cols = 1;
		break;
	case tokenid::string_:
		type.base = type::t_string;
		break;
	case tokenid::texture1d:
		type.base = type::t_texture1d;
		break;
	case tokenid::texture2d:
		type.base = type::t_texture2d;
		break;
	case tokenid::texture3d:
		type.base = type::t_texture3d;
		break;
	default:
		return false;
	}

	consume();

	return true;
}
bool reshadefx::parser::accept_type_qualifiers(type &type)
{
	unsigned int qualifiers = 0;

	// Storage
	if (accept(tokenid::extern_))
		qualifiers |= type::q_extern;
	if (accept(tokenid::static_))
		qualifiers |= type::q_static;
	if (accept(tokenid::uniform_))
		qualifiers |= type::q_uniform;
	if (accept(tokenid::volatile_))
		qualifiers |= type::q_volatile;
	if (accept(tokenid::precise))
		qualifiers |= type::q_precise;
	if (accept(tokenid::groupshared))
		qualifiers |= type::q_groupshared;

	if (accept(tokenid::in))
		qualifiers |= type::q_in;
	if (accept(tokenid::out))
		qualifiers |= type::q_out;
	if (accept(tokenid::inout))
		qualifiers |= type::q_inout;

	// Modifiers
	if (accept(tokenid::const_))
		qualifiers |= type::q_const;

	// Interpolation
	if (accept(tokenid::linear))
		qualifiers |= type::q_linear;
	if (accept(tokenid::noperspective))
		qualifiers |= type::q_noperspective;
	if (accept(tokenid::centroid))
		qualifiers |= type::q_centroid;
	if (accept(tokenid::nointerpolation))
		qualifiers |= type::q_nointerpolation;

	if (qualifiers == 0)
		return false;
	if ((type.qualifiers & qualifiers) == qualifiers)
		warning(_token.location, 3048, "duplicate usages specified");

	type.qualifiers |= qualifiers;

	// Continue parsing potential additional qualifiers until no more are found
	accept_type_qualifiers(type);

	return true;
}

bool reshadefx::parser::accept_unary_op()
{
	switch (_token_next.id)
	{
	case tokenid::exclaim: // !x (logical not)
	case tokenid::plus: // +x
	case tokenid::minus: // -x (negate)
	case tokenid::tilde: // ~x (bitwise not)
	case tokenid::plus_plus: // ++x
	case tokenid::minus_minus: // --x
		break;
	default:
		return false;
	}

	consume();

	return true;
}
bool reshadefx::parser::accept_postfix_op()
{
	switch (_token_next.id)
	{
	case tokenid::plus_plus: // ++x
	case tokenid::minus_minus: // --x
		break;
	default:
		return false;
	}

	consume();

	return true;
}
bool reshadefx::parser::peek_multary_op(unsigned int &precedence) const
{
	// Precedence values taken from https://cppreference.com/w/cpp/language/operator_precedence
	switch (_token_next.id)
	{
	case tokenid::question: precedence = 1; break; // x ? a : b
	case tokenid::pipe_pipe: precedence = 2; break; // a || b (logical or)
	case tokenid::ampersand_ampersand: precedence = 3; break; // a && b (logical and)
	case tokenid::pipe: precedence = 4; break; // a | b (bitwise or)
	case tokenid::caret: precedence = 5; break; // a ^ b (bitwise xor)
	case tokenid::ampersand: precedence = 6; break; // a & b (bitwise and)
	case tokenid::equal_equal: precedence = 7; break; // a == b (equal)
	case tokenid::exclaim_equal: precedence = 7; break; // a != b (not equal)
	case tokenid::less: precedence = 8; break; // a < b
	case tokenid::greater: precedence = 8; break; // a > b
	case tokenid::less_equal: precedence = 8; break; // a <= b
	case tokenid::greater_equal: precedence = 8; break; // a >= b
	case tokenid::less_less: precedence = 9; break; // a << b (left shift)
	case tokenid::greater_greater: precedence = 9; break; // a >> b (right shift)
	case tokenid::plus: precedence = 10; break; // a + b (add)
	case tokenid::minus: precedence = 10; break; // a - b (subtract)
	case tokenid::star: precedence = 11; break; // a * b (multiply)
	case tokenid::slash: precedence = 11; break; // a / b (divide)
	case tokenid::percent: precedence = 11; break; // a % b (modulo)
	default:
		return false;
	}

	// Do not consume token yet since the expression may be skipped due to precedence
	return true;
}
bool reshadefx::parser::accept_assignment_op()
{
	switch (_token_next.id)
	{
	case tokenid::equal: // a = b
	case tokenid::percent_equal: // a %= b
	case tokenid::ampersand_equal: // a &= b
	case tokenid::star_equal: // a *= b
	case tokenid::plus_equal: // a += b
	case tokenid::minus_equal: // a -= b
	case tokenid::slash_equal: // a /= b
	case tokenid::less_less_equal: // a <<= b
	case tokenid::greater_greater_equal: // a >>= b
	case tokenid::caret_equal: // a ^= b
	case tokenid::pipe_equal: // a |= b
		break;
	default:
		return false;
	}

	consume();

	return true;
}

bool reshadefx::parser::parse_expression(expression &exp)
{
	// Parse first expression
	if (!parse_expression_assignment(exp))
		return false;

	// Continue parsing if an expression sequence is next (in the form "a, b, c, ...")
	while (accept(','))
		// Overwrite 'exp' since conveniently the last expression in the sequence is the result
		if (!parse_expression_assignment(exp))
			return false;

	return true;
}

bool reshadefx::parser::parse_expression_unary(expression &exp)
{
	auto location = _token_next.location;

	// Check if a prefix operator exists
	if (accept_unary_op())
	{
		// Remember the operator token before parsing the expression that follows it
		const tokenid op = _token.id;

		// Parse the actual expression
		if (!parse_expression_unary(exp))
			return false;

		// Unary operators are only valid on basic types
		if (!exp.type.is_scalar() && !exp.type.is_vector() && !exp.type.is_matrix())
			return error(exp.location, 3022, "scalar, vector, or matrix expected"), false;

		// Special handling for the "++" and "--" operators
		if (op == tokenid::plus_plus || op == tokenid::minus_minus)
		{
			if (exp.type.has(type::q_const) || !exp.is_lvalue)
				return error(location, 3025, "l-value specifies const object"), false;

			// Create a constant one in the type of the expression
			constant one = {};
			for (unsigned int i = 0; i < exp.type.components(); ++i)
				if (exp.type.is_floating_point()) one.as_float[i] = 1.0f; else one.as_uint[i] = 1u;

			const auto value = _codegen->emit_load(exp);
			const auto result = _codegen->emit_binary_op(location, op, exp.type, value,
				_codegen->emit_constant(exp.type, one));

			// The "++" and "--" operands modify the source variable, so store result back into it
			_codegen->emit_store(exp, result);
		}
		else if (op != tokenid::plus) // Ignore "+" operator since it does not actually do anything
		{
			// The "~" bitwise operator is only valid on integral types
			if (op == tokenid::tilde && !exp.type.is_integral())
				return error(exp.location, 3082, "int or unsigned int type required"), false;
			// The logical not operator expects a boolean type as input, so perform cast if necessary
			if (op == tokenid::exclaim && !exp.type.is_boolean())
				exp.add_cast_operation({ type::t_bool, exp.type.rows, exp.type.cols }); // Note: The result will be boolean as well

			// Constant expressions can be evaluated at compile time
			if (!exp.evaluate_constant_expression(op))
			{
				const auto value = _codegen->emit_load(exp);
				const auto result = _codegen->emit_unary_op(location, op, exp.type, value);

				exp.reset_to_rvalue(location, result, exp.type);
			}
		}
	}
	else if (accept('('))
	{
		// Note: This backup may get overridden in 'accept_type_class', but should point to the same token still
		backup();

		// Check if this is a C-style cast expression
		if (type cast_type; accept_type_class(cast_type))
		{
			if (peek('('))
			{
				// This is not a C-style cast but a constructor call, so need to roll-back and parse that instead
				restore();
			}
			else if (expect(')'))
			{
				// Parse the expression behind cast operator
				if (!parse_expression_unary(exp))
					return false;

				// Check if the types already match, in which case there is nothing to do
				if (exp.type == cast_type)
					return true;

				// Check if a cast between these types is valid
				if (!type::rank(exp.type, cast_type))
					return error(location, 3017, "cannot convert these types (from " + exp.type.description() + " to " + cast_type.description() + ')'), false;

				exp.add_cast_operation(cast_type);
				return true;
			}
			else
			{
				// Type name was not followed by a closing parenthesis
				return false;
			}
		}

		// Parse expression between the parentheses
		if (!parse_expression(exp) || !expect(')'))
			return false;
	}
	else if (accept('{'))
	{
		bool is_constant = true;
		std::vector<expression> elements;
		type composite_type = { type::t_void, 1, 1 };

		while (!peek('}'))
		{
			// There should be a comma between arguments
			if (!elements.empty() && !expect(','))
				return consume_until('}'), false;

			// Initializer lists might contain a comma at the end, so break out of the loop if nothing follows afterwards
			if (peek('}'))
				break;

			expression &element = elements.emplace_back();

			// Parse the argument expression
			if (!parse_expression_assignment(element))
				return consume_until('}'), false;

			if (element.type.is_array())
				return error(element.location, 3119, "arrays cannot be multi-dimensional"), consume_until('}'), false;
			if (composite_type.base != type::t_void && element.type.definition != composite_type.definition)
				return error(element.location, 3017, "cannot convert these types (from " + element.type.description() + " to " + composite_type.description() + ')'), false;

			is_constant &= element.is_constant; // Result is only constant if all arguments are constant
			composite_type = type::merge(composite_type, element.type);
		}

		// Constant arrays can be constructed at compile time
		if (is_constant)
		{
			constant res = {};
			for (expression &element : elements)
			{
				element.add_cast_operation(composite_type);
				res.array_data.push_back(element.constant);
			}

			composite_type.array_length = static_cast<int>(elements.size());

			exp.reset_to_rvalue_constant(location, std::move(res), composite_type);
		}
		else
		{
			// Resolve all access chains
			for (expression &element : elements)
			{
				element.add_cast_operation(composite_type);
				element.reset_to_rvalue(element.location, _codegen->emit_load(element), composite_type);
			}

			composite_type.array_length = static_cast<int>(elements.size());

			const auto result = _codegen->emit_construct(location, composite_type, elements);

			exp.reset_to_rvalue(location, result, composite_type);
		}

		return expect('}');
	}
	else if (accept(tokenid::true_literal))
	{
		exp.reset_to_rvalue_constant(location, true);
	}
	else if (accept(tokenid::false_literal))
	{
		exp.reset_to_rvalue_constant(location, false);
	}
	else if (accept(tokenid::int_literal))
	{
		exp.reset_to_rvalue_constant(location, _token.literal_as_int);
	}
	else if (accept(tokenid::uint_literal))
	{
		exp.reset_to_rvalue_constant(location, _token.literal_as_uint);
	}
	else if (accept(tokenid::float_literal))
	{
		exp.reset_to_rvalue_constant(location, _token.literal_as_float);
	}
	else if (accept(tokenid::double_literal))
	{
		// Convert double literal to float literal for now
		warning(location, 5000, "double literal truncated to float literal");

		exp.reset_to_rvalue_constant(location, static_cast<float>(_token.literal_as_double));
	}
	else if (accept(tokenid::string_literal))
	{
		std::string value = std::move(_token.literal_as_string);

		// Multiple string literals in sequence are concatenated into a single string literal
		while (accept(tokenid::string_literal))
			value += _token.literal_as_string;

		exp.reset_to_rvalue_constant(location, std::move(value));
	}
	else if (type type; accept_type_class(type)) // Check if this is a constructor call expression
	{
		if (!expect('('))
			return false;
		if (!type.is_numeric())
			return error(location, 3037, "constructors only defined for numeric base types"), false;

		// Empty constructors do not exist
		if (accept(')'))
			return error(location, 3014, "incorrect number of arguments to numeric-type constructor"), false;

		// Parse entire argument expression list
		bool is_constant = true;
		unsigned int num_components = 0;
		std::vector<expression> arguments;

		while (!peek(')'))
		{
			// There should be a comma between arguments
			if (!arguments.empty() && !expect(','))
				return false;

			expression &argument = arguments.emplace_back();

			// Parse the argument expression
			if (!parse_expression_assignment(argument))
				return false;

			// Constructors are only defined for numeric base types
			if (!argument.type.is_numeric())
				return error(argument.location, 3017, "cannot convert non-numeric types"), false;

			is_constant &= argument.is_constant; // Result is only constant if all arguments are constant
			num_components += argument.type.components();
		}

		// The list should be terminated with a parenthesis
		if (!expect(')'))
			return false;

		// The total number of argument elements needs to match the number of elements in the result type
		if (num_components != type.components())
			return error(location, 3014, "incorrect number of arguments to numeric-type constructor"), false;

		assert(num_components > 0 && num_components <= 16 && !type.is_array());

		if (is_constant) // Constants can be converted at compile time
		{
			constant res = {};
			unsigned int i = 0;
			for (expression &argument : arguments)
			{
				argument.add_cast_operation({ type.base, argument.type.rows, argument.type.cols });
				for (unsigned int k = 0; k < argument.type.components(); ++k)
					res.as_uint[i++] = argument.constant.as_uint[k];
			}

			exp.reset_to_rvalue_constant(location, std::move(res), type);
		}
		else if (arguments.size() > 1)
		{
			// Flatten all arguments to a list of scalars
			for (auto it = arguments.begin(); it != arguments.end();)
			{
				// Argument is a scalar already, so only need to cast it
				if (it->type.is_scalar())
				{
					expression &argument = *it++;

					auto scalar_type = argument.type;
					scalar_type.base = type.base;
					argument.add_cast_operation(scalar_type);

					argument.reset_to_rvalue(argument.location, _codegen->emit_load(argument), scalar_type);
				}
				else
				{
					const expression argument = *it;
					it = arguments.erase(it);

					// Convert to a scalar value and re-enter the loop in the next iteration (in case a cast is necessary too)
					for (unsigned int i = argument.type.components(); i > 0; --i)
					{
						expression scalar = argument;
						scalar.add_constant_index_access(i - 1);

						it = arguments.insert(it, scalar);
					}
				}
			}

			const auto result = _codegen->emit_construct(location, type, arguments);

			exp.reset_to_rvalue(location, result, type);
		}
		else // A constructor call with a single argument is identical to a cast
		{
			assert(!arguments.empty());

			// Reset expression to only argument and add cast to expression access chain
			exp = std::move(arguments[0]); exp.add_cast_operation(type);
		}
	}
	// At this point only identifiers are left to check and resolve
	else
	{
		std::string identifier;
		scoped_symbol symbol;
		if (!accept_symbol(identifier, symbol))
			return false;

		// Check if this is a function call or variable reference
		if (accept('('))
		{
			// Can only call symbols that are functions, but do not abort yet if no symbol was found since the identifier may reference an intrinsic
			if (symbol.id && symbol.op != symbol_type::function)
				return error(location, 3005, "identifier '" + identifier + "' represents a variable, not a function"), false;

			// Parse entire argument expression list
			std::vector<expression> arguments;

			while (!peek(')'))
			{
				// There should be a comma between arguments
				if (!arguments.empty() && !expect(','))
					return false;

				expression &argument = arguments.emplace_back();

				// Parse the argument expression
				if (!parse_expression_assignment(argument))
					return false;
			}

			// The list should be terminated with a parenthesis
			if (!expect(')'))
				return false;

			// Function calls can only be made from within functions
			if (!_codegen->is_in_function())
				return error(location, 3005, "invalid function call outside of a function"), false;

			// Try to resolve the call by searching through both function symbols and intrinsics
			bool undeclared = !symbol.id, ambiguous = false;

			if (!resolve_function_call(identifier, arguments, symbol.scope, symbol, ambiguous))
			{
				if (undeclared)
					error(location, 3004, "undeclared identifier or no matching intrinsic overload for '" + identifier + '\'');
				else if (ambiguous)
					error(location, 3067, "ambiguous function call to '" + identifier + '\'');
				else
					error(location, 3013, "no matching function overload for '" + identifier + '\'');
				return false;
			}

			assert(symbol.function != nullptr);

			std::vector<expression> parameters(arguments.size());

			// We need to allocate some temporary variables to pass in and load results from pointer parameters
			for (size_t i = 0; i < arguments.size(); ++i)
			{
				const auto &param_type = symbol.function->parameter_list[i].type;

				if (param_type.has(type::q_out) && (!arguments[i].is_lvalue || (arguments[i].type.has(type::q_const) && !arguments[i].type.is_object())))
					return error(arguments[i].location, 3025, "l-value specifies const object for an 'out' parameter"), false;

				if (arguments[i].type.components() > param_type.components())
					warning(arguments[i].location, 3206, "implicit truncation of vector type");

				if (symbol.op == symbol_type::function || param_type.has(type::q_out))
				{
					if (param_type.is_object() || param_type.has(type::q_groupshared) /* Special case for atomic intrinsics */)
					{
						if (arguments[i].type != param_type)
							return error(location, 3004, "no matching intrinsic overload for '" + identifier + '\''), false;

						assert(arguments[i].is_lvalue);

						// Do not shadow object or pointer parameters to function calls
						size_t chain_index = 0;
						const auto access_chain = _codegen->emit_access_chain(arguments[i], chain_index);
						parameters[i].reset_to_lvalue(arguments[i].location, access_chain, param_type);
						assert(chain_index == arguments[i].chain.size());

						// This is referencing a l-value, but want to avoid copying below
						parameters[i].is_lvalue = false;
					}
					else
					{
						// All user-defined functions actually accept pointers as arguments, same applies to intrinsics with 'out' parameters
						const auto temp_variable = _codegen->define_variable(arguments[i].location, param_type);
						parameters[i].reset_to_lvalue(arguments[i].location, temp_variable, param_type);
					}
				}
				else
				{
					expression arg = arguments[i];
					arg.add_cast_operation(param_type);
					parameters[i].reset_to_rvalue(arg.location, _codegen->emit_load(arg), param_type);

					// Keep track of whether the parameter is a constant for code generation (this makes the expression invalid for all other uses)
					parameters[i].is_constant = arg.is_constant;
				}
			}

			// Copy in parameters from the argument access chains to parameter variables
			for (size_t i = 0; i < arguments.size(); ++i)
			{
				// Only do this for pointer parameters as discovered above
				if (parameters[i].is_lvalue && parameters[i].type.has(type::q_in) && !parameters[i].type.is_object())
				{
					expression arg = arguments[i];
					arg.add_cast_operation(parameters[i].type);
					_codegen->emit_store(parameters[i], _codegen->emit_load(arg));
				}
			}

			// Check if the call resolving found an intrinsic or function and invoke the corresponding code
			const auto result = symbol.op == symbol_type::function ?
				_codegen->emit_call(location, symbol.id, symbol.type, parameters) :
				_codegen->emit_call_intrinsic(location, symbol.id, symbol.type, parameters);

			exp.reset_to_rvalue(location, result, symbol.type);

			// Copy out parameters from parameter variables back to the argument access chains
			for (size_t i = 0; i < arguments.size(); ++i)
			{
				// Only do this for pointer parameters as discovered above
				if (parameters[i].is_lvalue && parameters[i].type.has(type::q_out) && !parameters[i].type.is_object())
				{
					expression arg = parameters[i];
					arg.add_cast_operation(arguments[i].type);
					_codegen->emit_store(arguments[i], _codegen->emit_load(arg));
				}
			}

			if (_current_function != nullptr)
			{
				// Calling a function makes the caller inherit all sampler and storage object references from the callee
				_current_function->referenced_samplers.insert(symbol.function->referenced_samplers.begin(), symbol.function->referenced_samplers.end());
				_current_function->referenced_storages.insert(symbol.function->referenced_storages.begin(), symbol.function->referenced_storages.end());
			}
		}
		else if (symbol.op == symbol_type::invalid)
		{
			// Show error if no symbol matching the identifier was found
			return error(location, 3004, "undeclared identifier '" + identifier + '\''), false;
		}
		else if (symbol.op == symbol_type::variable)
		{
			assert(symbol.id != 0);
			// Simply return the pointer to the variable, dereferencing is done on site where necessary
			exp.reset_to_lvalue(location, symbol.id, symbol.type);

			if (_current_function != nullptr &&
				symbol.scope.level == symbol.scope.namespace_level && symbol.id != 0xFFFFFFFF) // Ignore invalid symbols that were added during error recovery
			{
				// Keep track of any global sampler or storage objects referenced in the current function
				if (symbol.type.is_sampler())
					_current_function->referenced_samplers.insert(symbol.id);
				if (symbol.type.is_storage())
					_current_function->referenced_storages.insert(symbol.id);
			}
		}
		else if (symbol.op == symbol_type::constant)
		{
			// Constants are loaded into the access chain
			exp.reset_to_rvalue_constant(location, symbol.constant, symbol.type);
		}
		else
		{
			// Can only reference variables and constants by name, functions need to be called
			return error(location, 3005, "identifier '" + identifier + "' represents a function, not a variable"), false;
		}
	}

	while (!peek(tokenid::end_of_file))
	{
		location = _token_next.location;

		// Check if a postfix operator exists
		if (accept_postfix_op())
		{
			// Unary operators are only valid on basic types
			if (!exp.type.is_scalar() && !exp.type.is_vector() && !exp.type.is_matrix())
				return error(exp.location, 3022, "scalar, vector, or matrix expected"), false;
			if (exp.type.has(type::q_const) || !exp.is_lvalue)
				return error(exp.location, 3025, "l-value specifies const object"), false;

			// Create a constant one in the type of the expression
			constant one = {};
			for (unsigned int i = 0; i < exp.type.components(); ++i)
				if (exp.type.is_floating_point()) one.as_float[i] = 1.0f; else one.as_uint[i] = 1u;

			const auto value = _codegen->emit_load(exp, true);
			const auto result = _codegen->emit_binary_op(location, _token.id, exp.type, value, _codegen->emit_constant(exp.type, one));

			// The "++" and "--" operands modify the source variable, so store result back into it
			_codegen->emit_store(exp, result);

			// All postfix operators return a r-value rather than a l-value to the variable
			exp.reset_to_rvalue(location, value, exp.type);
		}
		else if (accept('.'))
		{
			if (!expect(tokenid::identifier))
				return false;

			location = std::move(_token.location);
			const auto subscript = std::move(_token.literal_as_string);

			if (accept('(')) // Methods (function calls on types) are not supported right now
			{
				if (!exp.type.is_struct() || exp.type.is_array())
					error(location, 3087, "object does not have methods");
				else
					error(location, 3088, "structures do not have methods");
				return false;
			}
			else if (exp.type.is_array()) // Arrays do not have subscripts
			{
				error(location, 3018, "invalid subscript on array");
				return false;
			}
			else if (exp.type.is_vector())
			{
				const size_t length = subscript.size();
				if (length > 4)
					return error(location, 3018, "invalid subscript '" + subscript + "', swizzle too long"), false;

				bool is_const = false;
				signed char offsets[4] = { -1, -1, -1, -1 };
				enum { xyzw, rgba, stpq } set[4];

				for (size_t i = 0; i < length; ++i)
				{
					switch (subscript[i])
					{
					case 'x': offsets[i] = 0, set[i] = xyzw; break;
					case 'y': offsets[i] = 1, set[i] = xyzw; break;
					case 'z': offsets[i] = 2, set[i] = xyzw; break;
					case 'w': offsets[i] = 3, set[i] = xyzw; break;
					case 'r': offsets[i] = 0, set[i] = rgba; break;
					case 'g': offsets[i] = 1, set[i] = rgba; break;
					case 'b': offsets[i] = 2, set[i] = rgba; break;
					case 'a': offsets[i] = 3, set[i] = rgba; break;
					case 's': offsets[i] = 0, set[i] = stpq; break;
					case 't': offsets[i] = 1, set[i] = stpq; break;
					case 'p': offsets[i] = 2, set[i] = stpq; break;
					case 'q': offsets[i] = 3, set[i] = stpq; break;
					default:
						return error(location, 3018, "invalid subscript '" + subscript + '\''), false;
					}

					if (i > 0 && (set[i] != set[i - 1]))
						return error(location, 3018, "invalid subscript '" + subscript + "', mixed swizzle sets"), false;
					if (static_cast<unsigned int>(offsets[i]) >= exp.type.rows)
						return error(location, 3018, "invalid subscript '" + subscript + "', swizzle out of range"), false;

					// The result is not modifiable if a swizzle appears multiple times
					for (size_t k = 0; k < i; ++k)
						if (offsets[k] == offsets[i]) {
							is_const = true;
							break;
						}
				}

				// Add swizzle to current access chain
				exp.add_swizzle_access(offsets, static_cast<unsigned int>(length));

				if (is_const)
					exp.type.qualifiers |= type::q_const;
			}
			else if (exp.type.is_matrix())
			{
				const size_t length = subscript.size();
				if (length < 3)
					return error(location, 3018, "invalid subscript '" + subscript + '\''), false;

				bool is_const = false;
				signed char offsets[4] = { -1, -1, -1, -1 };
				const unsigned int set = subscript[1] == 'm';
				const int coefficient = !set;

				for (size_t i = 0, j = 0; i < length; i += 3 + set, ++j)
				{
					if (subscript[i] != '_' ||
						subscript[i + set + 1] < '0' + coefficient ||
						subscript[i + set + 1] > '3' + coefficient ||
						subscript[i + set + 2] < '0' + coefficient ||
						subscript[i + set + 2] > '3' + coefficient)
						return error(location, 3018, "invalid subscript '" + subscript + '\''), false;
					if (set && subscript[i + 1] != 'm')
						return error(location, 3018, "invalid subscript '" + subscript + "', mixed swizzle sets"), false;

					const unsigned int row = static_cast<unsigned int>((subscript[i + set + 1] - '0') - coefficient);
					const unsigned int col = static_cast<unsigned int>((subscript[i + set + 2] - '0') - coefficient);

					if ((row >= exp.type.rows || col >= exp.type.cols) || j > 3)
						return error(location, 3018, "invalid subscript '" + subscript + "', swizzle out of range"), false;

					offsets[j] = static_cast<signed char>(row * 4 + col);

					// The result is not modifiable if a swizzle appears multiple times
					for (size_t k = 0; k < j; ++k)
						if (offsets[k] == offsets[j]) {
							is_const = true;
							break;
						}
				}

				// Add swizzle to current access chain
				exp.add_swizzle_access(offsets, static_cast<unsigned int>(length / (3 + set)));

				if (is_const)
					exp.type.qualifiers |= type::q_const;
			}
			else if (exp.type.is_struct())
			{
				const auto &member_list = _codegen->get_struct(exp.type.definition).member_list;

				// Find member with matching name is structure definition
				uint32_t member_index = 0;
				for (const struct_member_info &member : member_list) {
					if (member.name == subscript)
						break;
					++member_index;
				}

				if (member_index >= member_list.size())
					return error(location, 3018, "invalid subscript '" + subscript + '\''), false;

				// Add field index to current access chain
				exp.add_member_access(member_index, member_list[member_index].type);
			}
			else if (exp.type.is_scalar())
			{
				const size_t length = subscript.size();
				if (length > 4)
					return error(location, 3018, "invalid subscript '" + subscript + "', swizzle too long"), false;

				for (size_t i = 0; i < length; ++i)
					if ((subscript[i] != 'x' && subscript[i] != 'r' && subscript[i] != 's') || i > 3)
						return error(location, 3018, "invalid subscript '" + subscript + '\''), false;

				// Promote scalar to vector type using cast
				auto target_type = exp.type;
				target_type.rows = static_cast<unsigned int>(length);

				exp.add_cast_operation(target_type);

				if (length > 1)
					exp.type.qualifiers |= type::q_const;
			}
			else
			{
				error(location, 3018, "invalid subscript '" + subscript + '\'');
				return false;
			}
		}
		else if (accept('['))
		{
			if (!exp.type.is_array() && !exp.type.is_vector() && !exp.type.is_matrix())
				return error(_token.location, 3121, "array, matrix, vector, or indexable object type expected in index expression"), false;

			// Parse index expression
			expression index;
			if (!parse_expression(index) || !expect(']'))
				return false;
			else if (!index.type.is_scalar() || !index.type.is_integral())
				return error(index.location, 3120, "invalid type for index - index must be an integer scalar"), false;

			// Add index expression to current access chain
			if (index.is_constant)
			{
				// Check array bounds if known
				if (exp.type.array_length > 0 && index.constant.as_uint[0] >= static_cast<unsigned int>(exp.type.array_length))
					return error(index.location, 3504, "array index out of bounds"), false;

				exp.add_constant_index_access(index.constant.as_uint[0]);
			}
			else
			{
				if (exp.is_constant)
				{
					// To handle a dynamic index into a constant means we need to create a local variable first or else any of the indexing instructions do not work
					const auto temp_variable = _codegen->define_variable(location, exp.type, std::string(), false, _codegen->emit_constant(exp.type, exp.constant));
					exp.reset_to_lvalue(exp.location, temp_variable, exp.type);
				}

				exp.add_dynamic_index_access(_codegen->emit_load(index));
			}
		}
		else
		{
			break;
		}
	}

	return true;
}

bool reshadefx::parser::parse_expression_multary(expression &lhs, unsigned int left_precedence)
{
	// Parse left hand side of the expression
	if (!parse_expression_unary(lhs))
		return false;

	// Check if an operator exists so that this is a binary or ternary expression
	unsigned int right_precedence;

	while (peek_multary_op(right_precedence))
	{
		// Only process this operator if it has a lower precedence than the current operation, otherwise leave it for later and abort
		if (right_precedence <= left_precedence)
			break;

		// Finally consume the operator token
		consume();

		const tokenid op = _token.id;

		// Check if this is a binary or ternary operation
		if (op != tokenid::question)
		{
#if RESHADEFX_SHORT_CIRCUIT
			codegen::id lhs_block = 0;
			codegen::id rhs_block = 0;
			codegen::id merge_block = 0;

			// Switch block to a new one before parsing right-hand side value in case it needs to be skipped during short-circuiting
			if (op == tokenid::ampersand_ampersand || op == tokenid::pipe_pipe)
			{
				lhs_block = _codegen->set_block(0);
				rhs_block = _codegen->create_block();
				merge_block = _codegen->create_block();

				_codegen->enter_block(rhs_block);
			}
#endif
			// Parse the right hand side of the binary operation
			expression rhs;
			if (!parse_expression_multary(rhs, right_precedence))
				return false;

			// Deduce the result base type based on implicit conversion rules
			type type = type::merge(lhs.type, rhs.type);
			bool is_bool_result = false;

			// Do some error checking depending on the operator
			if (op == tokenid::equal_equal || op == tokenid::exclaim_equal)
			{
				// Equality checks return a boolean value
				is_bool_result = true;

				// Cannot check equality between incompatible types
				if (lhs.type.is_array() || rhs.type.is_array() || lhs.type.definition != rhs.type.definition)
					return error(rhs.location, 3020, "type mismatch"), false;
			}
			else if (op == tokenid::ampersand || op == tokenid::pipe || op == tokenid::caret)
			{
				if (type.is_boolean())
					type.base = type::t_int;

				// Cannot perform bitwise operations on non-integral types
				if (!lhs.type.is_integral())
					return error(lhs.location, 3082, "int or unsigned int type required"), false;
				if (!rhs.type.is_integral())
					return error(rhs.location, 3082, "int or unsigned int type required"), false;
			}
			else
			{
				if (op == tokenid::ampersand_ampersand || op == tokenid::pipe_pipe)
					type.base = type::t_bool;
				else if (op == tokenid::less || op == tokenid::less_equal || op == tokenid::greater || op == tokenid::greater_equal)
					is_bool_result = true; // Logical operations return a boolean value
				else if (type.is_boolean())
					type.base = type::t_int; // Arithmetic with boolean values treats the operands as integers

				// Cannot perform arithmetic operations on non-basic types
				if (!lhs.type.is_scalar() && !lhs.type.is_vector() && !lhs.type.is_matrix())
					return error(lhs.location, 3022, "scalar, vector, or matrix expected"), false;
				if (!rhs.type.is_scalar() && !rhs.type.is_vector() && !rhs.type.is_matrix())
					return error(rhs.location, 3022, "scalar, vector, or matrix expected"), false;
			}

			// Perform implicit type conversion
			if (lhs.type.components() > type.components())
				warning(lhs.location, 3206, "implicit truncation of vector type");
			if (rhs.type.components() > type.components())
				warning(rhs.location, 3206, "implicit truncation of vector type");

			lhs.add_cast_operation(type);
			rhs.add_cast_operation(type);

#if RESHADEFX_SHORT_CIRCUIT
			// Reset block to left-hand side since the load of the left-hand side value has to happen in there
			if (op == tokenid::ampersand_ampersand || op == tokenid::pipe_pipe)
				_codegen->set_block(lhs_block);
#endif

			// Constant expressions can be evaluated at compile time
			if (rhs.is_constant && lhs.evaluate_constant_expression(op, rhs.constant))
				continue;

			const auto lhs_value = _codegen->emit_load(lhs);

#if RESHADEFX_SHORT_CIRCUIT
			// Short circuit for logical && and || operators
			if (op == tokenid::ampersand_ampersand || op == tokenid::pipe_pipe)
			{
				// Emit "if ( lhs) result = rhs" for && expression
				codegen::id condition_value = lhs_value;
				// Emit "if (!lhs) result = rhs" for || expression
				if (op == tokenid::pipe_pipe)
					condition_value = _codegen->emit_unary_op(lhs.location, tokenid::exclaim, type, lhs_value);

				_codegen->leave_block_and_branch_conditional(condition_value, rhs_block, merge_block);

				_codegen->set_block(rhs_block);
				// Only load value of right hand side expression after entering the second block
				const auto rhs_value = _codegen->emit_load(rhs);
				_codegen->leave_block_and_branch(merge_block);

				_codegen->enter_block(merge_block);

				const auto result_value = _codegen->emit_phi(lhs.location, condition_value, lhs_block, rhs_value, rhs_block, lhs_value, lhs_block, type);

				lhs.reset_to_rvalue(lhs.location, result_value, type);
				continue;
			}
#endif
			const auto rhs_value = _codegen->emit_load(rhs);

			// Certain operations return a boolean type instead of the type of the input expressions
			if (is_bool_result)
				type = { type::t_bool, type.rows, type.cols };

			const auto result_value = _codegen->emit_binary_op(lhs.location, op, type, lhs.type, lhs_value, rhs_value);

			lhs.reset_to_rvalue(lhs.location, result_value, type);
		}
		else
		{
			// A conditional expression needs a scalar or vector type condition
			if (!lhs.type.is_scalar() && !lhs.type.is_vector())
				return error(lhs.location, 3022, "boolean or vector expression expected"), false;

#if RESHADEFX_SHORT_CIRCUIT
			// Switch block to a new one before parsing first part in case it needs to be skipped during short-circuiting
			const codegen::id merge_block = _codegen->create_block();
			const codegen::id condition_block = _codegen->set_block(0);
			codegen::id true_block = _codegen->create_block();
			codegen::id false_block = _codegen->create_block();

			_codegen->enter_block(true_block);
#endif
			// Parse the first part of the right hand side of the ternary operation
			expression true_exp;
			if (!parse_expression(true_exp))
				return false;

			if (!expect(':'))
				return false;

#if RESHADEFX_SHORT_CIRCUIT
			// Switch block to a new one before parsing second part in case it needs to be skipped during short-circuiting
			_codegen->set_block(0);
			_codegen->enter_block(false_block);
#endif
			// Parse the second part of the right hand side of the ternary operation
			expression false_exp;
			if (!parse_expression_assignment(false_exp))
				return false;

			// Check that the condition dimension matches that of at least one side
			if (lhs.type.rows != true_exp.type.rows && lhs.type.cols != true_exp.type.cols)
				return error(lhs.location, 3020, "dimension of conditional does not match value"), false;

			// Check that the two value expressions can be converted between each other
			if (true_exp.type.array_length != false_exp.type.array_length || true_exp.type.definition != false_exp.type.definition)
				return error(false_exp.location, 3020, "type mismatch between conditional values"), false;

			// Deduce the result base type based on implicit conversion rules
			const type type = type::merge(true_exp.type, false_exp.type);

			if (true_exp.type.components() > type.components())
				warning(true_exp.location, 3206, "implicit truncation of vector type");
			if (false_exp.type.components() > type.components())
				warning(false_exp.location, 3206, "implicit truncation of vector type");

#if RESHADEFX_SHORT_CIRCUIT
			// Reset block to left-hand side since the load of the condition value has to happen in there
			_codegen->set_block(condition_block);
#else
			// The conditional operator instruction expects the condition to be a boolean type
			lhs.add_cast_operation({ type::t_bool, type.rows, 1 });
#endif
			true_exp.add_cast_operation(type);
			false_exp.add_cast_operation(type);

			// Load condition value from expression
			const auto condition_value = _codegen->emit_load(lhs);

#if RESHADEFX_SHORT_CIRCUIT
			_codegen->leave_block_and_branch_conditional(condition_value, true_block, false_block);

			_codegen->set_block(true_block);
			// Only load true expression value after entering the first block
			const auto true_value = _codegen->emit_load(true_exp);
			true_block = _codegen->leave_block_and_branch(merge_block);

			_codegen->set_block(false_block);
			// Only load false expression value after entering the second block
			const auto false_value = _codegen->emit_load(false_exp);
			false_block = _codegen->leave_block_and_branch(merge_block);

			_codegen->enter_block(merge_block);

			const auto result_value = _codegen->emit_phi(lhs.location, condition_value, condition_block, true_value, true_block, false_value, false_block, type);
#else
			const auto true_value = _codegen->emit_load(true_exp);
			const auto false_value = _codegen->emit_load(false_exp);

			const auto result_value = _codegen->emit_ternary_op(lhs.location, op, type, condition_value, true_value, false_value);
#endif
			lhs.reset_to_rvalue(lhs.location, result_value, type);
		}
	}

	return true;
}

bool reshadefx::parser::parse_expression_assignment(expression &lhs)
{
	// Parse left hand side of the expression
	if (!parse_expression_multary(lhs))
		return false;

	// Check if an operator exists so that this is an assignment
	if (accept_assignment_op())
	{
		// Remember the operator token before parsing the expression that follows it
		const tokenid op = _token.id;

		// Parse right hand side of the assignment expression
		// This may be another assignment expression to support chains like "a = b = c = 0;"
		expression rhs;
		if (!parse_expression_assignment(rhs))
			return false;

		// Check if the assignment is valid
		if (lhs.type.has(type::q_const) || !lhs.is_lvalue)
			return error(lhs.location, 3025, "l-value specifies const object"), false;
		if (!type::rank(lhs.type, rhs.type))
			return error(rhs.location, 3020, "cannot convert these types (from " + rhs.type.description() + " to " + lhs.type.description() + ')'), false;

		// Cannot perform bitwise operations on non-integral types
		if (!lhs.type.is_integral() && (op == tokenid::ampersand_equal || op == tokenid::pipe_equal || op == tokenid::caret_equal))
			return error(lhs.location, 3082, "int or unsigned int type required"), false;

		// Perform implicit type conversion of right hand side value
		if (rhs.type.components() > lhs.type.components())
			warning(rhs.location, 3206, "implicit truncation of vector type");

		rhs.add_cast_operation(lhs.type);

		auto result = _codegen->emit_load(rhs);

		// Check if this is an assignment with an additional arithmetic instruction
		if (op != tokenid::equal)
		{
			// Load value for modification
			const auto value = _codegen->emit_load(lhs);

			// Handle arithmetic assignment operation
			result = _codegen->emit_binary_op(lhs.location, op, lhs.type, value, result);
		}

		// Write result back to variable
		_codegen->emit_store(lhs, result);

		// Return the result value since you can write assignments within expressions
		lhs.reset_to_rvalue(lhs.location, result, lhs.type);
	}

	return true;
}
