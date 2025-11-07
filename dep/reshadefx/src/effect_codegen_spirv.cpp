/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include <cassert>
#include <cstring> // std::memcmp
#include <charconv> // std::from_chars
#include <algorithm> // std::find_if, std::max, std::sort
#include <unordered_set>

// Use the C++ variant of the SPIR-V headers
#include <spirv.hpp>
namespace spv {
#include <GLSL.std.450.h>
}

using namespace reshadefx;

inline uint32_t align_up(uint32_t size, uint32_t alignment)
{
	alignment -= 1;
	return ((size + alignment) & ~alignment);
}

/// <summary>
/// A single instruction in a SPIR-V module
/// </summary>
struct spirv_instruction
{
	spv::Op op;
	spv::Id type;
	spv::Id result;
	std::vector<spv::Id> operands;

	explicit spirv_instruction(spv::Op op = spv::OpNop) : op(op), type(0), result(0) {}
	spirv_instruction(spv::Op op, spv::Id result) : op(op), type(result), result(0) {}
	spirv_instruction(spv::Op op, spv::Id type, spv::Id result) : op(op), type(type), result(result) {}

	/// <summary>
	/// Add a single operand to the instruction.
	/// </summary>
	spirv_instruction &add(spv::Id operand)
	{
		operands.push_back(operand);
		return *this;
	}

	/// <summary>
	/// Add a range of operands to the instruction.
	/// </summary>
	template <typename It>
	spirv_instruction &add(It begin, It end)
	{
		operands.insert(operands.end(), begin, end);
		return *this;
	}

	/// <summary>
	/// Add a null-terminated literal UTF-8 string to the instruction.
	/// </summary>
	spirv_instruction &add_string(const char *string)
	{
		uint32_t word;
		do {
			word = 0;
			for (uint32_t i = 0; i < 4 && *string; ++i)
				reinterpret_cast<uint8_t *>(&word)[i] = *string++;
			add(word);
		} while (*string || (word & 0xFF000000));
		return *this;
	}

	/// <summary>
	/// Write this instruction to a SPIR-V module.
	/// </summary>
	/// <param name="output">The output stream to append this instruction to.</param>
	void write(std::basic_string<char> &output) const
	{
		// See https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html
		// 0             | Opcode: The 16 high-order bits are the WordCount of the instruction. The 16 low-order bits are the opcode enumerant.
		// 1             | Optional instruction type <id>
		// .             | Optional instruction Result <id>
		// .             | Operand 1 (if needed)
		// .             | Operand 2 (if needed)
		// ...           | ...
		// WordCount - 1 | Operand N (N is determined by WordCount minus the 1 to 3 words used for the opcode, instruction type <id>, and instruction Result <id>).

		const uint32_t word_count = 1 + (type != 0) + (result != 0) + static_cast<uint32_t>(operands.size());
		write_word(output, (word_count << spv::WordCountShift) | op);

		// Optional instruction type ID
		if (type != 0)
			write_word(output, type);

		// Optional instruction result ID
		if (result != 0)
			write_word(output, result);

		// Write out the operands
		for (const uint32_t operand : operands)
			write_word(output, operand);
	}

	static void write_word(std::basic_string<char> &output, uint32_t word)
	{
		output.insert(output.end(), reinterpret_cast<const char *>(&word), reinterpret_cast<const char *>(&word + 1));
	}

	operator uint32_t() const
	{
		assert(result != 0);

		return result;
	}
};

/// <summary>
/// A list of instructions forming a basic block in the SPIR-V module
/// </summary>
struct spirv_basic_block
{
	std::vector<spirv_instruction> instructions;

	/// <summary>
	/// Append another basic block the end of this one.
	/// </summary>
	void append(const spirv_basic_block &block)
	{
		instructions.insert(instructions.end(), block.instructions.begin(), block.instructions.end());
	}
};

class codegen_spirv final : public codegen
{
	static_assert(sizeof(id) == sizeof(spv::Id), "unexpected SPIR-V id type size");

public:
	codegen_spirv(bool vulkan_semantics, bool debug_info, bool uniforms_to_spec_constants, bool enable_16bit_types, bool flip_vert_y, bool discard_is_demote) :
		_debug_info(debug_info),
		_vulkan_semantics(vulkan_semantics),
		_uniforms_to_spec_constants(uniforms_to_spec_constants),
		_enable_16bit_types(enable_16bit_types),
		_flip_vert_y(flip_vert_y),
		_discard_is_demote(discard_is_demote)
	{
		_glsl_ext = make_id();
	}

private:
	struct type_lookup
	{
		reshadefx::type type;
		bool is_ptr;
		uint32_t array_stride;
		std::pair<spv::StorageClass, spv::ImageFormat> storage;

		friend bool operator==(const type_lookup &lhs, const type_lookup &rhs)
		{
			return lhs.type == rhs.type && lhs.is_ptr == rhs.is_ptr && lhs.array_stride == rhs.array_stride && lhs.storage == rhs.storage;
		}
	};
	struct function_blocks
	{
		spirv_basic_block declaration;
		spirv_basic_block variables;
		spirv_basic_block definition;
		reshadefx::type return_type;
		std::vector<reshadefx::type> param_types;

		friend bool operator==(const function_blocks &lhs, const function_blocks &rhs)
		{
			if (lhs.param_types.size() != rhs.param_types.size())
				return false;
			for (size_t i = 0; i < lhs.param_types.size(); ++i)
				if (!(lhs.param_types[i] == rhs.param_types[i]))
					return false;
			return lhs.return_type == rhs.return_type;
		}
	};

	bool _debug_info = false;
	bool _vulkan_semantics = false;
	bool _uniforms_to_spec_constants = false;
	bool _enable_16bit_types = false;
	bool _flip_vert_y = false;
	bool _discard_is_demote = false;

	spirv_basic_block _entries;
	spirv_basic_block _execution_modes;
	spirv_basic_block _debug_a;
	spirv_basic_block _debug_b;
	spirv_basic_block _annotations;
	spirv_basic_block _types_and_constants;
	spirv_basic_block _variables;

	std::vector<function_blocks> _functions_blocks;
	std::unordered_map<id, spirv_basic_block> _block_data;
	spirv_basic_block *_current_block_data = nullptr;

	spv::Id _glsl_ext = 0;
	spv::Id _global_ubo_type = 0;
	spv::Id _global_ubo_variable = 0;
	std::vector<spv::Id> _global_ubo_types;
	function_blocks *_current_function_blocks = nullptr;

	std::vector<std::pair<type_lookup, spv::Id>> _type_lookup;
	std::vector<std::tuple<type, constant, spv::Id>> _constant_lookup;
	std::vector<std::pair<function_blocks, spv::Id>> _function_type_lookup;
	std::unordered_map<std::string, spv::Id> _string_lookup;
	std::unordered_map<spv::Id, std::pair<spv::StorageClass, spv::ImageFormat>> _storage_lookup;
	std::unordered_map<std::string, uint32_t> _semantic_to_location;

	std::unordered_set<spv::Id> _spec_constants;
	std::unordered_set<spv::Capability> _capabilities;

	void add_location(const location &loc, spirv_basic_block &block)
	{
		if (loc.source.empty() || !_debug_info)
			return;

		spv::Id file;

		if (const auto it = _string_lookup.find(loc.source);
			it != _string_lookup.end())
		{
			file = it->second;
		}
		else
		{
			file =
				add_instruction(spv::OpString, 0, _debug_a)
					.add_string(loc.source.c_str());
			_string_lookup.emplace(loc.source, file);
		}

		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpLine
		add_instruction_without_result(spv::OpLine, block)
			.add(file)
			.add(loc.line)
			.add(loc.column);
	}
	spirv_instruction &add_instruction(spv::Op op, spv::Id type = 0)
	{
		assert(is_in_function() && is_in_block());

		return add_instruction(op, type, *_current_block_data);
	}
	spirv_instruction &add_instruction(spv::Op op, spv::Id type, spirv_basic_block &block)
	{
		spirv_instruction &instruction = add_instruction_without_result(op, block);
		instruction.type = type;
		instruction.result = make_id();
		return instruction;
	}
	spirv_instruction &add_instruction_without_result(spv::Op op)
	{
		assert(is_in_function() && is_in_block());

		return add_instruction_without_result(op, *_current_block_data);
	}
	spirv_instruction &add_instruction_without_result(spv::Op op, spirv_basic_block &block)
	{
		return block.instructions.emplace_back(op);
	}

	void finalize_header_section(std::basic_string<char> &spirv) const
	{
		// Write SPIRV header info
		spirv_instruction::write_word(spirv, spv::MagicNumber);
		spirv_instruction::write_word(spirv, 0x10300); // Force SPIR-V 1.3
		spirv_instruction::write_word(spirv, 0u); // Generator magic number, see https://www.khronos.org/registry/spir-v/api/spir-v.xml
		spirv_instruction::write_word(spirv, _next_id); // Maximum ID
		spirv_instruction::write_word(spirv, 0u); // Reserved for instruction schema

		// All capabilities
		spirv_instruction(spv::OpCapability)
			.add(spv::CapabilityShader) // Implicitly declares the Matrix capability too
			.write(spirv);

		for (const spv::Capability capability : _capabilities)
			spirv_instruction(spv::OpCapability)
				.add(capability)
				.write(spirv);

		// Optional extension instructions
		spirv_instruction(spv::OpExtInstImport, _glsl_ext)
			.add_string("GLSL.std.450") // Import GLSL extension
			.write(spirv);

		// Single required memory model instruction
		spirv_instruction(spv::OpMemoryModel)
			.add(spv::AddressingModelLogical)
			.add(spv::MemoryModelGLSL450)
			.write(spirv);
	}
	void finalize_debug_info_section(std::basic_string<char> &spirv) const
	{
		spirv_instruction(spv::OpSource)
			.add(spv::SourceLanguageUnknown) // ReShade FX is not a reserved token at the moment
			.add(0) // Language version, TODO: Maybe fill in ReShade version here?
			.write(spirv);

		if (_debug_info)
		{
			// All debug instructions
			for (const spirv_instruction &inst : _debug_a.instructions)
				inst.write(spirv);
		}
	}
	void finalize_type_and_constants_section(std::basic_string<char> &spirv) const
	{
		// All type declarations
		for (const spirv_instruction &inst : _types_and_constants.instructions)
			inst.write(spirv);

		// Initialize the UBO type now that all member types are known
		if (_global_ubo_type == 0 || _global_ubo_variable == 0)
			return;

		const id global_ubo_type_ptr = _global_ubo_type + 1;

		spirv_instruction(spv::OpTypeStruct, _global_ubo_type)
			.add(_global_ubo_types.begin(), _global_ubo_types.end())
			.write(spirv);
		spirv_instruction(spv::OpTypePointer, global_ubo_type_ptr)
			.add(spv::StorageClassUniform)
			.add(_global_ubo_type)
			.write(spirv);

		spirv_instruction(spv::OpVariable, global_ubo_type_ptr, _global_ubo_variable)
			.add(spv::StorageClassUniform)
			.write(spirv);
	}

