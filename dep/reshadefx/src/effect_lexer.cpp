/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "effect_lexer.hpp"
#include <cassert>
#include <string_view>
#include <unordered_map> // Used for static lookup tables

using namespace reshadefx;

enum token_type
{
	DIGIT = '0',
	IDENT = 'A',
	SPACE = ' ',
};

// Lookup table which translates a given char to a token type
static const unsigned int s_type_lookup[256] = {
	 0xFF,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00, SPACE,
	 '\n', SPACE, SPACE, SPACE,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,
	 0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,  0x00,
	 0x00,  0x00, SPACE,   '!',   '"',   '#',   '$',   '%',   '&',  '\'',
	  '(',   ')',   '*',   '+',   ',',   '-',   '.',   '/', DIGIT, DIGIT,
	DIGIT, DIGIT, DIGIT, DIGIT, DIGIT, DIGIT, DIGIT, DIGIT,   ':',   ';',
	  '<',   '=',   '>',   '?',   '@', IDENT, IDENT, IDENT, IDENT, IDENT,
	IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT,
	IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT,
	IDENT,   '[',  '\\',   ']',   '^', IDENT,  0x00, IDENT, IDENT, IDENT,
	IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT,
	IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT, IDENT,
	IDENT, IDENT, IDENT,   '{',   '|',   '}',   '~',  0x00,  0x00,  0x00,
};

