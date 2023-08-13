/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "effect_lexer.hpp"
#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include <cctype> // std::toupper
#include <cassert>
#include <functional>
#include <string_view>

struct on_scope_exit
{
	template <typename F>
	explicit on_scope_exit(F lambda) : leave(lambda) { }
	~on_scope_exit() { leave(); }

	std::function<void()> leave;
};

bool reshadefx::parser::parse(std::string input, codegen *backend)
{
	_lexer.reset(new lexer(std::move(input)));

	// Set backend for subsequent code-generation
	_codegen = backend;
	assert(backend != nullptr);

	consume();

	bool parse_success = true;
	bool current_success = true;

	while (!peek(tokenid::end_of_file))
	{
		parse_top(current_success);
		if (!current_success)
			parse_success = false;
	}

	return parse_success;
}

void reshadefx::parser::parse_top(bool &parse_success)
{
	if (accept(tokenid::namespace_))
	{
		// Anonymous namespaces are not supported right now, so an identifier is a must
		if (!expect(tokenid::identifier))
		{
			parse_success = false;
			return;
		}

		const auto name = std::move(_token.literal_as_string);

		if (!expect('{'))
		{
			parse_success = false;
			return;
		}

		enter_namespace(name);

		bool current_success = true;
		bool parse_success_namespace = true;

		// Recursively parse top level statements until the namespace is closed again
		while (!peek('}')) // Empty namespaces are valid
		{
			parse_top(current_success);
			if (!current_success)
				parse_success_namespace = false;
		}

		leave_namespace();

		parse_success = expect('}') && parse_success_namespace;
	}
	else if (accept(tokenid::struct_)) // Structure keyword found, parse the structure definition
	{
		// Structure definitions are terminated with a semicolon
		parse_success = parse_struct() && expect(';');
	}
	else if (accept(tokenid::technique)) // Technique keyword found, parse the technique definition
	{
		parse_success = parse_technique();
	}
	else
	{
		if (type type; parse_type(type)) // Type found, this can be either a variable or a function declaration
		{
			parse_success = expect(tokenid::identifier);
			if (!parse_success)
				return;

			if (peek('('))
			{
				const auto name = std::move(_token.literal_as_string);
				// This is definitely a function declaration, so parse it
				if (!parse_function(type, name))
				{
					// Insert dummy function into symbol table, so later references can be resolved despite the error
					insert_symbol(name, { symbol_type::function, ~0u, { type::t_function } }, true);
					parse_success = false;
					return;
				}
			}
			else
			{
				// There may be multiple variable names after the type, handle them all
				unsigned int count = 0;
				do {
					if (count++ > 0 && !(expect(',') && expect(tokenid::identifier)))
					{
						parse_success = false;
						return;
					}
					const auto name = std::move(_token.literal_as_string);
					if (!parse_variable(type, name, true))
					{
						// Insert dummy variable into symbol table, so later references can be resolved despite the error
						insert_symbol(name, { symbol_type::variable, ~0u, type }, true);
						// Skip the rest of the statement
						consume_until(';');
						parse_success = false;
						return;
					}
				} while (!peek(';'));

				// Variable declarations are terminated with a semicolon
				parse_success = expect(';');
			}
		}
		else if (accept(';')) // Ignore single semicolons in the source
		{
			parse_success = true;
		}
		else
		{
			// Unexpected token in source stream, consume and report an error about it
			consume();
			// Only add another error message if succeeded parsing previously
			// This is done to avoid walls of error messages because of consequential errors following a top-level syntax mistake
			if (parse_success)
				error(_token.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token.id) + '\'');
			parse_success = false;
		}
	}
}