	std::basic_string<char> finalize_code() const override
	{
		std::basic_string<char> spirv;
		finalize_header_section(spirv);

		// All entry point declarations
		for (const spirv_instruction &inst : _entries.instructions)
			inst.write(spirv);

		// All execution mode declarations
		for (const spirv_instruction &inst : _execution_modes.instructions)
			inst.write(spirv);

		finalize_debug_info_section(spirv);

		for (const spirv_instruction& inst : _debug_b.instructions)
			inst.write(spirv);

		// All annotation instructions
		for (const spirv_instruction &inst : _annotations.instructions)
			inst.write(spirv);

		finalize_type_and_constants_section(spirv);

		for (const spirv_instruction &inst : _variables.instructions)
			inst.write(spirv);

		// All function definitions
		for (const function_blocks &func : _functions_blocks)
		{
			if (func.definition.instructions.empty())
				continue;

			for (const spirv_instruction &inst : func.declaration.instructions)
				inst.write(spirv);

			// Grab first label and move it in front of variable declarations
			func.definition.instructions.front().write(spirv);
			assert(func.definition.instructions.front().op == spv::OpLabel);

			for (const spirv_instruction &inst : func.variables.instructions)
				inst.write(spirv);
			for (auto inst_it = func.definition.instructions.begin() + 1; inst_it != func.definition.instructions.end(); ++inst_it)
				inst_it->write(spirv);
		}

		return spirv;
	}
	std::basic_string<char> finalize_code_for_entry_point(const std::string &entry_point_name) const override
	{
		const auto entry_point_it = std::find_if(_functions.begin(), _functions.end(),
			[&entry_point_name](const std::unique_ptr<function> &func) {
				return func->unique_name == entry_point_name;
			});
		if (entry_point_it == _functions.end())
			return {};
		const function &entry_point = *entry_point_it->get();

		const auto write_entry_point = [this](const spirv_instruction& oins, std::basic_string<char>& spirv) {
			assert(oins.operands.size() > 2);
			spirv_instruction nins(oins.op, oins.type, oins.result);
			nins.add(oins.operands[0]);
			nins.add(oins.operands[1]);
			nins.add_string("main");

			size_t param_start_index = 2;
			while (param_start_index < oins.operands.size() && (oins.operands[param_start_index] & 0xFF000000) != 0)
				param_start_index++;

			// skip zero
			param_start_index++;

			for (size_t i = param_start_index; i < oins.operands.size(); i++)
				nins.add(oins.operands[i]);
			nins.write(spirv);
		};

		// Build list of IDs to remove
		std::vector<spv::Id> variables_to_remove;
#if 1
		std::vector<spv::Id> functions_to_remove;
#else
		for (const sampler &info : _module.samplers)
			if (std::find(entry_point.referenced_samplers.begin(), entry_point.referenced_samplers.end(), info.id) == entry_point.referenced_samplers.end())
				variables_to_remove.push_back(info.id);
		for (const storage &info : _module.storages)
			if (std::find(entry_point.referenced_storages.begin(), entry_point.referenced_storages.end(), info.id) == entry_point.referenced_storages.end())
				variables_to_remove.push_back(info.id);
#endif

		std::basic_string<char> spirv;
		finalize_header_section(spirv);

		// The entry point and execution mode declaration
		for (const spirv_instruction &inst : _entries.instructions)
		{
			assert(inst.op == spv::OpEntryPoint);

			// Only add the matching entry point
			if (inst.operands[1] == entry_point.id)
			{
				write_entry_point(inst, spirv);
			}
			else
			{
#if 1
				functions_to_remove.push_back(inst.operands[1]);
#endif
				// Add interface variables to list of variables to remove
				for (uint32_t k = 2 + static_cast<uint32_t>((std::strlen(reinterpret_cast<const char *>(&inst.operands[2])) + 4) / 4); k < inst.operands.size(); ++k)
					variables_to_remove.push_back(inst.operands[k]);
			}
		}

		for (const spirv_instruction &inst : _execution_modes.instructions)
		{
			assert(inst.op == spv::OpExecutionMode);

			// Only add execution mode for the matching entry point
			if (inst.operands[0] == entry_point.id)
			{
				inst.write(spirv);
			}
		}

		finalize_debug_info_section(spirv);

		for (const spirv_instruction &inst : _debug_b.instructions)
		{
			// Remove all names of interface variables and functions for non-matching entry points
			if (std::find(variables_to_remove.begin(), variables_to_remove.end(), inst.operands[0]) != variables_to_remove.end() ||
				std::find(functions_to_remove.begin(), functions_to_remove.end(), inst.operands[0]) != functions_to_remove.end())
				continue;

			inst.write(spirv);
		}

		// All annotation instructions
		for (spirv_instruction inst : _annotations.instructions)
		{
			if (inst.op == spv::OpDecorate)
			{
				// Remove all decorations targeting any of the interface variables for non-matching entry points
				if (std::find(variables_to_remove.begin(), variables_to_remove.end(), inst.operands[0]) != variables_to_remove.end())
					continue;

				// Replace bindings
				if (inst.operands[1] == spv::DecorationBinding)
				{
					if (const auto referenced_sampler_it = std::find(entry_point.referenced_samplers.begin(), entry_point.referenced_samplers.end(), inst.operands[0]);
						referenced_sampler_it != entry_point.referenced_samplers.end())
						inst.operands[2] = static_cast<uint32_t>(std::distance(entry_point.referenced_samplers.begin(), referenced_sampler_it));
					else
					if (const auto referenced_storage_it = std::find(entry_point.referenced_storages.begin(), entry_point.referenced_storages.end(), inst.operands[0]);
						referenced_storage_it != entry_point.referenced_storages.end())
						inst.operands[2] = static_cast<uint32_t>(std::distance(entry_point.referenced_storages.begin(), referenced_storage_it));
				}
			}

			inst.write(spirv);
		}

		finalize_type_and_constants_section(spirv);

		for (const spirv_instruction &inst : _variables.instructions)
		{
			// Remove all declarations of the interface variables for non-matching entry points
			if (inst.op == spv::OpVariable && std::find(variables_to_remove.begin(), variables_to_remove.end(), inst.result) != variables_to_remove.end())
				continue;

			inst.write(spirv);
		}

		// All referenced function definitions
		for (const function_blocks &func : _functions_blocks)
		{
			if (func.definition.instructions.empty())
				continue;

			assert(func.declaration.instructions[func.declaration.instructions[0].op != spv::OpFunction ? 1 : 0].op == spv::OpFunction);
			const spv::Id definition = func.declaration.instructions[func.declaration.instructions[0].op != spv::OpFunction ? 1 : 0].result;

#if 1
			if (std::find(functions_to_remove.begin(), functions_to_remove.end(), definition) != functions_to_remove.end())
#else
			if (struct_definition != entry_point.struct_definition &&
				entry_point.referenced_functions.find(struct_definition) == entry_point.referenced_functions.end())
#endif
				continue;

			for (const spirv_instruction &inst : func.declaration.instructions)
				inst.write(spirv);

			// Grab first label and move it in front of variable declarations
			func.definition.instructions.front().write(spirv);
			assert(func.definition.instructions.front().op == spv::OpLabel);

			for (const spirv_instruction &inst : func.variables.instructions)
				inst.write(spirv);
			for (auto inst_it = func.definition.instructions.begin() + 1; inst_it != func.definition.instructions.end(); ++inst_it)
				inst_it->write(spirv);
		}

		return spirv;
	}