// Lookup tables which translate a given string literal to a token and backwards
static const std::unordered_map<tokenid, std::string_view> s_token_lookup = {
	{ tokenid::end_of_file, "end of file" },
	{ tokenid::exclaim, "!" },
	{ tokenid::hash, "#" },
	{ tokenid::dollar, "$" },
	{ tokenid::percent, "%" },
	{ tokenid::ampersand, "&" },
	{ tokenid::parenthesis_open, "(" },
	{ tokenid::parenthesis_close, ")" },
	{ tokenid::star, "*" },
	{ tokenid::plus, "+" },
	{ tokenid::comma, "," },
	{ tokenid::minus, "-" },
	{ tokenid::dot, "." },
	{ tokenid::slash, "/" },
	{ tokenid::colon, ":" },
	{ tokenid::semicolon, ";" },
	{ tokenid::less, "<" },
	{ tokenid::equal, "=" },
	{ tokenid::greater, ">" },
	{ tokenid::question, "?" },
	{ tokenid::at, "@" },
	{ tokenid::bracket_open, "[" },
	{ tokenid::backslash, "\\" },
	{ tokenid::bracket_close, "]" },
	{ tokenid::caret, "^" },
	{ tokenid::brace_open, "{" },
	{ tokenid::pipe, "|" },
	{ tokenid::brace_close, "}" },
	{ tokenid::tilde, "~" },
	{ tokenid::exclaim_equal, "!=" },
	{ tokenid::percent_equal, "%=" },
	{ tokenid::ampersand_ampersand, "&&" },
	{ tokenid::ampersand_equal, "&=" },
	{ tokenid::star_equal, "*=" },
	{ tokenid::plus_plus, "++" },
	{ tokenid::plus_equal, "+=" },
	{ tokenid::minus_minus, "--" },
	{ tokenid::minus_equal, "-=" },
	{ tokenid::arrow, "->" },
	{ tokenid::ellipsis, "..." },
	{ tokenid::slash_equal, "|=" },
	{ tokenid::colon_colon, "::" },
	{ tokenid::less_less_equal, "<<=" },
	{ tokenid::less_less, "<<" },
	{ tokenid::less_equal, "<=" },
	{ tokenid::equal_equal, "==" },
	{ tokenid::greater_greater_equal, ">>=" },
	{ tokenid::greater_greater, ">>" },
	{ tokenid::greater_equal, ">=" },
	{ tokenid::caret_equal, "^=" },
	{ tokenid::pipe_equal, "|=" },
	{ tokenid::pipe_pipe, "||" },
	{ tokenid::identifier, "identifier" },
	{ tokenid::reserved, "reserved word" },
	{ tokenid::true_literal, "true" },
	{ tokenid::false_literal, "false" },
	{ tokenid::int_literal, "integral literal" },
	{ tokenid::uint_literal, "integral literal" },
	{ tokenid::float_literal, "floating point literal" },
	{ tokenid::double_literal, "floating point literal" },
	{ tokenid::string_literal, "string literal" },
	{ tokenid::namespace_, "namespace" },
	{ tokenid::struct_, "struct" },
	{ tokenid::technique, "technique" },
	{ tokenid::pass, "pass" },
	{ tokenid::for_, "for" },
	{ tokenid::while_, "while" },
	{ tokenid::do_, "do" },
	{ tokenid::if_, "if" },
	{ tokenid::else_, "else" },
	{ tokenid::switch_, "switch" },
	{ tokenid::case_, "case" },
	{ tokenid::default_, "default" },
	{ tokenid::break_, "break" },
	{ tokenid::continue_, "continue" },
	{ tokenid::return_, "return" },
	{ tokenid::discard_, "discard" },
	{ tokenid::extern_, "extern" },
	{ tokenid::static_, "static" },
	{ tokenid::uniform_, "uniform" },
	{ tokenid::volatile_, "volatile" },
	{ tokenid::precise, "precise" },
	{ tokenid::groupshared, "groupshared" },
	{ tokenid::in, "in" },
	{ tokenid::out, "out" },
	{ tokenid::inout, "inout" },
	{ tokenid::const_, "const" },
	{ tokenid::linear, "linear" },
	{ tokenid::noperspective, "noperspective" },
	{ tokenid::centroid, "centroid" },
	{ tokenid::nointerpolation, "nointerpolation" },
	{ tokenid::void_, "void" },
	{ tokenid::bool_, "bool" },
	{ tokenid::bool2, "bool2" },
	{ tokenid::bool3, "bool3" },
	{ tokenid::bool4, "bool4" },
	{ tokenid::bool2x2, "bool2x2" },
	{ tokenid::bool2x3, "bool2x3" },
	{ tokenid::bool2x4, "bool2x4" },
	{ tokenid::bool3x2, "bool3x2" },
	{ tokenid::bool3x3, "bool3x3" },
	{ tokenid::bool3x4, "bool3x4" },
	{ tokenid::bool4x2, "bool4x2" },
	{ tokenid::bool4x3, "bool4x3" },
	{ tokenid::bool4x4, "bool4x4" },
	{ tokenid::int_, "int" },
	{ tokenid::int2, "int2" },
	{ tokenid::int3, "int3" },
	{ tokenid::int4, "int4" },
	{ tokenid::int2x2, "int2x2" },
	{ tokenid::int2x3, "int2x3" },
	{ tokenid::int2x4, "int2x4" },
	{ tokenid::int3x2, "int3x2" },
	{ tokenid::int3x3, "int3x3" },
	{ tokenid::int3x4, "int3x4" },
	{ tokenid::int4x2, "int4x2" },
	{ tokenid::int4x3, "int4x3" },
	{ tokenid::int4x4, "int4x4" },
	{ tokenid::min16int, "min16int" },
	{ tokenid::min16int2, "min16int2" },
	{ tokenid::min16int3, "min16int3" },
	{ tokenid::min16int4, "min16int4" },
	{ tokenid::uint_, "uint" },
	{ tokenid::uint2, "uint2" },
	{ tokenid::uint3, "uint3" },
	{ tokenid::uint4, "uint4" },
	{ tokenid::uint2x2, "uint2x2" },
	{ tokenid::uint2x3, "uint2x3" },
	{ tokenid::uint2x4, "uint2x4" },
	{ tokenid::uint3x2, "uint3x2" },
	{ tokenid::uint3x3, "uint3x3" },
	{ tokenid::uint3x4, "uint3x4" },
	{ tokenid::uint4x2, "uint4x2" },
	{ tokenid::uint4x3, "uint4x3" },
	{ tokenid::uint4x4, "uint4x4" },
	{ tokenid::min16uint, "min16uint" },
	{ tokenid::min16uint2, "min16uint2" },
	{ tokenid::min16uint3, "min16uint3" },
	{ tokenid::min16uint4, "min16uint4" },
	{ tokenid::float_, "float" },
	{ tokenid::float2, "float2" },
	{ tokenid::float3, "float3" },
	{ tokenid::float4, "float4" },
	{ tokenid::float2x2, "float2x2" },
	{ tokenid::float2x3, "float2x3" },
	{ tokenid::float2x4, "float2x4" },
	{ tokenid::float3x2, "float3x2" },
	{ tokenid::float3x3, "float3x3" },
	{ tokenid::float3x4, "float3x4" },
	{ tokenid::float4x2, "float4x2" },
	{ tokenid::float4x3, "float4x3" },
	{ tokenid::float4x4, "float4x4" },
	{ tokenid::min16float, "min16float" },
	{ tokenid::min16float2, "min16float2" },
	{ tokenid::min16float3, "min16float3" },
	{ tokenid::min16float4, "min16float4" },
	{ tokenid::vector, "vector" },
	{ tokenid::matrix, "matrix" },
	{ tokenid::string_, "string" },
	{ tokenid::texture1d, "texture1D" },
	{ tokenid::texture2d, "texture2D" },
	{ tokenid::texture3d, "texture3D" },
	{ tokenid::sampler1d, "sampler1D" },
	{ tokenid::sampler2d, "sampler2D" },
	{ tokenid::sampler3d, "sampler3D" },
	{ tokenid::storage1d, "storage1D" },
	{ tokenid::storage2d, "storage2D" },
	{ tokenid::storage3d, "storage3D" },
};
static const std::unordered_map<std::string_view, tokenid> s_keyword_lookup = {
	{ "asm", tokenid::reserved },
	{ "asm_fragment", tokenid::reserved },
	{ "auto", tokenid::reserved },
	{ "bool", tokenid::bool_ },
	{ "bool2", tokenid::bool2 },
	{ "bool2x1", tokenid::bool2 },
	{ "bool2x2", tokenid::bool2x2 },
	{ "bool2x3", tokenid::bool2x3 },
	{ "bool2x4", tokenid::bool2x4 },
	{ "bool3", tokenid::bool3 },
	{ "bool3x1", tokenid::bool3 },
	{ "bool3x2", tokenid::bool3x2 },
	{ "bool3x3", tokenid::bool3x3 },
	{ "bool3x4", tokenid::bool3x4 },
	{ "bool4", tokenid::bool4 },
	{ "bool4x1", tokenid::bool4 },
	{ "bool4x2", tokenid::bool4x2 },
	{ "bool4x3", tokenid::bool4x3 },
	{ "bool4x4", tokenid::bool4x4 },
	{ "break", tokenid::break_ },
	{ "case", tokenid::case_ },
	{ "cast", tokenid::reserved },
	{ "catch", tokenid::reserved },
	{ "centroid", tokenid::reserved },
	{ "char", tokenid::reserved },
	{ "class", tokenid::reserved },
	{ "column_major", tokenid::reserved },
	{ "compile", tokenid::reserved },
	{ "const", tokenid::const_ },
	{ "const_cast", tokenid::reserved },
	{ "continue", tokenid::continue_ },
	{ "default", tokenid::default_ },
	{ "delete", tokenid::reserved },
	{ "discard", tokenid::discard_ },
	{ "do", tokenid::do_ },
	{ "double", tokenid::reserved },
	{ "dword", tokenid::uint_ },
	{ "dword2", tokenid::uint2 },
	{ "dword2x1", tokenid::uint2 },
	{ "dword2x2", tokenid::uint2x2 },
	{ "dword2x3", tokenid::uint2x3 },
	{ "dword2x4", tokenid::uint2x4 },
	{ "dword3", tokenid::uint3, },
	{ "dword3x1", tokenid::uint3 },
	{ "dword3x2", tokenid::uint3x2 },
	{ "dword3x3", tokenid::uint3x3 },
	{ "dword3x4", tokenid::uint3x4 },
	{ "dword4", tokenid::uint4 },
	{ "dword4x1", tokenid::uint4 },
	{ "dword4x2", tokenid::uint4x2 },
	{ "dword4x3", tokenid::uint4x3 },
	{ "dword4x4", tokenid::uint4x4 },
	{ "dynamic_cast", tokenid::reserved },
	{ "else", tokenid::else_ },
	{ "enum", tokenid::reserved },
	{ "explicit", tokenid::reserved },
	{ "extern", tokenid::extern_ },
	{ "external", tokenid::reserved },
	{ "false", tokenid::false_literal },
	{ "FALSE", tokenid::false_literal },
	{ "float", tokenid::float_ },
	{ "float2", tokenid::float2 },
	{ "float2x1", tokenid::float2 },
	{ "float2x2", tokenid::float2x2 },
	{ "float2x3", tokenid::float2x3 },
	{ "float2x4", tokenid::float2x4 },
	{ "float3", tokenid::float3 },
	{ "float3x1", tokenid::float3 },
	{ "float3x2", tokenid::float3x2 },
	{ "float3x3", tokenid::float3x3 },
	{ "float3x4", tokenid::float3x4 },
	{ "float4", tokenid::float4 },
	{ "float4x1", tokenid::float4 },
	{ "float4x2", tokenid::float4x2 },
	{ "float4x3", tokenid::float4x3 },
	{ "float4x4", tokenid::float4x4 },
	{ "for", tokenid::for_ },
	{ "foreach", tokenid::reserved },
	{ "friend", tokenid::reserved },
	{ "globallycoherent", tokenid::reserved },
	{ "goto", tokenid::reserved },
	{ "groupshared", tokenid::groupshared },
	{ "half", tokenid::reserved },
	{ "half2", tokenid::reserved },
	{ "half2x1", tokenid::reserved },
	{ "half2x2", tokenid::reserved },
	{ "half2x3", tokenid::reserved },
	{ "half2x4", tokenid::reserved },
	{ "half3", tokenid::reserved },
	{ "half3x1", tokenid::reserved },
	{ "half3x2", tokenid::reserved },
	{ "half3x3", tokenid::reserved },
	{ "half3x4", tokenid::reserved },
	{ "half4", tokenid::reserved },
	{ "half4x1", tokenid::reserved },
	{ "half4x2", tokenid::reserved },
	{ "half4x3", tokenid::reserved },
	{ "half4x4", tokenid::reserved },
	{ "if", tokenid::if_ },
	{ "in", tokenid::in },
	{ "inline", tokenid::reserved },
	{ "inout", tokenid::inout },
	{ "int", tokenid::int_ },
	{ "int2", tokenid::int2 },
	{ "int2x1", tokenid::int2 },
	{ "int2x2", tokenid::int2x2 },
	{ "int2x3", tokenid::int2x3 },
	{ "int2x4", tokenid::int2x4 },
	{ "int3", tokenid::int3 },
	{ "int3x1", tokenid::int3 },
	{ "int3x2", tokenid::int3x2 },
	{ "int3x3", tokenid::int3x3 },
	{ "int3x4", tokenid::int3x4 },
	{ "int4", tokenid::int4 },
	{ "int4x1", tokenid::int4 },
	{ "int4x2", tokenid::int4x2 },
	{ "int4x3", tokenid::int4x3 },
	{ "int4x4", tokenid::int4x4 },
	{ "interface", tokenid::reserved },
	{ "linear", tokenid::linear },
	{ "long", tokenid::reserved },
	{ "matrix", tokenid::matrix },
	{ "min16float", tokenid::min16float },
	{ "min16float2", tokenid::min16float2 },
	{ "min16float3", tokenid::min16float3 },
	{ "min16float4", tokenid::min16float4 },
	{ "min16int", tokenid::min16int },
	{ "min16int2", tokenid::min16int2 },
	{ "min16int3", tokenid::min16int3 },
	{ "min16int4", tokenid::min16int4 },
	{ "min16uint", tokenid::min16uint },
	{ "min16uint2", tokenid::min16uint2 },
	{ "min16uint3", tokenid::min16uint3 },
	{ "min16uint4", tokenid::min16uint4 },
	{ "mutable", tokenid::reserved },
	{ "namespace", tokenid::namespace_ },
	{ "new", tokenid::reserved },
	{ "noinline", tokenid::reserved },
	{ "nointerpolation", tokenid::nointerpolation },
	{ "noperspective", tokenid::noperspective },
	{ "operator", tokenid::reserved },
	{ "out", tokenid::out },
	{ "packed", tokenid::reserved },
	{ "packoffset", tokenid::reserved },
	{ "pass", tokenid::pass },
	{ "precise", tokenid::precise },
	{ "private", tokenid::reserved },
	{ "protected", tokenid::reserved },
	{ "public", tokenid::reserved },
	{ "register", tokenid::reserved },
	{ "reinterpret_cast", tokenid::reserved },
	{ "restrict", tokenid::reserved },
	{ "return", tokenid::return_ },
	{ "row_major", tokenid::reserved },
	{ "sample", tokenid::reserved },
	{ "sampler", tokenid::sampler2d },
	{ "sampler1D", tokenid::sampler1d },
	{ "sampler1DArray", tokenid::reserved },
	{ "sampler2D", tokenid::sampler2d },
	{ "sampler2DArray", tokenid::reserved },
	{ "sampler2DMS", tokenid::reserved },
	{ "sampler2DMSArray", tokenid::reserved },
	{ "sampler3D", tokenid::sampler3d },
	{ "sampler_state", tokenid::reserved },
	{ "samplerCube", tokenid::reserved },
	{ "samplerCubeArray", tokenid::reserved },
	{ "samplerCUBE", tokenid::reserved },
	{ "samplerRect", tokenid::reserved },
	{ "samplerRECT", tokenid::reserved },
	{ "SamplerState", tokenid::reserved },
	{ "storage", tokenid::storage2d },
	{ "storage1D", tokenid::storage1d },
	{ "storage2D", tokenid::storage2d },
	{ "storage3D", tokenid::storage3d },
	{ "shared", tokenid::reserved },
	{ "short", tokenid::reserved },
	{ "signed", tokenid::reserved },
	{ "sizeof", tokenid::reserved },
	{ "snorm", tokenid::reserved },
	{ "static", tokenid::static_ },
	{ "static_cast", tokenid::reserved },
	{ "string", tokenid::string_ },
	{ "struct", tokenid::struct_ },
	{ "switch", tokenid::switch_ },
	{ "technique", tokenid::technique },
	{ "template", tokenid::reserved },
	{ "texture", tokenid::texture2d },
	{ "Texture1D", tokenid::reserved },
	{ "texture1D", tokenid::texture1d },
	{ "Texture1DArray", tokenid::reserved },
	{ "Texture2D", tokenid::reserved },
	{ "texture2D", tokenid::texture2d },
	{ "Texture2DArray", tokenid::reserved },
	{ "Texture2DMS", tokenid::reserved },
	{ "Texture2DMSArray", tokenid::reserved },
	{ "Texture3D", tokenid::reserved },
	{ "texture3D", tokenid::texture3d },
	{ "textureCUBE", tokenid::reserved },
	{ "TextureCube", tokenid::reserved },
	{ "TextureCubeArray", tokenid::reserved },
	{ "textureRECT", tokenid::reserved },
	{ "this", tokenid::reserved },
	{ "true", tokenid::true_literal },
	{ "TRUE", tokenid::true_literal },
	{ "try", tokenid::reserved },
	{ "typedef", tokenid::reserved },
	{ "uint", tokenid::uint_ },
	{ "uint2", tokenid::uint2 },
	{ "uint2x1", tokenid::uint2 },
	{ "uint2x2", tokenid::uint2x2 },
	{ "uint2x3", tokenid::uint2x3 },
	{ "uint2x4", tokenid::uint2x4 },
	{ "uint3", tokenid::uint3 },
	{ "uint3x1", tokenid::uint3 },
	{ "uint3x2", tokenid::uint3x2 },
	{ "uint3x3", tokenid::uint3x3 },
	{ "uint3x4", tokenid::uint3x4 },
	{ "uint4", tokenid::uint4 },
	{ "uint4x1", tokenid::uint4 },
	{ "uint4x2", tokenid::uint4x2 },
	{ "uint4x3", tokenid::uint4x3 },
	{ "uint4x4", tokenid::uint4x4 },
	{ "uniform", tokenid::uniform_ },
	{ "union", tokenid::reserved },
	{ "unorm", tokenid::reserved },
	{ "unsigned", tokenid::reserved },
	{ "using", tokenid::reserved },
	{ "vector", tokenid::vector },
	{ "virtual", tokenid::reserved },
	{ "void", tokenid::void_ },
	{ "volatile", tokenid::volatile_ },
	{ "while", tokenid::while_ }
};
static const std::unordered_map<std::string_view, tokenid> s_pp_directive_lookup = {
	{ "define", tokenid::hash_def },
	{ "undef", tokenid::hash_undef },
	{ "if", tokenid::hash_if },
	{ "ifdef", tokenid::hash_ifdef },
	{ "ifndef", tokenid::hash_ifndef },
	{ "else", tokenid::hash_else },
	{ "elif", tokenid::hash_elif },
	{ "endif", tokenid::hash_endif },
	{ "error", tokenid::hash_error },
	{ "warning", tokenid::hash_warning },
	{ "pragma", tokenid::hash_pragma },
	{ "include", tokenid::hash_include },
};

