/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "effect_module.hpp"
#include <memory> // std::unique_ptr
#include <algorithm> // std::find_if

namespace reshadefx
{
	/// <summary>
	/// A SSA code generation back-end interface for the parser to call into.
	/// </summary>
	class codegen
	{
		friend class parser;

	public:
		/// <summary>
		/// Virtual destructor to guarantee that memory of the implementations deriving from this interface is properly destroyed.
		/// </summary>
		virtual ~codegen() {}

		/// <summary>
		/// Gets the module describing the generated code.
		/// </summary>
		const effect_module &module() const { return _module; }

		/// <summary>
		/// Finalizes and returns the generated code for the entire module (all entry points).
		/// </summary>
		virtual std::basic_string<char> finalize_code() const = 0;
		/// <summary>
		/// Finalizes and returns the generated code for the specified entry point (and no other entry points).
		/// </summary>
		/// <param name="entry_point_name">Name of the entry point function to generate code for.</param>
		virtual std::basic_string<char> finalize_code_for_entry_point(const std::string &entry_point_name) const = 0;

	protected:
		/// <summary>
		/// An opaque ID referring to a SSA value or basic block.
		/// </summary>
		using id = uint32_t;

		/// <summary>
		/// Defines a new struct type.
		/// </summary>
		/// <param name="loc">Source location matching this definition (for debugging).</param>
		/// <param name="info">Description of the type.</param>
		/// <returns>New SSA ID of the type.</returns>
		virtual id define_struct(const location &loc, struct_type &info) = 0;
		/// <summary>
		/// Defines a new texture binding.
		/// </summary>
		/// <param name="loc">Source location matching this definition (for debugging).</param>
		/// <param name="info">Description of the texture object.</param>
		/// <returns>New SSA ID of the binding.</returns>
		virtual id define_texture(const location &loc, texture &info) = 0;
		/// <summary>
		/// Defines a new sampler binding.
		/// </summary>
		/// <param name="loc">Source location matching this definition (for debugging).</param>
		/// <param name="tex_info">Description of the texture this sampler object references.</param>
		/// <param name="info">Description of the sampler object.</param>
		/// <returns>New SSA ID of the binding.</returns>
		virtual id define_sampler(const location &loc, const texture &tex_info, sampler &info) = 0;
		/// <summary>
		/// Defines a new storage binding.
		/// </summary>
		/// <param name="loc">Source location matching this definition (for debugging).</param>
		/// <param name="tex_info">Description of the texture this storage object references.</param>
		/// <param name="info">Description of the storage object.</param>
		/// <returns>New SSA ID of the binding.</returns>
		virtual id define_storage(const location &loc, const texture &tex_info, storage &info) = 0;
		/// <summary>
		/// Defines a new uniform variable.
		/// </summary>
		/// <param name="loc">Source location matching this definition (for debugging).</param>
		/// <param name="info">Description of the uniform variable.</param>
		/// <returns>New SSA ID of the variable.</returns>
		virtual id define_uniform(const location &loc, uniform &info) = 0;
		/// <summary>
		/// Defines a new variable.
		/// </summary>
		/// <param name="loc">Source location matching this definition (for debugging).</param>
		/// <param name="type">Data type of the variable.</param>
		/// <param name="name">Name of the variable.</param>
		/// <param name="global"><c>true</c> if this variable is in global scope, <c>false</c> otherwise.</param>
		/// <param name="initializer_value">SSA ID of an optional initializer value.</param>
		/// <returns>New SSA ID of the variable.</returns>
		virtual id define_variable(const location &loc, const type &type, std::string name = std::string(), bool global = false, id initializer_value = 0) = 0;
		/// <summary>
		/// Defines a new function and its function parameters and make it current.
		/// Any code added after this call is added to this function.
		/// </summary>
		/// <param name="loc">Source location matching this definition (for debugging).</param>
		/// <param name="info">Description of the function.</param>
		/// <returns>New SSA ID of the function.</returns>
		virtual id define_function(const location &loc, function &info) = 0;