	spv::Id convert_type(type info, bool is_ptr = false, spv::StorageClass storage = spv::StorageClassFunction, spv::ImageFormat format = spv::ImageFormatUnknown, uint32_t array_stride = 0)
	{
		assert(array_stride == 0 || info.is_array());

		// The storage class is only relevant for pointers, so ignore it for other types during lookup
		if (is_ptr == false)
			storage = spv::StorageClassFunction;
		// There cannot be sampler variables that are local to a function, so always assume uniform storage for them
		if (info.is_object())
			storage = spv::StorageClassUniformConstant;
		else
			assert(format == spv::ImageFormatUnknown);

		if (info.is_sampler() || info.is_storage())
			info.rows = info.cols = 1;

		// Fall back to 32-bit types and use relaxed precision decoration instead if 16-bit types are not enabled
		if (!_enable_16bit_types && info.is_numeric() && info.precision() < 32)
			info.base = static_cast<type::datatype>(info.base + 1); // min16int -> int, min16uint -> uint, min16float -> float

		const type_lookup lookup { info, is_ptr, array_stride, { storage, format } };

		if (const auto lookup_it = std::find_if(_type_lookup.begin(), _type_lookup.end(),
				[&lookup](const std::pair<type_lookup, spv::Id> &lookup_entry) { return lookup_entry.first == lookup; });
			lookup_it != _type_lookup.end())
			return lookup_it->second;

		spv::Id type_id, elem_type_id;
		if (is_ptr)
		{
			elem_type_id = convert_type(info, false, storage, format, array_stride);
			type_id =
				add_instruction(spv::OpTypePointer, 0, _types_and_constants)
					.add(storage)
					.add(elem_type_id);
		}
		else if (info.is_array())
		{
			type elem_info = info;
			elem_info.array_length = 0;

			elem_type_id = convert_type(elem_info, false, storage, format);

			// Make sure we don't get any dynamic arrays here
			assert(info.is_bounded_array());

			const spv::Id array_length_id = emit_constant(info.array_length);

			type_id =
				add_instruction(spv::OpTypeArray, 0, _types_and_constants)
					.add(elem_type_id)
					.add(array_length_id);

			if (array_stride != 0)
				add_decoration(type_id, spv::DecorationArrayStride, { array_stride });
		}
		else if (info.is_matrix())
		{
			// Convert MxN matrix to a SPIR-V matrix with M vectors with N elements
			type elem_info = info;
			elem_info.rows = info.cols;
			elem_info.cols = 1;

			elem_type_id = convert_type(elem_info, false, storage, format);

			// Matrix types with just one row are interpreted as if they were a vector type
			if (info.rows == 1)
				return elem_type_id;

			type_id =
				add_instruction(spv::OpTypeMatrix, 0, _types_and_constants)
					.add(elem_type_id)
					.add(info.rows);
		}
		else if (info.is_vector())
		{
			type elem_info = info;
			elem_info.rows = 1;
			elem_info.cols = 1;

			elem_type_id = convert_type(elem_info, false, storage, format);
			type_id =
				add_instruction(spv::OpTypeVector, 0, _types_and_constants)
					.add(elem_type_id)
					.add(info.rows);
		}
		else
		{
			switch (info.base)
			{
			case type::t_void:
				assert(info.rows == 0 && info.cols == 0);
				type_id = add_instruction(spv::OpTypeVoid, 0, _types_and_constants);
				break;
			case type::t_bool:
				assert(info.rows == 1 && info.cols == 1);
				type_id = add_instruction(spv::OpTypeBool, 0, _types_and_constants);
				break;
			case type::t_min16int:
				assert(_enable_16bit_types && info.rows == 1 && info.cols == 1);
				add_capability(spv::CapabilityInt16);
				if (storage == spv::StorageClassInput || storage == spv::StorageClassOutput)
					add_capability(spv::CapabilityStorageInputOutput16);
				type_id =
					add_instruction(spv::OpTypeInt, 0, _types_and_constants)
						.add(16) // Width
						.add(1); // Signedness
				break;
			case type::t_int:
				assert(info.rows == 1 && info.cols == 1);
				type_id =
					add_instruction(spv::OpTypeInt, 0, _types_and_constants)
						.add(32) // Width
						.add(1); // Signedness
				break;
			case type::t_min16uint:
				assert(_enable_16bit_types && info.rows == 1 && info.cols == 1);
				add_capability(spv::CapabilityInt16);
				if (storage == spv::StorageClassInput || storage == spv::StorageClassOutput)
					add_capability(spv::CapabilityStorageInputOutput16);
				type_id =
					add_instruction(spv::OpTypeInt, 0, _types_and_constants)
						.add(16) // Width
						.add(0); // Signedness
				break;
			case type::t_uint:
				assert(info.rows == 1 && info.cols == 1);
				type_id =
					add_instruction(spv::OpTypeInt, 0, _types_and_constants)
						.add(32) // Width
						.add(0); // Signedness
				break;
			case type::t_min16float:
				assert(_enable_16bit_types && info.rows == 1 && info.cols == 1);
				add_capability(spv::CapabilityFloat16);
				if (storage == spv::StorageClassInput || storage == spv::StorageClassOutput)
					add_capability(spv::CapabilityStorageInputOutput16);
				type_id =
					add_instruction(spv::OpTypeFloat, 0, _types_and_constants)
						.add(16); // Width
				break;
			case type::t_float:
				assert(info.rows == 1 && info.cols == 1);
				type_id =
					add_instruction(spv::OpTypeFloat, 0, _types_and_constants)
						.add(32); // Width
				break;
			case type::t_struct:
				assert(info.rows == 0 && info.cols == 0 && info.struct_definition != 0);
				type_id = info.struct_definition;
				break;
			case type::t_sampler1d_int:
			case type::t_sampler1d_uint:
			case type::t_sampler1d_float:
				add_capability(spv::CapabilitySampled1D);
				[[fallthrough]];
			case type::t_sampler2d_int:
			case type::t_sampler2d_uint:
			case type::t_sampler2d_float:
			case type::t_sampler3d_int:
			case type::t_sampler3d_uint:
			case type::t_sampler3d_float:
				elem_type_id = convert_image_type(info, format);
				type_id =
					add_instruction(spv::OpTypeSampledImage, 0, _types_and_constants)
						.add(elem_type_id);
				break;
			case type::t_storage1d_int:
			case type::t_storage1d_uint:
			case type::t_storage1d_float:
				add_capability(spv::CapabilityImage1D);
				[[fallthrough]];
			case type::t_storage2d_int:
			case type::t_storage2d_uint:
			case type::t_storage2d_float:
			case type::t_storage3d_int:
			case type::t_storage3d_uint:
			case type::t_storage3d_float:
				// No format specified for the storage image
				if (format == spv::ImageFormatUnknown)
					add_capability(spv::CapabilityStorageImageWriteWithoutFormat);
				return convert_image_type(info, format);
			default:
				assert(false);
				return 0;
			}
		}

		_type_lookup.push_back({ lookup, type_id });

		return type_id;
	}
	spv::Id convert_type(const function_blocks &info)
	{
		if (const auto lookup_it = std::find_if(_function_type_lookup.begin(), _function_type_lookup.end(),
				[&lookup = info](const std::pair<function_blocks, spv::Id> &lookup_entry) { return lookup_entry.first == lookup; });
			lookup_it != _function_type_lookup.end())
			return lookup_it->second;

		const spv::Id return_type_id = convert_type(info.return_type);
		assert(return_type_id != 0);

		std::vector<spv::Id> param_type_ids;
		param_type_ids.reserve(info.param_types.size());
		for (const type &param_type : info.param_types)
			param_type_ids.push_back(convert_type(param_type, true));

		spirv_instruction &inst = add_instruction(spv::OpTypeFunction, 0, _types_and_constants)
			.add(return_type_id)
			.add(param_type_ids.begin(), param_type_ids.end());

		_function_type_lookup.push_back({ info, inst });

		return inst;
	}
	spv::Id convert_image_type(type info, spv::ImageFormat format = spv::ImageFormatUnknown)
	{
		type elem_info = info;
		elem_info.rows = 1;
		elem_info.cols = 1;

		if (!info.is_numeric())
		{
			if ((info.is_integral() && info.is_signed()) || (format >= spv::ImageFormatRgba32i && format <= spv::ImageFormatR8i))
				elem_info.base = type::t_int;
			else if ((info.is_integral() && info.is_unsigned()) || (format >= spv::ImageFormatRgba32ui && format <= spv::ImageFormatR8ui))
				elem_info.base = type::t_uint;
			else
				elem_info.base = type::t_float;
		}

		type_lookup lookup { info, false, 0u, { spv::StorageClassUniformConstant, format } };
		if (!info.is_storage())
		{
			lookup.type = elem_info;
			lookup.type.base = static_cast<type::datatype>(type::t_texture1d + info.texture_dimension() - 1);
			lookup.type.struct_definition = static_cast<uint32_t>(elem_info.base);
		}

		if (const auto lookup_it = std::find_if(_type_lookup.begin(), _type_lookup.end(),
				[&lookup](const std::pair<type_lookup, spv::Id> &lookup_entry) { return lookup_entry.first == lookup; });
			lookup_it != _type_lookup.end())
			return lookup_it->second;

		spv::Id type_id, elem_type_id = convert_type(elem_info, false, spv::StorageClassUniformConstant);
		type_id =
			add_instruction(spv::OpTypeImage, 0, _types_and_constants)
				.add(elem_type_id) // Sampled Type (always a scalar type)
				.add(spv::Dim1D + info.texture_dimension() - 1)
				.add(0) // Not a depth image
				.add(0) // Not an array
				.add(0) // Not multi-sampled
				.add(info.is_storage() ? 2 : 1) // Used with a sampler or as storage
				.add(format);

		_type_lookup.push_back({ lookup, type_id });

		return type_id;
	}

	uint32_t semantic_to_location(const std::string &semantic, uint32_t max_attributes = 1)
	{
		if (const auto it = _semantic_to_location.find(semantic);
			it != _semantic_to_location.end())
			return it->second;

		// Extract the semantic index from the semantic name (e.g. 2 for "TEXCOORD2")
		size_t digit_index = semantic.size() - 1;
		while (digit_index != 0 && semantic[digit_index] >= '0' && semantic[digit_index] <= '9')
			digit_index--;
		digit_index++;

		const std::string semantic_base = semantic.substr(0, digit_index);

		uint32_t semantic_digit = 0;
		std::from_chars(semantic.c_str() + digit_index, semantic.c_str() + semantic.size(), semantic_digit);

		if (semantic_base == "COLOR" || semantic_base == "SV_TARGET")
			return semantic_digit;

		uint32_t location = static_cast<uint32_t>(_semantic_to_location.size());

		// Now create adjoining location indices for all possible semantic indices belonging to this semantic name
		for (uint32_t a = 0; a < semantic_digit + max_attributes; ++a)
		{
			const auto insert = _semantic_to_location.emplace(semantic_base + std::to_string(a), location + a);
			if (!insert.second)
			{
				assert(a == 0 || (insert.first->second - a) == location);

				// Semantic was already created with a different location index, so need to remap to that
				location = insert.first->second - a;
			}
		}

		return location + semantic_digit;
	}

	spv::BuiltIn semantic_to_builtin(const std::string &semantic, shader_type stype) const
	{
		if (semantic == "SV_POSITION")
			return stype == shader_type::pixel ? spv::BuiltInFragCoord : spv::BuiltInPosition;
		if (semantic == "SV_POINTSIZE")
			return spv::BuiltInPointSize;
		if (semantic == "SV_DEPTH")
			return spv::BuiltInFragDepth;
		if (semantic == "SV_VERTEXID")
			return _vulkan_semantics ? spv::BuiltInVertexIndex : spv::BuiltInVertexId;
		if (semantic == "SV_ISFRONTFACE")
			return spv::BuiltInFrontFacing;
		if (semantic == "SV_GROUPID")
			return spv::BuiltInWorkgroupId;
		if (semantic == "SV_GROUPINDEX")
			return spv::BuiltInLocalInvocationIndex;
		if (semantic == "SV_GROUPTHREADID")
			return spv::BuiltInLocalInvocationId;
		if (semantic == "SV_DISPATCHTHREADID")
			return spv::BuiltInGlobalInvocationId;
		return spv::BuiltInMax;
	}
	spv::ImageFormat format_to_image_format(texture_format format)
	{
		switch (format)
		{
		default:
			assert(false);
			[[fallthrough]];
		case texture_format::unknown:
			return spv::ImageFormatUnknown;
		case texture_format::r8:
			add_capability(spv::CapabilityStorageImageExtendedFormats);
			return spv::ImageFormatR8;
		case texture_format::r16:
			add_capability(spv::CapabilityStorageImageExtendedFormats);
			return spv::ImageFormatR16;
		case texture_format::r16f:
			add_capability(spv::CapabilityStorageImageExtendedFormats);
			return spv::ImageFormatR16f;
		case texture_format::r32i:
			return spv::ImageFormatR32i;
		case texture_format::r32u:
			return spv::ImageFormatR32ui;
		case texture_format::r32f:
			return spv::ImageFormatR32f;
		case texture_format::rg8:
			add_capability(spv::CapabilityStorageImageExtendedFormats);
			return spv::ImageFormatRg8;
		case texture_format::rg16:
			add_capability(spv::CapabilityStorageImageExtendedFormats);
			return spv::ImageFormatRg16;
		case texture_format::rg16f:
			add_capability(spv::CapabilityStorageImageExtendedFormats);
			return spv::ImageFormatRg16f;
		case texture_format::rg32f:
			add_capability(spv::CapabilityStorageImageExtendedFormats);
			return spv::ImageFormatRg32f;
		case texture_format::rgba8:
			return spv::ImageFormatRgba8;
		case texture_format::rgba16:
			add_capability(spv::CapabilityStorageImageExtendedFormats);
			return spv::ImageFormatRgba16;
		case texture_format::rgba16f:
			return spv::ImageFormatRgba16f;
		case texture_format::rgba32f:
			return spv::ImageFormatRgba32f;
		case texture_format::rgb10a2:
			add_capability(spv::CapabilityStorageImageExtendedFormats);
			return spv::ImageFormatRgb10A2;
		}
	}

	void add_name(id id, const char *name)
	{
		if (!_debug_info)
			return;

		assert(name != nullptr);
		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpName
		add_instruction_without_result(spv::OpName, _debug_b)
			.add(id)
			.add_string(name);
	}
	void add_builtin(id id, spv::BuiltIn builtin)
	{
		add_instruction_without_result(spv::OpDecorate, _annotations)
			.add(id)
			.add(spv::DecorationBuiltIn)
			.add(builtin);
	}
	void add_decoration(id id, spv::Decoration decoration, std::initializer_list<uint32_t> values = {})
	{
		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpDecorate
		add_instruction_without_result(spv::OpDecorate, _annotations)
			.add(id)
			.add(decoration)
			.add(values.begin(), values.end());
	}
	void add_member_name(id id, uint32_t member_index, const char *name)
	{
		if (!_debug_info)
			return;

		assert(name != nullptr);
		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpMemberName
		add_instruction_without_result(spv::OpMemberName, _debug_b)
			.add(id)
			.add(member_index)
			.add_string(name);
	}
	void add_member_builtin(id id, uint32_t member_index, spv::BuiltIn builtin)
	{
		add_instruction_without_result(spv::OpMemberDecorate, _annotations)
			.add(id)
			.add(member_index)
			.add(spv::DecorationBuiltIn)
			.add(builtin);
	}
	void add_member_decoration(id id, uint32_t member_index, spv::Decoration decoration, std::initializer_list<uint32_t> values = {})
	{
		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpMemberDecorate
		add_instruction_without_result(spv::OpMemberDecorate, _annotations)
			.add(id)
			.add(member_index)
			.add(decoration)
			.add(values.begin(), values.end());
	}
	void add_capability(spv::Capability capability)
	{
		_capabilities.insert(capability);
	}

