/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "effect_token.hpp"

namespace reshadefx
{
	/// <summary>
	/// A lexical analyzer for C-like languages.
	/// </summary>
	class lexer
	{
	public:
		explicit lexer(
			std::string input,
			bool ignore_comments = true,
			bool ignore_whitespace = true,
			bool ignore_pp_directives = true,
			bool ignore_line_directives = false,
			bool ignore_keywords = false,
			bool escape_string_literals = true,
			const location &start_location = location()) :
			_input(std::move(input)),
			_cur_location(start_location),
			_ignore_comments(ignore_comments),
			_ignore_whitespace(ignore_whitespace),
			_ignore_pp_directives(ignore_pp_directives),
			_ignore_line_directives(ignore_line_directives),
			_ignore_keywords(ignore_keywords),
			_escape_string_literals(escape_string_literals)
		{
			_cur = _input.data();
			_end = _cur + _input.size();
		}

		lexer(const lexer &lexer) { operator=(lexer); }
		lexer &operator=(const lexer &lexer)
		{
			_input = lexer._input;
			_cur_location = lexer._cur_location;
			reset_to_offset(lexer._cur - lexer._input.data());
			_end = _input.data() + _input.size();
			_ignore_comments = lexer._ignore_comments;
			_ignore_whitespace = lexer._ignore_whitespace;
			_ignore_pp_directives = lexer._ignore_pp_directives;
			_ignore_keywords = lexer._ignore_keywords;
			_escape_string_literals = lexer._escape_string_literals;
			_ignore_line_directives = lexer._ignore_line_directives;

			return *this;
		}

		/// <summary>
		/// Gets the current position in the input string.
		/// </summary>
		size_t input_offset() const { return _cur - _input.data(); }

		/// <summary>
		/// Gets the input string this lexical analyzer works on.
		/// </summary>
		/// <returns>Constant reference to the input string.</returns>
		const std::string &input_string() const { return _input; }

		/// <summary>
		/// Performs lexical analysis on the input string and return the next token in sequence.
		/// </summary>
		/// <returns>Next token from the input string.</returns>
		token lex();

		/// <summary>
		/// Advances to the next token that is not whitespace.
		/// </summary>
		void skip_space();
		/// <summary>
		/// Advances to the next new line, ignoring all tokens.
		/// </summary>
		void skip_to_next_line();

		/// <summary>
		/// Resets position to the specified <paramref name="offset"/>.
		/// </summary>
		/// <param name="offset">Offset in characters from the start of the input string.</param>
		void reset_to_offset(size_t offset);

	private:
		/// <summary>
		/// Skips an arbitrary amount of characters in the input string.
		/// </summary>
		/// <param name="length">Number of input characters to skip.</param>
		void skip(size_t length);

		void parse_identifier(token &tok) const;
		bool parse_pp_directive(token &tok);
		void parse_string_literal(token &tok, bool escape);
		void parse_numeric_literal(token &tok) const;

		std::string _input;
		location _cur_location;
		const std::string::value_type *_cur, *_end;

		bool _ignore_comments;
		bool _ignore_whitespace;
		bool _ignore_pp_directives;
		bool _ignore_line_directives;
		bool _ignore_keywords;
		bool _escape_string_literals;
	};
}
