/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include <cmath> // std::isinf, std::isnan, std::signbit
#include <cassert>
#include <cstring> // std::memcmp
#include <charconv> // std::from_chars, std::to_chars
#include <algorithm> // std::find, std::find_if, std::max
#include <locale>
#include <sstream>
#include <iomanip>
#include <unordered_set>

using namespace reshadefx;

namespace {

inline char to_digit(unsigned int value)
{
	assert(value < 10);
	return '0' + static_cast<char>(value);
}

inline uint32_t align_up(uint32_t size, uint32_t alignment)
{
	alignment -= 1;
	return ((size + alignment) & ~alignment);
}

class codegen_glsl final : public codegen
{
public:
	codegen_glsl(unsigned version, bool gles, bool vulkan_semantics, bool debug_info, bool uniforms_to_spec_constants, bool enable_16bit_types, bool flip_vert_y) :
		_glsl_version(version),
		_gles(gles),
		_debug_info(debug_info),
		_vulkan_semantics(vulkan_semantics),
		_uniforms_to_spec_constants(uniforms_to_spec_constants),
		_enable_16bit_types(enable_16bit_types),
		_flip_vert_y(flip_vert_y)
	{
		// Create default block and reserve a memory block to avoid frequent reallocations
		std::string &block = _blocks.emplace(0, std::string()).first->second;
		block.reserve(8192);
	}

private:
	enum class naming
	{
		// After escaping, name should already be unique, so no additional steps are taken
		unique,
		// After escaping, will be numbered when clashing with another name
		general,
		// This is a special name that is not modified and should be unique
		reserved,
		// Replace name with a code snippet
		expression,
	};

	unsigned _glsl_version = 0;
	bool _gles = false;
	bool _debug_info = false;
	bool _vulkan_semantics = false;
	bool _uniforms_to_spec_constants = false;
	bool _enable_16bit_types = false;
	bool _flip_vert_y = false;

	std::unordered_map<id, std::string> _names;
	std::unordered_map<id, std::string> _blocks;
	std::string _ubo_block;
	std::string _compute_block;
	std::string _current_function_declaration;

	std::unordered_map<id, id> _remapped_sampler_variables;
	std::unordered_map<std::string, uint32_t> _semantic_to_location;
	std::vector<std::tuple<type, constant, id>> _constant_lookup;

	// Only write compatibility intrinsics to result if they are actually in use
	bool _uses_fmod = false;
	bool _uses_componentwise_or = false;
	bool _uses_componentwise_and = false;
	bool _uses_componentwise_cond = false;
	bool _uses_control_flow_attributes = false;

	std::string finalize_preamble() const
	{
		std::string preamble = "#version " + std::to_string(_glsl_version) + (_gles ? " es\n" : ((_glsl_version >= 330) ? " core\n" : "\n"));

		if (_gles)
			preamble += "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\n";

		if (_enable_16bit_types)
			// GL_NV_gpu_shader5, GL_AMD_gpu_shader_half_float or GL_EXT_shader_16bit_storage
			preamble += "#extension GL_NV_gpu_shader5 : require\n";
		if (_uses_control_flow_attributes)
			preamble += "#extension GL_EXT_control_flow_attributes : enable\n";

		if (_uses_fmod)
			preamble += "float fmodHLSL(float x, float y) { return x - y * trunc(x / y); }\n"
				"vec2 fmodHLSL(vec2 x, vec2 y) { return x - y * trunc(x / y); }\n"
				"vec3 fmodHLSL(vec3 x, vec3 y) { return x - y * trunc(x / y); }\n"
				"vec4 fmodHLSL(vec4 x, vec4 y) { return x - y * trunc(x / y); }\n"
				"mat2 fmodHLSL(mat2 x, mat2 y) { return x - matrixCompMult(y, mat2(trunc(x[0] / y[0]), trunc(x[1] / y[1]))); }\n"
				"mat3 fmodHLSL(mat3 x, mat3 y) { return x - matrixCompMult(y, mat3(trunc(x[0] / y[0]), trunc(x[1] / y[1]), trunc(x[2] / y[2]))); }\n"
				"mat4 fmodHLSL(mat4 x, mat4 y) { return x - matrixCompMult(y, mat4(trunc(x[0] / y[0]), trunc(x[1] / y[1]), trunc(x[2] / y[2]), trunc(x[3] / y[3]))); }\n";
		if (_uses_componentwise_or)
			preamble +=
				"bvec2 compOr(bvec2 a, bvec2 b) { return bvec2(a.x || b.x, a.y || b.y); }\n"
				"bvec3 compOr(bvec3 a, bvec3 b) { return bvec3(a.x || b.x, a.y || b.y, a.z || b.z); }\n"
				"bvec4 compOr(bvec4 a, bvec4 b) { return bvec4(a.x || b.x, a.y || b.y, a.z || b.z, a.w || b.w); }\n";
		if (_uses_componentwise_and)
			preamble +=
				"bvec2 compAnd(bvec2 a, bvec2 b) { return bvec2(a.x && b.x, a.y && b.y); }\n"
				"bvec3 compAnd(bvec3 a, bvec3 b) { return bvec3(a.x && b.x, a.y && b.y, a.z && b.z); }\n"
				"bvec4 compAnd(bvec4 a, bvec4 b) { return bvec4(a.x && b.x, a.y && b.y, a.z && b.z, a.w && b.w); }\n";
		if (_uses_componentwise_cond)
			preamble +=
				"vec2 compCond(bvec2 cond, vec2 a, vec2 b) { return vec2(cond.x ? a.x : b.x, cond.y ? a.y : b.y); }\n"
				"vec3 compCond(bvec3 cond, vec3 a, vec3 b) { return vec3(cond.x ? a.x : b.x, cond.y ? a.y : b.y, cond.z ? a.z : b.z); }\n"
				"vec4 compCond(bvec4 cond, vec4 a, vec4 b) { return vec4(cond.x ? a.x : b.x, cond.y ? a.y : b.y, cond.z ? a.z : b.z, cond.w ? a.w : b.w); }\n"
				"bvec2 compCond(bvec2 cond, bvec2 a, bvec2 b) { return bvec2(cond.x ? a.x : b.x, cond.y ? a.y : b.y); }\n"
				"bvec3 compCond(bvec3 cond, bvec3 a, bvec3 b) { return bvec3(cond.x ? a.x : b.x, cond.y ? a.y : b.y, cond.z ? a.z : b.z); }\n"
				"bvec4 compCond(bvec4 cond, bvec4 a, bvec4 b) { return bvec4(cond.x ? a.x : b.x, cond.y ? a.y : b.y, cond.z ? a.z : b.z, cond.w ? a.w : b.w); }\n"
				"ivec2 compCond(bvec2 cond, ivec2 a, ivec2 b) { return ivec2(cond.x ? a.x : b.x, cond.y ? a.y : b.y); }\n"
				"ivec3 compCond(bvec3 cond, ivec3 a, ivec3 b) { return ivec3(cond.x ? a.x : b.x, cond.y ? a.y : b.y, cond.z ? a.z : b.z); }\n"
				"ivec4 compCond(bvec4 cond, ivec4 a, ivec4 b) { return ivec4(cond.x ? a.x : b.x, cond.y ? a.y : b.y, cond.z ? a.z : b.z, cond.w ? a.w : b.w); }\n"
				"uvec2 compCond(bvec2 cond, uvec2 a, uvec2 b) { return uvec2(cond.x ? a.x : b.x, cond.y ? a.y : b.y); }\n"
				"uvec3 compCond(bvec3 cond, uvec3 a, uvec3 b) { return uvec3(cond.x ? a.x : b.x, cond.y ? a.y : b.y, cond.z ? a.z : b.z); }\n"
				"uvec4 compCond(bvec4 cond, uvec4 a, uvec4 b) { return uvec4(cond.x ? a.x : b.x, cond.y ? a.y : b.y, cond.z ? a.z : b.z, cond.w ? a.w : b.w); }\n";

		if (!_ubo_block.empty())
		{
			if (_vulkan_semantics)
			{
				preamble += "layout(std140, set = 0, binding = 0) uniform _Globals {\n" + _ubo_block + "};\n";
			}
			else
			{
				preamble += "layout(std140, binding = 0) uniform _Globals {\n" + _ubo_block + "};\n";
			}
		}

		return preamble;
	}