	id   define_struct(const location &loc, struct_type &info) override
	{
		// First define all member types to make sure they are declared before the struct type references them
		std::vector<spv::Id> member_types;
		member_types.reserve(info.member_list.size());
		for (const member_type &member : info.member_list)
			member_types.push_back(convert_type(member.type));

		// Afterwards define the actual struct type
		add_location(loc, _types_and_constants);

		const id res = info.id =
			add_instruction(spv::OpTypeStruct, 0, _types_and_constants)
				.add(member_types.begin(), member_types.end());

		if (!info.unique_name.empty())
			add_name(res, info.unique_name.c_str());

		for (uint32_t index = 0; index < info.member_list.size(); ++index)
		{
			const member_type &member = info.member_list[index];

			add_member_name(res, index, member.name.c_str());

			if (!_enable_16bit_types && member.type.is_numeric() && member.type.precision() < 32)
				add_member_decoration(res, index, spv::DecorationRelaxedPrecision);
		}

		_structs.push_back(info);

		return res;
	}
	id   define_texture(const location &, texture &info) override
	{
		const id res = info.id = make_id(); // Need to create an unique ID here too, so that the symbol lookup for textures works

		_module.textures.push_back(info);

		return res;
	}
	id   define_sampler(const location &loc, const texture &, sampler &info) override
	{
		const id res = info.id = define_variable(loc, info.type, info.unique_name.c_str(), spv::StorageClassUniformConstant);

		// Default to a binding index equivalent to the entry in the sampler list (this is later overwritten in 'finalize_code_for_entry_point' to a more optimal placement)
		const uint32_t default_binding = static_cast<uint32_t>(_module.samplers.size());
		add_decoration(res, spv::DecorationBinding, { default_binding });
		add_decoration(res, spv::DecorationDescriptorSet, { 1 });

		_module.samplers.push_back(info);

		return res;
	}
	id   define_storage(const location &loc, const texture &tex_info, storage &info) override
	{
		const id res = info.id = define_variable(loc, info.type, info.unique_name.c_str(), spv::StorageClassUniformConstant, format_to_image_format(tex_info.format));

		// Default to a binding index equivalent to the entry in the storage list (this is later overwritten in 'finalize_code_for_entry_point' to a more optimal placement)
		const uint32_t default_binding = static_cast<uint32_t>(_module.storages.size());
		add_decoration(res, spv::DecorationBinding, { default_binding });
		add_decoration(res, spv::DecorationDescriptorSet, { 2 });

		_module.storages.push_back(info);

		return res;
	}
	id   define_uniform(const location &, uniform &info) override
	{
		if (_uniforms_to_spec_constants && info.has_initializer_value)
		{
			const id res = emit_constant(info.type, info.initializer_value, true);

			add_name(res, info.name.c_str());

			const auto add_spec_constant = [this](const spirv_instruction &inst, const uniform &info, const constant &initializer_value, size_t initializer_offset) {
				assert(inst.op == spv::OpSpecConstant || inst.op == spv::OpSpecConstantTrue || inst.op == spv::OpSpecConstantFalse);

				const uint32_t spec_id = static_cast<uint32_t>(_module.spec_constants.size());
				add_decoration(inst, spv::DecorationSpecId, { spec_id });

				uniform scalar_info = info;
				scalar_info.type.rows = 1;
				scalar_info.type.cols = 1;
				scalar_info.size = 4;
				scalar_info.offset = static_cast<uint32_t>(initializer_offset);
				scalar_info.initializer_value = {};
				scalar_info.initializer_value.as_uint[0] = initializer_value.as_uint[initializer_offset];

				_module.spec_constants.push_back(std::move(scalar_info));
			};

			const spirv_instruction &base_inst = _types_and_constants.instructions.back();
			assert(base_inst == res);

			// External specialization constants need to be scalars
			if (info.type.is_scalar())
			{
				add_spec_constant(base_inst, info, info.initializer_value, 0);
			}
			else
			{
				assert(base_inst.op == spv::OpSpecConstantComposite);

				// Add each individual scalar component of the constant as a separate external specialization constant
				for (size_t i = 0; i < (info.type.is_array() ? base_inst.operands.size() : 1); ++i)
				{
					constant initializer_value = info.initializer_value;
					spirv_instruction elem_inst = base_inst;

					if (info.type.is_array())
					{
						elem_inst = *std::find_if(_types_and_constants.instructions.rbegin(), _types_and_constants.instructions.rend(),
							[operand_id = base_inst.operands[i]](const spirv_instruction &inst) { return inst == operand_id; });

						assert(initializer_value.array_data.size() == base_inst.operands.size());
						initializer_value = initializer_value.array_data[i];
					}

					for (size_t row = 0; row < elem_inst.operands.size(); ++row)
					{
						const spirv_instruction &row_inst = *std::find_if(_types_and_constants.instructions.rbegin(), _types_and_constants.instructions.rend(),
							[operand_id = elem_inst.operands[row]](const spirv_instruction &inst) { return inst == operand_id; });

						if (row_inst.op != spv::OpSpecConstantComposite)
						{
							add_spec_constant(row_inst, info, initializer_value, row);
							continue;
						}

						for (size_t col = 0; col < row_inst.operands.size(); ++col)
						{
							const spirv_instruction &col_inst = *std::find_if(_types_and_constants.instructions.rbegin(), _types_and_constants.instructions.rend(),
								[operand_id = row_inst.operands[col]](const spirv_instruction &inst) { return inst == operand_id; });

							add_spec_constant(col_inst, info, initializer_value, row * info.type.cols + col);
						}
					}
				}
			}

			return res;
		}
		else
		{
			// Create global uniform buffer variable on demand
			if (_global_ubo_type == 0)
			{
				_global_ubo_type = make_id();
				make_id(); // Pointer type for '_global_ubo_type'

				add_decoration(_global_ubo_type, spv::DecorationBlock);
			}
			if (_global_ubo_variable == 0)
			{
				_global_ubo_variable = make_id();

				add_decoration(_global_ubo_variable, spv::DecorationDescriptorSet, { 0 });
				add_decoration(_global_ubo_variable, spv::DecorationBinding, { 0 });
			}

			uint32_t alignment = (info.type.rows == 3 ? 4 : info.type.rows) * 4;
			info.size = info.type.rows * 4;

			uint32_t array_stride = 16;
			const uint32_t matrix_stride = 16;

			if (info.type.is_matrix())
			{
				alignment = matrix_stride;
				info.size = info.type.rows * matrix_stride;
			}
			if (info.type.is_array())
			{
				alignment = array_stride;
				array_stride = align_up(info.size, array_stride);
				// Uniform block rules do not permit anything in the padding of an array
				info.size = array_stride * info.type.array_length;
			}

			info.offset = _module.total_uniform_size;
			info.offset = align_up(info.offset, alignment);
			_module.total_uniform_size = info.offset + info.size;

			type ubo_type = info.type;
			// Convert boolean uniform variables to integer type so that they have a defined size
			if (info.type.is_boolean())
				ubo_type.base = type::t_uint;

			const uint32_t member_index = static_cast<uint32_t>(_global_ubo_types.size());

			// Composite objects in the uniform storage class must be explicitly laid out, which includes array types requiring a stride decoration
			_global_ubo_types.push_back(
				convert_type(ubo_type, false, spv::StorageClassUniform, spv::ImageFormatUnknown, info.type.is_array() ? array_stride : 0u));

			add_member_name(_global_ubo_type, member_index, info.name.c_str());

			add_member_decoration(_global_ubo_type, member_index, spv::DecorationOffset, { info.offset });

			if (info.type.is_matrix())
			{
				// Read matrices in column major layout, even though they are actually row major, to avoid transposing them on every access (since SPIR-V uses column matrices)
				// TODO: This technically only works with square matrices
				add_member_decoration(_global_ubo_type, member_index, spv::DecorationColMajor);
				add_member_decoration(_global_ubo_type, member_index, spv::DecorationMatrixStride, { matrix_stride });
			}

			_module.uniforms.push_back(info);

			return 0xF0000000 | member_index;
		}
	}
	id   define_variable(const location &loc, const type &type, std::string name, bool global, id initializer_value) override
	{
		spv::StorageClass storage = spv::StorageClassFunction;
		if (type.has(type::q_groupshared))
			storage = spv::StorageClassWorkgroup;
		else if (global)
			storage = spv::StorageClassPrivate;

		return define_variable(loc, type, name.c_str(), storage, spv::ImageFormatUnknown, initializer_value);
	}
	id   define_variable(const location &loc, const type &type, const char *name, spv::StorageClass storage, spv::ImageFormat format = spv::ImageFormatUnknown, id initializer_value = 0)
	{
		assert(storage != spv::StorageClassFunction || (_current_function_blocks != nullptr && _current_function != nullptr && !_current_function->unique_name.empty() && (_current_function->unique_name[0] == 'F' || _current_function->unique_name[0] == 'E')));

		spirv_basic_block &block = (storage != spv::StorageClassFunction) ?
			_variables : _current_function_blocks->variables;

		add_location(loc, block);

		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpVariable
		spirv_instruction &inst = add_instruction(spv::OpVariable, convert_type(type, true, storage, format), block);
		inst.add(storage);

		const id res = inst.result;

		if (initializer_value != 0)
		{
			if (storage != spv::StorageClassFunction || /* is_entry_point = */ _current_function->unique_name[0] == 'E')
			{
				// The initializer for variables must be a constant
				inst.add(initializer_value);
			}
			else
			{
				// Only use the variable initializer on global variables, since local variables for e.g. "for" statements need to be assigned in their respective scope and not their declaration
				expression variable;
				variable.reset_to_lvalue(loc, res, type);
				emit_store(variable, initializer_value);
			}
		}

		if (name != nullptr && *name != '\0')
			add_name(res, name);

		if (!_enable_16bit_types && type.is_numeric() && type.precision() < 32)
			add_decoration(res, spv::DecorationRelaxedPrecision);

		_storage_lookup[res] = { storage, format };

		return res;
	}
	id   define_function(const location &loc, function &info) override
	{
		assert(!is_in_function());

		function_blocks &func = _functions_blocks.emplace_back();
		func.return_type = info.return_type;

		for (const member_type &param : info.parameter_list)
			func.param_types.push_back(param.type);

		add_location(loc, func.declaration);

		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpFunction
		const id res = info.id =
			add_instruction(spv::OpFunction, convert_type(info.return_type), func.declaration)
				.add(spv::FunctionControlMaskNone)
				.add(convert_type(func));

		if (!info.name.empty())
			add_name(res, info.name.c_str());

		for (member_type &param : info.parameter_list)
		{
			add_location(param.location, func.declaration);

			param.id = add_instruction(spv::OpFunctionParameter, convert_type(param.type, true), func.declaration);

			add_name(param.id, param.name.c_str());
		}

		_functions.push_back(std::make_unique<function>(info));
		_current_function = _functions.back().get();
		_current_function_blocks = &func;

		return res;
	}

