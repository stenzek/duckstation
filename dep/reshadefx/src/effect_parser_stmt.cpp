/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "effect_lexer.hpp"
#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include <cctype> // std::toupper
#include <cassert>
#include <climits>
#include <algorithm> // std::max, std::replace, std::transform
#include <limits>
#include <string_view>

template <typename ENTER_TYPE, typename LEAVE_TYPE>
struct scope_guard
{
	explicit scope_guard(ENTER_TYPE &&enter_lambda, LEAVE_TYPE &&leave_lambda) :
		leave_lambda(std::forward<LEAVE_TYPE>(leave_lambda)) { enter_lambda(); }
	~scope_guard() { leave_lambda(); }

private:
	LEAVE_TYPE leave_lambda;
};

bool reshadefx::parser::parse(std::string input, codegen *backend)
{
	_lexer = std::make_unique<lexer>(std::move(input));

	// Set backend for subsequent code-generation
	_codegen = backend;
	assert(backend != nullptr);

	consume();

	bool parse_success = true;
	bool current_success = true;

	while (!peek(tokenid::end_of_file))
	{
		if (!parse_top(current_success))
			return false;
		if (!current_success)
			parse_success = false;
	}

	if (parse_success)
		backend->optimize_bindings();

	return parse_success;
}

bool reshadefx::parser::parse_top(bool &parse_success)
{
	if (accept(tokenid::namespace_))
	{
		// Anonymous namespaces are not supported right now, so an identifier is a must
		if (!expect(tokenid::identifier))
			return false;

		const std::string name = std::move(_token.literal_as_string);

		if (!expect('{'))
			return false;

		enter_namespace(name);

		bool current_success = true;
		bool parse_success_namespace = true;

		// Recursively parse top level statements until the namespace is closed again
		while (!peek('}')) // Empty namespaces are valid
		{
			if (!parse_top(current_success))
				return false;
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
		location attribute_location;
		shader_type stype = shader_type::unknown;
		int num_threads[3] = { 0, 0, 0 };

		// Read any function attributes first
		while (accept('['))
		{
			if (!expect(tokenid::identifier))
				return false;

			const std::string attribute = std::move(_token.literal_as_string);

			if (attribute == "shader")
			{
				attribute_location = _token_next.location;

				if (!expect('(') || !expect(tokenid::string_literal))
					return false;

				if (_token.literal_as_string == "vertex")
					stype = shader_type::vertex;
				else if (_token.literal_as_string == "pixel")
					stype = shader_type::pixel;
				else if (_token.literal_as_string == "compute")
					stype = shader_type::compute;

				if (!expect(')'))
					return false;
			}
			else if (attribute == "numthreads")
			{
				attribute_location = _token_next.location;

				expression x, y, z;
				if (!expect('(') || !parse_expression_multary(x, 8) || !expect(',') || !parse_expression_multary(y, 8) || !expect(',') || !parse_expression_multary(z, 8) || !expect(')'))
					return false;

				if (!x.is_constant)
				{
					error(x.location, 3011, "value must be a literal expression");
					parse_success = false;
				}
				if (!y.is_constant)
				{
					error(y.location, 3011, "value must be a literal expression");
					parse_success = false;
				}
				if (!z.is_constant)
				{
					error(z.location, 3011, "value must be a literal expression");
					parse_success = false;
				}
				x.add_cast_operation({ type::t_int, 1, 1 });
				y.add_cast_operation({ type::t_int, 1, 1 });
				z.add_cast_operation({ type::t_int, 1, 1 });
				num_threads[0] = x.constant.as_int[0];
				num_threads[1] = y.constant.as_int[0];
				num_threads[2] = z.constant.as_int[0];
			}
			else
			{
				warning(_token.location, 0, "unknown attribute '" + attribute + "'");
			}

			if (!expect(']'))
				return false;
		}

		if (type type = {}; parse_type(type)) // Type found, this can be either a variable or a function declaration
		{
			parse_success = expect(tokenid::identifier);
			if (!parse_success)
				return true;

			if (peek('('))
			{
				const std::string name = std::move(_token.literal_as_string);

				// This is definitely a function declaration, so parse it
				if (!parse_function(type, name, stype, num_threads))
				{
					// Insert dummy function into symbol table, so later references can be resolved despite the error
					insert_symbol(name, { symbol_type::function, UINT32_MAX, { type::t_function } }, true);
					parse_success = false;
					return true;
				}
			}
			else
			{
				if (!attribute_location.source.empty())
				{
					error(attribute_location, 0, "attribute is valid only on functions");
					parse_success = false;
				}

				// There may be multiple variable names after the type, handle them all
				unsigned int count = 0;
				do
				{
					if (count++ > 0 && !(expect(',') && expect(tokenid::identifier)))
					{
						parse_success = false;
						return false;
					}

					const std::string name = std::move(_token.literal_as_string);

					if (!parse_variable(type, name, true))
					{
						// Insert dummy variable into symbol table, so later references can be resolved despite the error
						insert_symbol(name, { symbol_type::variable, UINT32_MAX, type }, true);
						// Skip the rest of the statement
						consume_until(';');
						parse_success = false;
						return true;
					}
				}
				while (!peek(';'));

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

	return true;
}

bool reshadefx::parser::parse_statement(bool scoped)
{
	if (!_codegen->is_in_block())
	{
		error(_token_next.location, 0, "unreachable code");
		return false;
	}

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

		const std::string attribute = std::move(_token_next.literal_as_string);

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
			warning(_token.location, 0, "unknown attribute '" + attribute + "'");

		if ((loop_control & (unroll | dont_unroll)) == (unroll | dont_unroll))
		{
			error(_token.location, 3524, "can't use loop and unroll attributes together");
			return false;
		}
		if ((selection_control & (flatten | dont_flatten)) == (flatten | dont_flatten))
		{
			error(_token.location, 3524, "can't use branch and flatten attributes together");
			return false;
		}
	}

	// Shift by two so that the possible values are 0x01 for 'flatten' and 0x02 for 'dont_flatten', equivalent to 'unroll' and 'dont_unroll'
	selection_control >>= 4;

	if (peek('{')) // Parse statement block
		return parse_statement_block(scoped);

	if (accept(';')) // Ignore empty statements
		return true;

	// Most statements with the exception of declarations are only valid inside functions
	if (_codegen->is_in_function())
	{
		const location statement_location = _token_next.location;

		if (accept(tokenid::if_))
		{
			codegen::id true_block = _codegen->create_block(); // Block which contains the statements executed when the condition is true
			codegen::id false_block = _codegen->create_block(); // Block which contains the statements executed when the condition is false
			const codegen::id merge_block = _codegen->create_block(); // Block that is executed after the branch re-merged with the current control flow

			expression condition_exp;
			if (!expect('(') || !parse_expression(condition_exp) || !expect(')'))
				return false;

			if (!condition_exp.type.is_scalar())
			{
				error(condition_exp.location, 3019, "if statement conditional expressions must evaluate to a scalar");
				return false;
			}

			// Load condition and convert to boolean value as required by 'OpBranchConditional' in SPIR-V
			condition_exp.add_cast_operation({ type::t_bool, 1, 1 });

			const codegen::id condition_value = _codegen->emit_load(condition_exp);
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
			_codegen->emit_if(statement_location, condition_value, condition_block, true_block, false_block, selection_control);

			return true;
		}

		if (accept(tokenid::switch_))
		{
			const codegen::id merge_block = _codegen->create_block(); // Block that is executed after the switch re-merged with the current control flow

			expression selector_exp;
			if (!expect('(') || !parse_expression(selector_exp) || !expect(')'))
				return false;

			if (!selector_exp.type.is_scalar())
			{
				error(selector_exp.location, 3019, "switch statement expression must evaluate to a scalar");
				return false;
			}

			// Load selector and convert to integral value as required by switch instruction
			selector_exp.add_cast_operation({ type::t_int, 1, 1 });

			const codegen::id selector_value = _codegen->emit_load(selector_exp);
			const codegen::id selector_block = _codegen->leave_block_and_switch(selector_value, merge_block);

			if (!expect('{'))
				return false;

			scope_guard _(
				[this, merge_block]() {
					_loop_break_target_stack.push_back(merge_block);
				},
				[this]() {
					_loop_break_target_stack.pop_back();
				});

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
						{
							consume_until('}');
							return false;
						}

						if (!case_label.type.is_scalar() || !case_label.type.is_integral() || !case_label.is_constant)
						{
							error(case_label.location, 3020, "invalid type for case expression - value must be an integer scalar");
							consume_until('}');
							return false;
						}

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
					{
						consume_until('}');
						return false;
					}
				}

				// It is valid for no statement to follow if this is the last label in the switch body
				const bool end_of_switch = peek('}');

				if (!end_of_switch && !parse_statement(true))
				{
					consume_until('}');
					return false;
				}

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
				warning(statement_location, 5002, "switch statement contains no 'case' or 'default' labels");

			// Emit structured control flow for a switch statement and connect all basic blocks
			_codegen->emit_switch(statement_location, selector_value, selector_block, default_label, default_block, case_literal_and_labels, case_blocks, selection_control);

			return expect('}') && parse_success;
		}

		if (accept(tokenid::for_))
		{
			if (!expect('('))
				return false;

			scope_guard _(
				[this]() { enter_scope(); },
				[this]() { leave_scope(); });

			// Parse initializer first
			if (type type = {}; parse_type(type))
			{
				unsigned int count = 0;
				do
				{
					// There may be multiple declarations behind a type, so loop through them
					if (count++ > 0 && !expect(','))
						return false;

					if (!expect(tokenid::identifier) || !parse_variable(type, std::move(_token.literal_as_string)))
						return false;
				}
				while (!peek(';'));
			}
			else
			{
				// Initializer can also contain an expression if not a variable declaration list and not empty
				if (!peek(';'))
				{
					expression initializer_exp;
					if (!parse_expression(initializer_exp))
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
					expression condition_exp;
					if (!parse_expression(condition_exp))
						return false;

					if (!condition_exp.type.is_scalar())
					{
						error(condition_exp.location, 3019, "scalar value expected");
						return false;
					}

					// Evaluate condition and branch to the right target
					condition_exp.add_cast_operation({ type::t_bool, 1, 1 });

					condition_value = _codegen->emit_load(condition_exp);
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
			_codegen->emit_loop(statement_location, condition_value, prev_block, header_label, condition_block, loop_block, continue_label, loop_control);

			return true;
		}

		if (accept(tokenid::while_))
		{
			scope_guard _(
				[this]() { enter_scope(); },
				[this]() { leave_scope(); });

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

				expression condition_exp;
				if (!expect('(') || !parse_expression(condition_exp) || !expect(')'))
					return false;

				if (!condition_exp.type.is_scalar())
				{
					error(condition_exp.location, 3019, "scalar value expected");
					return false;
				}

				// Evaluate condition and branch to the right target
				condition_exp.add_cast_operation({ type::t_bool, 1, 1 });

				condition_value = _codegen->emit_load(condition_exp);
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
			_codegen->emit_loop(statement_location, condition_value, prev_block, header_label, condition_block, loop_block, continue_label, loop_control);

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

				expression condition_exp;
				if (!expect(tokenid::while_) || !expect('(') || !parse_expression(condition_exp) || !expect(')') || !expect(';'))
					return false;

				if (!condition_exp.type.is_scalar())
				{
					error(condition_exp.location, 3019, "scalar value expected");
					return false;
				}

				// Evaluate condition and branch to the right target
				condition_exp.add_cast_operation({ type::t_bool, 1, 1 });

				condition_value = _codegen->emit_load(condition_exp);

				_codegen->leave_block_and_branch_conditional(condition_value, header_label, merge_block);
			}

			// Add merge block label to the end of the loop
			_codegen->enter_block(merge_block);

			// Emit structured control flow for a loop statement and connect all basic blocks
			_codegen->emit_loop(statement_location, condition_value, prev_block, header_label, 0, loop_block, continue_label, loop_control);

			return true;
		}

		if (accept(tokenid::break_))
		{
			if (_loop_break_target_stack.empty())
			{
				error(statement_location, 3518, "break must be inside loop");
				return false;
			}

			// Branch to the break target of the inner most loop on the stack
			_codegen->leave_block_and_branch(_loop_break_target_stack.back(), 1);

			return expect(';');
		}

		if (accept(tokenid::continue_))
		{
			if (_loop_continue_target_stack.empty())
			{
				error(statement_location, 3519, "continue must be inside loop");
				return false;
			}

			// Branch to the continue target of the inner most loop on the stack
			_codegen->leave_block_and_branch(_loop_continue_target_stack.back(), 2);

			return expect(';');
		}

		if (accept(tokenid::return_))
		{
			const type &return_type = _codegen->_current_function->return_type;

			if (!peek(';'))
			{
				expression return_exp;
				if (!parse_expression(return_exp))
				{
					consume_until(';');
					return false;
				}

				// Cannot return to void
				if (return_type.is_void())
				{
					error(statement_location, 3079, "void functions cannot return a value");
					// Consume the semicolon that follows the return expression so that parsing may continue
					accept(';');
					return false;
				}

				// Cannot return arrays from a function
				if (return_exp.type.is_array() || !type::rank(return_exp.type, return_type))
				{
					error(statement_location, 3017, "expression (" + return_exp.type.description() + ") does not match function return type (" + return_type.description() + ')');
					accept(';');
					return false;
				}

				// Load return value and perform implicit cast to function return type
				if (return_exp.type.components() > return_type.components())
					warning(return_exp.location, 3206, "implicit truncation of vector type");

				return_exp.add_cast_operation(return_type);

				const codegen::id return_value = _codegen->emit_load(return_exp);

				_codegen->leave_block_and_return(return_value);
			}
			else if (!return_type.is_void())
			{
				// No return value was found, but the function expects one
				error(statement_location, 3080, "function must return a value");

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
	if (type type = {}; parse_type(type))
	{
		unsigned int count = 0;
		do
		{
			// There may be multiple declarations behind a type, so loop through them
			if (count++ > 0 && !expect(','))
			{
				// Try to consume the rest of the declaration so that parsing may continue despite the error
				consume_until(';');
				return false;
			}

			if (!expect(tokenid::identifier) || !parse_variable(type, std::move(_token.literal_as_string)))
			{
				consume_until(';');
				return false;
			}
		}
		while (!peek(';'));

		return expect(';');
	}

	// Handle expression statements
	expression statement_exp;
	if (parse_expression(statement_exp))
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
	{
		error(_token.location, 4576, "signature specifies invalid interpolation mode for integer component type");
		return false;
	}

	if (type.has(type::q_centroid) && !type.has(type::q_noperspective))
		type.qualifiers |= type::q_linear;

	return true;
}
bool reshadefx::parser::parse_array_length(type &type)
{
	// Reset array length to zero before checking if one exists
	type.array_length = 0;

	if (accept('['))
	{
		if (accept(']'))
		{
			// No length expression, so this is an unbounded array
			type.array_length = 0xFFFFFFFF;
		}
		else if (expression length_exp; parse_expression(length_exp) && expect(']'))
		{
			if (!length_exp.is_constant || !(length_exp.type.is_scalar() && length_exp.type.is_integral()))
			{
				error(length_exp.location, 3058, "array dimensions must be literal scalar expressions");
				return false;
			}

			type.array_length = length_exp.constant.as_uint[0];

			if (type.array_length < 1 || type.array_length > 65536)
			{
				error(length_exp.location, 3059, "array dimension must be between 1 and 65536");
				return false;
			}
		}
		else
		{
			return false;
		}
	}

	// Multi-dimensional arrays are not supported
	if (peek('['))
	{
		error(_token_next.location, 3119, "arrays cannot be multi-dimensional");
		return false;
	}

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
		if (type type /* = {} */; accept_type_class(type))
			warning(_token.location, 4717, "type prefixes for annotations are deprecated and ignored");

		if (!expect(tokenid::identifier))
		{
			consume_until('>');
			return false;
		}

		std::string name = std::move(_token.literal_as_string);

		expression annotation_exp;
		if (!expect('=') || !parse_expression_multary(annotation_exp) || !expect(';'))
		{
			consume_until('>');
			return false;
		}

		if (annotation_exp.is_constant)
		{
			annotations.push_back({ annotation_exp.type, std::move(name), std::move(annotation_exp.constant) });
		}
		else // Continue parsing annotations despite this not being a constant, since the syntax is still correct
		{
			parse_success = false;
			error(annotation_exp.location, 3011, "value must be a literal expression");
		}
	}

	return expect('>') && parse_success;
}

bool reshadefx::parser::parse_struct()
{
	const location struct_location = std::move(_token.location);

	struct_type info;
	// The structure name is optional
	if (accept(tokenid::identifier))
		info.name = std::move(_token.literal_as_string);
	else
		info.name = "_anonymous_struct_" + std::to_string(struct_location.line) + '_' + std::to_string(struct_location.column);

	info.unique_name = 'S' + current_scope().name + info.name;
	std::replace(info.unique_name.begin(), info.unique_name.end(), ':', '_');

	if (!expect('{'))
		return false;

	bool parse_success = true;

	while (!peek('}')) // Empty structures are possible
	{
		member_type member;

		if (!parse_type(member.type))
		{
			error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + "', expected struct member type");
			consume_until('}');
			accept(';');
			return false;
		}

		unsigned int count = 0;
		do
		{
			if ((count++ > 0 && !expect(',')) || !expect(tokenid::identifier))
			{
				consume_until('}');
				accept(';');
				return false;
			}

			member.name = std::move(_token.literal_as_string);
			member.location = std::move(_token.location);

			if (member.type.is_void())
			{
				parse_success = false;
				error(member.location, 3038, '\'' + member.name + "': struct members cannot be void");
			}
			if (member.type.is_struct()) // Nesting structures would make input/output argument flattening more complicated, so prevent it for now
			{
				parse_success = false;
				error(member.location, 3090, '\'' + member.name + "': nested struct members are not supported");
			}

			if (member.type.has(type::q_in) || member.type.has(type::q_out))
			{
				parse_success = false;
				error(member.location, 3055, '\'' + member.name + "': struct members cannot be declared 'in' or 'out'");
			}
			if (member.type.has(type::q_const))
			{
				parse_success = false;
				error(member.location, 3035, '\'' + member.name + "': struct members cannot be declared 'const'");
			}
			if (member.type.has(type::q_extern))
			{
				parse_success = false;
				error(member.location, 3006, '\'' + member.name + "': struct members cannot be declared 'extern'");
			}
			if (member.type.has(type::q_static))
			{
				parse_success = false;
				error(member.location, 3007, '\'' + member.name + "': struct members cannot be declared 'static'");
			}
			if (member.type.has(type::q_uniform))
			{
				parse_success = false;
				error(member.location, 3047, '\'' + member.name + "': struct members cannot be declared 'uniform'");
			}
			if (member.type.has(type::q_groupshared))
			{
				parse_success = false;
				error(member.location, 3010, '\'' + member.name + "': struct members cannot be declared 'groupshared'");
			}

			// Modify member specific type, so that following members in the declaration list are not affected by this
			if (!parse_array_length(member.type))
			{
				consume_until('}');
				accept(';');
				return false;
			}

			if (member.type.is_unbounded_array())
			{
				parse_success = false;
				error(member.location, 3072, '\'' + member.name + "': array dimensions of struct members must be explicit");
			}

			// Structure members may have semantics to use them as input/output types
			if (accept(':'))
			{
				if (!expect(tokenid::identifier))
				{
					consume_until('}');
					accept(';');
					return false;
				}

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

			// Save member name and type for bookkeeping
			info.member_list.push_back(member);
		}
		while (!peek(';'));

		if (!expect(';'))
		{
			consume_until('}');
			accept(';');
			return false;
		}
	}

	// Empty structures are valid, but not usually intended, so emit a warning
	if (info.member_list.empty())
		warning(struct_location, 5001, "struct has no members");

	// Define the structure now that information about all the member types was gathered
	const codegen::id id = _codegen->define_struct(struct_location, info);

	// Insert the symbol into the symbol table
	symbol symbol = { symbol_type::structure, id };

	if (!insert_symbol(info.name, symbol, true))
	{
		error(struct_location, 3003, "redefinition of '" + info.name + '\'');
		return false;
	}

	return expect('}') && parse_success;
}

bool reshadefx::parser::parse_function(type type, std::string name, shader_type stype, int num_threads[3])
{
	const location function_location = std::move(_token.location);

	if (!expect('(')) // Functions always have a parameter list
		return false;

	if (type.qualifiers != 0)
	{
		error(function_location, 3047, '\'' + name + "': function return type cannot have any qualifiers");
		return false;
	}

	function info;
	info.name = name;
	info.unique_name = 'F' + current_scope().name + name;
	std::replace(info.unique_name.begin(), info.unique_name.end(), ':', '_');

	info.return_type = type;
	info.type = stype;
	info.num_threads[0] = num_threads[0];
	info.num_threads[1] = num_threads[1];
	info.num_threads[2] = num_threads[2];

	_codegen->_current_function = &info;

	bool parse_success = true;
	bool expect_parenthesis = true;

	// Enter function scope (and leave it again when parsing this function finished)
	scope_guard _(
		[this]() {
			enter_scope();
		},
		[this]() {
			leave_scope();
			_codegen->leave_function();
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

		member_type param;

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
		{
			parse_success = false;
			error(param.location, 3038, '\'' + param.name + "': function parameters cannot be void");
		}

		if (param.type.has(type::q_extern))
		{
			parse_success = false;
			error(param.location, 3006, '\'' + param.name + "': function parameters cannot be declared 'extern'");
		}
		if (param.type.has(type::q_static))
		{
			parse_success = false;
			error(param.location, 3007, '\'' + param.name + "': function parameters cannot be declared 'static'");
		}
		if (param.type.has(type::q_uniform))
		{
			parse_success = false;
			error(param.location, 3047, '\'' + param.name + "': function parameters cannot be declared 'uniform', consider placing in global scope instead");
		}
		if (param.type.has(type::q_groupshared))
		{
			parse_success = false;
			error(param.location, 3010, '\'' + param.name + "': function parameters cannot be declared 'groupshared'");
		}

		if (param.type.has(type::q_out) && param.type.has(type::q_const))
		{
			parse_success = false;
			error(param.location, 3046, '\'' + param.name + "': output parameters cannot be declared 'const'");
		}
		else if (!param.type.has(type::q_out))
		{
			// Function parameters are implicitly 'in' if not explicitly defined as 'out'
			param.type.qualifiers |= type::q_in;
		}

		if (!parse_array_length(param.type))
		{
			parse_success = false;
			expect_parenthesis = false;
			consume_until(')');
			break;
		}

		if (param.type.is_unbounded_array())
		{
			parse_success = false;
			error(param.location, 3072, '\'' + param.name + "': array dimensions of function parameters must be explicit");
			param.type.array_length = 0;
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

		// Handle default argument
		if (accept('='))
		{
			expression default_value_exp;
			if (!parse_expression_multary(default_value_exp))
			{
				parse_success = false;
				expect_parenthesis = false;
				consume_until(')');
				break;
			}

			default_value_exp.add_cast_operation(param.type);

			if (!default_value_exp.is_constant)
			{
				parse_success = false;
				error(default_value_exp.location, 3011, '\'' + param.name + "': value must be a literal expression");
			}

			param.default_value = std::move(default_value_exp.constant);
			param.has_default_value = true;
		}
		else
		{
			if (!info.parameter_list.empty() && info.parameter_list.back().has_default_value)
			{
				parse_success = false;
				error(param.location, 3044, '\'' + name + "': missing default value for parameter '" + param.name + '\'');
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
		{
			error(_token.location, 3076, '\'' + name + "': void function cannot have a semantic");
			return false;
		}

		info.return_semantic = std::move(_token.literal_as_string);
		// Make semantic upper case to simplify comparison later on
		std::transform(info.return_semantic.begin(), info.return_semantic.end(), info.return_semantic.begin(),
			[](std::string::value_type c) {
				return static_cast<std::string::value_type>(std::toupper(c));
			});
	}

	// Check if this is a function declaration without a body
	if (accept(';'))
	{
		error(function_location, 3510, '\'' + name + "': function is missing an implementation");
		return false;
	}

	// Define the function now that information about the declaration was gathered
	const codegen::id id = _codegen->define_function(function_location, info);

	// Insert the function and parameter symbols into the symbol table and update current function pointer to the permanent one
	symbol symbol = { symbol_type::function, id, { type::t_function } };
	symbol.function = &_codegen->get_function(id);

	if (!insert_symbol(name, symbol, true))
	{
		error(function_location, 3003, "redefinition of '" + name + '\'');
		return false;
	}

	for (const member_type &param : info.parameter_list)
	{
		if (!insert_symbol(param.name, { symbol_type::variable, param.id, param.type }))
		{
			error(param.location, 3003, "redefinition of '" + param.name + '\'');
			return false;
		}
	}

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
	const location variable_location = std::move(_token.location);

	if (type.is_void())
	{
		error(variable_location, 3038, '\'' + name + "': variables cannot be void");
		return false;
	}
	if (type.has(type::q_in) || type.has(type::q_out))
	{
		error(variable_location, 3055, '\'' + name + "': variables cannot be declared 'in' or 'out'");
		return false;
	}

	// Local and global variables have different requirements
	if (global)
	{
		// Check that type qualifier combinations are valid
		if (type.has(type::q_static))
		{
			// Global variables that are 'static' cannot be of another storage class
			if (type.has(type::q_uniform))
			{
				error(variable_location, 3007, '\'' + name + "': uniform global variables cannot be declared 'static'");
				return false;
			}
			// The 'volatile' qualifier is only valid memory object declarations that are storage images or uniform blocks
			if (type.has(type::q_volatile))
			{
				error(variable_location, 3008, '\'' + name + "': global variables cannot be declared 'volatile'");
				return false;
			}
		}
		else if (!type.has(type::q_groupshared))
		{
			// Make all global variables 'uniform' by default, since they should be externally visible without the 'static' keyword
			if (!type.has(type::q_uniform) && !type.is_object())
				warning(variable_location, 5000, '\'' + name + "': global variables are considered 'uniform' by default");

			// Global variables that are not 'static' are always 'extern' and 'uniform'
			type.qualifiers |= type::q_extern | type::q_uniform;

			// It is invalid to make 'uniform' variables constant, since they can be modified externally
			if (type.has(type::q_const))
			{
				error(variable_location, 3035, '\'' + name + "': variables which are 'uniform' cannot be declared 'const'");
				return false;
			}
		}
	}
	else
	{
		// Static does not really have meaning on local variables
		if (type.has(type::q_static))
			type.qualifiers &= ~type::q_static;

		if (type.has(type::q_extern))
		{
			error(variable_location, 3006, '\'' + name + "': local variables cannot be declared 'extern'");
			return false;
		}
		if (type.has(type::q_uniform))
		{
			error(variable_location, 3047, '\'' + name + "': local variables cannot be declared 'uniform'");
			return false;
		}
		if (type.has(type::q_groupshared))
		{
			error(variable_location, 3010, '\'' + name + "': local variables cannot be declared 'groupshared'");
			return false;
		}

		if (type.is_object())
		{
			error(variable_location, 3038, '\'' + name + "': local variables cannot be texture, sampler or storage objects");
			return false;
		}
	}

	// The variable name may be followed by an optional array size expression
	if (!parse_array_length(type))
		return false;

	bool parse_success = true;
	expression initializer;
	texture texture_info;
	sampler sampler_info;
	storage storage_info;

	if (accept(':'))
	{
		if (!expect(tokenid::identifier))
			return false;

		if (!global) // Only global variables can have a semantic
		{
			error(_token.location, 3043, '\'' + name + "': local variables cannot have semantics");
			return false;
		}

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
			{
				error(initializer.location, 3009, '\'' + name + "': variables declared 'groupshared' cannot have an initializer");
				return false;
			}

			// TODO: This could be resolved by initializing these at the beginning of the entry point
			if (global && !initializer.is_constant)
			{
				error(initializer.location, 3011, '\'' + name + "': initial value must be a literal expression");
				return false;
			}

			// Check type compatibility
			if ((!type.is_unbounded_array() && initializer.type.array_length != type.array_length) || !type::rank(initializer.type, type))
			{
				error(initializer.location, 3017, '\'' + name + "': initial value (" + initializer.type.description() + ") does not match variable type (" + type.description() + ')');
				return false;
			}
			if ((initializer.type.rows < type.rows || initializer.type.cols < type.cols) && !initializer.type.is_scalar())
			{
				error(initializer.location, 3017, '\'' + name + "': cannot implicitly convert these vector types (from " + initializer.type.description() + " to " + type.description() + ')');
				return false;
			}

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
			{
				error(variable_location, 3012, '\'' + name + "': missing initial value");
				return false;
			}

			if (!type.has(type::q_uniform)) // Zero initialize all global variables
				initializer.reset_to_rvalue_constant(variable_location, {}, type);
		}
		else if (global && accept('{')) // Textures and samplers can have a property block attached to their declaration
		{
			// Non-numeric variables cannot be constants
			if (type.has(type::q_const))
			{
				error(variable_location, 3035, '\'' + name + "': this variable type cannot be declared 'const'");
				return false;
			}

			while (!peek('}'))
			{
				if (!expect(tokenid::identifier))
				{
					consume_until('}');
					return false;
				}

				location property_location = std::move(_token.location);
				const std::string property_name = std::move(_token.literal_as_string);

				if (!expect('='))
				{
					consume_until('}');
					return false;
				}

				backup();

				expression property_exp;

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
						{ "ANISOTROPIC", 0x55 },
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
						property_exp.reset_to_rvalue_constant(_token.location, it->second);
					else // No match found, so rewind to parser state before the identifier was consumed and try parsing it as a normal expression
						restore();
				}

				// Parse right hand side as normal expression if no special enumeration name was matched already
				if (!property_exp.is_constant && !parse_expression_multary(property_exp))
				{
					consume_until('}');
					return false;
				}

				if (property_name == "Texture")
				{
					// Ignore invalid symbols that were added during error recovery
					if (property_exp.base == UINT32_MAX)
					{
						consume_until('}');
						return false;
					}

					if (!property_exp.type.is_texture())
					{
						error(property_exp.location, 3020, "type mismatch, expected texture name");
						consume_until('}');
						return false;
					}

					if (type.is_sampler() || type.is_storage())
					{
						texture &target_info = _codegen->get_texture(property_exp.base);
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
					if (!property_exp.is_constant || !property_exp.type.is_scalar())
					{
						error(property_exp.location, 3538, "value must be a literal scalar expression");
						consume_until('}');
						return false;
					}

					// All states below expect the value to be of an integer type
					property_exp.add_cast_operation({ type::t_int, 1, 1 });
					const int value = property_exp.constant.as_int[0];

					if (value < 0) // There is little use for negative values, so warn in those cases
						warning(property_exp.location, 3571, "negative value specified for property '" + property_name + '\'');

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
							error(property_location, 3004, "unrecognized property '" + property_name + '\'');
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
							// Combine sampler filter components into a single filter enumeration value
							sampler_info.filter = static_cast<filter_mode>((uint32_t(sampler_info.filter) & 0x4F) | ((value & 0x03) << 4) | (value & 0x40));
						else if (property_name == "MagFilter")
							sampler_info.filter = static_cast<filter_mode>((uint32_t(sampler_info.filter) & 0x73) | ((value & 0x03) << 2) | (value & 0x40));
						else if (property_name == "MipFilter")
							sampler_info.filter = static_cast<filter_mode>((uint32_t(sampler_info.filter) & 0x7C) | ((value & 0x03)     ) | (value & 0x40));
						else if (property_name == "MinLOD" || property_name == "MaxMipLevel")
							sampler_info.min_lod = static_cast<float>(value);
						else if (property_name == "MaxLOD")
							sampler_info.max_lod = static_cast<float>(value);
						else if (property_name == "MipLODBias" || property_name == "MipMapLodBias")
							sampler_info.lod_bias = static_cast<float>(value);
						else
							error(property_location, 3004, "unrecognized property '" + property_name + '\'');
					}
					else if (type.is_storage())
					{
						if (property_name == "MipLOD" || property_name == "MipLevel")
							storage_info.level = value > 0 && value < std::numeric_limits<uint16_t>::max() ? static_cast<uint16_t>(value) : 0;
						else
							error(property_location, 3004, "unrecognized property '" + property_name + '\'');
					}
				}

				if (!expect(';'))
				{
					consume_until('}');
					return false;
				}
			}

			if (!expect('}'))
				return false;
		}
	}

	// At this point the array size should be known (either from the declaration or the initializer)
	if (type.is_unbounded_array())
	{
		error(variable_location, 3074, '\'' + name + "': implicit array missing initial value");
		return false;
	}

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

		const codegen::id id = _codegen->define_texture(variable_location, texture_info);
		symbol = { symbol_type::variable, id, type };
	}
	// Samplers are actually combined image samplers
	else if (type.is_sampler())
	{
		assert(global);

		if (sampler_info.texture_name.empty())
		{
			error(variable_location, 3012, '\'' + name + "': missing 'Texture' property");
			return false;
		}
		if (type.texture_dimension() != static_cast<unsigned int>(texture_info.type))
		{
			error(variable_location, 3521, '\'' + name + "': type mismatch between texture and sampler type");
			return false;
		}
		if (sampler_info.srgb && texture_info.format != texture_format::rgba8)
		{
			error(variable_location, 4582, '\'' + name + "': texture does not support sRGB sampling (only textures with RGBA8 format do)");
			return false;
		}

		if (texture_info.format == texture_format::r32i ?
				!type.is_integral() || !type.is_signed() :
			texture_info.format == texture_format::r32u ?
				!type.is_integral() || !type.is_unsigned() :
				!type.is_floating_point())
		{
			error(variable_location, 4582, '\'' + name + "': type mismatch between texture format and sampler element type");
			return false;
		}

		sampler_info.name = name;
		sampler_info.type = type;

		// Add namespace scope to avoid name clashes
		sampler_info.unique_name = 'V' + current_scope().name + name;
		std::replace(sampler_info.unique_name.begin(), sampler_info.unique_name.end(), ':', '_');

		const codegen::id id = _codegen->define_sampler(variable_location, texture_info, sampler_info);
		symbol = { symbol_type::variable, id, type };
	}
	else if (type.is_storage())
	{
		assert(global);

		if (storage_info.texture_name.empty())
		{
			error(variable_location, 3012, '\'' + name + "': missing 'Texture' property");
			return false;
		}
		if (type.texture_dimension() != static_cast<unsigned int>(texture_info.type))
		{
			error(variable_location, 3521, '\'' + name + "': type mismatch between texture and storage type");
			return false;
		}

		if (texture_info.format == texture_format::r32i ?
				!type.is_integral() || !type.is_signed() :
			texture_info.format == texture_format::r32u ?
				!type.is_integral() || !type.is_unsigned() :
				!type.is_floating_point())
		{
			error(variable_location, 4582, '\'' + name + "': type mismatch between texture format and storage element type");
			return false;
		}

		storage_info.name = name;
		storage_info.type = type;

		// Add namespace scope to avoid name clashes
		storage_info.unique_name = 'V' + current_scope().name + name;
		std::replace(storage_info.unique_name.begin(), storage_info.unique_name.end(), ':', '_');

		if (storage_info.level > texture_info.levels - 1)
			storage_info.level = texture_info.levels - 1;

		const codegen::id id = _codegen->define_storage(variable_location, texture_info, storage_info);
		symbol = { symbol_type::variable, id, type };
	}
	// Uniform variables are put into a global uniform buffer structure
	else if (type.has(type::q_uniform))
	{
		assert(global);

		uniform uniform_info;
		uniform_info.name = name;
		uniform_info.type = type;

		uniform_info.annotations = std::move(sampler_info.annotations);

		uniform_info.initializer_value = std::move(initializer.constant);
		uniform_info.has_initializer_value = initializer.is_constant;

		const codegen::id id = _codegen->define_uniform(variable_location, uniform_info);
		symbol = { symbol_type::variable, id, type };
	}
	// All other variables are separate entities
	else
	{
		// Update global variable names to contain the namespace scope to avoid name clashes
		std::string unique_name = global ? 'V' + current_scope().name + name : name;
		std::replace(unique_name.begin(), unique_name.end(), ':', '_');

		symbol = { symbol_type::variable, 0, type };
		symbol.id = _codegen->define_variable(variable_location, type, std::move(unique_name), global,
			// Shared variables cannot have an initializer
			type.has(type::q_groupshared) ? 0 : _codegen->emit_load(initializer));
	}

	// Insert the symbol into the symbol table
	if (!insert_symbol(name, symbol, global))
	{
		error(variable_location, 3003, "redefinition of '" + name + '\'');
		return false;
	}

	return parse_success;
}

bool reshadefx::parser::parse_technique()
{
	if (!expect(tokenid::identifier))
		return false;

	technique info;
	info.name = std::move(_token.literal_as_string);

	bool parse_success = parse_annotations(info.annotations);

	if (!expect('{'))
		return false;

	while (!peek('}'))
	{
		pass pass;
		if (parse_technique_pass(pass))
		{
			info.passes.push_back(std::move(pass));
		}
		else
		{
			parse_success = false;
			if (!peek(tokenid::pass) && !peek('}')) // If there is another pass definition following, try to parse that despite the error
			{
				consume_until('}');
				return false;
			}
		}
	}

	_codegen->define_technique(std::move(info));

	return expect('}') && parse_success;
}
bool reshadefx::parser::parse_technique_pass(pass &info)
{
	if (!expect(tokenid::pass))
		return false;

	const location pass_location = std::move(_token.location);

	// Passes can have an optional name
	if (accept(tokenid::identifier))
		info.name = std::move(_token.literal_as_string);

	bool parse_success = true;
	bool targets_support_srgb = true;
	function vs_info = {}, ps_info = {}, cs_info = {};

	if (!expect('{'))
		return false;

	while (!peek('}'))
	{
		// Parse pass states
		if (!expect(tokenid::identifier))
		{
			consume_until('}');
			return false;
		}

		location state_location = std::move(_token.location);
		const std::string state_name = std::move(_token.literal_as_string);

		if (!expect('='))
		{
			consume_until('}');
			return false;
		}

		const bool is_shader_state = state_name.size() > 6 && state_name.compare(state_name.size() - 6, 6, "Shader") == 0; // VertexShader, PixelShader, ComputeShader, ...
		const bool is_texture_state = state_name.compare(0, 12, "RenderTarget") == 0 && (state_name.size() == 12 || (state_name[12] >= '0' && state_name[12] < '8'));

		// Shader and render target assignment looks up values in the symbol table, so handle those separately from the other states
		if (is_shader_state || is_texture_state)
		{
			std::string identifier;
			scoped_symbol symbol;
			if (!accept_symbol(identifier, symbol))
			{
				consume_until('}');
				return false;
			}

			state_location = std::move(_token.location);

			int num_threads[3] = { 0, 0, 0 };
			if (accept('<'))
			{
				expression x, y, z;
				if (!parse_expression_multary(x, 8) || !expect(',') || !parse_expression_multary(y, 8))
				{
					consume_until('}');
					return false;
				}

				// Parse optional third dimension (defaults to 1)
				z.reset_to_rvalue_constant({}, 1);
				if (accept(',') && !parse_expression_multary(z, 8))
				{
					consume_until('}');
					return false;
				}

				if (!x.is_constant)
				{
					error(x.location, 3011, "value must be a literal expression");
					consume_until('}');
					return false;
				}
				if (!y.is_constant)
				{
					error(y.location, 3011, "value must be a literal expression");
					consume_until('}');
					return false;
				}
				if (!z.is_constant)
				{
					error(z.location, 3011, "value must be a literal expression");
					consume_until('}');
					return false;
				}
				x.add_cast_operation({ type::t_int, 1, 1 });
				y.add_cast_operation({ type::t_int, 1, 1 });
				z.add_cast_operation({ type::t_int, 1, 1 });
				num_threads[0] = x.constant.as_int[0];
				num_threads[1] = y.constant.as_int[0];
				num_threads[2] = z.constant.as_int[0];

				if (!expect('>'))
				{
					consume_until('}');
					return false;
				}
			}

			// Ignore invalid symbols that were added during error recovery
			if (symbol.id != UINT32_MAX)
			{
				if (is_shader_state)
				{
					if (!symbol.id)
					{
						parse_success = false;
						error(state_location, 3501, "undeclared identifier '" + identifier + "', expected function name");
					}
					else if (!symbol.type.is_function())
					{
						parse_success = false;
						error(state_location, 3020, "type mismatch, expected function name");
					}
					else
					{
						// Look up the matching function info for this function definition
						const function &function_info = _codegen->get_function(symbol.id);

						// We potentially need to generate a special entry point function which translates between function parameters and input/output variables
						switch (state_name[0])
						{
						case 'V':
							vs_info = function_info;
							if (vs_info.type != shader_type::unknown && vs_info.type != shader_type::vertex)
							{
								parse_success = false;
								error(state_location, 3020, "type mismatch, expected vertex shader function");
								break;
							}
							vs_info.type = shader_type::vertex;
							_codegen->define_entry_point(vs_info);
							info.vs_entry_point = vs_info.unique_name;
							break;
						case 'P':
							ps_info = function_info;
							if (ps_info.type != shader_type::unknown && ps_info.type != shader_type::pixel)
							{
								parse_success = false;
								error(state_location, 3020, "type mismatch, expected pixel shader function");
								break;
							}
							ps_info.type = shader_type::pixel;
							_codegen->define_entry_point(ps_info);
							info.ps_entry_point = ps_info.unique_name;
							break;
						case 'C':
							cs_info = function_info;
							if (cs_info.type != shader_type::unknown && cs_info.type != shader_type::compute)
							{
								parse_success = false;
								error(state_location, 3020, "type mismatch, expected compute shader function");
								break;
							}
							cs_info.type = shader_type::compute;
							// Only use number of threads from pass when specified, otherwise fall back to number specified on the function definition with an attribute
							if (num_threads[0] != 0)
							{
								cs_info.num_threads[0] = num_threads[0];
								cs_info.num_threads[1] = num_threads[1];
								cs_info.num_threads[2] = num_threads[2];
							}
							else
							{
								cs_info.num_threads[0] = std::max(cs_info.num_threads[0], 1);
								cs_info.num_threads[1] = std::max(cs_info.num_threads[1], 1);
								cs_info.num_threads[2] = std::max(cs_info.num_threads[2], 1);
							}
							_codegen->define_entry_point(cs_info);
							info.cs_entry_point = cs_info.unique_name;
							break;
						}
					}
				}
				else
				{
					assert(is_texture_state);

					if (!symbol.id)
					{
						parse_success = false;
						error(state_location, 3004, "undeclared identifier '" + identifier + "', expected texture name");
					}
					else if (!symbol.type.is_texture())
					{
						parse_success = false;
						error(state_location, 3020, "type mismatch, expected texture name");
					}
					else if (symbol.type.texture_dimension() != 2)
					{
						parse_success = false;
						error(state_location, 3020, "cannot use texture" + std::to_string(symbol.type.texture_dimension()) + "D as render target");
					}
					else
					{
						texture &target_info = _codegen->get_texture(symbol.id);

						if (target_info.semantic.empty())
						{
							// Texture is used as a render target
							target_info.render_target = true;

							// Verify that all render targets in this pass have the same dimensions
							if (info.viewport_width != 0 && info.viewport_height != 0 && (target_info.width != info.viewport_width || target_info.height != info.viewport_height))
							{
								parse_success = false;
								error(state_location, 4545, "cannot use multiple render targets with different texture dimensions (is " + std::to_string(target_info.width) + 'x' + std::to_string(target_info.height) + ", but expected " + std::to_string(info.viewport_width) + 'x' + std::to_string(info.viewport_height) + ')');
							}

							info.viewport_width = target_info.width;
							info.viewport_height = target_info.height;

							const int target_index = state_name.size() > 12 ? (state_name[12] - '0') : 0;
							info.render_target_names[target_index] = target_info.unique_name;

							// Only RGBA8 format supports sRGB writes across all APIs
							if (target_info.format != texture_format::rgba8)
								targets_support_srgb = false;
						}
						else
						{
							parse_success = false;
							error(state_location, 3020, "cannot use texture with semantic as render target");
						}
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

			expression state_exp;

			if (accept(tokenid::identifier)) // Handle special enumeration names for pass states
			{
				// Transform identifier to uppercase to do case-insensitive comparison
				std::transform(_token.literal_as_string.begin(), _token.literal_as_string.end(), _token.literal_as_string.begin(),
					[](std::string::value_type c) {
						return static_cast<std::string::value_type>(std::toupper(c));
					});

				static const std::unordered_map<std::string_view, uint32_t> s_enum_values = {
					{ "NONE", 0 }, { "ZERO", 0 }, { "ONE", 1 },
					{ "ADD", uint32_t(blend_op::add) },
					{ "SUBTRACT", uint32_t(blend_op::subtract) },
					{ "REVSUBTRACT", uint32_t(blend_op::reverse_subtract) },
					{ "MIN", uint32_t(blend_op::min) },
					{ "MAX", uint32_t(blend_op::max) },
					{ "SRCCOLOR", uint32_t(blend_factor::source_color) },
					{ "INVSRCCOLOR", uint32_t(blend_factor::one_minus_source_color) },
					{ "DESTCOLOR", uint32_t(blend_factor::dest_color) },
					{ "INVDESTCOLOR", uint32_t(blend_factor::one_minus_dest_color) },
					{ "SRCALPHA", uint32_t(blend_factor::source_alpha) },
					{ "INVSRCALPHA", uint32_t(blend_factor::one_minus_source_alpha) },
					{ "DESTALPHA", uint32_t(blend_factor::dest_alpha) },
					{ "INVDESTALPHA", uint32_t(blend_factor::one_minus_dest_alpha) },
					{ "KEEP", uint32_t(stencil_op::keep) },
					{ "REPLACE", uint32_t(stencil_op::replace) },
					{ "INVERT", uint32_t(stencil_op::invert) },
					{ "INCR", uint32_t(stencil_op::increment) },
					{ "INCRSAT", uint32_t(stencil_op::increment_saturate) },
					{ "DECR", uint32_t(stencil_op::decrement) },
					{ "DECRSAT", uint32_t(stencil_op::decrement_saturate) },
					{ "NEVER", uint32_t(stencil_func::never) },
					{ "EQUAL", uint32_t(stencil_func::equal) },
					{ "NEQUAL", uint32_t(stencil_func::not_equal) }, { "NOTEQUAL", uint32_t(stencil_func::not_equal)  },
					{ "LESS", uint32_t(stencil_func::less) },
					{ "GREATER", uint32_t(stencil_func::greater) },
					{ "LEQUAL", uint32_t(stencil_func::less_equal) }, { "LESSEQUAL", uint32_t(stencil_func::less_equal) },
					{ "GEQUAL", uint32_t(stencil_func::greater_equal) }, { "GREATEREQUAL", uint32_t(stencil_func::greater_equal) },
					{ "ALWAYS", uint32_t(stencil_func::always) },
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
					state_exp.reset_to_rvalue_constant(_token.location, it->second);
				else // No match found, so rewind to parser state before the identifier was consumed and try parsing it as a normal expression
					restore();
			}

			// Parse right hand side as normal expression if no special enumeration name was matched already
			if (!state_exp.is_constant && !parse_expression_multary(state_exp))
			{
				consume_until('}');
				return false;
			}

			if (!state_exp.is_constant || !state_exp.type.is_scalar())
			{
				parse_success = false;
				error(state_exp.location, 3011, "pass state value must be a literal scalar expression");
			}

			// All states below expect the value to be of an unsigned integer type
			state_exp.add_cast_operation({ type::t_uint, 1, 1 });
			const unsigned int value = state_exp.constant.as_uint[0];

#define SET_STATE_VALUE_INDEXED(name, info_name, value) \
	else if (constexpr size_t name##_len = sizeof(#name) - 1; state_name.compare(0, name##_len, #name) == 0 && \
		(state_name.size() == name##_len || (state_name[name##_len] >= '0' && state_name[name##_len] < ('0' + static_cast<char>(std::size(info.info_name)))))) \
	{ \
		if (state_name.size() != name##_len) \
			info.info_name[state_name[name##_len] - '0'] = (value); \
		else \
			for (int i = 0; i < static_cast<int>(std::size(info.info_name)); ++i) \
				info.info_name[i] = (value); \
	}

			if (state_name == "SRGBWriteEnable")
				info.srgb_write_enable = (value != 0);
			SET_STATE_VALUE_INDEXED(BlendEnable, blend_enable, value != 0)
			else if (state_name == "StencilEnable")
				info.stencil_enable = (value != 0);
			else if (state_name == "ClearRenderTargets")
				info.clear_render_targets = (value != 0);
			SET_STATE_VALUE_INDEXED(ColorWriteMask, render_target_write_mask, value & 0xFF)
			SET_STATE_VALUE_INDEXED(RenderTargetWriteMask, render_target_write_mask, value & 0xFF)
			else if (state_name == "StencilReadMask" || state_name == "StencilMask")
				info.stencil_read_mask = value & 0xFF;
			else if (state_name == "StencilWriteMask")
				info.stencil_write_mask = value & 0xFF;
			SET_STATE_VALUE_INDEXED(BlendOp, color_blend_op, static_cast<blend_op>(value))
			SET_STATE_VALUE_INDEXED(BlendOpAlpha, alpha_blend_op, static_cast<blend_op>(value))
			SET_STATE_VALUE_INDEXED(SrcBlend, source_color_blend_factor, static_cast<blend_factor>(value))
			SET_STATE_VALUE_INDEXED(SrcBlendAlpha, source_alpha_blend_factor, static_cast<blend_factor>(value))
			SET_STATE_VALUE_INDEXED(DestBlend, dest_color_blend_factor, static_cast<blend_factor>(value))
			SET_STATE_VALUE_INDEXED(DestBlendAlpha, dest_alpha_blend_factor, static_cast<blend_factor>(value))
			else if (state_name == "StencilFunc")
				info.stencil_comparison_func = static_cast<stencil_func>(value);
			else if (state_name == "StencilRef")
				info.stencil_reference_value = value;
			else if (state_name == "StencilPass" || state_name == "StencilPassOp")
				info.stencil_pass_op = static_cast<stencil_op>(value);
			else if (state_name == "StencilFail" || state_name == "StencilFailOp")
				info.stencil_fail_op = static_cast<stencil_op>(value);
			else if (state_name == "StencilZFail" || state_name == "StencilDepthFail" || state_name == "StencilDepthFailOp")
				info.stencil_depth_fail_op = static_cast<stencil_op>(value);
			else if (state_name == "VertexCount")
				info.num_vertices = value;
			else if (state_name == "PrimitiveType" || state_name == "PrimitiveTopology")
				info.topology = static_cast<primitive_topology>(value);
			else if (state_name == "DispatchSizeX")
				info.viewport_width = value;
			else if (state_name == "DispatchSizeY")
				info.viewport_height = value;
			else if (state_name == "DispatchSizeZ")
				info.viewport_dispatch_z = value;
			else if (state_name == "GenerateMipmaps" || state_name == "GenerateMipMaps")
				info.generate_mipmaps = (value != 0);
			else
				error(state_location, 3004, "unrecognized pass state '" + state_name + '\'');

#undef SET_STATE_VALUE_INDEXED
		}

		if (!expect(';'))
		{
			consume_until('}');
			return false;
		}
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
				warning(pass_location, 3089, "pass is specifying both 'PixelShader' and 'ComputeShader' which cannot be used together");
		}
		else
		{
			if (info.vs_entry_point.empty())
			{
				parse_success = false;
				error(pass_location, 3012, "pass is missing 'VertexShader' property");
			}

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

			for (const member_type &param : vs_info.parameter_list)
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

			for (const member_type &param : ps_info.parameter_list)
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
					{
						warning(pass_location, 4576, '\'' + ps_info.name + "': input parameter '" + param.name + "' semantic does not match vertex shader one");
					}
					else if (((it->second.qualifiers ^ param.type.qualifiers) & (type::q_linear | type::q_noperspective | type::q_centroid | type::q_nointerpolation)) != 0)
					{
						parse_success = false;
						error(  pass_location, 4568, '\'' + ps_info.name + "': input parameter '" + param.name + "' interpolation qualifiers do not match vertex shader ones");
					}
				}
			}

			for (codegen::id id : vs_info.referenced_samplers)
			{
				const sampler &sampler = _codegen->get_sampler(id);
				if (std::find(std::begin(info.render_target_names), std::end(info.render_target_names), sampler.texture_name) != std::end(info.render_target_names))
					error(pass_location, 3020, '\'' + sampler.texture_name + "': cannot sample from texture that is also used as render target in the same pass");
			}
			for (codegen::id id : ps_info.referenced_samplers)
			{
				const sampler &sampler = _codegen->get_sampler(id);
				if (std::find(std::begin(info.render_target_names), std::end(info.render_target_names), sampler.texture_name) != std::end(info.render_target_names))
					error(pass_location, 3020, '\'' + sampler.texture_name + "': cannot sample from texture that is also used as render target in the same pass");
			}

			if (!vs_info.referenced_storages.empty() || !ps_info.referenced_storages.empty())
			{
				parse_success = false;
				error(pass_location, 3667, "storage writes are only valid in compute shaders");
			}

			// Verify render target format supports sRGB writes if enabled
			if (info.srgb_write_enable && !targets_support_srgb)
			{
				parse_success = false;
				error(pass_location, 4582, "one or more render target(s) do not support sRGB writes (only textures with RGBA8 format do)");
			}
		}
	}

	return expect('}') && parse_success;
}

void reshadefx::codegen::optimize_bindings()
{
	struct sampler_group
	{
		std::vector<id> bindings;
		function *grouped_entry_point = nullptr;
	};
	struct entry_point_info
	{
		std::vector<sampler_group> sampler_groups;

		static void compare_and_update_bindings(std::unordered_map<function *, entry_point_info> &per_entry_point, sampler_group &a, sampler_group &b, size_t binding)
		{
			for (; binding < std::min(a.bindings.size(), b.bindings.size()); ++binding)
			{
				if (a.bindings[binding] != b.bindings[binding])
				{
					if (a.bindings[binding] == 0)
					{
						b.bindings.insert(b.bindings.begin() + binding, 0);

						if (b.grouped_entry_point != nullptr)
							for (sampler_group &c : per_entry_point.at(b.grouped_entry_point).sampler_groups)
								compare_and_update_bindings(per_entry_point, b, c, binding);
						continue;
					}

					if (b.bindings[binding] == 0)
					{
						a.bindings.insert(a.bindings.begin() + binding, 0);

						if (a.grouped_entry_point != nullptr)
							for (sampler_group &c : per_entry_point.at(a.grouped_entry_point).sampler_groups)
								compare_and_update_bindings(per_entry_point, a, c, binding);
						continue;
					}
				}
			}
		}
	};

	std::unordered_map<function *, entry_point_info> per_entry_point;
	for (const auto &[name, type] : _module.entry_points)
	{
		per_entry_point.emplace(&get_function(name), entry_point_info {});
	}

	std::unordered_map<id, int> usage_count;
	for (const auto &[entry_point, entry_point_info] : per_entry_point)
	{
		for (const id sampler_id : entry_point->referenced_samplers)
			usage_count[sampler_id]++;
		for (const id storage_id : entry_point->referenced_storages)
			usage_count[storage_id]++;
	}

	// First sort bindings by usage and for each pass arrange them so that VS and PS use matching bindings for the objects they use (so that the same bindings can be used for both entry points).
	// If the entry points VS1 and PS1 use the following objects A, B and C:
	//   - VS1: A B
	//   - PS1: B C
	// Then this generates the following bindings:
	//   - VS1: C A
	//   - PS1: C 0 B

	const auto usage_pred =
		[&](const id lhs, const id rhs) {
			return usage_count.at(lhs) > usage_count.at(rhs) || (usage_count.at(lhs) == usage_count.at(rhs) && lhs < rhs);
		};

	for (const auto &[entry_point, entry_point_info] : per_entry_point)
	{
		std::sort(entry_point->referenced_samplers.begin(), entry_point->referenced_samplers.end(), usage_pred);
		std::sort(entry_point->referenced_storages.begin(), entry_point->referenced_storages.end(), usage_pred);
	}

	for (const technique &tech : _module.techniques)
	{
		for (const pass &pass : tech.passes)
		{
			if (!pass.cs_entry_point.empty())
			{
				function &cs = get_function(pass.cs_entry_point);

				sampler_group cs_sampler_info;
				cs_sampler_info.bindings = cs.referenced_samplers;
				per_entry_point.at(&cs).sampler_groups.push_back(std::move(cs_sampler_info));
			}
			else
			{
				function &vs = get_function(pass.vs_entry_point);

				sampler_group vs_sampler_info;
				vs_sampler_info.bindings = vs.referenced_samplers;

				if (!pass.ps_entry_point.empty())
				{
					function &ps = get_function(pass.ps_entry_point);

					vs_sampler_info.grouped_entry_point = &ps;

					sampler_group ps_sampler_info;
					ps_sampler_info.bindings = ps.referenced_samplers;
					ps_sampler_info.grouped_entry_point = &vs;

					for (size_t binding = 0; binding < std::min(vs_sampler_info.bindings.size(), ps_sampler_info.bindings.size()); ++binding)
					{
						if (vs_sampler_info.bindings[binding] != ps_sampler_info.bindings[binding])
						{
							if (usage_pred(vs_sampler_info.bindings[binding], ps_sampler_info.bindings[binding]))
								ps_sampler_info.bindings.insert(ps_sampler_info.bindings.begin() + binding, 0);
							else
								vs_sampler_info.bindings.insert(vs_sampler_info.bindings.begin() + binding, 0);
						}
					}

					per_entry_point.at(&ps).sampler_groups.push_back(std::move(ps_sampler_info));
				}

				per_entry_point.at(&vs).sampler_groups.push_back(std::move(vs_sampler_info));
			}
		}
	}

	// Next walk through all entry point groups and shift bindings as needed so that there are no mismatches across passes.
	// If the entry points VS1, PS1 and PS2 use the following bindings (notice the mismatches of VS1 between pass 0 and pass 1, as well as PS2 between pass 1 and pass 2):
	//   - pass 0
	//     - VS1: C A
	//     - PS1: C 0 B
	//   - pass 1
	//     - VS1: C 0 A
	//     - PS2: 0 D A
	//   - pass 2
	//     - VS2: D
	//     - PS2: D A
	// Then this generates the following final bindings:
	//   - pass 0
	//     - VS1: C 0 A
	//     - PS1: C 0 B
	//   - pass 1
	//     - VS1: C 0 A
	//     - PS2: 0 D A
	//   - pass 2
	//     - VS2: 0 D
	//     - PS2: 0 D A

	for (auto &[entry_point, entry_point_info] : per_entry_point)
	{
		while (entry_point_info.sampler_groups.size() > 1)
		{
			entry_point_info::compare_and_update_bindings(per_entry_point, entry_point_info.sampler_groups[0], entry_point_info.sampler_groups[1], 0);
			entry_point_info.sampler_groups.erase(entry_point_info.sampler_groups.begin() + 1);
		}
	}

	for (auto &[entry_point, entry_point_info] : per_entry_point)
	{
		if (entry_point_info.sampler_groups.empty())
			continue;

		entry_point->referenced_samplers = std::move(entry_point_info.sampler_groups[0].bindings);
	}

	// Finally apply the generated bindings to all passes

	for (technique &tech : _module.techniques)
	{
		for (pass &pass : tech.passes)
		{
			std::vector<id> referenced_samplers;
			std::vector<id> referenced_storages;

			if (!pass.cs_entry_point.empty())
			{
				const function &cs = get_function(pass.cs_entry_point);

				referenced_samplers = cs.referenced_samplers;
				referenced_storages = cs.referenced_storages;
			}
			else
			{
				const function &vs = get_function(pass.vs_entry_point);

				referenced_samplers = vs.referenced_samplers;

				if (!pass.ps_entry_point.empty())
				{
					const function &ps = get_function(pass.ps_entry_point);

					if (ps.referenced_samplers.size() > referenced_samplers.size())
						referenced_samplers.resize(ps.referenced_samplers.size());

					for (uint32_t binding = 0; binding < ps.referenced_samplers.size(); ++binding)
						if (ps.referenced_samplers[binding] != 0)
							referenced_samplers[binding] = ps.referenced_samplers[binding];
				}
			}

			for (uint32_t binding = 0; binding < referenced_samplers.size(); ++binding)
			{
				if (referenced_samplers[binding] == 0)
					continue;

				const sampler &sampler = get_sampler(referenced_samplers[binding]);

				texture_binding t;
				t.texture_name = sampler.texture_name;
				t.binding = binding;
				t.srgb = sampler.srgb;
				pass.texture_bindings.push_back(std::move(t));

				if (binding >= _module.num_texture_bindings)
					_module.num_texture_bindings = binding + 1;

				sampler_binding s;
				s.binding = binding;
				s.filter = sampler.filter;
				s.address_u = sampler.address_u;
				s.address_v = sampler.address_v;
				s.address_w = sampler.address_w;
				s.min_lod = sampler.min_lod;
				s.max_lod = sampler.max_lod;
				s.lod_bias = sampler.lod_bias;
				pass.sampler_bindings.push_back(std::move(s));

				if (binding >= _module.num_sampler_bindings)
					_module.num_sampler_bindings = binding + 1;
			}

			for (uint32_t binding = 0; binding < referenced_storages.size(); ++binding)
			{
				if (referenced_storages[binding] == 0)
					continue;

				const storage &storage = get_storage(referenced_storages[binding]);

				storage_binding u;
				u.texture_name = storage.texture_name;
				u.binding = binding;
				u.level = storage.level;
				pass.storage_bindings.push_back(std::move(u));

				if (binding >= _module.num_storage_bindings)
					_module.num_storage_bindings = binding + 1;
			}
		}
	}
}