bool reshadefx::parser::parse_statement(bool scoped)
{
	if (!_codegen->is_in_block())
		return error(_token_next.location, 0, "unreachable code"), false;

	unsigned int loop_control = 0;
	unsigned int selection_control = 0;

	// Read any loop and branch control attributes first
	while (accept('['))
	{
		enum control_mask
		{
			unroll = 0x1,
			dont_unroll = 0x2,
			flatten = (0x1 << 4),
			dont_flatten = (0x2 << 4),
			switch_force_case = (0x4 << 4),
			switch_call = (0x8 << 4)
		};

		const auto attribute = std::move(_token_next.literal_as_string);

		if (!expect(tokenid::identifier) || !expect(']'))
			return false;

		if (attribute == "unroll")
			loop_control |= unroll;
		else if (attribute == "loop" || attribute == "fastopt")
			loop_control |= dont_unroll;
		else if (attribute == "flatten")
			selection_control |= flatten;
		else if (attribute == "branch")
			selection_control |= dont_flatten;
		else if (attribute == "forcecase")
			selection_control |= switch_force_case;
		else if (attribute == "call")
			selection_control |= switch_call;
		else
			warning(_token.location, 0, "unknown attribute");

		if ((loop_control & (unroll | dont_unroll)) == (unroll | dont_unroll))
			return error(_token.location, 3524, "can't use loop and unroll attributes together"), false;
		if ((selection_control & (flatten | dont_flatten)) == (flatten | dont_flatten))
			return error(_token.location, 3524, "can't use branch and flatten attributes together"), false;
	}

	// Shift by two so that the possible values are 0x01 for 'flatten' and 0x02 for 'dont_flatten', equivalent to 'unroll' and 'dont_unroll'
	selection_control >>= 4;

	if (peek('{')) // Parse statement block
		return parse_statement_block(scoped);
	else if (accept(';')) // Ignore empty statements
		return true;

	// Most statements with the exception of declarations are only valid inside functions
	if (_codegen->is_in_function())
	{
		assert(_current_function != nullptr);

		const auto location = _token_next.location;

		if (accept(tokenid::if_))
		{
			codegen::id true_block = _codegen->create_block(); // Block which contains the statements executed when the condition is true
			codegen::id false_block = _codegen->create_block(); // Block which contains the statements executed when the condition is false
			const codegen::id merge_block = _codegen->create_block(); // Block that is executed after the branch re-merged with the current control flow

			expression condition;
			if (!expect('(') || !parse_expression(condition) || !expect(')'))
				return false;
			else if (!condition.type.is_scalar())
				return error(condition.location, 3019, "if statement conditional expressions must evaluate to a scalar"), false;

			// Load condition and convert to boolean value as required by 'OpBranchConditional'
			condition.add_cast_operation({ type::t_bool, 1, 1 });

			const codegen::id condition_value = _codegen->emit_load(condition);
			const codegen::id condition_block = _codegen->leave_block_and_branch_conditional(condition_value, true_block, false_block);

			{ // Then block of the if statement
				_codegen->enter_block(true_block);

				if (!parse_statement(true))
					return false;

				true_block = _codegen->leave_block_and_branch(merge_block);
			}
			{ // Else block of the if statement
				_codegen->enter_block(false_block);

				if (accept(tokenid::else_) && !parse_statement(true))
					return false;

				false_block = _codegen->leave_block_and_branch(merge_block);
			}

			_codegen->enter_block(merge_block);

			// Emit structured control flow for an if statement and connect all basic blocks
			_codegen->emit_if(location, condition_value, condition_block, true_block, false_block, selection_control);

			return true;
		}

		if (accept(tokenid::switch_))
		{
			const codegen::id merge_block = _codegen->create_block(); // Block that is executed after the switch re-merged with the current control flow

			expression selector_exp;
			if (!expect('(') || !parse_expression(selector_exp) || !expect(')'))
				return false;
			else if (!selector_exp.type.is_scalar())
				return error(selector_exp.location, 3019, "switch statement expression must evaluate to a scalar"), false;

			// Load selector and convert to integral value as required by switch instruction
			selector_exp.add_cast_operation({ type::t_int, 1, 1 });

			const auto selector_value = _codegen->emit_load(selector_exp);
			const auto selector_block = _codegen->leave_block_and_switch(selector_value, merge_block);

			if (!expect('{'))
				return false;

			_loop_break_target_stack.push_back(merge_block);
			on_scope_exit _([this]() { _loop_break_target_stack.pop_back(); });

			bool parse_success = true;
			// The default case jumps to the end of the switch statement if not overwritten
			codegen::id default_label = merge_block, default_block = merge_block;
			codegen::id current_label = _codegen->create_block();
			std::vector<codegen::id> case_literal_and_labels, case_blocks;
			size_t last_case_label_index = 0;

			// Enter first switch statement body block
			_codegen->enter_block(current_label);

			while (!peek(tokenid::end_of_file))
			{
				while (accept(tokenid::case_) || accept(tokenid::default_))
				{
					if (_token.id == tokenid::case_)
					{
						expression case_label;
						if (!parse_expression(case_label))
							return consume_until('}'), false;
						else if (!case_label.type.is_scalar() || !case_label.type.is_integral() || !case_label.is_constant)
							return error(case_label.location, 3020, "invalid type for case expression - value must be an integer scalar"), consume_until('}'), false;

						// Check for duplicate case values
						for (size_t i = 0; i < case_literal_and_labels.size(); i += 2)
						{
							if (case_literal_and_labels[i] == case_label.constant.as_uint[0])
							{
								parse_success = false;
								error(case_label.location, 3532, "duplicate case " + std::to_string(case_label.constant.as_uint[0]));
								break;
							}
						}

						case_blocks.emplace_back(); // This is set to the actual block below
						case_literal_and_labels.push_back(case_label.constant.as_uint[0]);
						case_literal_and_labels.push_back(current_label);
					}
					else
					{
						// Check if the default label was already changed by a previous 'default' statement
						if (default_label != merge_block)
						{
							parse_success = false;
							error(_token.location, 3532, "duplicate default in switch statement");
						}

						default_label = current_label;
						default_block = 0; // This is set to the actual block below
					}

					if (!expect(':'))
						return consume_until('}'), false;
				}

				// It is valid for no statement to follow if this is the last label in the switch body
				const bool end_of_switch = peek('}');

				if (!end_of_switch && !parse_statement(true))
					return consume_until('}'), false;

				// Handle fall-through case and end of switch statement
				if (peek(tokenid::case_) || peek(tokenid::default_) || end_of_switch)
				{
					if (_codegen->is_in_block()) // Disallow fall-through for now
					{
						parse_success = false;
						error(_token_next.location, 3533, "non-empty case statements must have break or return");
					}

					const codegen::id next_label = end_of_switch ? merge_block : _codegen->create_block();
					// This is different from 'current_label', since there may have been branching logic inside the case, which would have changed the active block
					const codegen::id current_block = _codegen->leave_block_and_branch(next_label);

					if (0 == default_block)
						default_block = current_block;
					for (size_t i = last_case_label_index; i < case_blocks.size(); ++i)
						// Need to use the initial label for the switch table, but the current block to merge all the block data
						case_blocks[i] = current_block;

					current_label = next_label;
					_codegen->enter_block(current_label);

					if (end_of_switch) // We reached the end, nothing more to do
						break;

					last_case_label_index = case_blocks.size();
				}
			}

			if (case_literal_and_labels.empty() && default_label == merge_block)
				warning(location, 5002, "switch statement contains no 'case' or 'default' labels");

			// Emit structured control flow for a switch statement and connect all basic blocks
			_codegen->emit_switch(location, selector_value, selector_block, default_label, default_block, case_literal_and_labels, case_blocks, selection_control);

			return expect('}') && parse_success;
		}

		if (accept(tokenid::for_))
		{
			if (!expect('('))
				return false;

			enter_scope();
			on_scope_exit _([this]() { leave_scope(); });

			// Parse initializer first
			if (type type; parse_type(type))
			{
				unsigned int count = 0;
				do { // There may be multiple declarations behind a type, so loop through them
					if (count++ > 0 && !expect(','))
						return false;
					if (!expect(tokenid::identifier) || !parse_variable(type, std::move(_token.literal_as_string)))
						return false;
				} while (!peek(';'));
			}
			else
			{
				// Initializer can also contain an expression if not a variable declaration list and not empty
				if (!peek(';'))
				{
					expression expression;
					if (!parse_expression(expression))
						return false;
				}
			}

			if (!expect(';'))
				return false;

			const codegen::id merge_block = _codegen->create_block(); // Block that is executed after the loop
			const codegen::id header_label = _codegen->create_block(); // Pointer to the loop merge instruction
			const codegen::id continue_label = _codegen->create_block(); // Pointer to the continue block
			codegen::id loop_block = _codegen->create_block(); // Pointer to the main loop body block
			codegen::id condition_block = _codegen->create_block(); // Pointer to the condition check
			codegen::id condition_value = 0;

			// End current block by branching to the next label
			const codegen::id prev_block = _codegen->leave_block_and_branch(header_label);

			{ // Begin loop block (this header is used for explicit structured control flow)
				_codegen->enter_block(header_label);

				_codegen->leave_block_and_branch(condition_block);
			}

			{ // Parse condition block
				_codegen->enter_block(condition_block);

				if (!peek(';'))
				{
					expression condition;
					if (!parse_expression(condition))
						return false;

					if (!condition.type.is_scalar())
						return error(condition.location, 3019, "scalar value expected"), false;

					// Evaluate condition and branch to the right target
					condition.add_cast_operation({ type::t_bool, 1, 1 });

					condition_value = _codegen->emit_load(condition);

					condition_block = _codegen->leave_block_and_branch_conditional(condition_value, loop_block, merge_block);
				}
				else // It is valid for there to be no condition expression
				{
					condition_block = _codegen->leave_block_and_branch(loop_block);
				}

				if (!expect(';'))
					return false;
			}

			{ // Parse loop continue block into separate block so it can be appended to the end down the line
				_codegen->enter_block(continue_label);

				if (!peek(')'))
				{
					expression continue_exp;
					if (!parse_expression(continue_exp))
						return false;
				}

				if (!expect(')'))
					return false;

				// Branch back to the loop header at the end of the continue block
				_codegen->leave_block_and_branch(header_label);
			}

			{ // Parse loop body block
				_codegen->enter_block(loop_block);

				_loop_break_target_stack.push_back(merge_block);
				_loop_continue_target_stack.push_back(continue_label);

				const bool parse_success = parse_statement(false);

				_loop_break_target_stack.pop_back();
				_loop_continue_target_stack.pop_back();

				if (!parse_success)
					return false;

				loop_block = _codegen->leave_block_and_branch(continue_label);
			}

			// Add merge block label to the end of the loop
			_codegen->enter_block(merge_block);

			// Emit structured control flow for a loop statement and connect all basic blocks
			_codegen->emit_loop(location, condition_value, prev_block, header_label, condition_block, loop_block, continue_label, loop_control);

			return true;
		}

		if (accept(tokenid::while_))
		{
			enter_scope();
			on_scope_exit _([this]() { leave_scope(); });

			const codegen::id merge_block = _codegen->create_block();
			const codegen::id header_label = _codegen->create_block();
			const codegen::id continue_label = _codegen->create_block();
			codegen::id loop_block = _codegen->create_block();
			codegen::id condition_block = _codegen->create_block();
			codegen::id condition_value = 0;

			// End current block by branching to the next label
			const codegen::id prev_block = _codegen->leave_block_and_branch(header_label);

			{ // Begin loop block
				_codegen->enter_block(header_label);

				_codegen->leave_block_and_branch(condition_block);
			}

			{ // Parse condition block
				_codegen->enter_block(condition_block);

				expression condition;
				if (!expect('(') || !parse_expression(condition) || !expect(')'))
					return false;
				else if (!condition.type.is_scalar())
					return error(condition.location, 3019, "scalar value expected"), false;

				// Evaluate condition and branch to the right target
				condition.add_cast_operation({ type::t_bool, 1, 1 });

				condition_value = _codegen->emit_load(condition);

				condition_block = _codegen->leave_block_and_branch_conditional(condition_value, loop_block, merge_block);
			}

			{ // Parse loop body block
				_codegen->enter_block(loop_block);

				_loop_break_target_stack.push_back(merge_block);
				_loop_continue_target_stack.push_back(continue_label);

				const bool parse_success = parse_statement(false);

				_loop_break_target_stack.pop_back();
				_loop_continue_target_stack.pop_back();

				if (!parse_success)
					return false;

				loop_block = _codegen->leave_block_and_branch(continue_label);
			}

			{ // Branch back to the loop header in empty continue block
				_codegen->enter_block(continue_label);

				_codegen->leave_block_and_branch(header_label);
			}

			// Add merge block label to the end of the loop
			_codegen->enter_block(merge_block);

			// Emit structured control flow for a loop statement and connect all basic blocks
			_codegen->emit_loop(location, condition_value, prev_block, header_label, condition_block, loop_block, continue_label, loop_control);

			return true;
		}

		if (accept(tokenid::do_))
		{
			const codegen::id merge_block = _codegen->create_block();
			const codegen::id header_label = _codegen->create_block();
			const codegen::id continue_label = _codegen->create_block();
			codegen::id loop_block = _codegen->create_block();
			codegen::id condition_value = 0;

			// End current block by branching to the next label
			const codegen::id prev_block = _codegen->leave_block_and_branch(header_label);

			{ // Begin loop block
				_codegen->enter_block(header_label);

				_codegen->leave_block_and_branch(loop_block);
			}

			{ // Parse loop body block
				_codegen->enter_block(loop_block);

				_loop_break_target_stack.push_back(merge_block);
				_loop_continue_target_stack.push_back(continue_label);

				const bool parse_success = parse_statement(true);

				_loop_break_target_stack.pop_back();
				_loop_continue_target_stack.pop_back();

				if (!parse_success)
					return false;

				loop_block = _codegen->leave_block_and_branch(continue_label);
			}

			{ // Continue block does the condition evaluation
				_codegen->enter_block(continue_label);

				expression condition;
				if (!expect(tokenid::while_) || !expect('(') || !parse_expression(condition) || !expect(')') || !expect(';'))
					return false;
				else if (!condition.type.is_scalar())
					return error(condition.location, 3019, "scalar value expected"), false;

				// Evaluate condition and branch to the right target
				condition.add_cast_operation({ type::t_bool, 1, 1 });

				condition_value = _codegen->emit_load(condition);

				_codegen->leave_block_and_branch_conditional(condition_value, header_label, merge_block);
			}

			// Add merge block label to the end of the loop
			_codegen->enter_block(merge_block);

			// Emit structured control flow for a loop statement and connect all basic blocks
			_codegen->emit_loop(location, condition_value, prev_block, header_label, 0, loop_block, continue_label, loop_control);

			return true;
		}

		if (accept(tokenid::break_))
		{
			if (_loop_break_target_stack.empty())
				return error(location, 3518, "break must be inside loop"), false;

			// Branch to the break target of the inner most loop on the stack
			_codegen->leave_block_and_branch(_loop_break_target_stack.back(), 1);

			return expect(';');
		}

		if (accept(tokenid::continue_))
		{
			if (_loop_continue_target_stack.empty())
				return error(location, 3519, "continue must be inside loop"), false;

			// Branch to the continue target of the inner most loop on the stack
			_codegen->leave_block_and_branch(_loop_continue_target_stack.back(), 2);

			return expect(';');
		}

		if (accept(tokenid::return_))
		{
			const type &ret_type = _current_function->return_type;

			if (!peek(';'))
			{
				expression expression;
				if (!parse_expression(expression))
					return consume_until(';'), false;

				// Cannot return to void
				if (ret_type.is_void())
					// Consume the semicolon that follows the return expression so that parsing may continue
					return error(location, 3079, "void functions cannot return a value"), accept(';'), false;

				// Cannot return arrays from a function
				if (expression.type.is_array() || !type::rank(expression.type, ret_type))
					return error(location, 3017, "expression (" + expression.type.description() + ") does not match function return type (" + ret_type.description() + ')'), accept(';'), false;

				// Load return value and perform implicit cast to function return type
				if (expression.type.components() > ret_type.components())
					warning(expression.location, 3206, "implicit truncation of vector type");

				expression.add_cast_operation(ret_type);

				const auto return_value = _codegen->emit_load(expression);

				_codegen->leave_block_and_return(return_value);
			}
			else if (!ret_type.is_void())
			{
				// No return value was found, but the function expects one
				error(location, 3080, "function must return a value");

				// Consume the semicolon that follows the return expression so that parsing may continue
				accept(';');

				return false;
			}
			else
			{
				_codegen->leave_block_and_return();
			}

			return expect(';');
		}

		if (accept(tokenid::discard_))
		{
			// Leave the current function block
			_codegen->leave_block_and_kill();

			return expect(';');
		}
	}

	// Handle variable declarations
	if (type type; parse_type(type))
	{
		unsigned int count = 0;
		do { // There may be multiple declarations behind a type, so loop through them
			if (count++ > 0 && !expect(','))
				// Try to consume the rest of the declaration so that parsing may continue despite the error
				return consume_until(';'), false;
			if (!expect(tokenid::identifier) || !parse_variable(type, std::move(_token.literal_as_string)))
				return consume_until(';'), false;
		} while (!peek(';'));

		return expect(';');
	}

	// Handle expression statements
	if (expression expression; parse_expression(expression))
		return expect(';'); // A statement has to be terminated with a semicolon

	// Gracefully consume any remaining characters until the statement would usually end, so that parsing may continue despite the error
	consume_until(';');

	return false;
}
bool reshadefx::parser::parse_statement_block(bool scoped)
{
	if (!expect('{'))
		return false;

	if (scoped)
		enter_scope();

	// Parse statements until the end of the block is reached
	while (!peek('}') && !peek(tokenid::end_of_file))
	{
		if (!parse_statement(true))
		{
			if (scoped)
				leave_scope();

			// Ignore the rest of this block
			unsigned int level = 0;

			while (!peek(tokenid::end_of_file))
			{
				if (accept('{'))
				{
					++level;
				}
				else if (accept('}'))
				{
					if (level-- == 0)
						break;
				} // These braces are necessary to match the 'else' to the correct 'if' statement
				else
				{
					consume();
				}
			}

			return false;
		}
	}

	if (scoped)
		leave_scope();

	return expect('}');
}