	void define_entry_point(function &func) override
	{
		assert(!func.unique_name.empty() && func.unique_name[0] == 'F');
		func.unique_name[0] = 'E';

		// Modify entry point name so each thread configuration is made separate
		if (func.type == shader_type::compute)
			func.unique_name +=
				'_' + std::to_string(func.num_threads[0]) +
				'_' + std::to_string(func.num_threads[1]) +
				'_' + std::to_string(func.num_threads[2]);

		if (std::find_if(_module.entry_points.begin(), _module.entry_points.end(),
				[&func](const std::pair<std::string, shader_type> &entry_point) {
					return entry_point.first == func.unique_name;
				}) != _module.entry_points.end())
			return;

		_module.entry_points.emplace_back(func.unique_name, func.type);

		spv::Id position_variable = 0;
		spv::Id point_size_variable = 0;
		std::vector<spv::Id> inputs_and_outputs;
		std::vector<expression> call_params;

		// Generate the glue entry point function
		function entry_point = func;
		entry_point.referenced_functions.push_back(func.id);

		// Change function signature to 'void main()'
		entry_point.return_type = { type::t_void };
		entry_point.return_semantic.clear();
		entry_point.parameter_list.clear();

		const id entry_point_definition = define_function({}, entry_point);
		enter_block(create_block());

		const auto create_varying_param = [this, &call_params](const member_type &param) {
			// Initialize all output variables with zero
			const spv::Id variable = define_variable({}, param.type, nullptr, spv::StorageClassFunction, spv::ImageFormatUnknown, emit_constant(param.type, 0u));

			expression &call_param = call_params.emplace_back();
			call_param.reset_to_lvalue({}, variable, param.type);

			return variable;
		};

		const auto create_varying_variable = [this, &inputs_and_outputs, &position_variable, &point_size_variable, stype = func.type](const type &param_type, const std::string &semantic, spv::StorageClass storage, int a = 0) {
			const spv::Id variable = define_variable({}, param_type, nullptr, storage);

			if (const spv::BuiltIn builtin = semantic_to_builtin(semantic, stype);
				builtin != spv::BuiltInMax)
			{
				assert(a == 0); // Built-in variables cannot be arrays

				add_builtin(variable, builtin);

				if (builtin == spv::BuiltInPosition && storage == spv::StorageClassOutput)
					position_variable = variable;
				if (builtin == spv::BuiltInPointSize && storage == spv::StorageClassOutput)
					point_size_variable = variable;
			}
			else
			{
				assert(stype != shader_type::compute); // Compute shaders cannot have custom inputs or outputs

				const uint32_t location = semantic_to_location(semantic, std::max(1u, param_type.array_length));
				add_decoration(variable, spv::DecorationLocation, { location + a });
			}

			if (param_type.has(type::q_noperspective))
				add_decoration(variable, spv::DecorationNoPerspective);
			if (param_type.has(type::q_centroid))
				add_decoration(variable, spv::DecorationCentroid);
			if (param_type.has(type::q_nointerpolation))
				add_decoration(variable, spv::DecorationFlat);

			inputs_and_outputs.push_back(variable);
			return variable;
		};

		// Translate function parameters to input/output variables
		for (const member_type &param : func.parameter_list)
		{
			spv::Id param_var = create_varying_param(param);

			// Create separate input/output variables for "inout" parameters
			if (param.type.has(type::q_in))
			{
				spv::Id param_value = 0;

				// Flatten structure parameters
				if (param.type.is_struct())
				{
					const struct_type &struct_definition = get_struct(param.type.struct_definition);

					type struct_type = param.type;
					const auto array_length = std::max(1u, param.type.array_length);
					struct_type.array_length = 0;

					// Struct arrays need to be flattened into individual elements as well
					std::vector<spv::Id> array_element_ids;
					array_element_ids.reserve(array_length);
					for (unsigned int a = 0; a < array_length; a++)
					{
						std::vector<spv::Id> struct_element_ids;
						struct_element_ids.reserve(struct_definition.member_list.size());
						for (const member_type &member : struct_definition.member_list)
						{
							const spv::Id input_var = create_varying_variable(member.type, member.semantic, spv::StorageClassInput, a);

							param_value =
								add_instruction(spv::OpLoad, convert_type(member.type))
									.add(input_var);
							struct_element_ids.push_back(param_value);
						}

						param_value =
							add_instruction(spv::OpCompositeConstruct, convert_type(struct_type))
								.add(struct_element_ids.begin(), struct_element_ids.end());
						array_element_ids.push_back(param_value);
					}

					if (param.type.is_array())
					{
						// Build the array from all constructed struct elements
						param_value =
							add_instruction(spv::OpCompositeConstruct, convert_type(param.type))
								.add(array_element_ids.begin(), array_element_ids.end());
					}
				}
				else
				{
					const spv::Id input_var = create_varying_variable(param.type, param.semantic, spv::StorageClassInput);

					param_value =
						add_instruction(spv::OpLoad, convert_type(param.type))
							.add(input_var);
				}

				add_instruction_without_result(spv::OpStore)
					.add(param_var)
					.add(param_value);
			}

			if (param.type.has(type::q_out))
			{
				if (param.type.is_struct())
				{
					const struct_type &struct_definition = get_struct(param.type.struct_definition);

					for (unsigned int a = 0, array_length = std::max(1u, param.type.array_length); a < array_length; a++)
					{
						for (const member_type &member : struct_definition.member_list)
						{
							create_varying_variable(member.type, member.semantic, spv::StorageClassOutput, a);
						}
					}
				}
				else
				{
					create_varying_variable(param.type, param.semantic, spv::StorageClassOutput);
				}
			}
		}

		const id call_result = emit_call({}, func.id, func.return_type, call_params);

		for (size_t i = 0, inputs_and_outputs_index = 0; i < func.parameter_list.size(); ++i)
		{
			const member_type &param = func.parameter_list[i];

			if (param.type.has(type::q_out))
			{
				const spv::Id value =
					add_instruction(spv::OpLoad, convert_type(param.type))
						.add(call_params[i].base);

				if (param.type.is_struct())
				{
					const struct_type &struct_definition = get_struct(param.type.struct_definition);

					type struct_type = param.type;
					const auto array_length = std::max(1u, param.type.array_length);
					struct_type.array_length = 0;

					// Skip input variables if this is an "inout" parameter
					if (param.type.has(type::q_in))
						inputs_and_outputs_index += struct_definition.member_list.size() * array_length;

					// Split up struct array into individual struct elements again
					for (unsigned int a = 0; a < array_length; a++)
					{
						spv::Id element_value = value;
						if (param.type.is_array())
						{
							element_value =
								add_instruction(spv::OpCompositeExtract, convert_type(struct_type))
									.add(value)
									.add(a);
						}

						// Split out struct fields into separate output variables again
						for (uint32_t member_index = 0; member_index < struct_definition.member_list.size(); ++member_index)
						{
							const spv::Id member_value =
								add_instruction(spv::OpCompositeExtract, convert_type(struct_definition.member_list[member_index].type))
									.add(element_value)
									.add(member_index);

							add_instruction_without_result(spv::OpStore)
								.add(inputs_and_outputs[inputs_and_outputs_index++])
								.add(member_value);
						}
					}
				}
				else
				{
					// Skip input variable if this is an "inout" parameter (see loop above)
					if (param.type.has(type::q_in))
						inputs_and_outputs_index += 1;

					add_instruction_without_result(spv::OpStore)
						.add(inputs_and_outputs[inputs_and_outputs_index++])
						.add(value);
				}
			}
			else
			{
				// Input parameters do not need to store anything, but increase the input/output variable index
				if (param.type.is_struct())
				{
					const struct_type &struct_definition = get_struct(param.type.struct_definition);
					inputs_and_outputs_index += struct_definition.member_list.size() * std::max(1u, param.type.array_length);
				}
				else
				{
					inputs_and_outputs_index += 1;
				}
			}
		}

		if (func.return_type.is_struct())
		{
			const struct_type &struct_definition = get_struct(func.return_type.struct_definition);

			for (uint32_t member_index = 0; member_index < struct_definition.member_list.size(); ++member_index)
			{
				const member_type &member = struct_definition.member_list[member_index];

				const spv::Id result_var = create_varying_variable(member.type, member.semantic, spv::StorageClassOutput);

				const spv::Id member_result =
					add_instruction(spv::OpCompositeExtract, convert_type(member.type))
						.add(call_result)
						.add(member_index);

				add_instruction_without_result(spv::OpStore)
					.add(result_var)
					.add(member_result);
			}
		}
		else if (!func.return_type.is_void())
		{
			const spv::Id result_var = create_varying_variable(func.return_type, func.return_semantic, spv::StorageClassOutput);

			add_instruction_without_result(spv::OpStore)
				.add(result_var)
				.add(call_result);
		}

		// Add code to flip the output vertically
		if (_flip_vert_y && position_variable != 0 && func.type == shader_type::vertex)
		{
			expression position;
			position.reset_to_lvalue({}, position_variable, { type::t_float, 4, 1 });
			position.add_constant_index_access(1); // Y component

			// gl_Position.y = -gl_Position.y
			emit_store(position,
				emit_unary_op({}, tokenid::minus, { type::t_float, 1, 1 },
					emit_load(position, false)));
		}

#if 0
		// Disabled because it breaks on MacOS/Metal - point size should not be defined for a non-point primitive.
		// Add code that sets the point size to a default value (in case this vertex shader is used with point primitives)
		if (point_size_variable == 0 && func.type == shader_type::vertex)
		{
			create_varying_variable({ type::t_float, 1, 1 }, "SV_POINTSIZE", spv::StorageClassOutput);

			expression point_size;
			point_size.reset_to_lvalue({}, point_size_variable, { type::t_float, 1, 1 });

			// gl_PointSize = 1.0
			emit_store(point_size, emit_constant({ type::t_float, 1, 1 }, 1));
		}
#endif

		leave_block_and_return(0);
		leave_function();

		spv::ExecutionModel model;
		switch (func.type)
		{
		case shader_type::vertex:
			model = spv::ExecutionModelVertex;
			break;
		case shader_type::pixel:
			model = spv::ExecutionModelFragment;
			add_instruction_without_result(spv::OpExecutionMode, _execution_modes)
				.add(entry_point_definition)
				.add(_vulkan_semantics ? spv::ExecutionModeOriginUpperLeft : spv::ExecutionModeOriginLowerLeft);
			break;
		case shader_type::compute:
			model = spv::ExecutionModelGLCompute;
			add_instruction_without_result(spv::OpExecutionMode, _execution_modes)
				.add(entry_point_definition)
				.add(spv::ExecutionModeLocalSize)
				.add(func.num_threads[0])
				.add(func.num_threads[1])
				.add(func.num_threads[2]);
			break;
		default:
			assert(false);
			return;
		}

		add_instruction_without_result(spv::OpEntryPoint, _entries)
			.add(model)
			.add(entry_point_definition)
			.add_string(func.unique_name.c_str())
			.add(inputs_and_outputs.begin(), inputs_and_outputs.end());
	}