		/// <summary>
		/// Defines a new effect technique.
		/// </summary>
		/// <param name="loc">Source location matching this definition (for debugging).</param>
		/// <param name="info">Description of the technique.</param>
		void define_technique(technique &&info) { _module.techniques.push_back(std::move(info)); }
		/// <summary>
		/// Makes a function a shader entry point.
		/// </summary>
		/// <param name="function">Function to use as entry point. May be overwritten to point to a new uniquely generated function.</param>
		virtual void define_entry_point(function &function) = 0;

		/// <summary>
		/// Resolves the access chain and add a load operation to the output.
		/// </summary>
		/// <param name="chain">Access chain pointing to the variable to load from.</param>
		/// <param name="force_new_id">Set to <see langword="true"/> to force this to return a new SSA ID for l-value loads.</param>
		/// <returns>New SSA ID with the loaded value.</returns>
		virtual id emit_load(const expression &chain, bool force_new_id = false) = 0;
		/// <summary>
		/// Resolves the access chain and add a store operation to the output.
		/// </summary>
		/// <param name="chain">Access chain pointing to the variable to store to.</param>
		/// <param name="value">SSA ID of the value to store.</param>
		virtual void emit_store(const expression &chain, id value) = 0;
		/// <summary>
		/// Resolves the access chain, but do not add a load operation. This returns a pointer instead.
		/// </summary>
		/// <param name="chain">Access chain pointing to the variable to resolve.</param>
		/// <param name="chain_index">Output value which is set to the index in the access chain up to which the access chain went.</param>
		/// <returns>New SSA ID with a pointer to the value.</returns>
		virtual id emit_access_chain(const expression &chain, size_t &chain_index) { chain_index = chain.chain.size(); return emit_load(chain); }

		/// <summary>
		/// Creates a SSA constant value.
		/// </summary>
		/// <param name="type">Data type of the constant.</param>
		/// <param name="data">Actual constant data to convert into a SSA ID.</param>
		/// <returns>New SSA ID with the constant value.</returns>
		virtual id emit_constant(const type &type, const constant &data) = 0;
		id emit_constant(const type &data_type, uint32_t value)
		{
			// Create a constant value of the specified type
			constant data = {}; // Initialize to zero, so that components not set below still have a defined value for lookup via std::memcmp
			for (unsigned int i = 0; i < data_type.components(); ++i)
			{
				if (data_type.is_integral())
					data.as_uint[i] = value;
				else
					data.as_float[i] = static_cast<float>(value);
			}
			return emit_constant(data_type, data);
		}

