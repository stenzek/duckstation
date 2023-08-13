/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "effect_token.hpp"
#include <memory> // std::unique_ptr
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace reshadefx
{
	/// <summary>
	/// A C-style preprocessor implementation.
	/// </summary>
	class preprocessor
	{
	public:
		struct macro
		{
			std::string replacement_list;
			std::vector<std::string> parameters;
			bool is_predefined = false;
			bool is_variadic = false;
			bool is_function_like = false;
		};

		// Define constructor explicitly because lexer class is not included here
		preprocessor();
		~preprocessor();

		/// <summary>
		/// Adds an include directory to the list of search paths used when resolving #include directives.
		/// </summary>
		/// <param name="path">Path to the directory to add.</param>
		void add_include_path(const std::filesystem::path &path);

		/// <summary>
		/// Adds a new macro definition. This is equal to appending '#define name macro' to this preprocessor instance.
		/// </summary>
		/// <param name="name">Name of the macro to define.</param>
		/// <param name="macro">Definition of the macro function or value.</param>
		/// <returns></returns>
		bool add_macro_definition(const std::string &name, const macro &macro);
		/// <summary>
		/// Adds a new macro value definition. This is equal to appending '#define name macro' to this preprocessor instance.
		/// </summary>
		/// <param name="name">Name of the macro to define.</param>
		/// <param name="value">Value to define that macro to.</param>
		/// <returns></returns>
		bool add_macro_definition(const std::string &name, std::string value = "1")
		{
			return add_macro_definition(name, macro { std::move(value), {}, true });
		}

		/// <summary>
		/// Opens the specified file, parses its contents and appends them to the output.
		/// </summary>
		/// <param name="path">Path to the file to parse.</param>
		/// <returns><see langword="true"/> if parsing was successful, <see langword="false"/> otherwise.</returns>
		bool append_file(const std::filesystem::path &path);
		/// <summary>
		/// Parses the specified string and appends it to the output.
		/// </summary>
		/// <param name="source_code">String to parse.</param>
		/// <param name="path">Optional file path to identify this string with.</param>
		/// <returns><see langword="true"/> if parsing was successful, <see langword="false"/> otherwise.</returns>
		bool append_string(std::string source_code, const std::filesystem::path &path = std::filesystem::path());

		/// <summary>
		/// Gets the list of error messages.
		/// </summary>
		const std::string &errors() const { return _errors; }
		/// <summary>
		/// Gets the current pre-processed output string.
		/// </summary>
		const std::string &output() const { return _output; }

		/// <summary>
		/// Gets a list of all included files.
		/// </summary>
		std::vector<std::filesystem::path> included_files() const;

		/// <summary>
		/// Gets a list of all defines that were used in #ifdef and #ifndef lines.
		/// </summary>
		std::vector<std::pair<std::string, std::string>> used_macro_definitions() const;

		/// <summary>
		/// Gets a list of pragma directives that occured.
		/// </summary>
		std::vector<std::pair<std::string, std::string>> used_pragma_directives() const { return _used_pragmas; }

	private:
		struct if_level
		{
			bool value;
			bool skipping;
			token pp_token;
			size_t input_index;
		};
		struct input_level
		{
			std::string name;
			std::unique_ptr<class lexer> lexer;
			token next_token;
			std::unordered_set<std::string> hidden_macros;
		};

		void error(const location &location, const std::string &message);
		void warning(const location &location, const std::string &message);

		void push(std::string input, const std::string &name = std::string());

		bool peek(tokenid tokid) const;
		void consume();
		void consume_until(tokenid tokid);
		bool accept(tokenid tokid, bool ignore_whitespace = true);
		bool expect(tokenid tokid);

		void parse();
		void parse_def();
		void parse_undef();
		void parse_if();
		void parse_ifdef();
		void parse_ifndef();
		void parse_elif();
		void parse_else();
		void parse_endif();
		void parse_error();
		void parse_warning();
		void parse_pragma();
		void parse_include();

		bool evaluate_expression();
		bool evaluate_identifier_as_macro();

		bool is_defined(const std::string &name) const;
		void expand_macro(const std::string &name, const macro &macro, const std::vector<std::string> &arguments);
		void create_macro_replacement_list(macro &macro);

		bool _success = true;
		std::string _output, _errors;

		std::string _current_token_raw_data;
		reshadefx::token _token;
		location _output_location;
		std::vector<input_level> _input_stack;
		size_t _next_input_index = 0;
		size_t _current_input_index = 0;

		std::vector<if_level> _if_stack;

		unsigned short _recursion_count = 0;
		std::unordered_set<std::string> _used_macros;
		std::unordered_map<std::string, macro> _macros;

		std::vector<std::filesystem::path> _include_paths;
		std::unordered_map<std::string, std::string> _file_cache;

		std::vector<std::pair<std::string, std::string>> _used_pragmas;
	};
}