	id   emit_load(const expression &exp, bool) override
	{
		if (exp.is_constant) // Constant expressions do not have a complex access chain
			return emit_constant(exp.type, exp.constant);

		size_t i = 0;
		spv::Id result = exp.base;
		type base_type = exp.type;
		bool is_uniform_bool = false;

		if (exp.is_lvalue || !exp.chain.empty())
			add_location(exp.location, *_current_block_data);

		// If a variable is referenced, load the value first
		if (exp.is_lvalue && _spec_constants.find(exp.base) == _spec_constants.end())
		{
			if (!exp.chain.empty())
				base_type = exp.chain[0].from;

			std::pair<spv::StorageClass, spv::ImageFormat> storage = { spv::StorageClassFunction, spv::ImageFormatUnknown };
			if (const auto it = _storage_lookup.find(exp.base);
				it != _storage_lookup.end())
				storage = it->second;

			spirv_instruction *access_chain = nullptr;

			// Check if this is a uniform variable (see 'define_uniform' function above) and dereference it
			if (result & 0xF0000000)
			{
				const uint32_t member_index = result ^ 0xF0000000;

				storage.first = spv::StorageClassUniform;
				is_uniform_bool = base_type.is_boolean();

				if (is_uniform_bool)
					base_type.base = type::t_uint;

				access_chain = &add_instruction(spv::OpAccessChain)
					.add(_global_ubo_variable)
					.add(emit_constant(member_index));
			}

			// Any indexing expressions can be resolved during load with an 'OpAccessChain' already
			if (!exp.chain.empty() && (
				exp.chain[0].op == expression::operation::op_member ||
				exp.chain[0].op == expression::operation::op_dynamic_index ||
				exp.chain[0].op == expression::operation::op_constant_index))
			{
				// Ensure that 'access_chain' cannot get invalidated by calls to 'emit_constant' or 'convert_type'
				assert(_current_block_data != &_types_and_constants);

				// Use access chain from uniform if possible, otherwise create new one
				if (access_chain == nullptr) access_chain =
					&add_instruction(spv::OpAccessChain).add(result); // Base

				// Ignore first index into 1xN matrices, since they were translated to a vector type in SPIR-V
				if (exp.chain[0].from.rows == 1 && exp.chain[0].from.cols > 1)
					i = 1;

				for (; i < exp.chain.size() && (
					exp.chain[i].op == expression::operation::op_member ||
					exp.chain[i].op == expression::operation::op_dynamic_index ||
					exp.chain[i].op == expression::operation::op_constant_index); ++i)
					access_chain->add(exp.chain[i].op == expression::operation::op_dynamic_index ?
						exp.chain[i].index :
						emit_constant(exp.chain[i].index)); // Indexes

				base_type = exp.chain[i - 1].to;
				access_chain->type = convert_type(base_type, true, storage.first, storage.second); // Last type is the result
				result = access_chain->result;
			}
			else if (access_chain != nullptr)
			{
				access_chain->type = convert_type(base_type, true, storage.first, storage.second, base_type.is_array() ? 16u : 0u);
				result = access_chain->result;
			}

			result =
				add_instruction(spv::OpLoad, convert_type(base_type, false, spv::StorageClassFunction, storage.second))
					.add(result); // Pointer
		}

		// Need to convert boolean uniforms which are actually integers in SPIR-V
		if (is_uniform_bool)
		{
			base_type.base = type::t_bool;

			result =
				add_instruction(spv::OpINotEqual, convert_type(base_type))
					.add(result)
					.add(emit_constant(0));
		}

		// Work through all remaining operations in the access chain and apply them to the value
		for (; i < exp.chain.size(); ++i)
		{
			assert(result != 0);
			const expression::operation &op = exp.chain[i];

			switch (op.op)
			{
			case expression::operation::op_cast:
				if (op.from.is_scalar() && !op.to.is_scalar())
				{
					type cast_type = op.to;
					cast_type.base = op.from.base;

					std::vector<expression> args;
					args.reserve(op.to.components());
					for (unsigned int c = 0; c < op.to.components(); ++c)
						args.emplace_back().reset_to_rvalue(exp.location, result, op.from);

					result = emit_construct(exp.location, cast_type, args);
				}

				if (op.from.is_boolean())
				{
					const spv::Id true_constant = emit_constant(op.to, 1);
					const spv::Id false_constant = emit_constant(op.to, 0);

					result =
						add_instruction(spv::OpSelect, convert_type(op.to))
							.add(result) // Condition
							.add(true_constant)
							.add(false_constant);
				}
				else
				{
					spv::Op spv_op = spv::OpNop;
					switch (op.to.base)
					{
					case type::t_bool:
						if (op.from.is_floating_point())
							spv_op = spv::OpFOrdNotEqual;
						else
							spv_op = spv::OpINotEqual;
						// Add instruction to compare value against zero instead of casting
						result =
							add_instruction(spv_op, convert_type(op.to))
								.add(result)
								.add(emit_constant(op.from, 0));
						continue;
					case type::t_min16int:
					case type::t_int:
						if (op.from.is_floating_point())
							spv_op = spv::OpConvertFToS;
						else if (op.from.precision() == op.to.precision())
							spv_op = spv::OpBitcast;
						else if (_enable_16bit_types)
							spv_op = spv::OpSConvert;
						else
							continue; // Do not have to add conversion instruction between min16int/int if 16-bit types are not enabled
						break;
					case type::t_min16uint:
					case type::t_uint:
						if (op.from.is_floating_point())
							spv_op = spv::OpConvertFToU;
						else if (op.from.precision() == op.to.precision())
							spv_op = spv::OpBitcast;
						else if (_enable_16bit_types)
							spv_op = spv::OpUConvert;
						else
							continue;
						break;
					case type::t_min16float:
					case type::t_float:
						if (op.from.is_floating_point() && !_enable_16bit_types)
							continue; // Do not have to add conversion instruction between min16float/float if 16-bit types are not enabled
						else if (op.from.is_floating_point())
							spv_op = spv::OpFConvert;
						else if (op.from.is_signed())
							spv_op = spv::OpConvertSToF;
						else
							spv_op = spv::OpConvertUToF;
						break;
					default:
						assert(false);
					}

					result =
						add_instruction(spv_op, convert_type(op.to))
							.add(result);
				}
				break;
			case expression::operation::op_dynamic_index:
				assert(op.from.is_vector() && op.to.is_scalar());
				result =
					add_instruction(spv::OpVectorExtractDynamic, convert_type(op.to))
						.add(result) // Vector
						.add(op.index); // Index
				break;
			case expression::operation::op_member: // In case of struct return values, which are r-values
			case expression::operation::op_constant_index:
				assert(op.from.is_vector() || op.from.is_matrix() || op.from.is_struct());
				result =
					add_instruction(spv::OpCompositeExtract, convert_type(op.to))
						.add(result)
						.add(op.index); // Literal Index
				break;
			case expression::operation::op_swizzle:
				if (op.to.is_vector())
				{
					if (op.from.is_matrix())
					{
						spv::Id components[4];
						for (int c = 0; c < 4 && op.swizzle[c] >= 0; ++c)
						{
							const unsigned int row = op.swizzle[c] / 4;
							const unsigned int column = op.swizzle[c] - row * 4;

							type scalar_type = op.to;
							scalar_type.rows = 1;
							scalar_type.cols = 1;

							spirv_instruction &inst = add_instruction(spv::OpCompositeExtract, convert_type(scalar_type));
							inst.add(result);
							if (op.from.rows > 1) // Matrix types with a single row are actually vectors, so they don't need the extra index
								inst.add(row);
							inst.add(column);

							components[c] = inst;
						}

						spirv_instruction &inst = add_instruction(spv::OpCompositeConstruct, convert_type(op.to));
						for (int c = 0; c < 4 && op.swizzle[c] >= 0; ++c)
							inst.add(components[c]);
						result = inst;
					}
					else if (op.from.is_vector())
					{
						spirv_instruction &inst = add_instruction(spv::OpVectorShuffle, convert_type(op.to));
						inst.add(result); // Vector 1
						inst.add(result); // Vector 2
						for (int c = 0; c < 4 && op.swizzle[c] >= 0; ++c)
							inst.add(op.swizzle[c]);
						result = inst;
					}
					else
					{
						spirv_instruction &inst = add_instruction(spv::OpCompositeConstruct, convert_type(op.to));
						for (unsigned int c = 0; c < op.to.rows; ++c)
							inst.add(result);
						result = inst;
					}
					break;
				}
				else if (op.from.is_matrix() && op.to.is_scalar())
				{
					assert(op.swizzle[1] < 0);

					spirv_instruction &inst = add_instruction(spv::OpCompositeExtract, convert_type(op.to));
					inst.add(result); // Composite
					if (op.from.rows > 1)
					{
						const unsigned int row = op.swizzle[0] / 4;
						const unsigned int column = op.swizzle[0] - row * 4;
						inst.add(row);
						inst.add(column);
					}
					else
					{
						inst.add(op.swizzle[0]);
					}
					result = inst;
					break;
				}
				else
				{
					assert(false);
					break;
				}
			}
		}

		return result;
	}
	void emit_store(const expression &exp, id value) override
	{
		assert(value != 0 && exp.is_lvalue && !exp.is_constant && !exp.type.is_sampler());

		add_location(exp.location, *_current_block_data);

		size_t i = 0;
		// Any indexing expressions can be resolved with an 'OpAccessChain' already
		spv::Id target = emit_access_chain(exp, i);
		type base_type = exp.chain.empty() ? exp.type : i == 0 ? exp.chain[0].from : exp.chain[i - 1].to;

		// TODO: Complex access chains like float4x4[0].m00m10[0] = 0;
		// Work through all remaining operations in the access chain and apply them to the value
		for (; i < exp.chain.size(); ++i)
		{
			const expression::operation &op = exp.chain[i];
			switch (op.op)
			{
				case expression::operation::op_cast:
				case expression::operation::op_member:
					// These should have been handled above already (and casting does not make sense for a store operation)
					break;
				case expression::operation::op_dynamic_index:
				case expression::operation::op_constant_index:
					assert(false);
					break;
				case expression::operation::op_swizzle:
				{
					spv::Id result =
						add_instruction(spv::OpLoad, convert_type(base_type))
							.add(target); // Pointer

					if (base_type.is_vector())
					{
						spirv_instruction &inst = add_instruction(spv::OpVectorShuffle, convert_type(base_type));
						inst.add(result); // Vector 1
						inst.add(value); // Vector 2

						unsigned int shuffle[4] = { 0, 1, 2, 3 };
						for (unsigned int c = 0; c < base_type.rows; ++c)
							if (op.swizzle[c] >= 0)
								shuffle[op.swizzle[c]] = base_type.rows + c;
						for (unsigned int c = 0; c < base_type.rows; ++c)
							inst.add(shuffle[c]);

						value = inst;
					}
					else if (op.to.is_scalar())
					{
						assert(op.swizzle[1] < 0);

						spirv_instruction &inst = add_instruction(spv::OpCompositeInsert, convert_type(base_type));
						inst.add(value); // Object
						inst.add(result); // Composite

						if (op.from.is_matrix() && op.from.rows > 1)
						{
							const unsigned int row = op.swizzle[0] / 4;
							const unsigned int column = op.swizzle[0] - row * 4;
							inst.add(row);
							inst.add(column);
						}
						else
						{
							inst.add(op.swizzle[0]);
						}

						value = inst;
					}
					else
					{
						// TODO: Implement matrix to vector swizzles
						assert(false);
					}
					break;
				}
			}
		}

		add_instruction_without_result(spv::OpStore)
			.add(target)
			.add(value);
	}
	id   emit_access_chain(const expression &exp, size_t &i) override
	{
		// This function cannot create access chains for uniform variables
		assert((exp.base & 0xF0000000) == 0);

		i = 0;
		if (exp.chain.empty() || (
			exp.chain[0].op != expression::operation::op_member &&
			exp.chain[0].op != expression::operation::op_dynamic_index &&
			exp.chain[0].op != expression::operation::op_constant_index))
			return exp.base;

		std::pair<spv::StorageClass, spv::ImageFormat> storage = { spv::StorageClassFunction, spv::ImageFormatUnknown };
		if (const auto it = _storage_lookup.find(exp.base);
			it != _storage_lookup.end())
			storage = it->second;

		// Ensure that 'access_chain' cannot get invalidated by calls to 'emit_constant' or 'convert_type'
		assert(_current_block_data != &_types_and_constants);

		spirv_instruction *access_chain =
			&add_instruction(spv::OpAccessChain).add(exp.base); // Base

		// Ignore first index into 1xN matrices, since they were translated to a vector type in SPIR-V
		if (exp.chain[0].from.rows == 1 && exp.chain[0].from.cols > 1)
			i = 1;

		for (; i < exp.chain.size() && (
			exp.chain[i].op == expression::operation::op_member ||
			exp.chain[i].op == expression::operation::op_dynamic_index ||
			exp.chain[i].op == expression::operation::op_constant_index); ++i)
			access_chain->add(exp.chain[i].op == expression::operation::op_dynamic_index ?
				exp.chain[i].index :
				emit_constant(exp.chain[i].index)); // Indexes

		access_chain->type = convert_type(exp.chain[i - 1].to, true, storage.first, storage.second); // Last type is the result
		return access_chain->result;
	}