	std::string finalize_code() const override
	{
		std::string code = finalize_preamble();

		// Add sampler definitions
		for (const sampler &info : _module.samplers)
			code += _blocks.at(info.id);

		// Add storage definitions
		for (const storage &info : _module.storages)
			code += _blocks.at(info.id);

		// Add global definitions (struct types, global variables, ...)
		code += _blocks.at(0);

		// Add function definitions
		for (const std::unique_ptr<function> &func : _functions)
		{
			const bool is_entry_point = func->unique_name[0] == 'E';
			if (is_entry_point)
				code += "#ifdef " + func->unique_name + '\n';

			code += _blocks.at(func->id);

			if (is_entry_point)
				code += "#endif\n";
		}

		return code;
	}
	std::string finalize_code_for_entry_point(const std::string &entry_point_name) const override
	{
		const auto entry_point_it = std::find_if(_functions.begin(), _functions.end(),
			[&entry_point_name](const std::unique_ptr<function> &func) {
				return func->unique_name == entry_point_name;
			});
		if (entry_point_it == _functions.end())
			return {};
		const function &entry_point = *entry_point_it->get();

		std::string code = finalize_preamble();

		if (entry_point.type != shader_type::pixel)
			code +=
				// OpenGL does not allow using 'discard' in the vertex shader profile
				"#define discard\n"
				// 'dFdx', 'dFdx' and 'fwidth' too are only available in fragment shaders
				"#define dFdx(x) x\n"
				"#define dFdy(y) y\n"
				"#define fwidth(p) p\n";

		if (entry_point.type != shader_type::compute)
			code +=
				// OpenGL does not allow using 'shared' in vertex/fragment shader profile
				"#define shared\n"
				"#define atomicAdd(a, b) a\n"
				"#define atomicAnd(a, b) a\n"
				"#define atomicOr(a, b) a\n"
				"#define atomicXor(a, b) a\n"
				"#define atomicMin(a, b) a\n"
				"#define atomicMax(a, b) a\n"
				"#define atomicExchange(a, b) a\n"
				"#define atomicCompSwap(a, b, c) a\n"
				// Barrier intrinsics are only available in compute shaders
				"#define barrier()\n"
				"#define memoryBarrier()\n"
				"#define groupMemoryBarrier()\n";

		const auto replace_binding =
			[](std::string &code, uint32_t binding) {
				const size_t beg = code.find("layout(binding = ") + 17;
				const size_t end = code.find_first_of("),", beg);
				code.replace(beg, end - beg, std::to_string(binding));
			};

		// Add referenced sampler definitions
		for (uint32_t binding = 0; binding < entry_point.referenced_samplers.size(); ++binding)
		{
			if (entry_point.referenced_samplers[binding] == 0)
				continue;

			std::string block_code = _blocks.at(entry_point.referenced_samplers[binding]);
			replace_binding(block_code, binding);
			code += block_code;
		}

		// Add referenced storage definitions
		for (uint32_t binding = 0; binding < entry_point.referenced_storages.size(); ++binding)
		{
			if (entry_point.referenced_storages[binding] == 0)
				continue;

			std::string block_code = _blocks.at(entry_point.referenced_storages[binding]);
			replace_binding(block_code, binding);
			code += block_code;
		}

		// Add global definitions (struct types, global variables, ...)
		code += _blocks.at(0);

		// Add referenced function definitions
		for (const std::unique_ptr<function> &func : _functions)
		{
			if (func->id != entry_point.id &&
				std::find(entry_point.referenced_functions.begin(), entry_point.referenced_functions.end(), func->id) == entry_point.referenced_functions.end())
				continue;

			code += _blocks.at(func->id);
		}

		return code;
	}

	template <bool is_param = false, bool is_decl = true, bool is_interface = false>
	void write_type(std::string &s, const type &type) const
	{
		if constexpr (is_decl)
		{
			// Global variables are implicitly 'static' in GLSL, so the keyword does not exist
			if (type.has(type::q_precise))
				s += "precise ";
			if (type.has(type::q_groupshared))
				s += "shared ";
		}

		if constexpr (is_interface)
		{
			if (type.has(type::q_linear))
				s += "smooth ";
			if (type.has(type::q_noperspective))
				s += "noperspective ";
			if (type.has(type::q_centroid))
				s += "centroid ";
			if (type.has(type::q_nointerpolation))
				s += "flat ";
		}

		if constexpr (is_interface || is_param)
		{
			if (type.has(type::q_inout))
				s += "inout ";
			else if (type.has(type::q_in))
				s += "in ";
			else if (type.has(type::q_out))
				s += "out ";
		}

		switch (type.base)
		{
		case type::t_void:
			s += "void";
			break;
		case type::t_bool:
			if (type.cols > 1)
				s += "mat", s += to_digit(type.rows), s += 'x', s += to_digit(type.cols);
			else if (type.rows > 1)
				s += "bvec", s += to_digit(type.rows);
			else
				s += "bool";
			break;
		case type::t_min16int:
			if (_enable_16bit_types)
			{
				assert(type.cols == 1);
				if (type.rows > 1)
					s += "i16vec", s += to_digit(type.rows);
				else
					s += "int16_t";
				break;
			}
			else if constexpr (is_decl)
				s += "mediump ";
			[[fallthrough]];
		case type::t_int:
			if (type.cols > 1)
				s += "mat", s += to_digit(type.rows), s += 'x', s += to_digit(type.cols);
			else if (type.rows > 1)
				s += "ivec", s += to_digit(type.rows);
			else
				s += "int";
			break;
		case type::t_min16uint:
			if (_enable_16bit_types)
			{
				assert(type.cols == 1);
				if (type.rows > 1)
					s += "u16vec", s += to_digit(type.rows);
				else
					s += "uint16_t";
				break;
			}
			else if constexpr (is_decl)
				s += "mediump ";
			[[fallthrough]];
		case type::t_uint:
			if (type.cols > 1)
				s += "mat", s += to_digit(type.rows), s += 'x', s += to_digit(type.cols);
			else if (type.rows > 1)
				s += "uvec", s += to_digit(type.rows);
			else
				s += "uint";
			break;
		case type::t_min16float:
			if (_enable_16bit_types)
			{
				assert(type.cols == 1);
				if (type.rows > 1)
					s += "f16vec", s += to_digit(type.rows);
				else
					s += "float16_t";
				break;
			}
			else if constexpr (is_decl)
				s += "mediump ";
			[[fallthrough]];
		case type::t_float:
			if (type.cols > 1)
				s += "mat", s += to_digit(type.rows), s += 'x', s += to_digit(type.cols);
			else if (type.rows > 1)
				s += "vec", s += to_digit(type.rows);
			else
				s += "float";
			break;
		case type::t_struct:
			s += id_to_name(type.struct_definition);
			break;
		case type::t_sampler1d_int:
			s += "isampler1D";
			break;
		case type::t_sampler2d_int:
			s += "isampler2D";
			break;
		case type::t_sampler3d_int:
			s += "isampler3D";
			break;
		case type::t_sampler1d_uint:
			s += "usampler1D";
			break;
		case type::t_sampler3d_uint:
			s += "usampler3D";
			break;
		case type::t_sampler2d_uint:
			s += "usampler2D";
			break;
		case type::t_sampler1d_float:
			s += "sampler1D";
			break;
		case type::t_sampler2d_float:
			s += "sampler2D";
			break;
		case type::t_sampler3d_float:
			s += "sampler3D";
			break;
		case type::t_storage1d_int:
			if constexpr (is_param)
				s += "writeonly ";
			s += "iimage1D";
			break;
		case type::t_storage2d_int:
			if constexpr (is_param)
				s += "writeonly ";
			s += "iimage2D";
			break;
		case type::t_storage3d_int:
			if constexpr (is_param)
				s += "writeonly ";
			s += "iimage3D";
			break;
		case type::t_storage1d_uint:
			if constexpr (is_param)
				s += "writeonly ";
			s += "uimage1D";
			break;
		case type::t_storage2d_uint:
			if constexpr (is_param)
				s += "writeonly ";
			s += "uimage2D";
			break;
		case type::t_storage3d_uint:
			if constexpr (is_param)
				s += "writeonly ";
			s += "uimage3D";
			break;
		case type::t_storage1d_float:
			if constexpr (is_param)
				s += "writeonly ";
			s += "image1D";
			break;
		case type::t_storage2d_float:
			if constexpr (is_param) // Images need a format to be readable, but declaring that on function parameters is not well supported, so can only support write-only images there
				s += "writeonly ";
			s += "image2D";
			break;
		case type::t_storage3d_float:
			if constexpr (is_param)
				s += "writeonly ";
			s += "image3D";
			break;
		default:
			assert(false);
		}
	}
	void write_constant(std::string &s, const type &data_type, const constant &data) const
	{
		if (data_type.is_array())
		{
			assert(data_type.is_bounded_array());

			type elem_type = data_type;
			elem_type.array_length = 0;

			write_type<false, false>(s, elem_type);
			s += '[' + std::to_string(data_type.array_length) + "](";

			for (unsigned int a = 0; a < data_type.array_length; ++a)
			{
				write_constant(s, elem_type, a < static_cast<unsigned int>(data.array_data.size()) ? data.array_data[a] : constant {});
				s += ", ";
			}

			// Remove trailing ", "
			s.erase(s.size() - 2);

			s += ')';
			return;
		}

		// There can only be numeric constants
		assert(data_type.is_numeric());

		if (!data_type.is_scalar())
			write_type<false, false>(s, data_type), s += '(';

		for (unsigned int i = 0; i < data_type.components(); ++i)
		{
			switch (data_type.base)
			{
			case type::t_bool:
				s += data.as_uint[i] ? "true" : "false";
				break;
			case type::t_min16int:
			case type::t_int:
				s += std::to_string(data.as_int[i]);
				break;
			case type::t_min16uint:
			case type::t_uint:
				s += std::to_string(data.as_uint[i]) + 'u';
				break;
			case type::t_min16float:
			case type::t_float:
				if (std::isnan(data.as_float[i])) {
					s += "0.0/0.0/*nan*/";
					break;
				}
				if (std::isinf(data.as_float[i])) {
					s += std::signbit(data.as_float[i]) ? "1.0/0.0/*inf*/" : "-1.0/0.0/*-inf*/";
					break;
				}

				{
#ifdef _MSC_VER
					char temp[64];
					const std::to_chars_result res = std::to_chars(temp, temp + sizeof(temp), data.as_float[i], std::chars_format::scientific, 8);
					if (res.ec == std::errc())
						s.append(temp, res.ptr);
					else
						assert(false);
#else
					std::ostringstream ss;
					ss.imbue(std::locale::classic());
					ss << std::scientific << data.as_float[i];
					s += ss.str();
#endif
				}
				break;
			default:
				assert(false);
			}

			s += ", ";
		}

		// Remove trailing ", "
		s.erase(s.size() - 2);

		if (!data_type.is_scalar())
			s += ')';
	}
	void write_location(std::string &s, const location &loc) const
	{
		if (loc.source.empty() || !_debug_info)
			return;

		s += "#line " + std::to_string(loc.line) + '\n';
	}
	void write_texture_format(std::string &s, texture_format format)
	{
		switch (format)
		{
		case texture_format::r8:
			s += "r8";
			break;
		case texture_format::r16:
			s += "r16";
			break;
		case texture_format::r16f:
			s += "r16f";
			break;
		case texture_format::r32i:
			s += "r32i";
			break;
		case texture_format::r32u:
			s += "r32ui";
			break;
		case texture_format::r32f:
			s += "r32f";
			break;
		case texture_format::rg8:
			s += "rg8";
			break;
		case texture_format::rg16:
			s += "rg16";
			break;
		case texture_format::rg16f:
			s += "rg16f";
			break;
		case texture_format::rg32f:
			s += "rg32f";
			break;
		case texture_format::rgba8:
			s += "rgba8";
			break;
		case texture_format::rgba16:
			s += "rgba16";
			break;
		case texture_format::rgba16f:
			s += "rgba16f";
			break;
		case texture_format::rgba32f:
			s += "rgba32f";
			break;
		case texture_format::rgb10a2:
			s += "rgb10_a2";
			break;
		default:
			assert(false);
		}
	}

