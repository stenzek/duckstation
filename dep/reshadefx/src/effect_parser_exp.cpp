/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "effect_lexer.hpp"
#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include <cassert>
#include <iterator> // std::back_inserter
#include <algorithm> // std::lower_bound, std::set_union

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
	_errors += '(' + std::to_string(location.line) + ", " + std::to_string(location.column) + ')';
	_errors += ": error";
	if (code != 0)
		_errors += " X" + std::to_string(code);
	_errors += ": ";
	_errors += message;
	_errors += '\n';
}
void reshadefx::parser::warning(const location &location, unsigned int code, const std::string &message)
{
	_errors += location.source;
	_errors += '(' + std::to_string(location.line) + ", " + std::to_string(location.column) + ')';
	_errors += ": warning";
	if (code != 0)
		_errors += " X" + std::to_string(code);
	_errors += ": ";
	_errors += message;
	_errors += '\n';
}

void reshadefx::parser::backup()
{
	_token_backup = _token_next;
}
void reshadefx::parser::restore()
{
	_lexer->reset_to_offset(_token_backup.offset + _token_backup.length);
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
	scope scope = { "::", 0, 0 };
	if (!exclusive)
		scope = current_scope();

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
				type.struct_definition = symbol.id;
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
			{
				error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + "', expected vector element type");
				return false;
			}
			else if (!type.is_scalar())
			{
				error(_token.location, 3122, "vector element type must be a scalar type");
				return false;
			}

			if (!expect(',') || !expect(tokenid::int_literal))
			{
				return false;
			}
			else if (_token.literal_as_int < 1 || _token.literal_as_int > 4)
			{
				error(_token.location, 3052, "vector dimension must be between 1 and 4");
				return false;
			}

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
			{
				error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + "', expected matrix element type");
				return false;
			}
			else if (!type.is_scalar())
			{
				error(_token.location, 3123, "matrix element type must be a scalar type");
				return false;
			}

			if (!expect(',') || !expect(tokenid::int_literal))
			{
				return false;
			}
			else if (_token.literal_as_int < 1 || _token.literal_as_int > 4)
			{
				error(_token.location, 3053, "matrix dimensions must be between 1 and 4");
				return false;
			}

			type.rows = static_cast<unsigned int>(_token.literal_as_int);

			if (!expect(',') || !expect(tokenid::int_literal))
			{
				return false;
			}
			else if (_token.literal_as_int < 1 || _token.literal_as_int > 4)
			{
				error(_token.location, 3053, "matrix dimensions must be between 1 and 4");
				return false;
			}

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
			{
				error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + "', expected sampler element type");
				return false;
			}
			if (type.is_object())
			{
				error(_token.location, 3124, "object element type cannot be an object type");
				return false;
			}
			if (!type.is_numeric() || type.is_matrix())
			{
				error(_token.location, 3521, "sampler element type must fit in four 32-bit quantities");
				return false;
			}

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
			{
				error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + "', expected storage element type");
				return false;
			}
			if (type.is_object())
			{
				error(_token.location, 3124, "object element type cannot be an object type");
				return false;
			}
			if (!type.is_numeric() || type.is_matrix())
			{
				error(_token.location, 3521, "storage element type must fit in four 32-bit quantities");
				return false;
			}

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
	{
		// Overwrite 'exp' since conveniently the last expression in the sequence is the result
		if (!parse_expression_assignment(exp))
			return false;
	}

	return true;
}