	using codegen::emit_constant;
	id   emit_constant(uint32_t value)
	{
		return emit_constant({ type::t_uint, 1, 1 }, value);
	}
	id   emit_constant(const type &data_type, const constant &data) override
	{
		return emit_constant(data_type, data, false);
	}
	id   emit_constant(const type &data_type, const constant &data, bool spec_constant)
	{
		if (!spec_constant) // Specialization constants cannot reuse other constants
		{
			if (const auto it = std::find_if(_constant_lookup.begin(), _constant_lookup.end(),
					[&data_type, &data](std::tuple<type, constant, spv::Id> &x) {
						if (!(std::get<0>(x) == data_type && std::memcmp(&std::get<1>(x).as_uint[0], &data.as_uint[0], sizeof(uint32_t) * 16) == 0 && std::get<1>(x).array_data.size() == data.array_data.size()))
							return false;
						for (size_t i = 0; i < data.array_data.size(); ++i)
							if (std::memcmp(&std::get<1>(x).array_data[i].as_uint[0], &data.array_data[i].as_uint[0], sizeof(uint32_t) * 16) != 0)
								return false;
						return true;
					});
				it != _constant_lookup.end())
				return std::get<2>(*it); // Reuse existing constant instead of duplicating the definition
		}

		spv::Id result;
		if (data_type.is_array())
		{
			assert(data_type.is_bounded_array()); // Unbounded arrays cannot be constants

			type elem_type = data_type;
			elem_type.array_length = 0;

			std::vector<spv::Id> elements;
			elements.reserve(data_type.array_length);

			// Fill up elements with constant array data
			for (const constant &elem : data.array_data)
				elements.push_back(emit_constant(elem_type, elem, spec_constant));
			// Fill up any remaining elements with a default value (when the array data did not specify them)
			for (size_t i = elements.size(); i < static_cast<size_t>(data_type.array_length); ++i)
				elements.push_back(emit_constant(elem_type, {}, spec_constant));

			result =
				add_instruction(spec_constant ? spv::OpSpecConstantComposite : spv::OpConstantComposite, convert_type(data_type), _types_and_constants)
					.add(elements.begin(), elements.end());
		}
		else if (data_type.is_struct())
		{
			assert(!spec_constant); // Structures cannot be specialization constants

			result = add_instruction(spv::OpConstantNull, convert_type(data_type), _types_and_constants);
		}
		else if (data_type.is_vector() || data_type.is_matrix())
		{
			type elem_type = data_type;
			elem_type.rows = data_type.cols;
			elem_type.cols = 1;

			spv::Id rows[4] = {};

			// Construct matrix constant out of row vector constants
			// Construct vector constant out of scalar constants for each element
			for (unsigned int i = 0; i < data_type.rows; ++i)
			{
				constant row_data = {};
				for (unsigned int k = 0; k < data_type.cols; ++k)
					row_data.as_uint[k] = data.as_uint[i * data_type.cols + k];

				rows[i] = emit_constant(elem_type, row_data, spec_constant);
			}

			if (data_type.rows == 1)
			{
				result = rows[0];
			}
			else
			{
				spirv_instruction &inst = add_instruction(spec_constant ? spv::OpSpecConstantComposite : spv::OpConstantComposite, convert_type(data_type), _types_and_constants);
				for (unsigned int i = 0; i < data_type.rows; ++i)
					inst.add(rows[i]);
				result = inst;
			}
		}
		else if (data_type.is_boolean())
		{
			result = add_instruction(data.as_uint[0] ?
				(spec_constant ? spv::OpSpecConstantTrue : spv::OpConstantTrue) :
				(spec_constant ? spv::OpSpecConstantFalse : spv::OpConstantFalse), convert_type(data_type), _types_and_constants);
		}
		else
		{
			assert(data_type.is_scalar());

			result =
				add_instruction(spec_constant ? spv::OpSpecConstant : spv::OpConstant, convert_type(data_type), _types_and_constants)
					.add(data.as_uint[0]);
		}

		if (spec_constant) // Keep track of all specialization constants
			_spec_constants.insert(result);
		else
			_constant_lookup.push_back({ data_type, data, result });

		return result;
	}

	id   emit_unary_op(const location &loc, tokenid op, const type &res_type, id val) override
	{
		spv::Op spv_op = spv::OpNop;

		switch (op)
		{
		case tokenid::minus:
			spv_op = res_type.is_floating_point() ? spv::OpFNegate : spv::OpSNegate;
			break;
		case tokenid::tilde:
			spv_op = spv::OpNot;
			break;
		case tokenid::exclaim:
			spv_op = spv::OpLogicalNot;
			break;
		default:
			return assert(false), 0;
		}

		add_location(loc, *_current_block_data);

		spirv_instruction &inst = add_instruction(spv_op, convert_type(res_type));
		inst.add(val); // Operand

		return inst;
	}
	id   emit_binary_op(const location &loc, tokenid op, const type &res_type, const type &exp_type, id lhs, id rhs) override
	{
		spv::Op spv_op = spv::OpNop;

		switch (op)
		{
		case tokenid::plus:
		case tokenid::plus_plus:
		case tokenid::plus_equal:
			spv_op = exp_type.is_floating_point() ? spv::OpFAdd : spv::OpIAdd;
			break;
		case tokenid::minus:
		case tokenid::minus_minus:
		case tokenid::minus_equal:
			spv_op = exp_type.is_floating_point() ? spv::OpFSub : spv::OpISub;
			break;
		case tokenid::star:
		case tokenid::star_equal:
			spv_op = exp_type.is_floating_point() ? spv::OpFMul : spv::OpIMul;
			break;
		case tokenid::slash:
		case tokenid::slash_equal:
			spv_op = exp_type.is_floating_point() ? spv::OpFDiv : exp_type.is_signed() ? spv::OpSDiv : spv::OpUDiv;
			break;
		case tokenid::percent:
		case tokenid::percent_equal:
			spv_op = exp_type.is_floating_point() ? spv::OpFRem : exp_type.is_signed() ? spv::OpSRem : spv::OpUMod;
			break;
		case tokenid::caret:
		case tokenid::caret_equal:
			spv_op = spv::OpBitwiseXor;
			break;
		case tokenid::pipe:
		case tokenid::pipe_equal:
			spv_op = spv::OpBitwiseOr;
			break;
		case tokenid::ampersand:
		case tokenid::ampersand_equal:
			spv_op = spv::OpBitwiseAnd;
			break;
		case tokenid::less_less:
		case tokenid::less_less_equal:
			spv_op = spv::OpShiftLeftLogical;
			break;
		case tokenid::greater_greater:
		case tokenid::greater_greater_equal:
			spv_op = exp_type.is_signed() ? spv::OpShiftRightArithmetic : spv::OpShiftRightLogical;
			break;
		case tokenid::pipe_pipe:
			spv_op = spv::OpLogicalOr;
			break;
		case tokenid::ampersand_ampersand:
			spv_op = spv::OpLogicalAnd;
			break;
		case tokenid::less:
			spv_op = exp_type.is_floating_point() ? spv::OpFOrdLessThan :
				exp_type.is_signed() ? spv::OpSLessThan : spv::OpULessThan;
			break;
		case tokenid::less_equal:
			spv_op = exp_type.is_floating_point() ? spv::OpFOrdLessThanEqual :
				exp_type.is_signed() ? spv::OpSLessThanEqual : spv::OpULessThanEqual;
			break;
		case tokenid::greater:
			spv_op = exp_type.is_floating_point() ? spv::OpFOrdGreaterThan :
				exp_type.is_signed() ? spv::OpSGreaterThan : spv::OpUGreaterThan;
			break;
		case tokenid::greater_equal:
			spv_op = exp_type.is_floating_point() ? spv::OpFOrdGreaterThanEqual :
				exp_type.is_signed() ? spv::OpSGreaterThanEqual : spv::OpUGreaterThanEqual;
			break;
		case tokenid::equal_equal:
			spv_op = exp_type.is_floating_point() ? spv::OpFOrdEqual :
				exp_type.is_boolean() ? spv::OpLogicalEqual : spv::OpIEqual;
			break;
		case tokenid::exclaim_equal:
			spv_op = exp_type.is_floating_point() ? spv::OpFOrdNotEqual :
				exp_type.is_boolean() ? spv::OpLogicalNotEqual : spv::OpINotEqual;
			break;
		default:
			return assert(false), 0;
		}

		add_location(loc, *_current_block_data);

		// Binary operators generally only work on scalars and vectors in SPIR-V, so need to apply them to matrices component-wise
		if (exp_type.is_matrix() && exp_type.rows != 1)
		{
			std::vector<spv::Id> ids;
			ids.reserve(exp_type.cols);

			type vector_type = exp_type;
			vector_type.rows = exp_type.cols;
			vector_type.cols = 1;

			for (unsigned int row = 0; row < exp_type.rows; ++row)
			{
				const spv::Id lhs_elem = add_instruction(spv::OpCompositeExtract, convert_type(vector_type))
					.add(lhs)
					.add(row);
				const spv::Id rhs_elem = add_instruction(spv::OpCompositeExtract, convert_type(vector_type))
					.add(rhs)
					.add(row);

				spirv_instruction &inst = add_instruction(spv_op, convert_type(vector_type));
				inst.add(lhs_elem); // Operand 1
				inst.add(rhs_elem); // Operand 2

				if (res_type.has(type::q_precise))
					add_decoration(inst, spv::DecorationNoContraction);
				if (!_enable_16bit_types && res_type.precision() < 32)
					add_decoration(inst, spv::DecorationRelaxedPrecision);

				ids.push_back(inst);
			}

			spirv_instruction &inst = add_instruction(spv::OpCompositeConstruct, convert_type(res_type));
			inst.add(ids.begin(), ids.end());

			return inst;
		}
		else
		{
			spirv_instruction &inst = add_instruction(spv_op, convert_type(res_type));
			inst.add(lhs); // Operand 1
			inst.add(rhs); // Operand 2

			if (res_type.has(type::q_precise))
				add_decoration(inst, spv::DecorationNoContraction);
			if (!_enable_16bit_types && res_type.precision() < 32)
				add_decoration(inst, spv::DecorationRelaxedPrecision);

			return inst;
		}
	}
	id   emit_ternary_op(const location &loc, tokenid op, const type &res_type, id condition, id true_value, id false_value) override
	{
		if (op != tokenid::question)
			return assert(false), 0;

		add_location(loc, *_current_block_data);

		spirv_instruction &inst = add_instruction(spv::OpSelect, convert_type(res_type));
		inst.add(condition); // Condition
		inst.add(true_value); // Object 1
		inst.add(false_value); // Object 2

		return inst;
	}
	id   emit_call(const location &loc, id function, const type &res_type, const std::vector<expression> &args) override
	{
#ifndef NDEBUG
		for (const expression &arg : args)
			assert(arg.chain.empty() && arg.base != 0);
#endif
		add_location(loc, *_current_block_data);

		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpFunctionCall
		spirv_instruction &inst = add_instruction(spv::OpFunctionCall, convert_type(res_type));
		inst.add(function); // Function
		for (const expression &arg : args)
			inst.add(arg.base); // Arguments

		return inst;
	}
	id   emit_call_intrinsic(const location &loc, id intrinsic, const type &res_type, const std::vector<expression> &args) override
	{
#ifndef NDEBUG
		for (const expression &arg : args)
			assert(arg.chain.empty() && arg.base != 0);
#endif
		add_location(loc, *_current_block_data);

		enum
		{
		#define IMPLEMENT_INTRINSIC_SPIRV(name, i, code) name##i,
			#include "effect_symbol_table_intrinsics.inl"
		};

		switch (intrinsic)
		{
		#define IMPLEMENT_INTRINSIC_SPIRV(name, i, code) case name##i: code
			#include "effect_symbol_table_intrinsics.inl"
		default:
			return assert(false), 0;
		}
	}
	id   emit_construct(const location &loc, const type &res_type, const std::vector<expression> &args) override
	{
#ifndef NDEBUG
		for (const expression &arg : args)
			assert((arg.type.is_scalar() || res_type.is_array()) && arg.chain.empty() && arg.base != 0);
#endif
		add_location(loc, *_current_block_data);

		std::vector<spv::Id> ids;
		ids.reserve(args.size());

		// There must be exactly one constituent for each top-level component of the result
		if (res_type.is_matrix())
		{
			type vector_type = res_type;
			vector_type.rows = res_type.cols;
			vector_type.cols = 1;

			// Turn the list of scalar arguments into a list of column vectors
			for (size_t arg = 0; arg < args.size(); arg += vector_type.rows)
			{
				spirv_instruction &inst = add_instruction(spv::OpCompositeConstruct, convert_type(vector_type));
				for (unsigned row = 0; row < vector_type.rows; ++row)
					inst.add(args[arg + row].base);

				ids.push_back(inst);
			}
		}
		else
		{
			assert(res_type.is_vector() || res_type.is_array());

			// The exception is that for constructing a vector, a contiguous subset of the scalars consumed can be represented by a vector operand instead
			for (const expression &arg : args)
				ids.push_back(arg.base);
		}

		spirv_instruction &inst = add_instruction(spv::OpCompositeConstruct, convert_type(res_type));
		inst.add(ids.begin(), ids.end());

		return inst;
	}