static bool is_octal_digit(char c)
{
	return static_cast<unsigned>(c - '0') < 8;
}
static bool is_decimal_digit(char c)
{
	return static_cast<unsigned>(c - '0') < 10;
}
static bool is_hexadecimal_digit(char c)
{
	return is_decimal_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_digit(char c, int radix)
{
	switch (radix)
	{
	case 8:
		return is_octal_digit(c);
	case 10:
		return is_decimal_digit(c);
	case 16:
		return is_hexadecimal_digit(c);
	}

	return false;
}
static long long octal_to_decimal(long long n)
{
	long long m = 0;

	while (n != 0)
	{
		m *= 8;
		m += n & 7;
		n >>= 3;
	}

	while (m != 0)
	{
		n *= 10;
		n += m & 7;
		m >>= 3;
	}

	return n;
}

std::string reshadefx::token::id_to_name(tokenid id)
{
	const auto it = s_token_lookup.find(id);
	if (it != s_token_lookup.end())
		return std::string(it->second);
	return "unknown";
}

reshadefx::token reshadefx::lexer::lex()
{
	bool is_at_line_begin = _cur_location.column <= 1;

	token tok;
next_token:
	// Reset token data
	tok.location = _cur_location;
	tok.offset = input_offset();
	tok.length = 1;
	tok.literal_as_double = 0;
	tok.literal_as_string.clear();

	assert(_cur <= _end);

	// Do a character type lookup for the current character
	switch (s_type_lookup[uint8_t(*_cur)])
	{
	case 0xFF: // EOF
		tok.id = tokenid::end_of_file;
		return tok;
	case SPACE:
		skip_space();
		if (_ignore_whitespace || is_at_line_begin || *_cur == '\n')
			goto next_token;
		tok.id = tokenid::space;
		tok.length = input_offset() - tok.offset;
		return tok;
	case '\n':
		_cur++;
		_cur_location.line++;
		_cur_location.column = 1;
		is_at_line_begin = true;
		if (_ignore_whitespace)
			goto next_token;
		tok.id = tokenid::end_of_line;
		return tok;
	case DIGIT:
		parse_numeric_literal(tok);
		break;
	case IDENT:
		parse_identifier(tok);
		break;
	case '!':
		if (_cur[1] == '=')
			tok.id = tokenid::exclaim_equal,
			tok.length = 2;
		else
			tok.id = tokenid::exclaim;
		break;
	case '"':
		parse_string_literal(tok, _escape_string_literals);
		break;
	case '#':
		if (is_at_line_begin)
		{
			if (!parse_pp_directive(tok) || _ignore_pp_directives)
			{
				skip_to_next_line();
				goto next_token;
			}
		} // These braces are important so the 'else' is matched to the right 'if' statement
		else
		tok.id = tokenid::hash;
		break;
	case '$':
		tok.id = tokenid::dollar;
		break;
	case '%':
		if (_cur[1] == '=')
			tok.id = tokenid::percent_equal,
			tok.length = 2;
		else
			tok.id = tokenid::percent;
		break;
	case '&':
		if (_cur[1] == '&')
			tok.id = tokenid::ampersand_ampersand,
			tok.length = 2;
		else if (_cur[1] == '=')
			tok.id = tokenid::ampersand_equal,
			tok.length = 2;
		else
			tok.id = tokenid::ampersand;
		break;
	case '(':
		tok.id = tokenid::parenthesis_open;
		break;
	case ')':
		tok.id = tokenid::parenthesis_close;
		break;
	case '*':
		if (_cur[1] == '=')
			tok.id = tokenid::star_equal,
			tok.length = 2;
		else
			tok.id = tokenid::star;
		break;
	case '+':
		if (_cur[1] == '+')
			tok.id = tokenid::plus_plus,
			tok.length = 2;
		else if (_cur[1] == '=')
			tok.id = tokenid::plus_equal,
			tok.length = 2;
		else
			tok.id = tokenid::plus;
		break;
	case ',':
		tok.id = tokenid::comma;
		break;
	case '-':
		if (_cur[1] == '-')
			tok.id = tokenid::minus_minus,
			tok.length = 2;
		else if (_cur[1] == '=')
			tok.id = tokenid::minus_equal,
			tok.length = 2;
		else if (_cur[1] == '>')
			tok.id = tokenid::arrow,
			tok.length = 2;
		else
			tok.id = tokenid::minus;
		break;
	case '.':
		if (s_type_lookup[uint8_t(_cur[1])] == DIGIT)
			parse_numeric_literal(tok);
		else if (_cur[1] == '.' && _cur[2] == '.')
			tok.id = tokenid::ellipsis,
			tok.length = 3;
		else
			tok.id = tokenid::dot;
		break;
	case '/':
		if (_cur[1] == '/')
		{
			skip_to_next_line();
			if (_ignore_comments)
				goto next_token;
			tok.id = tokenid::single_line_comment;
			tok.length = input_offset() - tok.offset;
			return tok;
		}
		else if (_cur[1] == '*')
		{
			while (_cur < _end)
			{
				if (*_cur == '\n')
				{
					_cur_location.line++;
					_cur_location.column = 1;
				}
				else if (_cur[0] == '*' && _cur[1] == '/')
				{
					skip(2);
					break;
				}
				skip(1);
			}
			if (_ignore_comments)
				goto next_token;
			tok.id = tokenid::multi_line_comment;
			tok.length = input_offset() - tok.offset;
			return tok;
		}
		else if (_cur[1] == '=')
			tok.id = tokenid::slash_equal,
			tok.length = 2;
		else
			tok.id = tokenid::slash;
		break;
	case ':':
		if (_cur[1] == ':')
			tok.id = tokenid::colon_colon,
			tok.length = 2;
		else
			tok.id = tokenid::colon;
		break;
	case ';':
		tok.id = tokenid::semicolon;
		break;
	case '<':
		if (_cur[1] == '<')
			if (_cur[2] == '=')
				tok.id = tokenid::less_less_equal,
				tok.length = 3;
			else
				tok.id = tokenid::less_less,
				tok.length = 2;
		else if (_cur[1] == '=')
			tok.id = tokenid::less_equal,
			tok.length = 2;
		else
			tok.id = tokenid::less;
		break;
	case '=':
		if (_cur[1] == '=')
			tok.id = tokenid::equal_equal,
			tok.length = 2;
		else
			tok.id = tokenid::equal;
		break;
	case '>':
		if (_cur[1] == '>')
			if (_cur[2] == '=')
				tok.id = tokenid::greater_greater_equal,
				tok.length = 3;
			else
				tok.id = tokenid::greater_greater,
				tok.length = 2;
		else if (_cur[1] == '=')
			tok.id = tokenid::greater_equal,
			tok.length = 2;
		else
			tok.id = tokenid::greater;
		break;
	case '?':
		tok.id = tokenid::question;
		break;
	case '@':
		tok.id = tokenid::at;
		break;
	case '[':
		tok.id = tokenid::bracket_open;
		break;
	case '\\':
		if (_cur[1] == '\n' || (_cur[1] == '\r' && _cur[2] == '\n'))
		{
			// Skip to next line if current line ends with a backslash
			skip_space();
			if (_ignore_whitespace)
				goto next_token;
			tok.id = tokenid::space;
			tok.length = input_offset() - tok.offset;
			return tok;
		}
		tok.id = tokenid::backslash;
		break;
	case ']':
		tok.id = tokenid::bracket_close;
		break;
	case '^':
		if (_cur[1] == '=')
			tok.id = tokenid::caret_equal,
			tok.length = 2;
		else
			tok.id = tokenid::caret;
		break;
	case '{':
		tok.id = tokenid::brace_open;
		break;
	case '|':
		if (_cur[1] == '=')
			tok.id = tokenid::pipe_equal,
			tok.length = 2;
		else if (_cur[1] == '|')
			tok.id = tokenid::pipe_pipe,
			tok.length = 2;
		else
			tok.id = tokenid::pipe;
		break;
	case '}':
		tok.id = tokenid::brace_close;
		break;
	case '~':
		tok.id = tokenid::tilde;
		break;
	default:
		tok.id = tokenid::unknown;
		break;
	}

	skip(tok.length);

	return tok;
}

void reshadefx::lexer::skip(size_t length)
{
	_cur += length;
	_cur_location.column += static_cast<unsigned int>(length);
}
void reshadefx::lexer::skip_space()
{
	// Skip each character until a space is found
	while (_cur < _end)
	{
		if (_cur[0] == '\\' && (_cur[1] == '\n' || (_cur[1] == '\r' && _cur[2] == '\n')))
		{
			skip(_cur[1] == '\r' ? 3 : 2);
			_cur_location.line++;
			_cur_location.column = 1;
			continue;
		}

		if (s_type_lookup[uint8_t(*_cur)] == SPACE)
			skip(1);
		else
			break;
	}
}
void reshadefx::lexer::skip_to_next_line()
{
	// Skip each character until a new line feed is found
	while (*_cur != '\n' && _cur < _end)
	{
#if 0
		if (_cur[0] == '\\' && (_cur[1] == '\n' || (_cur[1] == '\r' && _cur[2] == '\n')))
		{
			skip(_cur[1] == '\r' ? 3 : 2);
			_cur_location.line++;
			_cur_location.column = 1;
			continue;
		}
#endif

		skip(1);
	}
}

void reshadefx::lexer::reset_to_offset(size_t offset)
{
	assert(offset < _input.size());
	_cur = _input.data() + offset;
}

void reshadefx::lexer::parse_identifier(token &tok) const
{
	auto *const begin = _cur, *end = begin;

	// Skip to the end of the identifier sequence
	while (s_type_lookup[uint8_t(*end)] == IDENT || s_type_lookup[uint8_t(*end)] == DIGIT)
		end++;

	tok.id = tokenid::identifier;
	tok.offset = input_offset();
	tok.length = end - begin;
	tok.literal_as_string.assign(begin, end);

	if (_ignore_keywords)
		return;

	if (const auto it = s_keyword_lookup.find(tok.literal_as_string);
		it != s_keyword_lookup.end())
		tok.id = it->second;
}
bool reshadefx::lexer::parse_pp_directive(token &tok)
{
	skip(1); // Skip the '#'
	skip_space(); // Skip any space between the '#' and directive
	parse_identifier(tok);

	if (const auto it = s_pp_directive_lookup.find(tok.literal_as_string);
		it != s_pp_directive_lookup.end())
	{
		tok.id = it->second;
		return true;
	}
	else if (!_ignore_line_directives && tok.literal_as_string == "line") // The #line directive needs special handling
	{
		skip(tok.length); // The 'parse_identifier' does not update the pointer to the current character, so do that now
		skip_space();
		parse_numeric_literal(tok);
		skip(tok.length);

		_cur_location.line = tok.literal_as_int;

		// Need to subtract one since the line containing #line does not count into the statistics
		if (_cur_location.line != 0)
			_cur_location.line--;

		skip_space();

		// Check if this #line directive has an file name attached to it
		if (_cur[0] == '"')
		{
			token temptok;
			parse_string_literal(temptok, false);

			_cur_location.source = std::move(temptok.literal_as_string);
		}

		// Do not return the #line directive as token to the caller
		return false;
	}

	tok.id = tokenid::hash_unknown;

	return true;
}
void reshadefx::lexer::parse_string_literal(token &tok, bool escape)
{
	auto *const begin = _cur, *end = begin + 1; // Skip first quote character right away

	for (auto c = *end; c != '"'; c = *++end)
	{
		if (c == '\n' || end >= _end)
		{
			// Line feed reached, the string literal is done (technically this should be an error, but the lexer does not report errors, so ignore it)
			end--;
			if (end[0] == '\r') end--;
			break;
		}

		if (c == '\r')
		{
			// Silently ignore carriage return characters
			continue;
		}

		if (unsigned int n = (end[1] == '\r' && end + 2 < _end) ? 2 : 1;
			c == '\\' && end[n] == '\n')
		{
			// Escape character found at end of line, the string literal continues on to the next line
			end += n;
			_cur_location.line++;
			continue;
		}

		// Handle escape sequences
		if (c == '\\' && escape)
		{
			unsigned int n = 0;

			// Any character following the '\' is not parsed as usual, so increment pointer here (this makes sure '\"' does not abort the outer loop as well)
			switch (c = *++end)
			{
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
				for (unsigned int i = 0; i < 3 && is_octal_digit(*end) && end < _end; i++)
				{
					c = *end++;
					n = (n << 3) | (c - '0');
				}
				// For simplicity the number is limited to what fits in a single character
				c = n & 0xFF;
				// The octal parsing loop above incremented one pass the escape sequence, so step back
				end--;
				break;
			case 'a':
				c = '\a';
				break;
			case 'b':
				c = '\b';
				break;
			case 'f':
				c = '\f';
				break;
			case 'n':
				c = '\n';
				break;
			case 'r':
				c = '\r';
				break;
			case 't':
				c = '\t';
				break;
			case 'v':
				c = '\v';
				break;
			case 'x':
				if (is_hexadecimal_digit(*++end))
				{
					while (is_hexadecimal_digit(*end) && end < _end)
					{
						c = *end++;
						n = (n << 4) | (is_decimal_digit(c) ? (c - '0') : (c - 55 - 32 * (c & 0x20)));
					}

					// For simplicity the number is limited to what fits in a single character
					c = n & 0xFF;
				}
				// The hexadecimal parsing loop and check above incremented one pass the escape sequence, so step back
				end--;
				break;
			}
		}

		tok.literal_as_string += c;
	}

	tok.id = tokenid::string_literal;
	tok.length = end - begin + 1;

	// Free up unused memory
	tok.literal_as_string.shrink_to_fit();
}
void reshadefx::lexer::parse_numeric_literal(token &tok) const
{
	// This routine handles both integer and floating point numbers
	auto *const begin = _cur, *end = _cur;
	int mantissa_size = 0, decimal_location = -1, radix = 10;
	long long fraction = 0, exponent = 0;

	// If a literal starts with '0' it is either an octal or hexadecimal ('0x') value
	if (begin[0] == '0')
	{
		if (begin[1] == 'x' || begin[1] == 'X')
		{
			end = begin + 2;
			radix = 16;
		}
		else
		{
			radix = 8;
		}
	}

	for (; mantissa_size <= 18; mantissa_size++, end++)
	{
		auto c = *end;

		if (is_decimal_digit(c))
		{
			c -= '0';

			if (c >= radix)
				break;
		}
		else if (radix == 16)
		{
			// Hexadecimal values can contain the letters A to F
			if (c >= 'A' && c <= 'F')
				c -= 'A' - 10;
			else if (c >= 'a' && c <= 'f')
				c -= 'a' - 10;
			else
				break;
		}
		else
		{
			if (c != '.' || decimal_location >= 0)
				break;

			// Found a decimal character, as such convert current values
			if (radix == 8)
			{
				radix = 10;
				fraction = octal_to_decimal(fraction);
			}

			decimal_location = mantissa_size;
			continue;
		}

		fraction *= radix;
		fraction += c;
	}

	// Ignore additional digits that cannot affect the value
	while (is_digit(*end, radix))
		end++;

	// If a decimal character was found, this is a floating point value, otherwise an integer one
	if (decimal_location < 0)
	{
		tok.id = tokenid::int_literal;
		decimal_location = mantissa_size;
	}
	else
	{
		tok.id = tokenid::float_literal;
		mantissa_size -= 1;
	}

	// Literals can be followed by an exponent
	if (*end == 'E' || *end == 'e')
	{
		auto tmp = end + 1;
		const bool negative = *tmp == '-';

		if (negative || *tmp == '+')
			tmp++;

		if (is_decimal_digit(*tmp))
		{
			end = tmp;

			tok.id = tokenid::float_literal;

			do {
				exponent *= 10;
				exponent += (*end++) - '0';
			} while (is_decimal_digit(*end));

			if (negative)
				exponent = -exponent;
		}
	}

	// Various suffixes force specific literal types
	if (*end == 'F' || *end == 'f')
	{
		end++; // Consume the suffix
		tok.id = tokenid::float_literal;
	}
	else if (*end == 'L' || *end == 'l')
	{
		end++; // Consume the suffix
		tok.id = tokenid::double_literal;
	}
	else if (tok.id == tokenid::int_literal && (*end == 'U' || *end == 'u')) // The 'u' suffix is only valid on integers and needs to be ignored otherwise
	{
		end++; // Consume the suffix
		tok.id = tokenid::uint_literal;
	}

	if (tok.id == tokenid::float_literal || tok.id == tokenid::double_literal)
	{
		exponent += decimal_location - mantissa_size;

		const bool exponent_negative = exponent < 0;

		if (exponent_negative)
			exponent = -exponent;

		// Limit exponent
		if (exponent > 511)
			exponent = 511;

		// Quick exponent calculation
		double e = 1.0;
		const double powers_of_10[] = {
			10.,
			100.,
			1.0e4,
			1.0e8,
			1.0e16,
			1.0e32,
			1.0e64,
			1.0e128,
			1.0e256
		};

		for (auto d = powers_of_10; exponent != 0; exponent >>= 1, d++)
			if (exponent & 1)
				e *= *d;

		if (tok.id == tokenid::float_literal)
			tok.literal_as_float = exponent_negative ? fraction / static_cast<float>(e) : fraction * static_cast<float>(e);
		else
			tok.literal_as_double = exponent_negative ? fraction / e : fraction * e;
	}
	else
	{
		// Limit the maximum value to what fits into our token structure
		tok.literal_as_uint = static_cast<unsigned int>(fraction & 0xFFFFFFFF);
	}

	tok.length = end - begin;
}