		/// <summary>
		/// Adds an unary operation to the output (built-in operation with one argument).
		/// </summary>
		/// <param name="loc">Source location matching this operation (for debugging).</param>
		/// <param name="op">Unary operator to use.</param>
		/// <param name="type">Data type of the input value.</param>
		/// <param name="val">SSA ID of value to perform the operation on.</param>
		/// <returns>New SSA ID with the result of the operation.</returns>
		virtual id emit_unary_op(const location &loc, tokenid op, const type &type, id val) = 0;
		/// <summary>
		/// Adds a binary operation to the output (built-in operation with two arguments).
		/// </summary>
		/// <param name="loc">Source location matching this operation (for debugging).</param>
		/// <param name="op">Binary operator to use.</param>
		/// <param name="res_type">Data type of the result.</param>
		/// <param name="type">Data type of the input values.</param>
		/// <param name="lhs">SSA ID of the value on the left-hand side of the binary operation.</param>
		/// <param name="rhs">SSA ID of the value on the right-hand side of the binary operation.</param>
		/// <returns>New SSA ID with the result of the operation.</returns>
		virtual id emit_binary_op(const location &loc, tokenid op, const type &res_type, const type &type, id lhs, id rhs) = 0;
		id emit_binary_op(const location &loc, tokenid op, const type &type, id lhs, id rhs) { return emit_binary_op(loc, op, type, type, lhs, rhs); }
		/// <summary>
		/// Adds a ternary operation to the output (built-in operation with three arguments).
		/// </summary>
		/// <param name="loc">Source location matching this operation (for debugging).</param>
		/// <param name="op">Ternary operator to use.</param>
		/// <param name="type">Data type of the input values.</param>
		/// <param name="condition">SSA ID of the condition value of the ternary operation.</param>
		/// <param name="true_value">SSA ID of the first value of the ternary operation.</param>
		/// <param name="false_value">SSA ID of the second value of the ternary operation.</param>
		/// <returns>New SSA ID with the result of the operation.</returns>
		virtual id emit_ternary_op(const location &loc, tokenid op, const type &type, id condition, id true_value, id false_value) = 0;
		/// <summary>
		/// Adds a function call to the output.
		/// </summary>
		/// <param name="loc">Source location matching this operation (for debugging).</param>
		/// <param name="function">SSA ID of the function to call.</param>
		/// <param name="res_type">Data type of the call result.</param>
		/// <param name="args">List of SSA IDs representing the call arguments.</param>
		/// <returns>New SSA ID with the result of the function call.</returns>
		virtual id emit_call(const location &loc, id function, const type &res_type, const std::vector<expression> &args) = 0;
		/// <summary>
		/// Adds an intrinsic function call to the output.
		/// </summary>
		/// <param name="loc">Source location matching this operation (for debugging).</param>
		/// <param name="function">Intrinsic to call.</param>
		/// <param name="res_type">Data type of the call result.</param>
		/// <param name="args">List of SSA IDs representing the call arguments.</param>
		/// <returns>New SSA ID with the result of the function call.</returns>
		virtual id emit_call_intrinsic(const location &loc, id function, const type &res_type, const std::vector<expression> &args) = 0;
		/// <summary>
		/// Adds a type constructor call to the output.
		/// </summary>
		/// <param name="type">Data type to construct.</param>
		/// <param name="args">List of SSA IDs representing the scalar constructor arguments.</param>
		/// <returns>New SSA ID with the constructed value.</returns>
		virtual id emit_construct(const location &loc, const type &type, const std::vector<expression> &args) = 0;

		/// <summary>
		/// Adds a structured branch control flow to the output.
		/// </summary>
		/// <param name="loc">Source location matching this branch (for debugging).</param>
		/// <param name="flags">0 - default, 1 - flatten, 2 - do not flatten</param>
		virtual void emit_if(const location &loc, id condition_value, id condition_block, id true_statement_block, id false_statement_block, unsigned int flags) = 0;
		/// <summary>
		/// Adds a branch control flow with a SSA phi operation to the output.
		/// </summary>
		/// <param name="loc">Source location matching this branch (for debugging).</param>
		/// <returns>New SSA ID with the result of the phi operation.</returns>
		virtual id   emit_phi(const location &loc, id condition_value, id condition_block, id true_value, id true_statement_block, id false_value, id false_statement_block, const type &type) = 0;
		/// <summary>
		/// Adds a structured loop control flow to the output.
		/// </summary>
		/// <param name="loc">Source location matching this loop (for debugging).</param>
		/// <param name="flags">0 - default, 1 - unroll, 2 - do not unroll</param>
		virtual void emit_loop(const location &loc, id condition_value, id prev_block, id header_block, id condition_block, id loop_block, id continue_block, unsigned int flags) = 0;
		/// <summary>
		/// Adds a structured switch control flow to the output.
		/// </summary>
		/// <param name="loc">Source location matching this switch (for debugging).</param>
		/// <param name="flags">0 - default, 1 - flatten, 2 - do not flatten</param>
		virtual void emit_switch(const location &loc, id selector_value, id selector_block, id default_label, id default_block, const std::vector<id> &case_literal_and_labels, const std::vector<id> &case_blocks, unsigned int flags) = 0;