	void emit_if(const location &loc, id, id condition_block, id true_statement_block, id false_statement_block, unsigned int selection_control) override
	{
		spirv_instruction merge_label = _current_block_data->instructions.back();
		assert(merge_label.op == spv::OpLabel);
		_current_block_data->instructions.pop_back();

		// Add previous block containing the condition value first
		_current_block_data->append(_block_data[condition_block]);

		spirv_instruction branch_inst = _current_block_data->instructions.back();
		assert(branch_inst.op == spv::OpBranchConditional);
		_current_block_data->instructions.pop_back();

		// Add structured control flow instruction
		add_location(loc, *_current_block_data);
		add_instruction_without_result(spv::OpSelectionMerge)
			.add(merge_label)
			.add(selection_control & 0x3); // 'SelectionControl' happens to match the flags produced by the parser

		// Append all blocks belonging to the branch
		_current_block_data->instructions.push_back(branch_inst);
		_current_block_data->append(_block_data[true_statement_block]);
		_current_block_data->append(_block_data[false_statement_block]);

		_current_block_data->instructions.push_back(merge_label);
	}
	id   emit_phi(const location &loc, id, id condition_block, id true_value, id true_statement_block, id false_value, id false_statement_block, const type &res_type) override
	{
		spirv_instruction merge_label = _current_block_data->instructions.back();
		assert(merge_label.op == spv::OpLabel);
		_current_block_data->instructions.pop_back();

		// Add previous block containing the condition value first
		_current_block_data->append(_block_data[condition_block]);

		if (true_statement_block != condition_block)
			_current_block_data->append(_block_data[true_statement_block]);
		if (false_statement_block != condition_block)
			_current_block_data->append(_block_data[false_statement_block]);

		_current_block_data->instructions.push_back(merge_label);

		add_location(loc, *_current_block_data);

		// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#OpPhi
		spirv_instruction &inst = add_instruction(spv::OpPhi, convert_type(res_type))
			.add(true_value) // Variable 0
			.add(true_statement_block) // Parent 0
			.add(false_value) // Variable 1
			.add(false_statement_block); // Parent 1

		return inst;
	}
	void emit_loop(const location &loc, id, id prev_block, id header_block, id condition_block, id loop_block, id continue_block, unsigned int loop_control) override
	{
		spirv_instruction merge_label = _current_block_data->instructions.back();
		assert(merge_label.op == spv::OpLabel);
		_current_block_data->instructions.pop_back();

		// Add previous block first
		_current_block_data->append(_block_data[prev_block]);

		// Fill header block
		assert(_block_data[header_block].instructions.size() == 2);
		_current_block_data->instructions.push_back(_block_data[header_block].instructions[0]);
		assert(_current_block_data->instructions.back().op == spv::OpLabel);

		// Add structured control flow instruction
		add_location(loc, *_current_block_data);
		add_instruction_without_result(spv::OpLoopMerge)
			.add(merge_label)
			.add(continue_block)
			.add(loop_control & 0x3); // 'LoopControl' happens to match the flags produced by the parser

		_current_block_data->instructions.push_back(_block_data[header_block].instructions[1]);
		assert(_current_block_data->instructions.back().op == spv::OpBranch);

		// Add condition block if it exists
		if (condition_block != 0)
			_current_block_data->append(_block_data[condition_block]);

		// Append loop body block before continue block
		_current_block_data->append(_block_data[loop_block]);
		_current_block_data->append(_block_data[continue_block]);

		_current_block_data->instructions.push_back(merge_label);
	}
	void emit_switch(const location &loc, id, id selector_block, id default_label, id default_block, const std::vector<id> &case_literal_and_labels, const std::vector<id> &case_blocks, unsigned int selection_control) override
	{
		assert(case_blocks.size() == case_literal_and_labels.size() / 2);

		spirv_instruction merge_label = _current_block_data->instructions.back();
		assert(merge_label.op == spv::OpLabel);
		_current_block_data->instructions.pop_back();

		// Add previous block containing the selector value first
		_current_block_data->append(_block_data[selector_block]);

		spirv_instruction switch_inst = _current_block_data->instructions.back();
		assert(switch_inst.op == spv::OpSwitch);
		_current_block_data->instructions.pop_back();

		// Add structured control flow instruction
		add_location(loc, *_current_block_data);
		add_instruction_without_result(spv::OpSelectionMerge)
			.add(merge_label)
			.add(selection_control & 0x3); // 'SelectionControl' happens to match the flags produced by the parser

		// Update switch instruction to contain all case labels
		switch_inst.operands[1] = default_label;
		switch_inst.add(case_literal_and_labels.begin(), case_literal_and_labels.end());

		// Append all blocks belonging to the switch
		_current_block_data->instructions.push_back(switch_inst);

		std::vector<id> blocks = case_blocks;
		if (default_label != merge_label)
			blocks.push_back(default_block);
		// Eliminate duplicates (because of multiple case labels pointing to the same block)
		std::sort(blocks.begin(), blocks.end());
		blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
		for (const id case_block : blocks)
			_current_block_data->append(_block_data[case_block]);

		_current_block_data->instructions.push_back(merge_label);
	}

	bool is_in_function() const { return _current_function_blocks != nullptr; }

	id   set_block(id id) override
	{
		_last_block = _current_block;
		_current_block = id;
		_current_block_data = &_block_data[id];

		return _last_block;
	}
	void enter_block(id id) override
	{
		assert(id != 0);
		// Can only use labels inside functions and should never be in another basic block if creating a new one
		assert(is_in_function() && !is_in_block());

		set_block(id);

		add_instruction_without_result(spv::OpLabel).result = id;
	}
	id   leave_block_and_kill() override
	{
		assert(is_in_function()); // Can only discard inside functions

		if (!is_in_block())
			return 0;

		// DXC chokes when discarding inside a function. Return a null value and use demote instead, since that's
		// what the HLSL discard instruction compiles to anyway.
		if (!_discard_is_demote || _current_function_blocks->return_type.is_void())
		{
			add_instruction_without_result(spv::OpKill);
		}
		else
		{
			add_instruction_without_result(spv::OpDemoteToHelperInvocation);

			const id return_id = emit_constant(_current_function_blocks->return_type, constant{}, false);
			add_instruction_without_result(spv::OpReturnValue).add(return_id);
		}

		return set_block(0);
	}
	id   leave_block_and_return(id value) override
	{
		assert(is_in_function()); // Can only return from inside functions

		if (!is_in_block()) // Might already have left the last block in which case this has to be ignored
			return 0;

		if (_current_function_blocks->return_type.is_void())
		{
			add_instruction_without_result(spv::OpReturn);
		}
		else
		{
			if (0 == value) // The implicit return statement needs this
				value = add_instruction(spv::OpUndef, convert_type(_current_function_blocks->return_type), _types_and_constants);

			add_instruction_without_result(spv::OpReturnValue)
				.add(value);
		}

		return set_block(0);
	}
	id   leave_block_and_switch(id value, id default_target) override
	{
		assert(value != 0 && default_target != 0);
		assert(is_in_function()); // Can only switch inside functions

		if (!is_in_block())
			return _last_block;

		add_instruction_without_result(spv::OpSwitch)
			.add(value)
			.add(default_target);

		return set_block(0);
	}
	id   leave_block_and_branch(id target, unsigned int) override
	{
		assert(target != 0);
		assert(is_in_function()); // Can only branch inside functions

		if (!is_in_block())
			return _last_block;

		add_instruction_without_result(spv::OpBranch)
			.add(target);

		return set_block(0);
	}
	id   leave_block_and_branch_conditional(id condition, id true_target, id false_target) override
	{
		assert(condition != 0 && true_target != 0 && false_target != 0);
		assert(is_in_function()); // Can only branch inside functions

		if (!is_in_block())
			return _last_block;

		add_instruction_without_result(spv::OpBranchConditional)
			.add(condition)
			.add(true_target)
			.add(false_target);

		return set_block(0);
	}
	void leave_function() override
	{
		assert(is_in_function()); // Can only leave if there was a function to begin with

		_current_function_blocks->definition = _block_data[_last_block];

		// Append function end instruction
		add_instruction_without_result(spv::OpFunctionEnd, _current_function_blocks->definition);

		_current_function = nullptr;
		_current_function_blocks = nullptr;
	}
};

codegen *reshadefx::create_codegen_spirv(bool vulkan_semantics, bool debug_info, bool uniforms_to_spec_constants, bool enable_16bit_types, bool flip_vert_y, bool discard_is_demote)
{
	return new codegen_spirv(vulkan_semantics, debug_info, uniforms_to_spec_constants, enable_16bit_types, flip_vert_y, discard_is_demote);
}