bool reshadefx::parser::parse_expression_unary(expression &exp)
{
	location location = _token_next.location;

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
		{
			error(exp.location, 3022, "scalar, vector, or matrix expected");
			return false;
		}

		// Special handling for the "++" and "--" operators
		if (op == tokenid::plus_plus || op == tokenid::minus_minus)
		{
			if (exp.type.has(type::q_const) || !exp.is_lvalue)
			{
				error(location, 3025, "l-value specifies const object");
				return false;
			}

			// Create a constant one in the type of the expression
			const codegen::id constant_one = _codegen->emit_constant(exp.type, 1);

			const codegen::id value = _codegen->emit_load(exp);
			const codegen::id result = _codegen->emit_binary_op(location, op, exp.type, value, constant_one);

			// The "++" and "--" operands modify the source variable, so store result back into it
			_codegen->emit_store(exp, result);
		}
		else if (op != tokenid::plus) // Ignore "+" operator since it does not actually do anything
		{
			// The "~" bitwise operator is only valid on integral types
			if (op == tokenid::tilde && !exp.type.is_integral())
			{
				error(exp.location, 3082, "int or unsigned int type required");
				return false;
			}

			// The logical not operator expects a boolean type as input, so perform cast if necessary
			if (op == tokenid::exclaim && !exp.type.is_boolean())
				exp.add_cast_operation({ type::t_bool, exp.type.rows, exp.type.cols }); // The result will be boolean as well

			// Constant expressions can be evaluated at compile time
			if (!exp.evaluate_constant_expression(op))
			{
				const codegen::id value = _codegen->emit_load(exp);
				const codegen::id result = _codegen->emit_unary_op(location, op, exp.type, value);

				exp.reset_to_rvalue(location, result, exp.type);
			}
		}
	}
	else if (accept('('))
	{
		// This backup may get overridden in 'accept_type_class', but should point to the same token still
		backup();

		// Check if this is a C-style cast expression
		if (type cast_type = {}; accept_type_class(cast_type))
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
				{
					error(location, 3017, "cannot convert these types (from " + exp.type.description() + " to " + cast_type.description() + ')');
					return false;
				}

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
			{
				consume_until('}');
				return false;
			}

			// Initializer lists might contain a comma at the end, so break out of the loop if nothing follows afterwards
			if (peek('}'))
				break;

			expression &element_exp = elements.emplace_back();

			// Parse the argument expression
			if (!parse_expression_assignment(element_exp))
			{
				consume_until('}');
				return false;
			}

			if (element_exp.type.is_array())
			{
				error(element_exp.location, 3119, "arrays cannot be multi-dimensional");
				consume_until('}');
				return false;
			}
			if (composite_type.base != type::t_void && element_exp.type.struct_definition != composite_type.struct_definition)
			{
				error(element_exp.location, 3017, "cannot convert these types (from " + element_exp.type.description() + " to " + composite_type.description() + ')');
				consume_until('}');
				return false;
			}

			is_constant &= element_exp.is_constant; // Result is only constant if all arguments are constant
			composite_type = type::merge(composite_type, element_exp.type);
		}

		// Constant arrays can be constructed at compile time
		if (is_constant)
		{
			constant result = {};
			for (expression &element_exp : elements)
			{
				element_exp.add_cast_operation(composite_type);
				result.array_data.push_back(element_exp.constant);
			}

			composite_type.array_length = static_cast<unsigned int>(elements.size());

			exp.reset_to_rvalue_constant(location, std::move(result), composite_type);
		}
		else
		{
			// Resolve all access chains
			for (expression &element_exp : elements)
			{
				element_exp.add_cast_operation(composite_type);
				const codegen::id element_value = _codegen->emit_load(element_exp);
				element_exp.reset_to_rvalue(element_exp.location, element_value, composite_type);
			}

			composite_type.array_length = static_cast<unsigned int>(elements.size());

			const codegen::id result = _codegen->emit_construct(location, composite_type, elements);
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
	else if (type type = {}; accept_type_class(type)) // Check if this is a constructor call expression
	{
		if (!expect('('))
			return false;

		if (!type.is_numeric())
		{
			error(location, 3037, "constructors only defined for numeric base types");
			return false;
		}

		// Empty constructors do not exist
		if (accept(')'))
		{
			error(location, 3014, "incorrect number of arguments to numeric-type constructor");
			return false;
		}

		// Parse entire argument expression list
		bool is_constant = true;
		unsigned int num_components = 0;
		std::vector<expression> arguments;

		while (!peek(')'))
		{
			// There should be a comma between arguments
			if (!arguments.empty() && !expect(','))
				return false;

			expression &argument_exp = arguments.emplace_back();

			// Parse the argument expression
			if (!parse_expression_assignment(argument_exp))
				return false;

			// Constructors are only defined for numeric base types
			if (!argument_exp.type.is_numeric())
			{
				error(argument_exp.location, 3017, "cannot convert non-numeric types");
				return false;
			}

			is_constant &= argument_exp.is_constant; // Result is only constant if all arguments are constant
			num_components += argument_exp.type.components();
		}

		// The list should be terminated with a parenthesis
		if (!expect(')'))
			return false;

		// The total number of argument elements needs to match the number of elements in the result type
		if (num_components != type.components())
		{
			error(location, 3014, "incorrect number of arguments to numeric-type constructor");
			return false;
		}

		assert(num_components > 0 && num_components <= 16 && !type.is_array());

		if (is_constant) // Constants can be converted at compile time
		{
			constant result = {};
			unsigned int i = 0;
			for (expression &argument_exp : arguments)
			{
				argument_exp.add_cast_operation({ type.base, argument_exp.type.rows, argument_exp.type.cols });

				for (unsigned int k = 0; k < argument_exp.type.components(); ++k)
					result.as_uint[i++] = argument_exp.constant.as_uint[k];
			}

			exp.reset_to_rvalue_constant(location, std::move(result), type);
		}
		else if (arguments.size() > 1)
		{
			// Flatten all arguments to a list of scalars
			for (auto it = arguments.begin(); it != arguments.end();)
			{
				// Argument is a scalar already, so only need to cast it
				if (it->type.is_scalar())
				{
					expression &argument_exp = *it++;

					struct type scalar_type = argument_exp.type;
					scalar_type.base = type.base;
					argument_exp.add_cast_operation(scalar_type);

					argument_exp.reset_to_rvalue(argument_exp.location, _codegen->emit_load(argument_exp), scalar_type);
				}
				else
				{
					const expression argument_exp = std::move(*it);
					it = arguments.erase(it);

					// Convert to a scalar value and re-enter the loop in the next iteration (in case a cast is necessary too)
					for (unsigned int i = argument_exp.type.components(); i > 0; --i)
					{
						expression argument_scalar_exp = argument_exp;
						argument_scalar_exp.add_constant_index_access(i - 1);

						it = arguments.insert(it, argument_scalar_exp);
					}
				}
			}

			const codegen::id result = _codegen->emit_construct(location, type, arguments);

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
			{
				error(location, 3005, "identifier '" + identifier + "' represents a variable, not a function");
				return false;
			}

			// Parse entire argument expression list
			std::vector<expression> arguments;

			while (!peek(')'))
			{
				// There should be a comma between arguments
				if (!arguments.empty() && !expect(','))
					return false;

				expression &argument_exp = arguments.emplace_back();

				// Parse the argument expression
				if (!parse_expression_assignment(argument_exp))
					return false;
			}

			// The list should be terminated with a parenthesis
			if (!expect(')'))
				return false;

			// Function calls can only be made from within functions
			if (!_codegen->is_in_function())
			{
				error(location, 3005, "invalid function call outside of a function");
				return false;
			}

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

			std::vector<expression> parameters(symbol.function->parameter_list.size());

			// We need to allocate some temporary variables to pass in and load results from pointer parameters
			for (size_t i = 0; i < arguments.size(); ++i)
			{
				const auto &param_type = symbol.function->parameter_list[i].type;

				if (param_type.has(type::q_out) && (!arguments[i].is_lvalue || (arguments[i].type.has(type::q_const) && !arguments[i].type.is_object())))
				{
					error(arguments[i].location, 3025, "l-value specifies const object for an 'out' parameter");
					return false;
				}

				if (arguments[i].type.components() > param_type.components())
					warning(arguments[i].location, 3206, "implicit truncation of vector type");

				if (symbol.op == symbol_type::function || param_type.has(type::q_out))
				{
					if (param_type.is_object() || param_type.has(type::q_groupshared) /* Special case for atomic intrinsics */)
					{
						if (arguments[i].type != param_type)
						{
							error(location, 3004, "no matching intrinsic overload for '" + identifier + '\'');
							return false;
						}

						assert(arguments[i].is_lvalue);

						// Do not shadow object or pointer parameters to function calls
						size_t chain_index = 0;
						const codegen::id access_chain = _codegen->emit_access_chain(arguments[i], chain_index);
						parameters[i].reset_to_lvalue(arguments[i].location, access_chain, param_type);
						assert(chain_index == arguments[i].chain.size());

						// This is referencing a l-value, but want to avoid copying below
						parameters[i].is_lvalue = false;
					}
					else
					{
						// All user-defined functions actually accept pointers as arguments, same applies to intrinsics with 'out' parameters
						const codegen::id temp_variable = _codegen->define_variable(arguments[i].location, param_type);
						parameters[i].reset_to_lvalue(arguments[i].location, temp_variable, param_type);
					}
				}
				else
				{
					expression argument_exp = arguments[i];
					argument_exp.add_cast_operation(param_type);
					const codegen::id argument_value = _codegen->emit_load(argument_exp);
					parameters[i].reset_to_rvalue(argument_exp.location, argument_value, param_type);

					// Keep track of whether the parameter is a constant for code generation (this makes the expression invalid for all other uses)
					parameters[i].is_constant = argument_exp.is_constant;
				}
			}

			// Copy in parameters from the argument access chains to parameter variables
			for (size_t i = 0; i < arguments.size(); ++i)
			{
				// Only do this for pointer parameters as discovered above
				if (parameters[i].is_lvalue && parameters[i].type.has(type::q_in) && !parameters[i].type.is_object())
				{
					expression argument_exp = arguments[i];
					argument_exp.add_cast_operation(parameters[i].type);
					const codegen::id argument_value = _codegen->emit_load(argument_exp);
					_codegen->emit_store(parameters[i], argument_value);
				}
			}

			// Add remaining default arguments
			for (size_t i = arguments.size(); i < parameters.size(); ++i)
			{
				const auto &param = symbol.function->parameter_list[i];
				assert(param.has_default_value || !_errors.empty());

				const codegen::id argument_value = _codegen->emit_constant(param.type, param.default_value);
				parameters[i].reset_to_rvalue(param.location, argument_value, param.type);

				// Keep track of whether the parameter is a constant for code generation (this makes the expression invalid for all other uses)
				parameters[i].is_constant = true;
			}

			// Check if the call resolving found an intrinsic or function and invoke the corresponding code
			const codegen::id result = (symbol.op == symbol_type::function) ?
				_codegen->emit_call(location, symbol.id, symbol.type, parameters) :
				_codegen->emit_call_intrinsic(location, symbol.id, symbol.type, parameters);

			exp.reset_to_rvalue(location, result, symbol.type);

			// Copy out parameters from parameter variables back to the argument access chains
			for (size_t i = 0; i < arguments.size(); ++i)
			{
				// Only do this for pointer parameters as discovered above
				if (parameters[i].is_lvalue && parameters[i].type.has(type::q_out) && !parameters[i].type.is_object())
				{
					expression argument_exp = parameters[i];
					argument_exp.add_cast_operation(arguments[i].type);
					const codegen::id argument_value = _codegen->emit_load(argument_exp);
					_codegen->emit_store(arguments[i], argument_value);
				}
			}

			if (_codegen->_current_function != nullptr && symbol.op == symbol_type::function)
			{
				// Calling a function makes the caller inherit all sampler and storage object references from the callee
				if (!symbol.function->referenced_samplers.empty())
				{
					std::vector<codegen::id> referenced_samplers;
					referenced_samplers.reserve(_codegen->_current_function->referenced_samplers.size() + symbol.function->referenced_samplers.size());
					std::set_union(_codegen->_current_function->referenced_samplers.begin(), _codegen->_current_function->referenced_samplers.end(), symbol.function->referenced_samplers.begin(), symbol.function->referenced_samplers.end(), std::back_inserter(referenced_samplers));
					_codegen->_current_function->referenced_samplers = std::move(referenced_samplers);
				}
				if (!symbol.function->referenced_storages.empty())
				{
					std::vector<codegen::id> referenced_storages;
					referenced_storages.reserve(_codegen->_current_function->referenced_storages.size() + symbol.function->referenced_storages.size());
					std::set_union(_codegen->_current_function->referenced_storages.begin(), _codegen->_current_function->referenced_storages.end(), symbol.function->referenced_storages.begin(), symbol.function->referenced_storages.end(), std::back_inserter(referenced_storages));
					_codegen->_current_function->referenced_storages = std::move(referenced_storages);
				}

				// Add callee and all its function references to the callers function references
				{
					std::vector<codegen::id> referenced_functions;
					std::set_union(_codegen->_current_function->referenced_functions.begin(), _codegen->_current_function->referenced_functions.end(), symbol.function->referenced_functions.begin(), symbol.function->referenced_functions.end(), std::back_inserter(referenced_functions));
					const auto it = std::lower_bound(referenced_functions.begin(), referenced_functions.end(), symbol.id);
					if (it == referenced_functions.end() || *it != symbol.id)
						referenced_functions.insert(it, symbol.id);
					_codegen->_current_function->referenced_functions = std::move(referenced_functions);
				}
			}
		}
		else if (symbol.op == symbol_type::invalid)
		{
			// Show error if no symbol matching the identifier was found
			error(location, 3004, "undeclared identifier '" + identifier + '\'');
			return false;
		}
		else if (symbol.op == symbol_type::variable)
		{
			assert(symbol.id != 0);
			// Simply return the pointer to the variable, dereferencing is done on site where necessary
			exp.reset_to_lvalue(location, symbol.id, symbol.type);

			if (_codegen->_current_function != nullptr &&
				symbol.scope.level == symbol.scope.namespace_level &&
				// Ignore invalid symbols that were added during error recovery
				symbol.id != 0xFFFFFFFF)
			{
				// Keep track of any global sampler or storage objects referenced in the current function
				if (symbol.type.is_sampler())
				{
					const auto it = std::lower_bound(_codegen->_current_function->referenced_samplers.begin(), _codegen->_current_function->referenced_samplers.end(), symbol.id);
					if (it == _codegen->_current_function->referenced_samplers.end() || *it != symbol.id)
						_codegen->_current_function->referenced_samplers.insert(it, symbol.id);
				}
				if (symbol.type.is_storage())
				{
					const auto it = std::lower_bound(_codegen->_current_function->referenced_storages.begin(), _codegen->_current_function->referenced_storages.end(), symbol.id);
					if (it == _codegen->_current_function->referenced_storages.end() || *it != symbol.id)
						_codegen->_current_function->referenced_storages.insert(it, symbol.id);
				}
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
			error(location, 3005, "identifier '" + identifier + "' represents a function, not a variable");
			return false;
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
			{
				error(exp.location, 3022, "scalar, vector, or matrix expected");
				return false;
			}
			if (exp.type.has(type::q_const) || !exp.is_lvalue)
			{
				error(exp.location, 3025, "l-value specifies const object");
				return false;
			}

			// Create a constant one in the type of the expression
			const codegen::id constant_one = _codegen->emit_constant(exp.type, 1);

			const codegen::id value = _codegen->emit_load(exp, true);
			const codegen::id result = _codegen->emit_binary_op(location, _token.id, exp.type, value, constant_one);

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
			const std::string subscript = std::move(_token.literal_as_string);

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
				const int length = static_cast<int>(subscript.size());
				if (length > 4)
				{
					error(location, 3018, "invalid subscript '" + subscript + "', swizzle too long");
					return false;
				}

				bool is_const = false;
				signed char offsets[4] = { -1, -1, -1, -1 };
				enum { xyzw, rgba, stpq } set[4];

				for (int i = 0; i < length; ++i)
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
						error(location, 3018, "invalid subscript '" + subscript + '\'');
						return false;
					}

					if (i > 0 && (set[i] != set[i - 1]))
					{
						error(location, 3018, "invalid subscript '" + subscript + "', mixed swizzle sets");
						return false;
					}
					if (static_cast<unsigned int>(offsets[i]) >= exp.type.rows)
					{
						error(location, 3018, "invalid subscript '" + subscript + "', swizzle out of range");
						return false;
					}

					// The result is not modifiable if a swizzle appears multiple times
					for (int k = 0; k < i; ++k)
					{
						if (offsets[k] == offsets[i])
						{
							is_const = true;
							break;
						}
					}
				}

				// Add swizzle to current access chain
				exp.add_swizzle_access(offsets, static_cast<unsigned int>(length));

				if (is_const)
					exp.type.qualifiers |= type::q_const;
			}
			else if (exp.type.is_matrix())
			{
				const int length = static_cast<int>(subscript.size());
				if (length < 3)
				{
					error(location, 3018, "invalid subscript '" + subscript + '\'');
					return false;
				}

				bool is_const = false;
				signed char offsets[4] = { -1, -1, -1, -1 };
				const int set = subscript[1] == 'm';
				const int coefficient = !set;

				for (int i = 0, j = 0; i < length; i += 3 + set, ++j)
				{
					if (subscript[i] != '_' ||
						subscript[i + set + 1] < '0' + coefficient ||
						subscript[i + set + 1] > '3' + coefficient ||
						subscript[i + set + 2] < '0' + coefficient ||
						subscript[i + set + 2] > '3' + coefficient)
					{
						error(location, 3018, "invalid subscript '" + subscript + '\'');
						return false;
					}
					if (set && subscript[i + 1] != 'm')
					{
						error(location, 3018, "invalid subscript '" + subscript + "', mixed swizzle sets");
						return false;
					}

					const auto row = static_cast<unsigned int>((subscript[i + set + 1] - '0') - coefficient);
					const auto col = static_cast<unsigned int>((subscript[i + set + 2] - '0') - coefficient);

					if ((row >= exp.type.rows || col >= exp.type.cols) || j > 3)
					{
						error(location, 3018, "invalid subscript '" + subscript + "', swizzle out of range");
						return false;
					}

					offsets[j] = static_cast<signed char>(row * 4 + col);

					// The result is not modifiable if a swizzle appears multiple times
					for (int k = 0; k < j; ++k)
					{
						if (offsets[k] == offsets[j])
						{
							is_const = true;
							break;
						}
					}
				}

				// Add swizzle to current access chain
				exp.add_swizzle_access(offsets, static_cast<unsigned int>(length / (3 + set)));

				if (is_const)
					exp.type.qualifiers |= type::q_const;
			}
			else if (exp.type.is_struct())
			{
				const std::vector<member_type> &member_list = _codegen->get_struct(exp.type.struct_definition).member_list;

				// Find member with matching name is structure definition
				uint32_t member_index = 0;
				for (const member_type &member : member_list)
				{
					if (member.name == subscript)
						break;
					++member_index;
				}

				if (member_index >= member_list.size())
				{
					error(location, 3018, "invalid subscript '" + subscript + '\'');
					return false;
				}

				// Add field index to current access chain
				exp.add_member_access(member_index, member_list[member_index].type);
			}
			else if (exp.type.is_scalar())
			{
				const int length = static_cast<int>(subscript.size());
				if (length > 4)
				{
					error(location, 3018, "invalid subscript '" + subscript + "', swizzle too long");
					return false;
				}

				for (int i = 0; i < length; ++i)
				{
					if ((subscript[i] != 'x' && subscript[i] != 'r' && subscript[i] != 's') || i > 3)
					{
						error(location, 3018, "invalid subscript '" + subscript + '\'');
						return false;
					}
				}

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
			{
				error(_token.location, 3121, "array, matrix, vector, or indexable object type expected in index expression");
				return false;
			}

			// Parse index expression
			expression index_exp;
			if (!parse_expression(index_exp) || !expect(']'))
				return false;

			if (!index_exp.type.is_scalar() || !index_exp.type.is_integral())
			{
				error(index_exp.location, 3120, "invalid type for index - index must be an integer scalar");
				return false;
			}

			// Add index expression to current access chain
			if (index_exp.is_constant)
			{
				// Check array bounds if known
				if (exp.type.is_bounded_array() && index_exp.constant.as_uint[0] >= exp.type.array_length)
				{
					error(index_exp.location, 3504, "array index out of bounds");
					return false;
				}

				exp.add_constant_index_access(index_exp.constant.as_uint[0]);
			}
			else
			{
				if (exp.is_constant)
				{
					// To handle a dynamic index into a constant means we need to create a local variable first or else any of the indexing instructions do not work
					const codegen::id temp_variable = _codegen->define_variable(location, exp.type, std::string(), false, _codegen->emit_constant(exp.type, exp.constant));
					exp.reset_to_lvalue(exp.location, temp_variable, exp.type);
				}

				exp.add_dynamic_index_access(_codegen->emit_load(index_exp));
			}
		}
		else
		{
			break;
		}
	}

	return true;
}