		/// <summary>
		/// Returns <see langword="true"/> if code is currently added to a basic block.
		/// </summary>
		bool is_in_block() const { return _current_block != 0; }
		/// <summary>
		/// Returns <see langword="true"/> if code is currently added to a function.
		/// </summary>
		bool is_in_function() const { return _current_function != nullptr; }

		/// <summary>
		/// Creates a new basic block.
		/// </summary>
		/// <returns>New ID of the basic block.</returns>
		virtual id create_block() { return make_id(); }
		/// <summary>
		/// Overwrites the current block ID.
		/// </summary>
		/// <param name="id">ID of the block to make current.</param>
		/// <returns>ID of the previous basic block.</returns>
		virtual id set_block(id id) = 0;
		/// <summary>
		/// Creates a new basic block and make it current.
		/// </summary>
		/// <param name="id">ID of the basic block to create and make current.</param>
		virtual void enter_block(id id) = 0;
		/// <summary>
		/// Returns from the current basic block and kill the shader invocation.
		/// </summary>
		/// <returns>ID of the current basic block.</returns>
		virtual id leave_block_and_kill() = 0;
		/// <summary>
		/// Returns from the current basic block and hand control flow over to the function call side.
		/// </summary>
		/// <param name="value">Optional SSA ID of a return value.</param>
		/// <returns>ID of the current basic block.</returns>
		virtual id leave_block_and_return(id value = 0) = 0;
		/// <summary>
		/// Diverges the current control flow and enter a switch.
		/// </summary>
		/// <param name="value">SSA ID of the selector value to decide the switch path.</param>
		/// <returns>ID of the current basic block.</returns>
		virtual id leave_block_and_switch(id value, id default_target) = 0;
		/// <summary>
		/// Diverges the current control flow and jump to the specified target block.
		/// </summary>
		/// <param name="target">ID of the basic block to jump to.</param>
		/// <param name="is_continue">Set to <see langword="true"/> if this corresponds to a loop continue statement.</param>
		/// <returns>ID of the current basic block.</returns>
		virtual id leave_block_and_branch(id target, unsigned int loop_flow = 0) = 0;
		/// <summary>
		/// Diverges the current control flow and jump to one of the specified target blocks, depending on the condition.
		/// </summary>
		/// <param name="condition">SSA ID of a value used to choose which path to take.</param>
		/// <param name="true_target">ID of the basic block to jump to when the condition is true.</param>
		/// <param name="false_target">ID of the basic block to jump to when the condition is false.</param>
		/// <returns>ID of the current basic block.</returns>
		virtual id leave_block_and_branch_conditional(id condition, id true_target, id false_target) = 0;

		/// <summary>
		/// Leaves the current function. Any code added after this call is added in the global scope.
		/// </summary>
		virtual void leave_function() = 0;

		/// <summary>
		/// Recalculates sampler and storage bindings to take as little binding space as possible for each entry point.
		/// </summary>
		virtual void optimize_bindings();