	std::string id_to_name(id id) const
	{
		if (const auto it = _remapped_sampler_variables.find(id);
			it != _remapped_sampler_variables.end())
			id = it->second;

		assert(id != 0);
		if (const auto names_it = _names.find(id);
			names_it != _names.end())
			return names_it->second;
		return '_' + std::to_string(id);
	}

	template <naming naming_type = naming::general>
	void define_name(const id id, std::string name)
	{
		assert(!name.empty());
		if constexpr (naming_type != naming::expression)
			if (name[0] == '_')
				return; // Filter out names that may clash with automatic ones
		if constexpr (naming_type != naming::reserved)
			name = escape_name(std::move(name));
		if constexpr (naming_type == naming::general)
			if (std::find_if(_names.begin(), _names.end(),
					[&name](const auto &names_it) { return names_it.second == name; }) != _names.end())
				name += '_' + std::to_string(id); // Append a numbered suffix if the name already exists
		_names[id] = std::move(name);
	}

	uint32_t semantic_to_location(const std::string &semantic, uint32_t max_attributes = 1)
	{
		if (const auto location_it = _semantic_to_location.find(semantic);
			location_it != _semantic_to_location.end())
			return location_it->second;

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

	std::string escape_name(std::string name) const
	{
		static const std::unordered_set<std::string> s_reserverd_names = {
			"common", "partition", "input", "output", "active", "filter", "superp", "invariant",
			"attribute", "varying", "buffer", "resource", "coherent", "readonly", "writeonly",
			"layout", "flat", "smooth", "lowp", "mediump", "highp", "precision", "patch", "subroutine",
			"atomic_uint", "fixed",
			"vec2", "vec3", "vec4", "ivec2", "dvec2", "dvec3", "dvec4", "ivec3", "ivec4", "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4", "fvec2", "fvec3", "fvec4", "hvec2", "hvec3", "hvec4",
			"mat2", "mat3", "mat4", "dmat2", "dmat3", "dmat4", "mat2x2", "mat2x3", "mat2x4", "dmat2x2", "dmat2x3", "dmat2x4", "mat3x2", "mat3x3", "mat3x4", "dmat3x2", "dmat3x3", "dmat3x4", "mat4x2", "mat4x3", "mat4x4", "dmat4x2", "dmat4x3", "dmat4x4",
			"sampler1DShadow", "sampler1DArrayShadow", "isampler1D", "isampler1DArray", "usampler1D", "usampler1DArray",
			"sampler2DShadow", "sampler2DArrayShadow", "isampler2D", "isampler2DArray", "usampler2D", "usampler2DArray", "sampler2DRect", "sampler2DRectShadow", "isampler2DRect", "usampler2DRect", "isampler2DMS", "usampler2DMS", "isampler2DMSArray", "usampler2DMSArray",
			"isampler3D", "usampler3D", "sampler3DRect",
			"samplerCubeShadow", "samplerCubeArrayShadow", "isamplerCube", "isamplerCubeArray", "usamplerCube", "usamplerCubeArray",
			"samplerBuffer", "isamplerBuffer", "usamplerBuffer",
			"image1D", "iimage1D", "uimage1D", "image1DArray", "iimage1DArray", "uimage1DArray",
			"image2D", "iimage2D", "uimage2D", "image2DArray", "iimage2DArray", "uimage2DArray", "image2DRect", "iimage2DRect", "uimage2DRect", "image2DMS", "iimage2DMS", "uimage2DMS", "image2DMSArray", "iimage2DMSArray", "uimage2DMSArray",
			"image3D", "iimage3D", "uimage3D",
			"imageCube", "iimageCube", "uimageCube", "imageCubeArray", "iimageCubeArray", "uimageCubeArray",
			"imageBuffer", "iimageBuffer", "uimageBuffer",
			"abs", "sign", "all", "any", "sin", "sinh", "cos", "cosh", "tan", "tanh", "asin", "acos", "atan",
			"exp", "exp2", "log", "log2", "sqrt", "inversesqrt", "ceil", "floor", "fract", "trunc", "round",
			"radians", "degrees", "length", "normalize", "transpose", "determinant", "intBitsToFloat", "uintBitsToFloat",
			"floatBitsToInt", "floatBitsToUint", "matrixCompMult", "not", "lessThan", "greaterThan", "lessThanEqual",
			"greaterThanEqual", "equal", "notEqual", "dot", "cross", "distance", "pow", "modf", "frexp", "ldexp",
			"min", "max", "step", "reflect", "texture", "textureOffset", "fma", "mix", "clamp", "smoothstep", "refract",
			"faceforward", "textureLod", "textureLodOffset", "texelFetch", "main"
		};

		// Escape reserved names so that they do not fail to compile
		if (name.compare(0, 3, "gl_") == 0 || s_reserverd_names.count(name))
			// Append an underscore at start instead of the end, since another one may get added in 'define_name' when there is a suffix
			// This is guaranteed to not clash with user defined names, since those starting with an underscore are filtered out in 'define_name'
			name = '_' + name;

		// Remove duplicated underscore symbols from name which can occur due to namespaces but are not allowed in GLSL
		for (size_t pos = 0; (pos = name.find("__", pos)) != std::string::npos;)
			name.replace(pos, 2, "_x");

		return name;
	}
	std::string semantic_to_builtin(std::string name, const std::string &semantic, shader_type stype) const
	{
		if (semantic == "SV_POSITION")
			return stype == shader_type::pixel ? "gl_FragCoord" : "gl_Position";
		if (semantic == "SV_POINTSIZE")
			return "gl_PointSize";
		if (semantic == "SV_DEPTH")
			return "gl_FragDepth";
		if (semantic == "SV_VERTEXID")
			return _vulkan_semantics ? "gl_VertexIndex" : "gl_VertexID";
		if (semantic == "SV_ISFRONTFACE")
			return "gl_FrontFacing";
		if (semantic == "SV_GROUPID")
			return "gl_WorkGroupID";
		if (semantic == "SV_GROUPINDEX")
			return "gl_LocalInvocationIndex";
		if (semantic == "SV_GROUPTHREADID")
			return "gl_LocalInvocationID";
		if (semantic == "SV_DISPATCHTHREADID")
			return "gl_GlobalInvocationID";

		return escape_name(std::move(name));
	}

	static void increase_indentation_level(std::string &block)
	{
		if (block.empty())
			return;

		for (size_t pos = 0; (pos = block.find("\n\t", pos)) != std::string::npos; pos += 3)
			block.replace(pos, 2, "\n\t\t");

		block.insert(block.begin(), '\t');
	}

	id   define_struct(const location &loc, struct_type &info) override
	{
		const id res = info.id = make_id();
		define_name<naming::unique>(res, info.unique_name);

		_structs.push_back(info);

		std::string &code = _blocks.at(_current_block);

		write_location(code, loc);

		code += "struct " + id_to_name(res) + "\n{\n";

		for (const member_type &member : info.member_list)
		{
			code += '\t';
			write_type(code, member.type); // GLSL does not allow interpolation attributes on struct members
			code += ' ';
			code += escape_name(member.name);
			if (member.type.is_array())
				code += '[' + std::to_string(member.type.array_length) + ']';
			code += ";\n";
		}

		if (info.member_list.empty())
			code += "float _dummy;\n";

		code += "};\n";

		return res;
	}
	id   define_texture(const location &, texture &info) override
	{
		const id res = info.id = make_id();

		_module.textures.push_back(info);

		return res;
	}
	id   define_sampler(const location &loc, const texture &, sampler &info) override
	{
		const id res = info.id = create_block();
		define_name<naming::unique>(res, info.unique_name);

		std::string &code = _blocks.at(res);

		write_location(code, loc);

		// Default to a binding index equivalent to the entry in the sampler list (this is later overwritten in 'finalize_code_for_entry_point' to a more optimal placement)
		const uint32_t default_binding = static_cast<uint32_t>(_module.samplers.size());

		code += "layout(binding = " + std::to_string(default_binding);
		if (_vulkan_semantics)
			code += ", set = 1";
		code += ") uniform ";
		write_type(code, info.type);
		code += ' ' + id_to_name(res) + ";\n";

		_module.samplers.push_back(info);

		return res;
	}
	id   define_storage(const location &loc, const texture &tex_info, storage &info) override
	{
		const id res = info.id = create_block();
		define_name<naming::unique>(res, info.unique_name);

		std::string &code = _blocks.at(res);

		write_location(code, loc);

		// Default to a binding index equivalent to the entry in the storage list (this is later overwritten in 'finalize_code_for_entry_point' to a more optimal placement)
		const uint32_t default_binding = static_cast<uint32_t>(_module.storages.size());

		code += "layout(binding = " + std::to_string(default_binding) + ", ";
		write_texture_format(code, tex_info.format);
		code += ") uniform ";
		write_type(code, info.type);
		code += ' ' + id_to_name(res) + ";\n";

		_module.storages.push_back(info);

		return res;
	}
	id   define_uniform(const location &loc, uniform &info) override
	{
		const id res = make_id();
		define_name<naming::unique>(res, info.name);

		if (_uniforms_to_spec_constants && info.has_initializer_value)
		{
			info.size = info.type.components() * 4;
			if (info.type.is_array())
				info.size *= info.type.array_length;

			std::string &code = _blocks.at(_current_block);

			write_location(code, loc);

			assert(!info.type.has(type::q_static) && !info.type.has(type::q_const));

			if (!_gles)
				code += "const ";
			write_type(code, info.type);
			code += ' ' + id_to_name(res) + " = ";
			if (!info.type.is_scalar())
				write_type<false, false>(code, info.type);
			code += "(SPEC_CONSTANT_" + info.name + ");\n";

			_module.spec_constants.push_back(info);
		}
		else
		{
			// GLSL specification on std140 layout:
			// 1. If the member is a scalar consuming N basic machine units, the base alignment is N.
			// 2. If the member is a two- or four-component vector with components consuming N basic machine units, the base alignment is 2N or 4N, respectively.
			// 3. If the member is a three-component vector with components consuming N basic machine units, the base alignment is 4N.
			// 4. If the member is an array of scalars or vectors, the base alignment and array stride are set to match the base alignment of a single array element,
			//    according to rules (1), (2), and (3), and rounded up to the base alignment of a four-component vector.
			// 7. If the member is a row-major matrix with C columns and R rows, the matrix is stored identically to an array of R row vectors with C components each, according to rule (4).
			// 8. If the member is an array of S row-major matrices with C columns and R rows, the matrix is stored identically to a row of S*R row vectors with C components each, according to rule (4).
			uint32_t alignment = (info.type.rows == 3 ? 4 /* (3) */ : info.type.rows /* (2) */) * 4 /* (1) */;
			info.size = info.type.rows * 4;

			if (info.type.is_matrix())
			{
				alignment = 16 /* (4) */;
				info.size = info.type.rows * alignment /* (7), (8) */;
			}
			if (info.type.is_array())
			{
				alignment = 16 /* (4) */;
				info.size = align_up(info.size, alignment) * info.type.array_length;
			}

			// Adjust offset according to alignment rules from above
			info.offset = _module.total_uniform_size;
			info.offset = align_up(info.offset, alignment);
			_module.total_uniform_size = info.offset + info.size;

			write_location(_ubo_block, loc);

			_ubo_block += '\t';
			// Note: All matrices are floating-point, even if the uniform type says different!!
			write_type(_ubo_block, info.type);
			_ubo_block += ' ' + id_to_name(res);

			if (info.type.is_array())
				_ubo_block += '[' + std::to_string(info.type.array_length) + ']';

			_ubo_block += ";\n";

			_module.uniforms.push_back(info);
		}

		return res;
	}
	id   define_variable(const location &loc, const type &type, std::string name, bool global, id initializer_value) override
	{
		// Constant variables with a constant initializer can just point to the initializer SSA variable, since they cannot be modified anyway, thus saving an unnecessary assignment
		if (initializer_value != 0 && type.has(type::q_const) &&
			std::find_if(_constant_lookup.begin(), _constant_lookup.end(),
				[initializer_value](const auto &x) {
					return initializer_value == std::get<2>(x);
				}) != _constant_lookup.end())
			return initializer_value;

		const id res = make_id();

		// GLSL does not allow local sampler variables, so try to remap those
		if (!global && type.is_sampler())
		{
			_remapped_sampler_variables[res] = 0;
			return res;
		}

		if (!name.empty())
			define_name<naming::general>(res, name);

		std::string &code = _blocks.at(_current_block);

		write_location(code, loc);

		if (!global)
			code += '\t';

		write_type(code, type);
		code += ' ' + id_to_name(res);

		if (type.is_array())
			code += '[' + std::to_string(type.array_length) + ']';

		if (initializer_value != 0)
			code += " = " + id_to_name(initializer_value);

		code += ";\n";

		return res;
	}
	id   define_function(const location &loc, function &info) override
	{
		const id res = info.id = make_id();

		// Name is used in other places like the entry point defines, so escape it here
		info.unique_name = escape_name(info.unique_name);

		assert(!info.unique_name.empty() && (info.unique_name[0] == 'F' || info.unique_name[0] == 'E'));
		const bool is_entry_point = info.unique_name[0] == 'E';
		if (!is_entry_point)
			define_name<naming::unique>(res, info.unique_name);
		else
			define_name<naming::reserved>(res, "main");

		assert(_current_block == 0 && (_current_function_declaration.empty() || is_entry_point));
		std::string &code = _current_function_declaration;

		write_location(code, loc);

		write_type(code, info.return_type);
		code += ' ' + id_to_name(res) + '(';

		assert(info.parameter_list.empty() || !is_entry_point);

		for (member_type &param : info.parameter_list)
		{
			param.id = make_id();
			define_name<naming::unique>(param.id, param.name);

			code += '\n';
			write_location(code, param.location);
			code += '\t';
			write_type<true>(code, param.type); // GLSL does not allow interpolation attributes on function parameters
			code += ' ' + id_to_name(param.id);

			if (param.type.is_array())
				code += '[' + std::to_string(param.type.array_length) + ']';

			code += ',';
		}

		// Remove trailing comma
		if (!info.parameter_list.empty())
			code.pop_back();

		code += ")\n";

		_functions.push_back(std::make_unique<function>(info));
		_current_function = _functions.back().get();

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

		assert(_current_function_declaration.empty());
		if (func.type == shader_type::compute)
			_current_function_declaration +=
				"layout(local_size_x = " + std::to_string(func.num_threads[0]) +
			         ", local_size_y = " + std::to_string(func.num_threads[1]) +
			         ", local_size_z = " + std::to_string(func.num_threads[2]) + ") in;\n";

		// Generate the glue entry point function
		function entry_point = func;
		entry_point.referenced_functions.push_back(func.id);

		// Change function signature to 'void main()'
		entry_point.return_type = { type::t_void };
		entry_point.return_semantic.clear();
		entry_point.parameter_list.clear();

		std::unordered_map<std::string, std::string> semantic_to_varying_variable;

		const auto create_varying_variable = [this, stype = func.type, &semantic_to_varying_variable](type type, unsigned int extra_qualifiers, const std::string &name, const std::string &semantic) {
			// Skip built in variables
			if (!semantic_to_builtin(std::string(), semantic, stype).empty())
				return;

			// Do not create multiple input/output variables for duplicate semantic usage (since every input/output location may only be defined once in GLSL)
			if ((extra_qualifiers & type::q_in) != 0 &&
				!semantic_to_varying_variable.emplace(semantic, name).second)
				return;

			type.qualifiers |= extra_qualifiers;
			assert((type.has(type::q_in) || type.has(type::q_out)) && !type.has(type::q_inout));

			// OpenGL does not allow varying of type boolean
			if (type.is_boolean())
				type.base = type::t_float;

			const uint32_t location = semantic_to_location(semantic, std::max(1u, type.array_length));

			for (unsigned int a = 0; a < std::max(1u, type.array_length); ++a)
			{
				_current_function_declaration += "layout(location = " + std::to_string(location + a) + ") ";
				write_type<false, false, true>(_current_function_declaration, type);
				_current_function_declaration += ' ';
				_current_function_declaration += escape_name(type.is_array() ? name + '_' + std::to_string(a) : name);
				_current_function_declaration += ";\n";
			}
		};

		// Translate return value to output variable
		if (func.return_type.is_struct())
		{
			const struct_type &definition = get_struct(func.return_type.struct_definition);

			for (const member_type &member : definition.member_list)
				create_varying_variable(member.type, type::q_out, "_return_" + member.name, member.semantic);
		}
		else if (!func.return_type.is_void())
		{
			create_varying_variable(func.return_type, type::q_out, "_return", func.return_semantic);
		}

		// Translate function parameters to input/output variables
		for (const member_type &param : func.parameter_list)
		{
			type param_type = param.type;
			param_type.qualifiers &= ~type::q_inout;

			// Create separate input/output variables for "inout" parameters (since "inout" is not valid on those in GLSL)
			if (param.type.has(type::q_in))
			{
				// Flatten structure parameters
				if (param_type.is_struct())
				{
					const struct_type &definition = get_struct(param_type.struct_definition);

					for (unsigned int a = 0, array_length = std::max(1u, param_type.array_length); a < array_length; a++)
					{
						for (const member_type &member : definition.member_list)
							create_varying_variable(member.type, param_type.qualifiers | type::q_in, "_in_" + id_to_name(param.id) + '_' + std::to_string(a) + '_' + member.name, member.semantic);
					}
				}
				else
				{
					create_varying_variable(param_type, type::q_in, "_in_" + id_to_name(param.id), param.semantic);
				}
			}

			if (param.type.has(type::q_out))
			{
				if (param_type.is_struct())
				{
					const struct_type &definition = get_struct(param_type.struct_definition);

					for (unsigned int a = 0, array_length = std::max(1u, param_type.array_length); a < array_length; a++)
					{
						for (const member_type &member : definition.member_list)
							create_varying_variable(member.type, param_type.qualifiers | type::q_out, "_out_" + id_to_name(param.id) + '_' + std::to_string(a) + '_' + member.name, member.semantic);
					}
				}
				else
				{
					create_varying_variable(param_type, type::q_out, "_out_" + id_to_name(param.id), param.semantic);
				}
			}
		}

		define_function({}, entry_point);
		enter_block(create_block());

		std::string &code = _blocks.at(_current_block);

		// Handle input parameters
		for (const member_type &param : func.parameter_list)
		{
			if (param.type.has(type::q_in))
			{
				const std::string param_name = id_to_name(param.id);

				// Create local array element variables
				for (unsigned int a = 0, array_length = std::max(1u, param.type.array_length); a < array_length; a++)
				{
					if (param.type.is_struct())
					{
						// Build struct from separate member input variables
						code += '\t';
						write_type<false, true>(code, param.type);
						code += ' ';
						code += escape_name(param.type.is_array() ? "_in_" + param_name + '_' + std::to_string(a) : "_in_" + param_name);
						code += " = ";
						write_type<false, false>(code, param.type);
						code += '(';

						const struct_type &struct_definition = get_struct(param.type.struct_definition);

						for (const member_type &member : struct_definition.member_list)
						{
							std::string in_param_name;
							{
								if (const auto it = semantic_to_varying_variable.find(member.semantic);
									it != semantic_to_varying_variable.end())
									in_param_name = it->second;
								else
									in_param_name = "_in_" + param_name + '_' + std::to_string(a) + '_' + member.name;
							}

							if (member.type.is_array())
							{
								write_type<false, false>(code, member.type);
								code += "[](";

								for (unsigned int b = 0; b < member.type.array_length; b++)
								{
									// OpenGL does not allow varying of type boolean, so need to cast here
									if (member.type.is_boolean())
									{
										write_type<false, false>(code, member.type);
										code += '(';
									}

									code += escape_name(in_param_name + '_' + std::to_string(b));

									if (member.type.is_boolean())
										code += ')';

									code += ", ";
								}

								// Remove trailing ", "
								code.erase(code.size() - 2);

								code += ')';
							}
							else
							{
								if (member.type.is_boolean() || (_gles && member.type.is_integral()))
								{
									write_type<false, false>(code, member.type);
									code += '(';
								}

								code += semantic_to_builtin(std::move(in_param_name), member.semantic, func.type);

								if (member.type.is_boolean() || (_gles && member.type.is_integral()))
									code += ')';
							}

							code += ", ";
						}

						// There can be no empty structs, so can assume that the last two characters are always ", "
						code.erase(code.size() - 2);

						code += ");\n";
					}
					else
					if (const auto it = semantic_to_varying_variable.find(param.semantic);
						it != semantic_to_varying_variable.end() &&
						it->second != "_in_" + id_to_name(param.id))
					{
						// Create local variables for duplicated semantics (since no input/output variable is created for those, see 'create_varying_variable')
						code += '\t';
						write_type<false, true>(code, param.type);
						code += ' ';
						code += escape_name(param.type.is_array() ? "_in_" + id_to_name(param.id) + '_' + std::to_string(a) : "_in_" + id_to_name(param.id));
						code += " = ";

						if (param.type.is_boolean())
						{
							write_type<false, false>(code, param.type);
							code += '(';
						}

						code += escape_name(param.type.is_array() ? it->second + '_' + std::to_string(a) : it->second);

						if (param.type.is_boolean())
							code += ')';

						code += ";\n";
					}
				}
			}

			// Create local parameter variables which are used as arguments in the entry point function call below
			code += '\t';
			write_type<false, true>(code, param.type);
			code += ' ';
			code += id_to_name(param.id);
			if (param.type.is_array())
				code += '[' + std::to_string(param.type.array_length) + ']';

			// Initialize those local variables with the input value if existing
			// Parameters with only an "out" qualifier are written to by the entry point function, so do not need to be initialized
			if (param.type.has(type::q_in))
			{
				code += " = ";

				// Build array from separate array element variables
				if (param.type.is_array())
				{
					write_type<false, false>(code, param.type);
					code += "[](";

					for (unsigned int a = 0; a < param.type.array_length; ++a)
					{
						// OpenGL does not allow varying of type boolean, so need to cast here
						if (param.type.is_boolean())
						{
							write_type<false, false>(code, param.type);
							code += '(';
						}

						code += escape_name("_in_" + id_to_name(param.id) + '_' + std::to_string(a));

						if (param.type.is_boolean())
							code += ')';

						code += ", ";
					}

					// Remove trailing ", "
					code.erase(code.size() - 2);

					code += ')';
				}
				else
				{
					if (param.type.is_boolean() || (_gles && param.type.is_integral()))
					{
						write_type<false, false>(code, param.type);
						code += '(';
					}

					code += semantic_to_builtin("_in_" + id_to_name(param.id), param.semantic, func.type);

					if (param.type.is_boolean() || (_gles && param.type.is_integral()))
						code += ')';
				}
			}

			code += ";\n";
		}

		code += '\t';
		// Structs cannot be output variables, so have to write to a temporary first and then output each member separately
		if (func.return_type.is_struct())
		{
			write_type(code, func.return_type);
			code += " _return = ";
		}
		// All other output types can write to the output variable directly
		else if (!func.return_type.is_void())
		{
			code += semantic_to_builtin("_return", func.return_semantic, func.type);
			code += " = ";
		}

		// Call the function this entry point refers to
		code += id_to_name(func.id) + '(';

		for (const member_type &param : func.parameter_list)
		{
			code += id_to_name(param.id);
			code += ", ";
		}

		// Remove trailing ", "
		if (!func.parameter_list.empty())
			code.erase(code.size() - 2);

		code += ");\n";

		// Handle output parameters
		for (const member_type &param : func.parameter_list)
		{
			if (param.type.has(type::q_out))
			{
				const std::string param_name = id_to_name(param.id);

				if (param.type.is_struct())
				{
					const struct_type &definition = get_struct(param.type.struct_definition);

					// Split out struct fields into separate output variables again
					for (unsigned int a = 0, array_length = std::max(1u, param.type.array_length); a < array_length; a++)
					{
						for (const member_type &member : definition.member_list)
						{
							if (member.type.is_array())
							{
								for (unsigned int b = 0; b < member.type.array_length; b++)
								{
									code += '\t';
									code += escape_name("_out_" + param_name + '_' + std::to_string(a) + '_' + member.name + '_' + std::to_string(b));
									code += " = ";

									// OpenGL does not allow varying of type boolean, so need to cast here
									if (member.type.is_boolean())
									{
										type varying_type = member.type;
										varying_type.base = type::t_float;
										write_type<false, false>(code, varying_type);
										code += '(';
									}

									code += param_name;
									if (param.type.is_array())
										code += '[' + std::to_string(a) + ']';
									code += '.';
									code += member.name;
									code += '[' + std::to_string(b) + ']';

									if (member.type.is_boolean())
										code += ')';

									code += ";\n";
								}
							}
							else
							{
								code += '\t';
								code += semantic_to_builtin("_out_" + param_name + '_' + std::to_string(a) + '_' + member.name, member.semantic, func.type);
								code += " = ";

								if (member.type.is_boolean())
								{
									type varying_type = member.type;
									varying_type.base = type::t_float;
									write_type<false, false>(code, varying_type);
									code += '(';
								}

								code += param_name;
								if (param.type.is_array())
									code += '[' + std::to_string(a) + ']';
								code += '.';
								code += member.name;

								if (member.type.is_boolean())
									code += ')';

								code += ";\n";
							}
						}
					}
				}
				else if (param.type.is_array())
				{
					// Split up array output into individual array elements again
					for (unsigned int a = 0; a < param.type.array_length; a++)
					{
						code += '\t';
						code += escape_name("_out_" + param_name + '_' + std::to_string(a));
						code += " = ";

						// OpenGL does not allow varying of type boolean, so need to cast here
						if (param.type.is_boolean())
						{
							type varying_type = param.type;
							varying_type.base = type::t_float;
							write_type<false, false>(code, varying_type);
							code += '(';
						}

						code += param_name;
						code += '[' + std::to_string(a) + ']';

						if (param.type.is_boolean())
							code += ')';

						code += ";\n";
					}
				}
				else
				{
					code += '\t';
					code += semantic_to_builtin("_out_" + param_name, param.semantic, func.type);
					code += " = ";

					if (param.type.is_boolean())
					{
						type varying_type = param.type;
						varying_type.base = type::t_float;
						write_type<false, false>(code, varying_type);
						code += '(';
					}

					code += param_name;

					if (param.type.is_boolean())
						code += ')';

					code += ";\n";
				}
			}
		}

		// Handle return struct output variables
		if (func.return_type.is_struct())
		{
			const struct_type &struct_definition = get_struct(func.return_type.struct_definition);

			for (const member_type &member : struct_definition.member_list)
			{
				code += '\t';
				code += semantic_to_builtin("_return_" + member.name, member.semantic, func.type);
				code += " = _return." + escape_name(member.name) + ";\n";
			}
		}

		// Add code to flip the output vertically
		if (_flip_vert_y && func.type == shader_type::vertex)
			code += "\tgl_Position.y = -gl_Position.y;\n";

		leave_block_and_return(0);
		leave_function();
	}

	id   emit_load(const expression &exp, bool force_new_id) override
	{
		if (exp.is_constant)
			return emit_constant(exp.type, exp.constant);
		else if (exp.chain.empty() && !force_new_id) // Can refer to values without access chain directly
			return exp.base;

		const id res = make_id();

		std::string type, expr_code = id_to_name(exp.base);

		for (const expression::operation &op : exp.chain)
		{
			switch (op.op)
			{
			case expression::operation::op_cast:
				type.clear();
				write_type<false, false>(type, op.to);
				expr_code = type + '(' + expr_code + ')';
				break;
			case expression::operation::op_member:
				expr_code += '.';
				expr_code += escape_name(get_struct(op.from.struct_definition).member_list[op.index].name);
				break;
			case expression::operation::op_dynamic_index:
				// For matrices this will extract a column, but that is fine, since they are initialized column-wise too
				// Also cast to an integer, since it could be a boolean too, but GLSL does not allow those in index expressions
				expr_code += "[int(" + id_to_name(op.index) + ")]";
				break;
			case expression::operation::op_constant_index:
				if (op.from.is_vector() && !op.from.is_array())
					expr_code += '.',
					expr_code += "xyzw"[op.index];
				else
					expr_code += '[' + std::to_string(op.index) + ']';
				break;
			case expression::operation::op_swizzle:
				if (op.from.is_matrix())
				{
					if (op.swizzle[1] < 0)
					{
						const char row = (op.swizzle[0] % 4);
						const char col = (op.swizzle[0] - row) / 4;

						expr_code += '[';
						expr_code += to_digit(row);
						expr_code += "][";
						expr_code += to_digit(col);
						expr_code += ']';
					}
					else
					{
						// TODO: Implement matrix to vector swizzles
						assert(false);
						expr_code += "_NOT_IMPLEMENTED_"; // Make sure compilation fails
					}
				}
				else
				{
					// can't swizzle scalars
					if (_gles && op.from.is_scalar())
					{
						// => e.g. vec3(expr, expr, expr).xyz
						type.clear();
						write_type<false, false>(type, op.to);
						std::string new_code = type;
						new_code += '(';

						const unsigned int components = op.to.components();
						for (unsigned int i = 0; i < components; ++i)
						{
								if (i > 0)
									new_code += ',';
								new_code += '(' + expr_code + ')';
						}
						new_code += ')';
						expr_code = std::move(new_code);
					}
					else
					{
						expr_code += '.';
						for (int i = 0; i < 4 && op.swizzle[i] >= 0; ++i)
							expr_code += "xyzw"[op.swizzle[i]];
					}
				}
				break;
			}
		}

		// GLSL matrices are always floating point, so need to cast result to the target type
		if (!exp.chain.empty() && exp.chain[0].from.is_matrix() && !exp.chain[0].from.is_floating_point())
		{
			type.clear();
			write_type<false, false>(type, exp.type);
			expr_code = type + '(' + expr_code + ')';
		}

		if (force_new_id)
		{
			// Need to store value in a new variable to comply with request for a new ID
			std::string &code = _blocks.at(_current_block);

			code += '\t';
			write_type(code, exp.type);
			code += ' ' + id_to_name(res) + " = " + expr_code + ";\n";
		}
		else
		{
			// Avoid excessive variable definitions by instancing simple load operations in code every time
			define_name<naming::expression>(res, std::move(expr_code));
		}

		return res;
	}
	void emit_store(const expression &exp, id value) override
	{
		if (const auto it = _remapped_sampler_variables.find(exp.base);
			it != _remapped_sampler_variables.end())
		{
			assert(it->second == 0);
			it->second = value;
			return;
		}

		std::string &code = _blocks.at(_current_block);

		write_location(code, exp.location);

		code += '\t' + id_to_name(exp.base);

		for (const expression::operation &op : exp.chain)
		{
			switch (op.op)
			{
			case expression::operation::op_member:
				code += '.';
				code += escape_name(get_struct(op.from.struct_definition).member_list[op.index].name);
				break;
			case expression::operation::op_dynamic_index:
				code += "[int(" + id_to_name(op.index) + ")]";
				break;
			case expression::operation::op_constant_index:
				code += '[' + std::to_string(op.index) + ']';
				break;
			case expression::operation::op_swizzle:
				if (op.from.is_matrix())
				{
					if (op.swizzle[1] < 0)
					{
						const char row = (op.swizzle[0] % 4);
						const char col = (op.swizzle[0] - row) / 4;

						code += '[';
						code += '1' + row - 1;
						code += "][";
						code += '1' + col - 1;
						code += ']';
					}
					else
					{
						// TODO: Implement matrix to vector swizzles
						assert(false);
						code += "_NOT_IMPLEMENTED_"; // Make sure compilation fails
					}
				}
				else
				{
					code += '.';
					for (int i = 0; i < 4 && op.swizzle[i] >= 0; ++i)
						code += "xyzw"[op.swizzle[i]];
				}
				break;
			}
		}

		code += " = ";

		// GLSL matrices are always floating point, so need to cast type
		if (!exp.chain.empty() && exp.chain[0].from.is_matrix() && !exp.chain[0].from.is_floating_point())
			// Only supporting scalar assignments to matrices currently, so can assume to always cast to float
			code += "float(" + id_to_name(value) + ");\n";
		else
			code += id_to_name(value) + ";\n";
	}

	id   emit_constant(const type &data_type, const constant &data) override
	{
		const id res = make_id();

		if (data_type.is_array() || data_type.is_struct())
		{
			assert(data_type.has(type::q_const));

			if (const auto it = std::find_if(_constant_lookup.begin(), _constant_lookup.end(),
					[&data_type, &data](const std::tuple<type, constant, id> &x) {
						if (!(std::get<0>(x) == data_type && std::memcmp(&std::get<1>(x).as_uint[0], &data.as_uint[0], sizeof(uint32_t) * 16) == 0 && std::get<1>(x).array_data.size() == data.array_data.size()))
							return false;
						for (size_t i = 0; i < data.array_data.size(); ++i)
							if (std::memcmp(&std::get<1>(x).array_data[i].as_uint[0], &data.array_data[i].as_uint[0], sizeof(uint32_t) * 16) != 0)
								return false;
						return true;
					});
				it != _constant_lookup.end())
				return std::get<2>(*it); // Reuse existing constant instead of duplicating the definition
			else if (data_type.is_array())
				_constant_lookup.push_back({ data_type, data, res });

			// Put constant variable into global scope, so that it can be reused in different blocks
			std::string &code = _blocks.at(0);

			// GLSL requires constants to be initialized, but struct initialization is not supported right now
			if (!data_type.is_struct())
				code += "const ";

			write_type<false, false>(code, data_type);
			code += ' ' + id_to_name(res);

			// Array constants need to be stored in a constant variable as they cannot be used in-place
			if (data_type.is_array())
				code += '[' + std::to_string(data_type.array_length) + ']';

			// Struct initialization is not supported right now
			if (!data_type.is_struct()) {
				code += " = ";
				write_constant(code, data_type, data);
			}

			code += ";\n";
			return res;
		}

		std::string code;
		write_constant(code, data_type, data);
		define_name<naming::expression>(res, std::move(code));

		return res;
	}

	id   emit_unary_op(const location &loc, tokenid op, const type &res_type, id val) override
	{
		const id res = make_id();

		std::string &code = _blocks.at(_current_block);

		write_location(code, loc);

		code += '\t';
		write_type(code, res_type);
		code += ' ' + id_to_name(res) + " = ";

		switch (op)
		{
		case tokenid::minus:
			code += '-';
			break;
		case tokenid::tilde:
			code += '~';
			break;
		case tokenid::exclaim:
			if (res_type.is_vector())
				code += "not";
			else
				code += "!bool";
			break;
		default:
			assert(false);
		}

		code += '(' + id_to_name(val) + ");\n";

		return res;
	}
	id   emit_binary_op(const location &loc, tokenid op, const type &res_type, const type &exp_type, id lhs, id rhs) override
	{
		const id res = make_id();

		std::string &code = _blocks.at(_current_block);

		write_location(code, loc);

		code += '\t';
		write_type(code, res_type);
		code += ' ' + id_to_name(res) + " = ";

		std::string intrinsic, operator_code;

		switch (op)
		{
		case tokenid::plus:
		case tokenid::plus_plus:
		case tokenid::plus_equal:
			operator_code = '+';
			break;
		case tokenid::minus:
		case tokenid::minus_minus:
		case tokenid::minus_equal:
			operator_code = '-';
			break;
		case tokenid::star:
		case tokenid::star_equal:
			if (exp_type.is_matrix())
				intrinsic = "matrixCompMult";
			else
				operator_code = '*';
			break;
		case tokenid::slash:
		case tokenid::slash_equal:
			operator_code = '/';
			break;
		case tokenid::percent:
		case tokenid::percent_equal:
			if (exp_type.is_floating_point())
				intrinsic = "fmodHLSL",
				_uses_fmod = true;
			else
				operator_code = '%';
			break;
		case tokenid::caret:
		case tokenid::caret_equal:
			operator_code = '^';
			break;
		case tokenid::pipe:
		case tokenid::pipe_equal:
			operator_code = '|';
			break;
		case tokenid::ampersand:
		case tokenid::ampersand_equal:
			operator_code = '&';
			break;
		case tokenid::less_less:
		case tokenid::less_less_equal:
			operator_code = "<<";
			break;
		case tokenid::greater_greater:
		case tokenid::greater_greater_equal:
			operator_code = ">>";
			break;
		case tokenid::pipe_pipe:
			if (exp_type.is_vector())
				intrinsic = "compOr",
				_uses_componentwise_or = true;
			else
				operator_code = "||";
			break;
		case tokenid::ampersand_ampersand:
			if (exp_type.is_vector())
				intrinsic = "compAnd",
				_uses_componentwise_and = true;
			else
				operator_code = "&&";
			break;
		case tokenid::less:
			if (exp_type.is_vector())
				intrinsic = "lessThan";
			else
				operator_code = '<';
			break;
		case tokenid::less_equal:
			if (exp_type.is_vector())
				intrinsic = "lessThanEqual";
			else
				operator_code = "<=";
			break;
		case tokenid::greater:
			if (exp_type.is_vector())
				intrinsic = "greaterThan";
			else
				operator_code = '>';
			break;
		case tokenid::greater_equal:
			if (exp_type.is_vector())
				intrinsic = "greaterThanEqual";
			else
				operator_code = ">=";
			break;
		case tokenid::equal_equal:
			if (exp_type.is_vector())
				intrinsic = "equal";
			else
				operator_code = "==";
			break;
		case tokenid::exclaim_equal:
			if (exp_type.is_vector())
				intrinsic = "notEqual";
			else
				operator_code = "!=";
			break;
		default:
			assert(false);
		}

		if (!intrinsic.empty())
			code += intrinsic + '(' + id_to_name(lhs) + ", " + id_to_name(rhs) + ')';
		else
			code += id_to_name(lhs) + ' ' + operator_code + ' ' + id_to_name(rhs);

		code += ";\n";

		return res;
	}
	id   emit_ternary_op(const location &loc, tokenid op, const type &res_type, id condition, id true_value, id false_value) override
	{
		if (op != tokenid::question)
			return assert(false), 0; // Should never happen, since this is the only ternary operator currently supported

		const id res = make_id();

		std::string &code = _blocks.at(_current_block);

		write_location(code, loc);

		code += '\t';
		write_type(code, res_type);
		code += ' ' + id_to_name(res);

		if (res_type.is_array())
			code += '[' + std::to_string(res_type.array_length) + ']';

		code += " = ";

		if (res_type.is_vector())
			code += "compCond(" + id_to_name(condition) + ", " + id_to_name(true_value) + ", " + id_to_name(false_value) + ");\n",
			_uses_componentwise_cond = true;
		else // GLSL requires the conditional expression to be a scalar boolean
			code += id_to_name(condition) + " ? " + id_to_name(true_value) + " : " + id_to_name(false_value) + ";\n";

		return res;
	}
	id   emit_call(const location &loc, id function, const type &res_type, const std::vector<expression> &args) override
	{
#ifndef NDEBUG
		for (const expression &arg : args)
			assert(arg.chain.empty() && arg.base != 0);
#endif

		const id res = make_id();

		std::string &code = _blocks.at(_current_block);

		write_location(code, loc);

		code += '\t';

		if (!res_type.is_void())
		{
			write_type(code, res_type);
			code += ' ' + id_to_name(res);

			if (res_type.is_array())
				code += '[' + std::to_string(res_type.array_length) + ']';

			code += " = ";
		}

		code += id_to_name(function) + '(';

		for (const expression &arg : args)
		{
			code += id_to_name(arg.base);
			code += ", ";
		}

		// Remove trailing ", "
		if (!args.empty())
			code.erase(code.size() - 2);

		code += ");\n";

		return res;
	}
	id   emit_call_intrinsic(const location &loc, id intrinsic, const type &res_type, const std::vector<expression> &args) override
	{
#ifndef NDEBUG
		for (const expression &arg : args)
			assert(arg.chain.empty() && arg.base != 0);
#endif

		const id res = make_id();

		std::string &code = _blocks.at(_current_block);

		write_location(code, loc);

		code += '\t';

		if (!res_type.is_void())
		{
			write_type(code, res_type);
			code += ' ' + id_to_name(res) + " = ";
		}

		enum
		{
		#define IMPLEMENT_INTRINSIC_GLSL(name, i, code) name##i,
			#include "effect_symbol_table_intrinsics.inl"
		};

		switch (intrinsic)
		{
		#define IMPLEMENT_INTRINSIC_GLSL(name, i, code) case name##i: code break;
			#include "effect_symbol_table_intrinsics.inl"
		default:
			assert(false);
		}

		code += ";\n";

		return res;
	}
	id   emit_construct(const location &loc, const type &res_type, const std::vector<expression> &args) override
	{
#ifndef NDEBUG
		for (const expression &arg : args)
			assert((arg.type.is_scalar() || res_type.is_array()) && arg.chain.empty() && arg.base != 0);
#endif

		const id res = make_id();

		std::string &code = _blocks.at(_current_block);

		write_location(code, loc);

		code += '\t';
		write_type(code, res_type);
		code += ' ' + id_to_name(res);

		if (res_type.is_array())
			code += '[' + std::to_string(res_type.array_length) + ']';

		code += " = ";

		write_type<false, false>(code, res_type);

		if (res_type.is_array())
			code += '[' + std::to_string(res_type.array_length) + ']';

		code += '(';

		for (const expression &arg : args)
		{
			code += id_to_name(arg.base);
			code += ", ";
		}

		// Remove trailing ", "
		if (!args.empty())
			code.erase(code.size() - 2);

		code += ");\n";

		return res;
	}

	void emit_if(const location &loc, id condition_value, id condition_block, id true_statement_block, id false_statement_block, unsigned int flags) override
	{
		assert(condition_value != 0 && condition_block != 0 && true_statement_block != 0 && false_statement_block != 0);

		std::string &code = _blocks.at(_current_block);

		std::string &true_statement_data = _blocks.at(true_statement_block);
		std::string &false_statement_data = _blocks.at(false_statement_block);

		increase_indentation_level(true_statement_data);
		increase_indentation_level(false_statement_data);

		code += _blocks.at(condition_block);

		write_location(code, loc);

		if (flags != 0 && !_gles)
		{
			_uses_control_flow_attributes = true;

			code += "#if GL_EXT_control_flow_attributes\n\t[[";
			if ((flags & 0x1) == 0x1)
				code += "flatten";
			if ((flags & 0x3) == 0x3)
				code += ", ";
			if ((flags & 0x2) == 0x2)
				code += "dont_flatten";
			code += "]]\n#endif\n";
		}

		code += '\t';
		code += "if (" + id_to_name(condition_value) + ")\n\t{\n";
		code += true_statement_data;
		code += "\t}\n";

		if (!false_statement_data.empty())
		{
			code += "\telse\n\t{\n";
			code += false_statement_data;
			code += "\t}\n";
		}

		// Remove consumed blocks to save memory
		_blocks.erase(condition_block);
		_blocks.erase(true_statement_block);
		_blocks.erase(false_statement_block);
	}
	id   emit_phi(const location &loc, id condition_value, id condition_block, id true_value, id true_statement_block, id false_value, id false_statement_block, const type &res_type) override
	{
		assert(condition_value != 0 && condition_block != 0 && true_value != 0 && true_statement_block != 0 && false_value != 0 && false_statement_block != 0);

		std::string &code = _blocks.at(_current_block);

		std::string &true_statement_data = _blocks.at(true_statement_block);
		std::string &false_statement_data = _blocks.at(false_statement_block);

		increase_indentation_level(true_statement_data);
		increase_indentation_level(false_statement_data);

		const id res = make_id();

		code += _blocks.at(condition_block);

		code += '\t';
		write_type(code, res_type);
		code += ' ' + id_to_name(res) + ";\n";

		write_location(code, loc);

		code += "\tif (" + id_to_name(condition_value) + ")\n\t{\n";
		code += (true_statement_block != condition_block ? true_statement_data : std::string());
		code += "\t\t" + id_to_name(res) + " = " + id_to_name(true_value) + ";\n";
		code += "\t}\n\telse\n\t{\n";
		code += (false_statement_block != condition_block ? false_statement_data : std::string());
		code += "\t\t" + id_to_name(res) + " = " + id_to_name(false_value) + ";\n";
		code += "\t}\n";

		// Remove consumed blocks to save memory
		_blocks.erase(condition_block);
		_blocks.erase(true_statement_block);
		_blocks.erase(false_statement_block);

		return res;
	}
	void emit_loop(const location &loc, id condition_value, id prev_block, id header_block, id condition_block, id loop_block, id continue_block, unsigned int flags) override
	{
		assert(prev_block != 0 && header_block != 0 && loop_block != 0 && continue_block != 0);

		std::string &code = _blocks.at(_current_block);

		std::string &loop_data = _blocks.at(loop_block);
		std::string &continue_data = _blocks.at(continue_block);

		increase_indentation_level(loop_data);
		increase_indentation_level(loop_data);
		increase_indentation_level(continue_data);

		code += _blocks.at(prev_block);

		std::string attributes;
		if (flags != 0 && !_gles)
		{
			_uses_control_flow_attributes = true;

			attributes += "#if GL_EXT_control_flow_attributes\n\t[[";
			if ((flags & 0x1) == 0x1)
				attributes += "unroll";
			if ((flags & 0x3) == 0x3)
				attributes += ", ";
			if ((flags & 0x2) == 0x2)
				attributes += "dont_unroll";
			attributes += "]]\n#endif\n";
		}

		// Condition value can be missing in infinite loop constructs like "for (;;)"
		std::string condition_name = condition_value != 0 ? id_to_name(condition_value) : "true";

		if (condition_block == 0)
		{
			// Convert the last SSA variable initializer to an assignment statement
			const size_t pos_assign = continue_data.rfind(condition_name);
			const size_t pos_prev_assign = continue_data.rfind('\t', pos_assign);
			continue_data.erase(pos_prev_assign + 1, pos_assign - pos_prev_assign - 1);

			// We need to add the continue block to all "continue" statements as well
			const std::string continue_id = "__CONTINUE__" + std::to_string(continue_block);
			for (size_t offset = 0; (offset = loop_data.find(continue_id, offset)) != std::string::npos; offset += continue_data.size())
				loop_data.replace(offset, continue_id.size(), continue_data);

			code += "\tbool " + condition_name + ";\n";

			write_location(code, loc);

			code += attributes;
			code += '\t';
			code += "do\n\t{\n\t\t{\n";
			code += loop_data; // Encapsulate loop body into another scope, so not to confuse any local variables with the current iteration variable accessed in the continue block below
			code += "\t\t}\n";
			code += continue_data;
			code += "\t}\n\twhile (" + condition_name + ");\n";
		}
		else
		{
			std::string &condition_data = _blocks.at(condition_block);

			// If the condition data is just a single line, then it is a simple expression, which we can just put into the loop condition as-is
			if (std::count(condition_data.begin(), condition_data.end(), '\n') == 1)
			{
				// Convert SSA variable initializer back to a condition expression
				const size_t pos_assign = condition_data.find('=');
				condition_data.erase(0, pos_assign + 2);
				const size_t pos_semicolon = condition_data.rfind(';');
				condition_data.erase(pos_semicolon);

				condition_name = std::move(condition_data);
				assert(condition_data.empty());
			}
			else
			{
				code += condition_data;

				increase_indentation_level(condition_data);

				// Convert the last SSA variable initializer to an assignment statement
				const size_t pos_assign = condition_data.rfind(condition_name);
				const size_t pos_prev_assign = condition_data.rfind('\t', pos_assign);
				condition_data.erase(pos_prev_assign + 1, pos_assign - pos_prev_assign - 1);
			}

			const std::string continue_id = "__CONTINUE__" + std::to_string(continue_block);
			for (size_t offset = 0; (offset = loop_data.find(continue_id, offset)) != std::string::npos; offset += continue_data.size())
				loop_data.replace(offset, continue_id.size(), continue_data + condition_data);

			code += attributes;
			code += '\t';
			code += "while (" + condition_name + ")\n\t{\n\t\t{\n";
			code += loop_data;
			code += "\t\t}\n";
			code += continue_data;
			code += condition_data;
			code += "\t}\n";

			_blocks.erase(condition_block);
		}

		// Remove consumed blocks to save memory
		_blocks.erase(prev_block);
		_blocks.erase(header_block);
		_blocks.erase(loop_block);
		_blocks.erase(continue_block);
	}
	void emit_switch(const location &loc, id selector_value, id selector_block, id default_label, id default_block, const std::vector<id> &case_literal_and_labels, const std::vector<id> &case_blocks, unsigned int) override
	{
		assert(selector_value != 0 && selector_block != 0 && default_label != 0 && default_block != 0);
		assert(case_blocks.size() == case_literal_and_labels.size() / 2);

		std::string &code = _blocks.at(_current_block);

		code += _blocks.at(selector_block);

		write_location(code, loc);

		code += "\tswitch (" + id_to_name(selector_value) + ")\n\t{\n";

		std::vector<id> labels = case_literal_and_labels;
		for (size_t i = 0; i < labels.size(); i += 2)
		{
			if (labels[i + 1] == 0)
				continue; // Happens if a case was already handled, see below

			code += "\tcase " + std::to_string(labels[i]) + ": ";

			if (labels[i + 1] == default_label)
			{
				code += "default: ";
				default_label = 0;
			}
			else
			{
				for (size_t k = i + 2; k < labels.size(); k += 2)
				{
					if (labels[k + 1] == 0 || labels[k + 1] != labels[i + 1])
						continue;

					code += "case " + std::to_string(labels[k]) + ": ";
					labels[k + 1] = 0;
				}
			}

			assert(case_blocks[i / 2] != 0);
			std::string &case_data = _blocks.at(case_blocks[i / 2]);

			increase_indentation_level(case_data);

			code += "{\n";
			code += case_data;
			code += "\t}\n";
		}


		if (default_label != 0 && default_block != _current_block)
		{
			std::string &default_data = _blocks.at(default_block);

			increase_indentation_level(default_data);

			code += "\tdefault: {\n";
			code += default_data;
			code += "\t}\n";

			_blocks.erase(default_block);
		}

		code += "\t}\n";

		// Remove consumed blocks to save memory
		_blocks.erase(selector_block);
		for (const id case_block : case_blocks)
			_blocks.erase(case_block);
	}

	id   create_block() override
	{
		const id res = make_id();

		std::string &block = _blocks.emplace(res, std::string()).first->second;
		// Reserve a decently big enough memory block to avoid frequent reallocations
		block.reserve(4096);

		return res;
	}
	id   set_block(id id) override
	{
		_last_block = _current_block;
		_current_block = id;

		return _last_block;
	}
	void enter_block(id id) override
	{
		_current_block = id;
	}
	id   leave_block_and_kill() override
	{
		if (!is_in_block())
			return 0;

		std::string &code = _blocks.at(_current_block);

		code += "\tdiscard;\n";

		const type &return_type = _current_function->return_type;
		if (!return_type.is_void())
		{
			// Add a return statement to exit functions in case discard is the last control flow statement
			code += "\treturn ";
			write_constant(code, return_type, constant());
			code += ";\n";
		}

		return set_block(0);
	}
	id   leave_block_and_return(id value) override
	{
		if (!is_in_block())
			return 0;

		// Skip implicit return statement
		if (!_current_function->return_type.is_void() && value == 0)
			return set_block(0);

		std::string &code = _blocks.at(_current_block);

		code += "\treturn";

		if (value != 0)
			code += ' ' + id_to_name(value);

		code += ";\n";

		return set_block(0);
	}
	id   leave_block_and_switch(id, id) override
	{
		if (!is_in_block())
			return _last_block;

		return set_block(0);
	}
	id   leave_block_and_branch(id target, unsigned int loop_flow) override
	{
		if (!is_in_block())
			return _last_block;

		std::string &code = _blocks.at(_current_block);

		switch (loop_flow)
		{
		case 1:
			code += "\tbreak;\n";
			break;
		case 2: // Keep track of continue target block, so we can insert its code here later
			code += "__CONTINUE__" + std::to_string(target) + "\tcontinue;\n";
			break;
		}

		return set_block(0);
	}
	id   leave_block_and_branch_conditional(id, id, id) override
	{
		if (!is_in_block())
			return _last_block;

		return set_block(0);
	}
	void leave_function() override
	{
		assert(_current_function != nullptr && _last_block != 0);

		_blocks.emplace(_current_function->id, _current_function_declaration + "{\n" + _blocks.at(_last_block) + "}\n");

		_current_function = nullptr;
		_current_function_declaration.clear();
	}
};
} // namespace

codegen *reshadefx::create_codegen_glsl(unsigned version, bool gles, bool vulkan_semantics, bool debug_info, bool uniforms_to_spec_constants, bool enable_16bit_types, bool flip_vert_y)
{
	return new codegen_glsl(version, gles, vulkan_semantics, debug_info, uniforms_to_spec_constants, enable_16bit_types, flip_vert_y);
}