bool reshadefx::parser::parse_type(type &type)
{
	type.qualifiers = 0;

	accept_type_qualifiers(type);

	if (!accept_type_class(type))
		return false;

	if (type.is_integral() && (type.has(type::q_centroid) || type.has(type::q_noperspective)))
		return error(_token.location, 4576, "signature specifies invalid interpolation mode for integer component type"), false;
	else if (type.has(type::q_centroid) && !type.has(type::q_noperspective))
		type.qualifiers |= type::q_linear;

	return true;
}
bool reshadefx::parser::parse_array_size(type &type)
{
	// Reset array length to zero before checking if one exists
	type.array_length = 0;

	if (accept('['))
	{
		if (accept(']'))
		{
			// No length expression, so this is an unsized array
			type.array_length = -1;
		}
		else if (expression expression; parse_expression(expression) && expect(']'))
		{
			if (!expression.is_constant || !(expression.type.is_scalar() && expression.type.is_integral()))
				return error(expression.location, 3058, "array dimensions must be literal scalar expressions"), false;

			type.array_length = expression.constant.as_uint[0];

			if (type.array_length < 1 || type.array_length > 65536)
				return error(expression.location, 3059, "array dimension must be between 1 and 65536"), false;
		}
		else
		{
			return false;
		}
	}

	// Multi-dimensional arrays are not supported
	if (peek('['))
		return error(_token_next.location, 3119, "arrays cannot be multi-dimensional"), false;

	return true;
}

bool reshadefx::parser::parse_annotations(std::vector<annotation> &annotations)
{
	// Check if annotations exist and return early if none do
	if (!accept('<'))
		return true;

	bool parse_success = true;

	while (!peek('>'))
	{
		if (type type; accept_type_class(type))
			warning(_token.location, 4717, "type prefixes for annotations are deprecated and ignored");

		if (!expect(tokenid::identifier))
			return consume_until('>'), false;

		auto name = std::move(_token.literal_as_string);

		if (expression expression; !expect('=') || !parse_expression_multary(expression) || !expect(';'))
			return consume_until('>'), false;
		else if (expression.is_constant)
			annotations.push_back({ expression.type, std::move(name), std::move(expression.constant) });
		else // Continue parsing annotations despite this not being a constant, since the syntax is still correct
			parse_success = false,
			error(expression.location, 3011, "value must be a literal expression");
	}

	return expect('>') && parse_success;
}