		/// <summary>
		/// Looks up an existing struct type.
		/// </summary>
		/// <param name="id">SSA ID of the type to find.</param>
		/// <returns>Reference to the struct description.</returns>
		const struct_type &get_struct(id id) const
		{
			return *std::find_if(_structs.begin(), _structs.end(),
				[id](const struct_type &info) { return info.id == id; });
		}
		/// <summary>
		/// Looks up an existing texture binding.
		/// </summary>
		/// <param name="id">SSA ID of the texture binding to find.</param>
		/// <returns>Reference to the texture description.</returns>
		texture &get_texture(id id)
		{
			return *std::find_if(_module.textures.begin(), _module.textures.end(),
				[id](const texture &info) { return info.id == id; });
		}
		/// <summary>
		/// Looks up an existing sampler binding.
		/// </summary>
		/// <param name="id">SSA ID of the sampler binding to find.</param>
		/// <returns>Reference to the sampler description.</returns>
		const sampler &get_sampler(id id) const
		{
			return *std::find_if(_module.samplers.begin(), _module.samplers.end(),
				[id](const sampler &info) { return info.id == id; });
		}
		/// <summary>
		/// Looks up an existing storage binding.
		/// </summary>
		/// <param name="id">SSA ID of the storage binding to find.</param>
		/// <returns>Reference to the storage description.</returns>
		const storage &get_storage(id id) const
		{
			return *std::find_if(_module.storages.begin(), _module.storages.end(),
				[id](const storage &info) { return info.id == id; });
		}
		/// <summary>
		/// Looks up an existing function definition.
		/// </summary>
		/// <param name="id">SSA ID of the function variable to find.</param>
		/// <returns>Reference to the function description.</returns>
		function &get_function(id id)
		{
			return *std::find_if(_functions.begin(), _functions.end(),
				[id](const std::unique_ptr<function> &info) { return info->id == id; })->get();
		}
		function &get_function(const std::string &unique_name)
		{
			return *std::find_if(_functions.begin(), _functions.end(),
				[&unique_name](const std::unique_ptr<function> &info) { return info->unique_name == unique_name; })->get();
		}

		id make_id() { return _next_id++; }

		effect_module _module;
		std::vector<struct_type> _structs;
		std::vector<std::unique_ptr<function>> _functions;

		id _next_id = 1;
		id _last_block = 0;
		id _current_block = 0;
		function *_current_function = nullptr;
	};

	/// <summary>
	/// Creates a back-end implementation for GLSL code generation.
	/// </summary>
	/// <param name="version">GLSL version to insert at the beginning of the file.</param>
	/// <param name="gles">Generate GLSL ES code instead of core OpenGL.</param>
	/// <param name="vulkan_semantics">Generate GLSL for OpenGL or for Vulkan.</param>
	/// <param name="debug_info">Whether to append debug information like line directives to the generated code.</param>
	/// <param name="uniforms_to_spec_constants">Whether to convert uniform variables to specialization constants.</param>
	/// <param name="enable_16bit_types">Use real 16-bit types for the minimum precision types "min16int", "min16uint" and "min16float".</param>
	/// <param name="flip_vert_y">Insert code to flip the Y component of the output position in vertex shaders.</param>
	codegen *create_codegen_glsl(unsigned version, bool gles, bool vulkan_semantics, bool debug_info, bool uniforms_to_spec_constants, bool enable_16bit_types = false, bool flip_vert_y = false);
	/// <summary>
	/// Creates a back-end implementation for HLSL code generation.
	/// </summary>
	/// <param name="shader_model">The HLSL shader model version (e.g. 30, 41, 50, 60, ...)</param>
	/// <param name="debug_info">Whether to append debug information like line directives to the generated code.</param>
	/// <param name="uniforms_to_spec_constants">Whether to convert uniform variables to specialization constants.</param>
	codegen *create_codegen_hlsl(unsigned int shader_model, bool debug_info, bool uniforms_to_spec_constants);
	/// <summary>
	/// Creates a back-end implementation for SPIR-V code generation.
	/// </summary>
	/// <param name="vulkan_semantics">Generate SPIR-V for OpenGL or for Vulkan.</param>
	/// <param name="debug_info">Whether to append debug information like line directives to the generated code.</param>
	/// <param name="uniforms_to_spec_constants">Whether to convert uniform variables to specialization constants.</param>
	/// <param name="enable_16bit_types">Use real 16-bit types for the minimum precision types "min16int", "min16uint" and "min16float".</param>
	/// <param name="flip_vert_y">Insert code to flip the Y component of the output position in vertex shaders.</param>
	codegen *create_codegen_spirv(bool vulkan_semantics, bool debug_info, bool uniforms_to_spec_constants, bool enable_16bit_types = false, bool flip_vert_y = false, bool discard_is_demote = false);
}