bool reshadefx::parser::parse_expression_multary(expression &lhs_exp, unsigned int left_precedence)
{
	// Parse left hand side of the expression
	if (!parse_expression_unary(lhs_exp))
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
			expression rhs_exp;
			if (!parse_expression_multary(rhs_exp, right_precedence))
				return false;

			// Deduce the result base type based on implicit conversion rules
			type type = type::merge(lhs_exp.type, rhs_exp.type);
			bool is_bool_result = false;

			// Do some error checking depending on the operator
			if (op == tokenid::equal_equal || op == tokenid::exclaim_equal)
			{
				// Equality checks return a boolean value
				is_bool_result = true;

				// Cannot check equality between incompatible types
				if (lhs_exp.type.is_array() || rhs_exp.type.is_array() || lhs_exp.type.struct_definition != rhs_exp.type.struct_definition)
				{
					error(rhs_exp.location, 3020, "type mismatch");
					return false;
				}
			}
			else if (op == tokenid::ampersand || op == tokenid::pipe || op == tokenid::caret)
			{
				if (type.is_boolean())
					type.base = type::t_int;

				// Cannot perform bitwise operations on non-integral types
				if (!lhs_exp.type.is_integral())
				{
					error(lhs_exp.location, 3082, "int or unsigned int type required");
					return false;
				}
				if (!rhs_exp.type.is_integral())
				{
					error(rhs_exp.location, 3082, "int or unsigned int type required");
					return false;
				}
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
				if (!lhs_exp.type.is_scalar() && !lhs_exp.type.is_vector() && !lhs_exp.type.is_matrix())
				{
					error(lhs_exp.location, 3022, "scalar, vector, or matrix expected");
					return false;
				}
				if (!rhs_exp.type.is_scalar() && !rhs_exp.type.is_vector() && !rhs_exp.type.is_matrix())
				{
					error(rhs_exp.location, 3022, "scalar, vector, or matrix expected");
					return false;
				}
			}

			// Perform implicit type conversion
			if (lhs_exp.type.components() > type.components())
				warning(lhs_exp.location, 3206, "implicit truncation of vector type");
			if (rhs_exp.type.components() > type.components())
				warning(rhs_exp.location, 3206, "implicit truncation of vector type");

			lhs_exp.add_cast_operation(type);
			rhs_exp.add_cast_operation(type);

#if RESHADEFX_SHORT_CIRCUIT
			// Reset block to left-hand side since the load of the left-hand side value has to happen in there
			if (op == tokenid::ampersand_ampersand || op == tokenid::pipe_pipe)
				_codegen->set_block(lhs_block);
#endif

			// Constant expressions can be evaluated at compile time
			if (rhs_exp.is_constant && lhs_exp.evaluate_constant_expression(op, rhs_exp.constant))
				continue;

			const codegen::id lhs_value = _codegen->emit_load(lhs_exp);

#if RESHADEFX_SHORT_CIRCUIT
			// Short circuit for logical && and || operators
			if (op == tokenid::ampersand_ampersand || op == tokenid::pipe_pipe)
			{
				// Emit "if ( lhs) result = rhs" for && expression
				codegen::id condition_value = lhs_value;
				// Emit "if (!lhs) result = rhs" for || expression
				if (op == tokenid::pipe_pipe)
					condition_value = _codegen->emit_unary_op(lhs_exp.location, tokenid::exclaim, type, lhs_value);

				_codegen->leave_block_and_branch_conditional(condition_value, rhs_block, merge_block);

				_codegen->set_block(rhs_block);
				// Only load value of right hand side expression after entering the second block
				const codegen::id rhs_value = _codegen->emit_load(rhs_exp);
				_codegen->leave_block_and_branch(merge_block);

				_codegen->enter_block(merge_block);

				const codegen::id result_value = _codegen->emit_phi(lhs_exp.location, condition_value, lhs_block, rhs_value, rhs_block, lhs_value, lhs_block, type);

				lhs_exp.reset_to_rvalue(lhs_exp.location, result_value, type);
				continue;
			}
#endif
			const codegen::id rhs_value = _codegen->emit_load(rhs_exp);

			// Certain operations return a boolean type instead of the type of the input expressions
			if (is_bool_result)
				type = { type::t_bool, type.rows, type.cols };

			const codegen::id result_value = _codegen->emit_binary_op(lhs_exp.location, op, type, lhs_exp.type, lhs_value, rhs_value);

			lhs_exp.reset_to_rvalue(lhs_exp.location, result_value, type);
		}
		else
		{
			// A conditional expression needs a scalar or vector type condition
			if (!lhs_exp.type.is_scalar() && !lhs_exp.type.is_vector())
			{
				error(lhs_exp.location, 3022, "boolean or vector expression expected");
				return false;
			}

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
			if (lhs_exp.type.rows != true_exp.type.rows && lhs_exp.type.cols != true_exp.type.cols)
			{
				error(lhs_exp.location, 3020, "dimension of conditional does not match value");
				return false;
			}

			// Check that the two value expressions can be converted between each other
			if (true_exp.type.array_length != false_exp.type.array_length || true_exp.type.struct_definition != false_exp.type.struct_definition)
			{
				error(false_exp.location, 3020, "type mismatch between conditional values");
				return false;
			}

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
			lhs_exp.add_cast_operation({ type::t_bool, type.rows, 1 });
#endif
			true_exp.add_cast_operation(type);
			false_exp.add_cast_operation(type);

			// Load condition value from expression
			const codegen::id condition_value = _codegen->emit_load(lhs_exp);

#if RESHADEFX_SHORT_CIRCUIT
			_codegen->leave_block_and_branch_conditional(condition_value, true_block, false_block);

			_codegen->set_block(true_block);
			// Only load true expression value after entering the first block
			const codegen::id true_value = _codegen->emit_load(true_exp);
			true_block = _codegen->leave_block_and_branch(merge_block);

			_codegen->set_block(false_block);
			// Only load false expression value after entering the second block
			const codegen::id false_value = _codegen->emit_load(false_exp);
			false_block = _codegen->leave_block_and_branch(merge_block);

			_codegen->enter_block(merge_block);

			const codegen::id result_value = _codegen->emit_phi(lhs_exp.location, condition_value, condition_block, true_value, true_block, false_value, false_block, type);
#else
			const codegen::id true_value = _codegen->emit_load(true_exp);
			const codegen::id false_value = _codegen->emit_load(false_exp);

			const codegen::id result_value = _codegen->emit_ternary_op(lhs_exp.location, op, type, condition_value, true_value, false_value);
#endif
			lhs_exp.reset_to_rvalue(lhs_exp.location, result_value, type);
		}
	}

	return true;
}