bool reshadefx::parser::parse_struct()
{
	const auto location = std::move(_token.location);

	struct_info info;
	// The structure name is optional
	if (accept(tokenid::identifier))
		info.name = std::move(_token.literal_as_string);
	else
		info.name = "_anonymous_struct_" + std::to_string(location.line) + '_' + std::to_string(location.column);

	info.unique_name = 'S' + current_scope().name + info.name;
	std::replace(info.unique_name.begin(), info.unique_name.end(), ':', '_');

	if (!expect('{'))
		return false;

	bool parse_success = true;

	while (!peek('}')) // Empty structures are possible
	{
		struct_member_info member;

		if (!parse_type(member.type))
			return error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + "', expected struct member type"), consume_until('}'), accept(';'), false;

		unsigned int count = 0;
		do {
			if (count++ > 0 && !expect(','))
				return consume_until('}'), accept(';'), false;

			if (!expect(tokenid::identifier))
				return consume_until('}'), accept(';'), false;

			member.name = std::move(_token.literal_as_string);
			member.location = std::move(_token.location);

			if (member.type.is_void())
				parse_success = false,
				error(member.location, 3038, '\'' + member.name + "': struct members cannot be void");
			if (member.type.is_struct()) // Nesting structures would make input/output argument flattening more complicated, so prevent it for now
				parse_success = false,
				error(member.location, 3090, '\'' + member.name + "': nested struct members are not supported");

			if (member.type.has(type::q_in) || member.type.has(type::q_out))
				parse_success = false,
				error(member.location, 3055, '\'' + member.name + "': struct members cannot be declared 'in' or 'out'");
			if (member.type.has(type::q_const))
				parse_success = false,
				error(member.location, 3035, '\'' + member.name + "': struct members cannot be declared 'const'");
			if (member.type.has(type::q_extern))
				parse_success = false,
				error(member.location, 3006, '\'' + member.name + "': struct members cannot be declared 'extern'");
			if (member.type.has(type::q_static))
				parse_success = false,
				error(member.location, 3007, '\'' + member.name + "': struct members cannot be declared 'static'");
			if (member.type.has(type::q_uniform))
				parse_success = false,
				error(member.location, 3047, '\'' + member.name + "': struct members cannot be declared 'uniform'");
			if (member.type.has(type::q_groupshared))
				parse_success = false,
				error(member.location, 3010, '\'' + member.name + "': struct members cannot be declared 'groupshared'");

			// Modify member specific type, so that following members in the declaration list are not affected by this
			if (!parse_array_size(member.type))
				return consume_until('}'), accept(';'), false;
			else if (member.type.array_length < 0)
				parse_success = false,
				error(member.location, 3072, '\'' + member.name + "': array dimensions of struct members must be explicit");

			// Structure members may have semantics to use them as input/output types
			if (accept(':'))
			{
				if (!expect(tokenid::identifier))
					return consume_until('}'), accept(';'), false;

				member.semantic = std::move(_token.literal_as_string);
				// Make semantic upper case to simplify comparison later on
				std::transform(member.semantic.begin(), member.semantic.end(), member.semantic.begin(),
					[](std::string::value_type c) {
						return static_cast<std::string::value_type>(std::toupper(c));
					});

				if (member.semantic.compare(0, 3, "SV_") != 0)
				{
					// Always numerate semantics, so that e.g. TEXCOORD and TEXCOORD0 point to the same location
					if (const char c = member.semantic.back(); c < '0' || c > '9')
						member.semantic += '0';

					if (member.type.is_integral() && !member.type.has(type::q_nointerpolation))
					{
						member.type.qualifiers |= type::q_nointerpolation; // Integer fields do not interpolate, so make this explicit (to avoid issues with GLSL)
						warning(member.location, 4568, '\'' + member.name + "': integer fields have the 'nointerpolation' qualifier by default");
					}
				}
				else
				{
					// Remove optional trailing zero from system value semantics, so that e.g. SV_POSITION and SV_POSITION0 mean the same thing
					if (member.semantic.back() == '0' && (member.semantic[member.semantic.size() - 2] < '0' || member.semantic[member.semantic.size() - 2] > '9'))
						member.semantic.pop_back();
				}
			}

			// Save member name and type for book keeping
			info.member_list.push_back(member);
		} while (!peek(';'));

		if (!expect(';'))
			return consume_until('}'), accept(';'), false;
	}

	// Empty structures are valid, but not usually intended, so emit a warning
	if (info.member_list.empty())
		warning(location, 5001, "struct has no members");

	// Define the structure now that information about all the member types was gathered
	const auto id = _codegen->define_struct(location, info);

	// Insert the symbol into the symbol table
	const symbol symbol = { symbol_type::structure, id };

	if (!insert_symbol(info.name, symbol, true))
		return error(location, 3003, "redefinition of '" + info.name + '\''), false;

	return expect('}') && parse_success;
}

bool reshadefx::parser::parse_function(type type, std::string name)
{
	const auto location = std::move(_token.location);

	if (!expect('(')) // Functions always have a parameter list
		return false;
	if (type.qualifiers != 0)
		return error(location, 3047, '\'' + name + "': function return type cannot have any qualifiers"), false;

	function_info info;
	info.name = name;
	info.unique_name = 'F' + current_scope().name + name;
	std::replace(info.unique_name.begin(), info.unique_name.end(), ':', '_');

	info.return_type = type;
	_current_function = &info;

	bool parse_success = true;
	bool expect_parenthesis = true;

	// Enter function scope (and leave it again when finished parsing this function)
	enter_scope();
	on_scope_exit _([this]() {
		leave_scope();
		_codegen->leave_function();
		_current_function = nullptr;
	});

	while (!peek(')'))
	{
		if (!info.parameter_list.empty() && !expect(','))
		{
			parse_success = false;
			expect_parenthesis = false;
			consume_until(')');
			break;
		}

		struct_member_info param;

		if (!parse_type(param.type))
		{
			error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + "', expected parameter type");
			parse_success = false;
			expect_parenthesis = false;
			consume_until(')');
			break;
		}

		if (!expect(tokenid::identifier))
		{
			parse_success = false;
			expect_parenthesis = false;
			consume_until(')');
			break;
		}

		param.name = std::move(_token.literal_as_string);
		param.location = std::move(_token.location);

		if (param.type.is_void())
			parse_success = false,
			error(param.location, 3038, '\'' + param.name + "': function parameters cannot be void");

		if (param.type.has(type::q_extern))
			parse_success = false,
			error(param.location, 3006, '\'' + param.name + "': function parameters cannot be declared 'extern'");
		if (param.type.has(type::q_static))
			parse_success = false,
			error(param.location, 3007, '\'' + param.name + "': function parameters cannot be declared 'static'");
		if (param.type.has(type::q_uniform))
			parse_success = false,
			error(param.location, 3047, '\'' + param.name + "': function parameters cannot be declared 'uniform', consider placing in global scope instead");
		if (param.type.has(type::q_groupshared))
			parse_success = false,
			error(param.location, 3010, '\'' + param.name + "': function parameters cannot be declared 'groupshared'");

		if (param.type.has(type::q_out) && param.type.has(type::q_const))
			parse_success = false,
			error(param.location, 3046, '\'' + param.name + "': output parameters cannot be declared 'const'");
		else if (!param.type.has(type::q_out))
			param.type.qualifiers |= type::q_in; // Function parameters are implicitly 'in' if not explicitly defined as 'out'

		if (!parse_array_size(param.type))
		{
			parse_success = false;
			expect_parenthesis = false;
			consume_until(')');
			break;
		}
		else if (param.type.array_length < 0)
		{
			parse_success = false;
			error(param.location, 3072, '\'' + param.name + "': array dimensions of function parameters must be explicit");
		}

		// Handle parameter type semantic
		if (accept(':'))
		{
			if (!expect(tokenid::identifier))
			{
				parse_success = false;
				expect_parenthesis = false;
				consume_until(')');
				break;
			}

			param.semantic = std::move(_token.literal_as_string);
			// Make semantic upper case to simplify comparison later on
			std::transform(param.semantic.begin(), param.semantic.end(), param.semantic.begin(),
				[](std::string::value_type c) {
					return static_cast<std::string::value_type>(std::toupper(c));
				});

			if (param.semantic.compare(0, 3, "SV_") != 0)
			{
				// Always numerate semantics, so that e.g. TEXCOORD and TEXCOORD0 point to the same location
				if (const char c = param.semantic.back(); c < '0' || c > '9')
					param.semantic += '0';

				if (param.type.is_integral() && !param.type.has(type::q_nointerpolation))
				{
					param.type.qualifiers |= type::q_nointerpolation; // Integer parameters do not interpolate, so make this explicit (to avoid issues with GLSL)
					warning(param.location, 4568, '\'' + param.name + "': integer parameters have the 'nointerpolation' qualifier by default");
				}
			}
			else
			{
				// Remove optional trailing zero from system value semantics, so that e.g. SV_POSITION and SV_POSITION0 mean the same thing
				if (param.semantic.back() == '0' && (param.semantic[param.semantic.size() - 2] < '0' || param.semantic[param.semantic.size() - 2] > '9'))
					param.semantic.pop_back();
			}
		}

		info.parameter_list.push_back(std::move(param));
	}

	if (expect_parenthesis && !expect(')'))
		return false;

	// Handle return type semantic
	if (accept(':'))
	{
		if (!expect(tokenid::identifier))
			return false;
		if (type.is_void())
			return error(_token.location, 3076, '\'' + name + "': void function cannot have a semantic"), false;

		info.return_semantic = std::move(_token.literal_as_string);
		// Make semantic upper case to simplify comparison later on
		std::transform(info.return_semantic.begin(), info.return_semantic.end(), info.return_semantic.begin(),
			[](std::string::value_type c) {
				return static_cast<std::string::value_type>(std::toupper(c));
			});
	}

	// Check if this is a function declaration without a body
	if (accept(';'))
		return error(location, 3510, '\'' + name + "': function is missing an implementation"), false;

	// Define the function now that information about the declaration was gathered
	const auto id = _codegen->define_function(location, info);

	// Insert the function and parameter symbols into the symbol table and update current function pointer to the permanent one
	symbol symbol = { symbol_type::function, id, { type::t_function } };
	symbol.function = _current_function = &_codegen->get_function(id);

	if (!insert_symbol(name, symbol, true))
		return error(location, 3003, "redefinition of '" + name + '\''), false;

	for (const struct_member_info &param : info.parameter_list)
		if (!insert_symbol(param.name, { symbol_type::variable, param.definition, param.type }))
			return error(param.location, 3003, "redefinition of '" + param.name + '\''), false;

	// A function has to start with a new block
	_codegen->enter_block(_codegen->create_block());

	if (!parse_statement_block(false))
		parse_success = false;

	// Add implicit return statement to the end of functions
	if (_codegen->is_in_block())
		_codegen->leave_block_and_return();

	return parse_success;
}

