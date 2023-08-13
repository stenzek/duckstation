/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "effect_module.hpp"
#include <unordered_map> // Used for symbol lookup table

namespace reshadefx
{
	/// <summary>
	/// A scope encapsulating symbols.
	/// </summary>
	struct scope
	{
		std::string name;
		uint32_t level, namespace_level;
	};

	/// <summary>
	/// Enumeration of all possible symbol types.
	/// </summary>
	enum class symbol_type
	{
		invalid,
		variable,
		constant,
		function,
		intrinsic,
		structure,
	};

	/// <summary>
	/// A single symbol in the symbol table.
	/// </summary>
	struct symbol
	{
		symbol_type op = symbol_type::invalid;
		uint32_t id = 0;
		reshadefx::type type = {};
		reshadefx::constant constant = {};
		const reshadefx::function_info *function = nullptr;
	};
	struct scoped_symbol : symbol
	{
		struct scope scope; // Store scope together with symbol data
	};

	/// <summary>
	/// A symbol table managing a list of scopes and symbols.
	/// </summary>
	class symbol_table
	{
	public:
		symbol_table();

		/// <summary>
		/// Enters a new scope as child of the current one.
		/// </summary>
		void enter_scope();
		/// <summary>
		/// Enters a new namespace as child of the current one.
		/// </summary>
		void enter_namespace(const std::string &name);
		/// <summary>
		/// Leaves the current scope and enter the parent one.
		/// </summary>
		void leave_scope();
		/// <summary>
		/// Leaves the current namespace and enter the parent one.
		/// </summary>
		void leave_namespace();

		/// <summary>
		/// Gets the current scope the symbol table operates in.
		/// </summary>
		const scope &current_scope() const { return _current_scope; }

		/// <summary>
		/// Inserts an new symbol in the symbol table.
		/// Returns <see langword="false"/> if a symbol by that name and type already exists.
		/// </summary>
		bool insert_symbol(const std::string &name, const symbol &symbol, bool global = false);

		/// <summary>
		/// Looks for an existing symbol with the specified <paramref name="name"/>.
		/// </summary>
		scoped_symbol find_symbol(const std::string &name) const;
		scoped_symbol find_symbol(const std::string &name, const scope &scope, bool exclusive) const;

		/// <summary>
		/// Searches for the best function or intrinsic overload matching the argument list.
		/// </summary>
		bool resolve_function_call(const std::string &name, const std::vector<expression> &args, const scope &scope, symbol &data, bool &ambiguous) const;

	private:
		scope _current_scope;
		// Lookup table from name to matching symbols
		std::unordered_map<std::string, std::vector<scoped_symbol>> _symbol_stack;
	};
}