bool reshadefx::parser::parse_expression_assignment(expression &lhs_exp)
{
	// Parse left hand side of the expression
	if (!parse_expression_multary(lhs_exp))
		return false;

	// Check if an operator exists so that this is an assignment
	if (accept_assignment_op())
	{
		// Remember the operator token before parsing the expression that follows it
		const tokenid op = _token.id;

		// Parse right hand side of the assignment expression
		// This may be another assignment expression to support chains like "a = b = c = 0;"
		expression rhs_exp;
		if (!parse_expression_assignment(rhs_exp))
			return false;

		// Check if the assignment is valid
		if (lhs_exp.type.has(type::q_const) || !lhs_exp.is_lvalue)
		{
			error(lhs_exp.location, 3025, "l-value specifies const object");
			return false;
		}
		if (!type::rank(lhs_exp.type, rhs_exp.type))
		{
			error(rhs_exp.location, 3020, "cannot convert these types (from " + rhs_exp.type.description() + " to " + lhs_exp.type.description() + ')');
			return false;
		}

		// Cannot perform bitwise operations on non-integral types
		if (!lhs_exp.type.is_integral() && (op == tokenid::ampersand_equal || op == tokenid::pipe_equal || op == tokenid::caret_equal))
		{
			error(lhs_exp.location, 3082, "int or unsigned int type required");
			return false;
		}

		// Perform implicit type conversion of right hand side value
		if (rhs_exp.type.components() > lhs_exp.type.components())
			warning(rhs_exp.location, 3206, "implicit truncation of vector type");

		rhs_exp.add_cast_operation(lhs_exp.type);

		codegen::id result_value = _codegen->emit_load(rhs_exp);

		// Check if this is an assignment with an additional arithmetic instruction
		if (op != tokenid::equal)
		{
			// Load value for modification
			const codegen::id lhs_value = _codegen->emit_load(lhs_exp);

			// Handle arithmetic assignment operation
			result_value = _codegen->emit_binary_op(lhs_exp.location, op, lhs_exp.type, lhs_value, result_value);
		}

		// Write result back to variable
		_codegen->emit_store(lhs_exp, result_value);

		// Return the result value since you can write assignments within expressions
		lhs_exp.reset_to_rvalue(lhs_exp.location, result_value, lhs_exp.type);
	}

	return true;
}