bool reshadefx::parser::parse_variable(type type, std::string name, bool global)
{
	const auto location = std::move(_token.location);

	if (type.is_void())
		return error(location, 3038, '\'' + name + "': variables cannot be void"), false;
	if (type.has(type::q_in) || type.has(type::q_out))
		return error(location, 3055, '\'' + name + "': variables cannot be declared 'in' or 'out'"), false;

	// Local and global variables have different requirements
	if (global)
	{
		// Check that type qualifier combinations are valid
		if (type.has(type::q_static))
		{
			// Global variables that are 'static' cannot be of another storage class
			if (type.has(type::q_uniform))
				return error(location, 3007, '\'' + name + "': uniform global variables cannot be declared 'static'"), false;
			// The 'volatile' qualifier is only valid memory object declarations that are storage images or uniform blocks
			if (type.has(type::q_volatile))
				return error(location, 3008, '\'' + name + "': global variables cannot be declared 'volatile'"), false;
		}
		else if (!type.has(type::q_groupshared))
		{
			// Make all global variables 'uniform' by default, since they should be externally visible without the 'static' keyword
			if (!type.has(type::q_uniform) && !type.is_object())
				warning(location, 5000, '\'' + name + "': global variables are considered 'uniform' by default");

			// Global variables that are not 'static' are always 'extern' and 'uniform'
			type.qualifiers |= type::q_extern | type::q_uniform;

			// It is invalid to make 'uniform' variables constant, since they can be modified externally
			if (type.has(type::q_const))
				return error(location, 3035, '\'' + name + "': variables which are 'uniform' cannot be declared 'const'"), false;
		}
	}
	else
	{
		// Static does not really have meaning on local variables
		if (type.has(type::q_static))
			type.qualifiers &= ~type::q_static;

		if (type.has(type::q_extern))
			return error(location, 3006, '\'' + name + "': local variables cannot be declared 'extern'"), false;
		if (type.has(type::q_uniform))
			return error(location, 3047, '\'' + name + "': local variables cannot be declared 'uniform'"), false;
		if (type.has(type::q_groupshared))
			return error(location, 3010, '\'' + name + "': local variables cannot be declared 'groupshared'"), false;

		if (type.is_object())
			return error(location, 3038, '\'' + name + "': local variables cannot be texture, sampler or storage objects"), false;
	}

	// The variable name may be followed by an optional array size expression
	if (!parse_array_size(type))
		return false;

	bool parse_success = true;
	expression initializer;
	texture_info texture_info;
	sampler_info sampler_info;
	storage_info storage_info;

	if (accept(':'))
	{
		if (!expect(tokenid::identifier))
			return false;
		else if (!global) // Only global variables can have a semantic
			return error(_token.location, 3043, '\'' + name + "': local variables cannot have semantics"), false;

		std::string &semantic = texture_info.semantic;
		semantic = std::move(_token.literal_as_string);

		// Make semantic upper case to simplify comparison later on
		std::transform(semantic.begin(), semantic.end(), semantic.begin(),
			[](std::string::value_type c) {
				return static_cast<std::string::value_type>(std::toupper(c));
			});
	}
	else
	{
		// Global variables can have optional annotations
		if (global && !parse_annotations(sampler_info.annotations))
			parse_success = false;

		// Variables without a semantic may have an optional initializer
		if (accept('='))
		{
			if (!parse_expression_assignment(initializer))
				return false;

			if (type.has(type::q_groupshared))
				return error(initializer.location, 3009, '\'' + name + "': variables declared 'groupshared' cannot have an initializer"), false;
			// TODO: This could be resolved by initializing these at the beginning of the entry point
			if (global && !initializer.is_constant)
				return error(initializer.location, 3011, '\'' + name + "': initial value must be a literal expression"), false;

			// Check type compatibility
			if ((type.array_length >= 0 && initializer.type.array_length != type.array_length) || !type::rank(initializer.type, type))
				return error(initializer.location, 3017, '\'' + name + "': initial value (" + initializer.type.description() + ") does not match variable type (" + type.description() + ')'), false;
			if ((initializer.type.rows < type.rows || initializer.type.cols < type.cols) && !initializer.type.is_scalar())
				return error(initializer.location, 3017, '\'' + name + "': cannot implicitly convert these vector types (from " + initializer.type.description() + " to " + type.description() + ')'), false;

			// Deduce array size from the initializer expression
			if (initializer.type.is_array())
				type.array_length = initializer.type.array_length;

			// Perform implicit cast from initializer expression to variable type
			if (initializer.type.components() > type.components())
				warning(initializer.location, 3206, "implicit truncation of vector type");

			initializer.add_cast_operation(type);

			if (type.has(type::q_static))
				initializer.type.qualifiers |= type::q_static;
		}
		else if (type.is_numeric() || type.is_struct()) // Numeric variables without an initializer need special handling
		{
			if (type.has(type::q_const)) // Constants have to have an initial value
				return error(location, 3012, '\'' + name + "': missing initial value"), false;
			else if (!type.has(type::q_uniform)) // Zero initialize all global variables
				initializer.reset_to_rvalue_constant(location, {}, type);
		}
		else if (global && accept('{')) // Textures and samplers can have a property block attached to their declaration
		{
			// Non-numeric variables cannot be constants
			if (type.has(type::q_const))
				return error(location, 3035, '\'' + name + "': this variable type cannot be declared 'const'"), false;

			while (!peek('}'))
			{
				if (!expect(tokenid::identifier))
					return consume_until('}'), false;

				const auto property_name = std::move(_token.literal_as_string);
				const auto property_location = std::move(_token.location);

				if (!expect('='))
					return consume_until('}'), false;

				backup();

				expression expression;

				if (accept(tokenid::identifier)) // Handle special enumeration names for property values
				{
					// Transform identifier to uppercase to do case-insensitive comparison
					std::transform(_token.literal_as_string.begin(), _token.literal_as_string.end(), _token.literal_as_string.begin(),
						[](std::string::value_type c) {
							return static_cast<std::string::value_type>(std::toupper(c));
						});

					static const std::unordered_map<std::string_view, uint32_t> s_enum_values = {
						{ "NONE", 0 }, { "POINT", 0 },
						{ "LINEAR", 1 },
						{ "WRAP", uint32_t(texture_address_mode::wrap) }, { "REPEAT", uint32_t(texture_address_mode::wrap) },
						{ "MIRROR", uint32_t(texture_address_mode::mirror) },
						{ "CLAMP", uint32_t(texture_address_mode::clamp) },
						{ "BORDER", uint32_t(texture_address_mode::border) },
						{ "R8", uint32_t(texture_format::r8) },
						{ "R16", uint32_t(texture_format::r16) },
						{ "R16F", uint32_t(texture_format::r16f) },
						{ "R32I", uint32_t(texture_format::r32i) },
						{ "R32U", uint32_t(texture_format::r32u) },
						{ "R32F", uint32_t(texture_format::r32f) },
						{ "RG8", uint32_t(texture_format::rg8) }, { "R8G8", uint32_t(texture_format::rg8) },
						{ "RG16", uint32_t(texture_format::rg16) }, { "R16G16", uint32_t(texture_format::rg16) },
						{ "RG16F", uint32_t(texture_format::rg16f) }, { "R16G16F", uint32_t(texture_format::rg16f) },
						{ "RG32F", uint32_t(texture_format::rg32f) }, { "R32G32F", uint32_t(texture_format::rg32f) },
						{ "RGBA8", uint32_t(texture_format::rgba8) }, { "R8G8B8A8", uint32_t(texture_format::rgba8) },
						{ "RGBA16", uint32_t(texture_format::rgba16) }, { "R16G16B16A16", uint32_t(texture_format::rgba16) },
						{ "RGBA16F", uint32_t(texture_format::rgba16f) }, { "R16G16B16A16F", uint32_t(texture_format::rgba16f) },
						{ "RGBA32F", uint32_t(texture_format::rgba32f) }, { "R32G32B32A32F", uint32_t(texture_format::rgba32f) },
						{ "RGB10A2", uint32_t(texture_format::rgb10a2) }, { "R10G10B10A2", uint32_t(texture_format::rgb10a2) },
					};

					// Look up identifier in list of possible enumeration names
					if (const auto it = s_enum_values.find(_token.literal_as_string);
						it != s_enum_values.end())
						expression.reset_to_rvalue_constant(_token.location, it->second);
					else // No match found, so rewind to parser state before the identifier was consumed and try parsing it as a normal expression
						restore();
				}

				// Parse right hand side as normal expression if no special enumeration name was matched already
				if (!expression.is_constant && !parse_expression_multary(expression))
					return consume_until('}'), false;

				if (property_name == "Texture")
				{
					// Ignore invalid symbols that were added during error recovery
					if (expression.base == 0xFFFFFFFF)
						return consume_until('}'), false;

					if (!expression.type.is_texture())
						return error(expression.location, 3020, "type mismatch, expected texture name"), consume_until('}'), false;

					if (type.is_sampler() || type.is_storage())
					{
						reshadefx::texture_info &target_info = _codegen->get_texture(expression.base);
						if (type.is_storage())
							// Texture is used as storage
							target_info.storage_access = true;

						texture_info = target_info;
						sampler_info.texture_name = target_info.unique_name;
						storage_info.texture_name = target_info.unique_name;
					}
				}
				else
				{
					if (!expression.is_constant || !expression.type.is_scalar())
						return error(expression.location, 3538, "value must be a literal scalar expression"), consume_until('}'), false;

					// All states below expect the value to be of an integer type
					expression.add_cast_operation({ type::t_int, 1, 1 });
					const int value = expression.constant.as_int[0];

					if (value < 0) // There is little use for negative values, so warn in those cases
						warning(expression.location, 3571, "negative value specified for property '" + property_name + '\'');

					if (type.is_texture())
					{
						if (property_name == "Width")
							texture_info.width = value > 0 ? value : 1;
						else if (type.texture_dimension() >= 2 && property_name == "Height")
							texture_info.height = value > 0 ? value : 1;
						else if (type.texture_dimension() >= 3 && property_name == "Depth")
							texture_info.depth = value > 0 && value <= std::numeric_limits<uint16_t>::max() ? static_cast<uint16_t>(value) : 1;
						else if (property_name == "MipLevels")
							// Also ensures negative values do not cause problems
							texture_info.levels = value > 0 && value <= std::numeric_limits<uint16_t>::max() ? static_cast<uint16_t>(value) : 1;
						else if (property_name == "Format")
							texture_info.format = static_cast<texture_format>(value);
						else
							return error(property_location, 3004, "unrecognized property '" + property_name + '\''), consume_until('}'), false;
					}
					else if (type.is_sampler())
					{
						if (property_name == "SRGBTexture" || property_name == "SRGBReadEnable")
							sampler_info.srgb = value != 0;
						else if (property_name == "AddressU")
							sampler_info.address_u = static_cast<texture_address_mode>(value);
						else if (property_name == "AddressV")
							sampler_info.address_v = static_cast<texture_address_mode>(value);
						else if (property_name == "AddressW")
							sampler_info.address_w = static_cast<texture_address_mode>(value);
						else if (property_name == "MinFilter")
							sampler_info.filter = static_cast<filter_mode>((uint32_t(sampler_info.filter) & 0x0F) | ((value << 4) & 0x30)); // Combine sampler filter components into a single filter enumeration value
						else if (property_name == "MagFilter")
							sampler_info.filter = static_cast<filter_mode>((uint32_t(sampler_info.filter) & 0x33) | ((value << 2) & 0x0C));
						else if (property_name == "MipFilter")
							sampler_info.filter = static_cast<filter_mode>((uint32_t(sampler_info.filter) & 0x3C) |  (value       & 0x03));
						else if (property_name == "MinLOD" || property_name == "MaxMipLevel")
							sampler_info.min_lod = static_cast<float>(value);
						else if (property_name == "MaxLOD")
							sampler_info.max_lod = static_cast<float>(value);
						else if (property_name == "MipLODBias" || property_name == "MipMapLodBias")
							sampler_info.lod_bias = static_cast<float>(value);
						else
							return error(property_location, 3004, "unrecognized property '" + property_name + '\''), consume_until('}'), false;
					}
					else if (type.is_storage())
					{
						if (property_name == "MipLOD" || property_name == "MipLevel")
							storage_info.level = value > 0 && value < std::numeric_limits<uint16_t>::max() ? static_cast<uint16_t>(value) : 0;
						else
							return error(property_location, 3004, "unrecognized property '" + property_name + '\''), consume_until('}'), false;
					}
				}

				if (!expect(';'))
					return consume_until('}'), false;
			}

			if (!expect('}'))
				return false;
		}
	}

	// At this point the array size should be known (either from the declaration or the initializer)
	if (type.array_length < 0)
		return error(location, 3074, '\'' + name + "': implicit array missing initial value"), false;

	symbol symbol;

	// Variables with a constant initializer and constant type are named constants
	// Skip this for very large arrays though, to avoid large amounts of duplicated values when that array constant is accessed with a dynamic index
	if (type.is_numeric() && type.has(type::q_const) && initializer.is_constant && type.array_length < 100)
	{
		// Named constants are special symbols
		symbol = { symbol_type::constant, 0, type, initializer.constant };
	}
	else if (type.is_texture())
	{
		assert(global);

		texture_info.name = name;
		texture_info.type = static_cast<texture_type>(type.texture_dimension());

		// Add namespace scope to avoid name clashes
		texture_info.unique_name = 'V' + current_scope().name + name;
		std::replace(texture_info.unique_name.begin(), texture_info.unique_name.end(), ':', '_');

		texture_info.annotations = std::move(sampler_info.annotations);

		symbol = { symbol_type::variable, 0, type };
		symbol.id = _codegen->define_texture(location, texture_info);
	}
	// Samplers are actually combined image samplers
	else if (type.is_sampler())
	{
		assert(global);

		if (sampler_info.texture_name.empty())
			return error(location, 3012, '\'' + name + "': missing 'Texture' property"), false;
		if (type.texture_dimension() != static_cast<unsigned int>(texture_info.type))
			return error(location, 3521, '\'' + name + "': type mismatch between texture and sampler type"), false;
		if (sampler_info.srgb && texture_info.format != texture_format::rgba8)
			return error(location, 4582, '\'' + name + "': texture does not support sRGB sampling (only textures with RGBA8 format do)"), false;

		if (texture_info.format == texture_format::r32i ?
				!type.is_integral() || !type.is_signed() :
			texture_info.format == texture_format::r32u ?
				!type.is_integral() || !type.is_unsigned() :
				!type.is_floating_point())
			return error(location, 4582, '\'' + name + "': type mismatch between texture format and sampler element type"), false;

		sampler_info.name = name;
		sampler_info.type = type;

		// Add namespace scope to avoid name clashes
		sampler_info.unique_name = 'V' + current_scope().name + name;
		std::replace(sampler_info.unique_name.begin(), sampler_info.unique_name.end(), ':', '_');

		symbol = { symbol_type::variable, 0, type };
		symbol.id = _codegen->define_sampler(location, texture_info, sampler_info);
	}
	else if (type.is_storage())
	{
		assert(global);

		if (storage_info.texture_name.empty())
			return error(location, 3012, '\'' + name + "': missing 'Texture' property"), false;
		if (type.texture_dimension() != static_cast<unsigned int>(texture_info.type))
			return error(location, 3521, '\'' + name + "': type mismatch between texture and storage type"), false;

		if (texture_info.format == texture_format::r32i ?
				!type.is_integral() || !type.is_signed() :
			texture_info.format == texture_format::r32u ?
				!type.is_integral() || !type.is_unsigned() :
				!type.is_floating_point())
			return error(location, 4582, '\'' + name + "': type mismatch between texture format and storage element type"), false;

		storage_info.name = name;
		storage_info.type = type;

		// Add namespace scope to avoid name clashes
		storage_info.unique_name = 'V' + current_scope().name + name;
		std::replace(storage_info.unique_name.begin(), storage_info.unique_name.end(), ':', '_');

		if (storage_info.level > texture_info.levels - 1)
			storage_info.level = texture_info.levels - 1;

		symbol = { symbol_type::variable, 0, type };
		symbol.id = _codegen->define_storage(location, texture_info, storage_info);
	}
	// Uniform variables are put into a global uniform buffer structure
	else if (type.has(type::q_uniform))
	{
		assert(global);

		uniform_info uniform_info;
		uniform_info.name = name;
		uniform_info.type = type;

		uniform_info.annotations = std::move(sampler_info.annotations);

		uniform_info.initializer_value = std::move(initializer.constant);
		uniform_info.has_initializer_value = initializer.is_constant;

		symbol = { symbol_type::variable, 0, type };
		symbol.id = _codegen->define_uniform(location, uniform_info);
	}
	// All other variables are separate entities
	else
	{
		// Update global variable names to contain the namespace scope to avoid name clashes
		std::string unique_name = global ? 'V' + current_scope().name + name : name;
		std::replace(unique_name.begin(), unique_name.end(), ':', '_');

		symbol = { symbol_type::variable, 0, type };
		symbol.id = _codegen->define_variable(location, type, std::move(unique_name), global,
			// Shared variables cannot have an initializer
			type.has(type::q_groupshared) ? 0 : _codegen->emit_load(initializer));
	}

	// Insert the symbol into the symbol table
	if (!insert_symbol(name, symbol, global))
		return error(location, 3003, "redefinition of '" + name + '\''), false;

	return parse_success;
}

bool reshadefx::parser::parse_technique()
{
	if (!expect(tokenid::identifier))
		return false;

	technique_info info;
	info.name = std::move(_token.literal_as_string);

	bool parse_success = parse_annotations(info.annotations);

	if (!expect('{'))
		return false;

	while (!peek('}'))
	{
		if (pass_info pass; parse_technique_pass(pass))
			info.passes.push_back(std::move(pass));
		else {
			parse_success = false;
			if (!peek(tokenid::pass) && !peek('}')) // If there is another pass definition following, try to parse that despite the error
				return consume_until('}'), false;
		}
	}

	_codegen->define_technique(std::move(info));

	return expect('}') && parse_success;
}
bool reshadefx::parser::parse_technique_pass(pass_info &info)
{
	if (!expect(tokenid::pass))
		return false;

	const auto pass_location = std::move(_token.location);

	// Passes can have an optional name
	if (accept(tokenid::identifier))
		info.name = std::move(_token.literal_as_string);

	bool parse_success = true;
	bool targets_support_srgb = true;
	function_info vs_info, ps_info, cs_info;

	if (!expect('{'))
		return false;

	while (!peek('}'))
	{
		// Parse pass states
		if (!expect(tokenid::identifier))
			return consume_until('}'), false;

		auto location = std::move(_token.location);
		const auto state = std::move(_token.literal_as_string);

		if (!expect('='))
			return consume_until('}'), false;

		const bool is_shader_state = state == "VertexShader" || state == "PixelShader" || state == "ComputeShader";
		const bool is_texture_state = state.compare(0, 12, "RenderTarget") == 0 && (state.size() == 12 || (state[12] >= '0' && state[12] < '8'));

		// Shader and render target assignment looks up values in the symbol table, so handle those separately from the other states
		if (is_shader_state || is_texture_state)
		{
			std::string identifier;
			scoped_symbol symbol;
			if (!accept_symbol(identifier, symbol))
				return consume_until('}'), false;

			location = std::move(_token.location);

			int num_threads[3] = { 1, 1, 1 };
			if (accept('<'))
			{
				expression x, y, z;
				if (!parse_expression_multary(x, 8) || !expect(',') || !parse_expression_multary(y, 8))
					return consume_until('}'), false;

				// Parse optional third dimension (defaults to 1)
				z.reset_to_rvalue_constant({}, 1);
				if (accept(',') && !parse_expression_multary(z, 8))
					return consume_until('}'), false;

				if (!x.is_constant)
					return error(x.location, 3011, "value must be a literal expression"), consume_until('}'), false;
				if (!y.is_constant)
					return error(y.location, 3011, "value must be a literal expression"), consume_until('}'), false;
				if (!z.is_constant)
					return error(z.location, 3011, "value must be a literal expression"), consume_until('}'), false;
				x.add_cast_operation({ type::t_int, 1, 1 });
				y.add_cast_operation({ type::t_int, 1, 1 });
				z.add_cast_operation({ type::t_int, 1, 1 });
				num_threads[0] = x.constant.as_int[0];
				num_threads[1] = y.constant.as_int[0];
				num_threads[2] = z.constant.as_int[0];

				if (!expect('>'))
					return consume_until('}'), false;
			}

			// Ignore invalid symbols that were added during error recovery
			if (symbol.id != 0xFFFFFFFF)
			{
				if (is_shader_state)
				{
					if (!symbol.id)
						parse_success = false,
						error(location, 3501, "undeclared identifier '" + identifier + "', expected function name");
					else if (!symbol.type.is_function())
						parse_success = false,
						error(location, 3020, "type mismatch, expected function name");
					else {
						// Look up the matching function info for this function definition
						function_info &function_info = _codegen->get_function(symbol.id);

						// We potentially need to generate a special entry point function which translates between function parameters and input/output variables
						switch (state[0])
						{
						case 'V':
							vs_info = function_info;
							_codegen->define_entry_point(vs_info, shader_type::vs);
							info.vs_entry_point = vs_info.unique_name;
							break;
						case 'P':
							ps_info = function_info;
							_codegen->define_entry_point(ps_info, shader_type::ps);
							info.ps_entry_point = ps_info.unique_name;
							break;
						case 'C':
							cs_info = function_info;
							_codegen->define_entry_point(cs_info, shader_type::cs, num_threads);
							info.cs_entry_point = cs_info.unique_name;
							break;
						}
					}
				}
				else
				{
					assert(is_texture_state);

					if (!symbol.id)
						parse_success = false,
						error(location, 3004, "undeclared identifier '" + identifier + "', expected texture name");
					else if (!symbol.type.is_texture())
						parse_success = false,
						error(location, 3020, "type mismatch, expected texture name");
					else if (symbol.type.texture_dimension() != 2)
						parse_success = false,
						error(location, 3020, "cannot use texture" + std::to_string(symbol.type.texture_dimension()) + "D as render target");
					else {
						reshadefx::texture_info &target_info = _codegen->get_texture(symbol.id);
						// Texture is used as a render target
						target_info.render_target = true;

						// Verify that all render targets in this pass have the same dimensions
						if (info.viewport_width != 0 && info.viewport_height != 0 && (target_info.width != info.viewport_width || target_info.height != info.viewport_height))
							parse_success = false,
							error(location, 4545, "cannot use multiple render targets with different texture dimensions (is " + std::to_string(target_info.width) + 'x' + std::to_string(target_info.height) + ", but expected " + std::to_string(info.viewport_width) + 'x' + std::to_string(info.viewport_height) + ')');

						info.viewport_width = target_info.width;
						info.viewport_height = target_info.height;

						const auto target_index = state.size() > 12 ? (state[12] - '0') : 0;
						info.render_target_names[target_index] = target_info.unique_name;

						// Only RGBA8 format supports sRGB writes across all APIs
						if (target_info.format != texture_format::rgba8)
							targets_support_srgb = false;
					}
				}
			}
			else
			{
				parse_success = false;
			}
		}
		else // Handle the rest of the pass states
		{
			backup();

			expression expression;

			if (accept(tokenid::identifier)) // Handle special enumeration names for pass states
			{
				// Transform identifier to uppercase to do case-insensitive comparison
				std::transform(_token.literal_as_string.begin(), _token.literal_as_string.end(), _token.literal_as_string.begin(),
					[](std::string::value_type c) {
						return static_cast<std::string::value_type>(std::toupper(c));
					});

				static const std::unordered_map<std::string_view, uint32_t> s_enum_values = {
					{ "NONE", 0 }, { "ZERO", 0 }, { "ONE", 1 },
					{ "ADD", uint32_t(pass_blend_op::add) },
					{ "SUBTRACT", uint32_t(pass_blend_op::subtract) },
					{ "REVSUBTRACT", uint32_t(pass_blend_op::reverse_subtract) },
					{ "MIN", uint32_t(pass_blend_op::min) },
					{ "MAX", uint32_t(pass_blend_op::max) },
					{ "SRCCOLOR", uint32_t(pass_blend_factor::source_color) },
					{ "INVSRCCOLOR", uint32_t(pass_blend_factor::one_minus_source_color) },
					{ "DESTCOLOR", uint32_t(pass_blend_factor::dest_color) },
					{ "INVDESTCOLOR", uint32_t(pass_blend_factor::one_minus_dest_color) },
					{ "SRCALPHA", uint32_t(pass_blend_factor::source_alpha) },
					{ "INVSRCALPHA", uint32_t(pass_blend_factor::one_minus_source_alpha) },
					{ "DESTALPHA", uint32_t(pass_blend_factor::dest_alpha) },
					{ "INVDESTALPHA", uint32_t(pass_blend_factor::one_minus_dest_alpha) },
					{ "KEEP", uint32_t(pass_stencil_op::keep) },
					{ "REPLACE", uint32_t(pass_stencil_op::replace) },
					{ "INVERT", uint32_t(pass_stencil_op::invert) },
					{ "INCR", uint32_t(pass_stencil_op::increment) },
					{ "INCRSAT", uint32_t(pass_stencil_op::increment_saturate) },
					{ "DECR", uint32_t(pass_stencil_op::decrement) },
					{ "DECRSAT", uint32_t(pass_stencil_op::decrement_saturate) },
					{ "NEVER", uint32_t(pass_stencil_func::never) },
					{ "EQUAL", uint32_t(pass_stencil_func::equal) },
					{ "NEQUAL", uint32_t(pass_stencil_func::not_equal) }, { "NOTEQUAL", uint32_t(pass_stencil_func::not_equal)  },
					{ "LESS", uint32_t(pass_stencil_func::less) },
					{ "GREATER", uint32_t(pass_stencil_func::greater) },
					{ "LEQUAL", uint32_t(pass_stencil_func::less_equal) }, { "LESSEQUAL", uint32_t(pass_stencil_func::less_equal) },
					{ "GEQUAL", uint32_t(pass_stencil_func::greater_equal) }, { "GREATEREQUAL", uint32_t(pass_stencil_func::greater_equal) },
					{ "ALWAYS", uint32_t(pass_stencil_func::always) },
					{ "POINTS", uint32_t(primitive_topology::point_list) },
					{ "POINTLIST", uint32_t(primitive_topology::point_list) },
					{ "LINES", uint32_t(primitive_topology::line_list) },
					{ "LINELIST", uint32_t(primitive_topology::line_list) },
					{ "LINESTRIP", uint32_t(primitive_topology::line_strip) },
					{ "TRIANGLES", uint32_t(primitive_topology::triangle_list) },
					{ "TRIANGLELIST", uint32_t(primitive_topology::triangle_list) },
					{ "TRIANGLESTRIP", uint32_t(primitive_topology::triangle_strip) },
				};

				// Look up identifier in list of possible enumeration names
				if (const auto it = s_enum_values.find(_token.literal_as_string);
					it != s_enum_values.end())
					expression.reset_to_rvalue_constant(_token.location, it->second);
				else // No match found, so rewind to parser state before the identifier was consumed and try parsing it as a normal expression
					restore();
			}

			// Parse right hand side as normal expression if no special enumeration name was matched already
			if (!expression.is_constant && !parse_expression_multary(expression))
				return consume_until('}'), false;
			else if (!expression.is_constant || !expression.type.is_scalar())
				parse_success = false,
				error(expression.location, 3011, "pass state value must be a literal scalar expression");

			// All states below expect the value to be of an unsigned integer type
			expression.add_cast_operation({ type::t_uint, 1, 1 });
			const unsigned int value = expression.constant.as_uint[0];

#define SET_STATE_VALUE_INDEXED(name, info_name, value) \
	else if (constexpr size_t name##_len = sizeof(#name) - 1; state.compare(0, name##_len, #name) == 0 && (state.size() == name##_len || (state[name##_len] >= '0' && state[name##_len] < ('0' + static_cast<char>(std::size(info.info_name)))))) \
	{ \
		if (state.size() != name##_len) \
			info.info_name[state[name##_len] - '0'] = (value); \
		else \
			for (int i = 0; i < static_cast<int>(std::size(info.info_name)); ++i) \
				info.info_name[i] = (value); \
	}

			if (state == "SRGBWriteEnable")
				info.srgb_write_enable = (value != 0);
			SET_STATE_VALUE_INDEXED(BlendEnable, blend_enable, value != 0)
			else if (state == "StencilEnable")
				info.stencil_enable = (value != 0);
			else if (state == "ClearRenderTargets")
				info.clear_render_targets = (value != 0);
			SET_STATE_VALUE_INDEXED(ColorWriteMask, color_write_mask, value & 0xFF)
			SET_STATE_VALUE_INDEXED(RenderTargetWriteMask, color_write_mask, value & 0xFF)
			else if (state == "StencilReadMask" || state == "StencilMask")
				info.stencil_read_mask = value & 0xFF;
			else if (state == "StencilWriteMask")
				info.stencil_write_mask = value & 0xFF;
			SET_STATE_VALUE_INDEXED(BlendOp, blend_op, static_cast<pass_blend_op>(value))
			SET_STATE_VALUE_INDEXED(BlendOpAlpha, blend_op_alpha, static_cast<pass_blend_op>(value))
			SET_STATE_VALUE_INDEXED(SrcBlend, src_blend, static_cast<pass_blend_factor>(value))
			SET_STATE_VALUE_INDEXED(SrcBlendAlpha, src_blend_alpha, static_cast<pass_blend_factor>(value))
			SET_STATE_VALUE_INDEXED(DestBlend, dest_blend, static_cast<pass_blend_factor>(value))
			SET_STATE_VALUE_INDEXED(DestBlendAlpha, dest_blend_alpha, static_cast<pass_blend_factor>(value))
			else if (state == "StencilFunc")
				info.stencil_comparison_func = static_cast<pass_stencil_func>(value);
			else if (state == "StencilRef")
				info.stencil_reference_value = value;
			else if (state == "StencilPass" || state == "StencilPassOp")
				info.stencil_op_pass = static_cast<pass_stencil_op>(value);
			else if (state == "StencilFail" || state == "StencilFailOp")
				info.stencil_op_fail = static_cast<pass_stencil_op>(value);
			else if (state == "StencilZFail" || state == "StencilDepthFail" || state == "StencilDepthFailOp")
				info.stencil_op_depth_fail = static_cast<pass_stencil_op>(value);
			else if (state == "VertexCount")
				info.num_vertices = value;
			else if (state == "PrimitiveType" || state == "PrimitiveTopology")
				info.topology = static_cast<primitive_topology>(value);
			else if (state == "DispatchSizeX")
				info.viewport_width = value;
			else if (state == "DispatchSizeY")
				info.viewport_height = value;
			else if (state == "DispatchSizeZ")
				info.viewport_dispatch_z = value;
			else if (state == "GenerateMipmaps" || state == "GenerateMipMaps")
				info.generate_mipmaps = (value != 0);
			else
				parse_success = false,
				error(location, 3004, "unrecognized pass state '" + state + '\'');

#undef SET_STATE_VALUE_INDEXED
		}

		if (!expect(';'))
			return consume_until('}'), false;
	}

	if (parse_success)
	{
		if (!info.cs_entry_point.empty())
		{
			if (info.viewport_width == 0 || info.viewport_height == 0)
			{
				parse_success = false;
				error(pass_location, 3012, "pass is missing 'DispatchSizeX' or 'DispatchSizeY' property");
			}

			if (!info.vs_entry_point.empty())
				warning(pass_location, 3089, "pass is specifying both 'VertexShader' and 'ComputeShader' which cannot be used together");
			if (!info.ps_entry_point.empty())
				warning(pass_location, 3089,  "pass is specifying both 'PixelShader' and 'ComputeShader' which cannot be used together");

			for (codegen::id id : cs_info.referenced_samplers)
				info.samplers.push_back(_codegen->get_sampler(id));
			for (codegen::id id : cs_info.referenced_storages)
				info.storages.push_back(_codegen->get_storage(id));
		}
		else if (info.vs_entry_point.empty() || info.ps_entry_point.empty())
		{
			parse_success = false;

			if (info.vs_entry_point.empty())
				error(pass_location, 3012, "pass is missing 'VertexShader' property");
			if (info.ps_entry_point.empty())
				error(pass_location, 3012,  "pass is missing 'PixelShader' property");
		}
		else
		{
			// Verify that shader signatures between VS and PS match (both semantics and interpolation qualifiers)
			std::unordered_map<std::string_view, type> vs_semantic_mapping;
			if (vs_info.return_semantic.empty())
			{
				if (!vs_info.return_type.is_void() && !vs_info.return_type.is_struct())
				{
					parse_success = false;
					error(pass_location, 3503, '\'' + vs_info.name + "': function return value is missing semantics");
				}
			}
			else
			{
				vs_semantic_mapping[vs_info.return_semantic] = vs_info.return_type;
			}

			for (const struct_member_info &param : vs_info.parameter_list)
			{
				if (param.semantic.empty())
				{
					if (!param.type.is_struct())
					{
						parse_success = false;
						if (param.type.has(type::q_in))
							error(pass_location, 3502, '\'' + vs_info.name + "': input parameter '" + param.name + "' is missing semantics");
						else
							error(pass_location, 3503, '\'' + vs_info.name + "': output parameter '" + param.name + "' is missing semantics");
					}
				}
				else if (param.type.has(type::q_out))
				{
					vs_semantic_mapping[param.semantic] = param.type;
				}
			}

			if (ps_info.return_semantic.empty())
			{
				if (!ps_info.return_type.is_void() && !ps_info.return_type.is_struct())
				{
					parse_success = false;
					error(pass_location, 3503, '\'' + ps_info.name + "': function return value is missing semantics");
				}
			}

			for (const struct_member_info &param : ps_info.parameter_list)
			{
				if (param.semantic.empty())
				{
					if (!param.type.is_struct())
					{
						parse_success = false;
						if (param.type.has(type::q_in))
							error(pass_location, 3502, '\'' + ps_info.name + "': input parameter '" + param.name + "' is missing semantics");
						else
							error(pass_location, 3503, '\'' + ps_info.name + "': output parameter '" + param.name + "' is missing semantics");
					}
				}
				else if (param.type.has(type::q_in))
				{
					if (const auto it = vs_semantic_mapping.find(param.semantic);
						it == vs_semantic_mapping.end() || it->second != param.type)
						warning(pass_location, 4576, '\'' + ps_info.name + "': input parameter '" + param.name + "' semantic does not match vertex shader one");
					else if (((it->second.qualifiers ^ param.type.qualifiers) & (type::q_linear | type::q_noperspective | type::q_centroid | type::q_nointerpolation)) != 0)
						parse_success = false,
						error(  pass_location, 4568, '\'' + ps_info.name + "': input parameter '" + param.name + "' interpolation qualifiers do not match vertex shader ones");
				}
			}

			for (codegen::id id : vs_info.referenced_samplers)
				info.samplers.push_back(_codegen->get_sampler(id));
			for (codegen::id id : ps_info.referenced_samplers)
				info.samplers.push_back(_codegen->get_sampler(id));
			if (!vs_info.referenced_storages.empty() || !ps_info.referenced_storages.empty())
			{
				parse_success = false;
				error(pass_location, 3667, "storage writes are only valid in compute shaders");
			}
		}

		// Verify render target format supports sRGB writes if enabled
		if (info.srgb_write_enable && !targets_support_srgb)
			parse_success = false,
			error(pass_location, 4582, "one or more render target(s) do not support sRGB writes (only textures with RGBA8 format do)");
	}

	return expect('}') && parse_success;
}
