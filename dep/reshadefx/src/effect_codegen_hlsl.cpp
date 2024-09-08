/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include <cmath> // std::isinf, std::isnan, std::signbit
#include <cctype> // std::tolower
#include <cassert>
#include <cstring> // stricmp, std::memcmp
#include <charconv> // std::from_chars, std::to_chars
#include <algorithm> // std::equal, std::find, std::find_if, std::max
#include <locale>
#include <sstream>

using namespace reshadefx;

inline char to_digit(unsigned int value)
{
	assert(value < 10);
	return '0' + static_cast<char>(value);
}

inline uint32_t align_up(uint32_t size, uint32_t alignment, uint32_t elements)
{
	alignment -= 1;
	return ((size + alignment) & ~alignment) * (elements - 1) + size;
}

class codegen_hlsl final : public codegen
{
public:
	codegen_hlsl(unsigned int shader_model, bool debug_info, bool uniforms_to_spec_constants) :
		_shader_model(shader_model),
		_debug_info(debug_info),
		_uniforms_to_spec_constants(uniforms_to_spec_constants)
	{
		// Create default block and reserve a memory block to avoid frequent reallocations
		std::string &block = _blocks.emplace(0, std::string()).first->second;
		block.reserve(8192);
	}

private:
	enum class naming
	{
		// Name should already be unique, so no additional steps are taken
		unique,
		// Will be numbered when clashing with another name
		general,
		// Replace name with a code snippet
		expression,
	};

	unsigned int _shader_model = 0;
	bool _debug_info = false;
	bool _uniforms_to_spec_constants = false;

	std::unordered_map<id, std::string> _names;
	std::unordered_map<id, std::string> _blocks;
	std::string _cbuffer_block;
	std::string _current_location;
	std::string _current_function_declaration;

	std::string _remapped_semantics[15];
	std::vector<std::tuple<type, constant, id>> _constant_lookup;
#if 0
	std::vector<sampler_binding> _sampler_lookup;
#endif

	// Only write compatibility intrinsics to result if they are actually in use
	bool _uses_bitwise_cast = false;
	bool _uses_bitwise_intrinsics = false;

	void optimize_bindings() override
	{
		codegen::optimize_bindings();

#if 0
		if (_shader_model < 40)
			return;

		_module.num_sampler_bindings = static_cast<uint32_t>(_sampler_lookup.size());

		for (technique &tech : _module.techniques)
			for (pass &pass : tech.passes)
				pass.sampler_bindings.assign(_sampler_lookup.begin(), _sampler_lookup.end());
#endif
	}

	std::string finalize_preamble() const
	{
		std::string preamble;

#define IMPLEMENT_INTRINSIC_FALLBACK_ASINT(n) \
		"int" #n " __asint(float" #n " v) {" \
			"float" #n " e = 0;" \
			"float" #n " f = frexp(v, e) * 2 - 1;" /* frexp does not include sign bit in HLSL, so can use as is */ \
			"float" #n " m = ldexp(f, 23);" \
			"return (v == 0) ? 0 : (v < 0 ? 2147483648 : 0) + (" /* Zero (does not handle negative zero) */ \
			/*	isnan(v) ? 2147483647 : */ /* NaN */ \
			/*	isinf(v) ? 2139095040 : */ /* Infinity */ \
				"ldexp(e + 126, 23) + m);" \
		"}"
#define IMPLEMENT_INTRINSIC_FALLBACK_ASUINT(n) \
		"int" #n " __asuint(float" #n " v) { return __asint(v); }"
#define IMPLEMENT_INTRINSIC_FALLBACK_ASFLOAT(n) \
		"float" #n " __asfloat(int" #n " v) {" \
			"float" #n " m = v % exp2(23);" \
			"float" #n " f = ldexp(m, -23);" \
			"float" #n " e = floor(ldexp(v, -23) % 256);" \
			"return (v > 2147483647 ? -1 : 1) * (" \
			/*	e == 0 ? ldexp(f, -126) : */ /* Denormalized */ \
			/*	e == 255 ? (m == 0 ? 1.#INF : -1.#IND) : */ /* Infinity and NaN */ \
				"ldexp(1 + f, e - 127));" \
		"}"

		// See https://graphics.stanford.edu/%7Eseander/bithacks.html#CountBitsSetParallel
#define IMPLEMENT_INTRINSIC_FALLBACK_COUNTBITS(n) \
		"uint" #n " __countbits(uint" #n " v) {" \
			"v = v - ((v >> 1) & 0x55555555);" \
			"v = (v & 0x33333333) + ((v >> 2) & 0x33333333);" \
			"v = (v + (v >> 4)) & 0x0F0F0F0F;" \
			"v *= 0x01010101;" \
			"return v >> 24;" \
		"}"
#define IMPLEMENT_INTRINSIC_FALLBACK_COUNTBITS_LOOP(n) \
		"uint" #n " __countbits(uint" #n " v) {" \
			"uint" #n " c = 0;" \
			"while (any(v > 0)) {" \
				"c += v % 2;" \
				"v /= 2;" \
			"}" \
			"return c;" \
		"}"

		// See https://graphics.stanford.edu/%7Eseander/bithacks.html#ReverseParallel
#define IMPLEMENT_INTRINSIC_FALLBACK_REVERSEBITS(n) \
		"uint" #n " __reversebits(uint" #n " v) {" \
			"v = ((v >> 1) & 0x55555555) | ((v & 0x55555555) << 1);" \
			"v = ((v >> 2) & 0x33333333) | ((v & 0x33333333) << 2);" \
			"v = ((v >> 4) & 0x0F0F0F0F) | ((v & 0x0F0F0F0F) << 4);" \
			"v = ((v >> 8) & 0x00FF00FF) | ((v & 0x00FF00FF) << 8);" \
			"return (v >> 16) | (v << 16);" \
		"}"
#define IMPLEMENT_INTRINSIC_FALLBACK_REVERSEBITS_LOOP(n) \
		"uint" #n " __reversebits(uint" #n " v) {" \
			"uint" #n " r = 0;" \
			"for (int i = 0; i < 32; i++) {" \
				"r *= 2;" \
				"r += floor(x % 2);" \
				"v /= 2;" \
			"}" \
			"return r;" \
		"}"

		// See https://graphics.stanford.edu/%7Eseander/bithacks.html#ZerosOnRightParallel
#define IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITLOW(n) \
		"uint" #n " __firstbitlow(uint" #n " v) {" \
			"uint" #n " c = (v != 0) ? 31 : 32;" \
			"v &= -int" #n "(v);" \
			"c = (v & 0x0000FFFF) ? c - 16 : c;" \
			"c = (v & 0x00FF00FF) ? c -  8 : c;" \
			"c = (v & 0x0F0F0F0F) ? c -  4 : c;" \
			"c = (v & 0x33333333) ? c -  2 : c;" \
			"c = (v & 0x55555555) ? c -  1 : c;" \
			"return c;" \
		"}"
#define IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITLOW_LOOP(n) \
		"uint" #n " __firstbitlow(uint" #n " v) {" \
			"uint" #n " c = (v != 0) ? 31 : 32;" \
			"for (int i = 0; i < 32; i++) {" \
				"c = c > i && (v % 2) != 0 ? i : c;" \
				"v /= 2;" \
			"}" \
			"return c;" \
		"}"


#define IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITHIGH(n) \
		"uint" #n " __firstbithigh(uint" #n " v) { return __firstbitlow(__reversebits(v)); }"

		if (_shader_model >= 40)
		{
			preamble +=
				"struct __sampler1D_int { Texture1D<int> t; SamplerState s; };\n"
				"struct __sampler2D_int { Texture2D<int> t; SamplerState s; };\n"
				"struct __sampler3D_int { Texture3D<int> t; SamplerState s; };\n"
				"struct __sampler1D_uint { Texture1D<uint> t; SamplerState s; };\n"
				"struct __sampler2D_uint { Texture2D<uint> t; SamplerState s; };\n"
				"struct __sampler3D_uint { Texture3D<uint> t; SamplerState s; };\n"
				"struct __sampler1D_float { Texture1D<float> t; SamplerState s; };\n"
				"struct __sampler2D_float { Texture2D<float> t; SamplerState s; };\n"
				"struct __sampler3D_float { Texture3D<float> t; SamplerState s; };\n"
				"struct __sampler1D_float4 { Texture1D<float4> t; SamplerState s; };\n"
				"struct __sampler2D_float4 { Texture2D<float4> t; SamplerState s; };\n"
				"struct __sampler3D_float4 { Texture3D<float4> t; SamplerState s; };\n";

			if (_uses_bitwise_intrinsics && _shader_model < 50)
				preamble +=
					IMPLEMENT_INTRINSIC_FALLBACK_COUNTBITS(1) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_COUNTBITS(2) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_COUNTBITS(3) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_COUNTBITS(4) "\n"

					IMPLEMENT_INTRINSIC_FALLBACK_REVERSEBITS(1) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_REVERSEBITS(2) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_REVERSEBITS(3) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_REVERSEBITS(4) "\n"

					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITLOW(1) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITLOW(2) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITLOW(3) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITLOW(4) "\n"

					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITHIGH(1) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITHIGH(2) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITHIGH(3) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITHIGH(4) "\n";

			if (!_cbuffer_block.empty())
			{
#if 0
				if (_shader_model >= 60)
					preamble += "[[vk::binding(0, 0)]] "; // Descriptor set 0
#endif

				preamble += "cbuffer _Globals {\n" + _cbuffer_block + "};\n";
			}
		}
		else
		{
			preamble +=
				"struct __sampler1D { sampler1D s; float1 pixelsize; };\n"
				"struct __sampler2D { sampler2D s; float2 pixelsize; };\n"
				"struct __sampler3D { sampler3D s; float3 pixelsize; };\n"
				"uniform float2 __TEXEL_SIZE__ : register(c255);\n";

			if (_uses_bitwise_cast)
				preamble +=
					IMPLEMENT_INTRINSIC_FALLBACK_ASINT(1) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_ASINT(2) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_ASINT(3) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_ASINT(4) "\n"

					IMPLEMENT_INTRINSIC_FALLBACK_ASUINT(1) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_ASUINT(2) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_ASUINT(3) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_ASUINT(4) "\n"

					IMPLEMENT_INTRINSIC_FALLBACK_ASFLOAT(1) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_ASFLOAT(2) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_ASFLOAT(3) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_ASFLOAT(4) "\n";

			if (_uses_bitwise_intrinsics)
				preamble +=
					IMPLEMENT_INTRINSIC_FALLBACK_COUNTBITS_LOOP(1) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_COUNTBITS_LOOP(2) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_COUNTBITS_LOOP(3) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_COUNTBITS_LOOP(4) "\n"

					IMPLEMENT_INTRINSIC_FALLBACK_REVERSEBITS_LOOP(1) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_REVERSEBITS_LOOP(2) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_REVERSEBITS_LOOP(3) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_REVERSEBITS_LOOP(4) "\n"

					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITLOW_LOOP(1) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITLOW_LOOP(2) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITLOW_LOOP(3) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITLOW_LOOP(4) "\n"

					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITHIGH(1) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITHIGH(2) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITHIGH(3) "\n"
					IMPLEMENT_INTRINSIC_FALLBACK_FIRSTBITHIGH(4) "\n";

			if (!_cbuffer_block.empty())
			{
				preamble += _cbuffer_block;
			}
		}

		return preamble;
	}

	std::string finalize_code() const override
	{
		std::string code = finalize_preamble();

		// Add global definitions (struct types, global variables, sampler state declarations, ...)
		code += _blocks.at(0);

		// Add texture and sampler definitions
		for (const sampler &info : _module.samplers)
			code += _blocks.at(info.id);

		// Add storage definitions
		for (const storage &info : _module.storages)
			code += _blocks.at(info.id);

		// Add function definitions
		for (const std::unique_ptr<function> &func : _functions)
			code += _blocks.at(func->id);

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

		if (_shader_model < 40 && entry_point.type == shader_type::pixel)
			// Overwrite position semantic in pixel shaders
			code += "#define POSITION VPOS\n";

		// Add global definitions (struct types, global variables, sampler state declarations, ...)
		code += _blocks.at(0);

		const auto replace_binding =
			[](std::string &code, uint32_t binding) {
				for (size_t start = 0;;)
				{
					const size_t pos = code.find(": register(", start);
					if (pos == std::string::npos)
						break;
					const size_t beg = pos + 12;
					const size_t end = code.find(')', beg);
					const std::string replacement = std::to_string(binding);
					code.replace(beg, end - beg, replacement);
					start = beg + replacement.length();
				}
			};

		// Add referenced texture and sampler definitions
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

	template <bool is_param = false, bool is_decl = true>
	void write_type(std::string &s, const type &type) const
	{
		if constexpr (is_decl)
		{
			if (type.has(type::q_static))
				s += "static ";
			if (type.has(type::q_precise))
				s += "precise ";
			if (type.has(type::q_groupshared))
				s += "groupshared ";
		}

		if constexpr (is_param)
		{
			if (type.has(type::q_linear))
				s += "linear ";
			if (type.has(type::q_noperspective))
				s += "noperspective ";
			if (type.has(type::q_centroid))
				s += "centroid ";
			if (type.has(type::q_nointerpolation))
				s += "nointerpolation ";

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
			return;
		case type::t_bool:
			s += "bool";
			break;
		case type::t_min16int:
			// Minimum precision types are only supported in shader model 4 and up
			// Real 16-bit types were added in shader model 6.2
			s += _shader_model >= 62 ? "int16_t" : _shader_model >= 40 ? "min16int" : "int";
			break;
		case type::t_int:
			s += "int";
			break;
		case type::t_min16uint:
			s += _shader_model >= 62 ? "uint16_t" : _shader_model >= 40 ? "min16uint" : "int";
			break;
		case type::t_uint:
			// In shader model 3, uints can only be used with known-positive values, so use ints instead
			s += _shader_model >= 40 ? "uint" : "int";
			break;
		case type::t_min16float:
			s += _shader_model >= 62 ? "float16_t" : _shader_model >= 40 ? "min16float" : "float";
			break;
		case type::t_float:
			s += "float";
			break;
		case type::t_struct:
			s += id_to_name(type.struct_definition);
			return;
		case type::t_sampler1d_int:
		case type::t_sampler2d_int:
		case type::t_sampler3d_int:
			s += "__sampler";
			s += to_digit(type.texture_dimension());
			s += 'D';
			if (_shader_model >= 40)
			{
				s += "_int";
				if (type.rows > 1)
					s += to_digit(type.rows);
			}
			return;
		case type::t_sampler1d_uint:
		case type::t_sampler2d_uint:
		case type::t_sampler3d_uint:
			s += "__sampler";
			s += to_digit(type.texture_dimension());
			s += 'D';
			if (_shader_model >= 40)
			{
				s += "_uint";
				if (type.rows > 1)
					s += to_digit(type.rows);
			}
			return;
		case type::t_sampler1d_float:
		case type::t_sampler2d_float:
		case type::t_sampler3d_float:
			s += "__sampler";
			s += to_digit(type.texture_dimension());
			s += 'D';
			if (_shader_model >= 40)
			{
				s += "_float";
				if (type.rows > 1)
					s += to_digit(type.rows);
			}
			return;
		case type::t_storage1d_int:
		case type::t_storage2d_int:
		case type::t_storage3d_int:
			s += "RWTexture";
			s += to_digit(type.texture_dimension());
			s += "D<";
			s += "int";
			if (type.rows > 1)
				s += to_digit(type.rows);
			s += '>';
			return;
		case type::t_storage1d_uint:
		case type::t_storage2d_uint:
		case type::t_storage3d_uint:
			s += "RWTexture";
			s += to_digit(type.texture_dimension());
			s += "D<";
			s += "uint";
			if (type.rows > 1)
				s += to_digit(type.rows);
			s += '>';
			return;
		case type::t_storage1d_float:
		case type::t_storage2d_float:
		case type::t_storage3d_float:
			s += "RWTexture";
			s += to_digit(type.texture_dimension());
			s += "D<";
			s += "float";
			if (type.rows > 1)
				s += to_digit(type.rows);
			s += '>';
			return;
		default:
			assert(false);
			return;
		}

		if (type.rows > 1)
			s += to_digit(type.rows);
		if (type.cols > 1)
			s += 'x', s += to_digit(type.cols);
	}
	void write_constant(std::string &s, const type &data_type, const constant &data) const
	{
		if (data_type.is_array())
		{
			assert(data_type.is_bounded_array());

			type elem_type = data_type;
			elem_type.array_length = 0;

			s += "{ ";

			for (unsigned int a = 0; a < data_type.array_length; ++a)
			{
				write_constant(s, elem_type, a < static_cast<unsigned int>(data.array_data.size()) ? data.array_data[a] : constant {});
				s += ", ";
			}

			// Remove trailing ", "
			s.erase(s.size() - 2);

			s += " }";
			return;
		}

		if (data_type.is_struct())
		{
			// The can only be zero initializer struct constants
			assert(data.as_uint[0] == 0);

			s += '(' + id_to_name(data_type.struct_definition) + ")0";
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
				s += std::to_string(data.as_uint[i]);
				break;
			case type::t_min16float:
			case type::t_float:
				if (std::isnan(data.as_float[i])) {
					s += "-1.#IND";
					break;
				}
				if (std::isinf(data.as_float[i])) {
					s += std::signbit(data.as_float[i]) ? "1.#INF" : "-1.#INF";
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
					ss << data.as_float[i];
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
	template <bool force_source = false>
	void write_location(std::string &s, const location &loc)
	{
		if (loc.source.empty() || !_debug_info)
			return;

		s += "#line " + std::to_string(loc.line);

		size_t offset = s.size();

		// Avoid writing the file name every time to reduce output text size
		if constexpr (force_source)
		{
			s += " \"" + loc.source + '\"';
		}
		else if (loc.source != _current_location)
		{
			s += " \"" + loc.source + '\"';

			_current_location = loc.source;
		}

		// Need to escape string for new DirectX Shader Compiler (dxc)
		if (_shader_model >= 60)
		{
			for (; (offset = s.find('\\', offset)) != std::string::npos; offset += 2)
				s.insert(offset, "\\", 1);
		}

		s += '\n';
	}
	void write_texture_format(std::string &s, texture_format format)
	{
		switch (format)
		{
		case texture_format::r32i:
			s += "int";
			break;
		case texture_format::r32u:
			s += "uint";
			break;
		default:
			assert(false);
			[[fallthrough]];
		case texture_format::unknown:
		case texture_format::r8:
		case texture_format::r16:
		case texture_format::r16f:
		case texture_format::r32f:
		case texture_format::rg8:
		case texture_format::rg16:
		case texture_format::rg16f:
		case texture_format::rg32f:
		case texture_format::rgba8:
		case texture_format::rgba16:
		case texture_format::rgba16f:
		case texture_format::rgba32f:
		case texture_format::rgb10a2:
			s += "float4";
			break;
		}
	}

	std::string id_to_name(id id) const
	{
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
		name = escape_name(std::move(name));
		if constexpr (naming_type == naming::general)
			if (std::find_if(_names.begin(), _names.end(),
					[&name](const auto &names_it) { return names_it.second == name; }) != _names.end())
				name += '_' + std::to_string(id); // Append a numbered suffix if the name already exists
		_names[id] = std::move(name);
	}

	std::string convert_semantic(const std::string &semantic, uint32_t max_attributes = 1)
	{
		if (_shader_model < 40)
		{
			if (semantic == "SV_POSITION")
				return "POSITION"; // For pixel shaders this has to be "VPOS", so need to redefine that in post
			if (semantic == "VPOS")
				return "VPOS";
			if (semantic == "SV_POINTSIZE")
				return "PSIZE";
			if (semantic.compare(0, 9, "SV_TARGET") == 0)
				return "COLOR" + semantic.substr(9);
			if (semantic == "SV_DEPTH")
				return "DEPTH";
			if (semantic == "SV_VERTEXID")
				return "TEXCOORD0 /* VERTEXID */";
			if (semantic == "SV_ISFRONTFACE")
				return "VFACE";

			size_t digit_index = semantic.size() - 1;
			while (digit_index != 0 && semantic[digit_index] >= '0' && semantic[digit_index] <= '9')
				digit_index--;
			digit_index++;

			const std::string semantic_base = semantic.substr(0, digit_index);

			uint32_t semantic_digit = 0;
			std::from_chars(semantic.c_str() + digit_index, semantic.c_str() + semantic.size(), semantic_digit);

			if (semantic_base == "TEXCOORD")
			{
				if (semantic_digit < 15)
				{
					assert(_remapped_semantics[semantic_digit].empty() || _remapped_semantics[semantic_digit] == semantic); // Mixing custom semantic names and multiple TEXCOORD indices is not supported
					_remapped_semantics[semantic_digit] = semantic;
				}
			}
			// Shader model 3 only supports a selected list of semantic names, so need to remap custom ones to that
			else if (
				semantic_base != "COLOR" &&
				semantic_base != "NORMAL" &&
				semantic_base != "TANGENT" &&
				semantic_base != "BINORMAL")
			{
				// Legal semantic indices are between 0 and 15, but skip first entry in case both custom semantic names and the common TEXCOORD0 exist
				for (int i = 1; i < 15; ++i)
				{
					if (_remapped_semantics[i].empty() || _remapped_semantics[i] == semantic)
					{
						for (uint32_t a = 0; a < max_attributes && i + a < 15; ++a)
							_remapped_semantics[i + a] = semantic_base + std::to_string(semantic_digit + a);

						return "TEXCOORD" + std::to_string(i) + " /* " + semantic + " */";
					}
				}
			}
		}
		else
		{
			if (semantic.compare(0, 5, "COLOR") == 0)
				return "SV_TARGET" + semantic.substr(5);
		}

		return semantic;
	}

	static std::string escape_name(std::string name)
	{
		static const auto stringicmp = [](const std::string &a, const std::string &b) {
#ifdef _WIN32
			return _stricmp(a.c_str(), b.c_str()) == 0;
#else
			return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](std::string::value_type a, std::string::value_type b) { return std::tolower(a) == std::tolower(b); });
#endif
		};

		// HLSL compiler complains about "technique" and "pass" names in strict mode (no matter the casing)
		if (stringicmp(name, "line") ||
			stringicmp(name, "pass") ||
			stringicmp(name, "technique") ||
			stringicmp(name, "point") ||
			stringicmp(name, "export") ||
			stringicmp(name, "extern") ||
			stringicmp(name, "compile") ||
			stringicmp(name, "discard") ||
			stringicmp(name, "half") ||
			stringicmp(name, "in") ||
			stringicmp(name, "lineadj") ||
			stringicmp(name, "matrix") ||
			stringicmp(name, "sample") ||
			stringicmp(name, "sampler") ||
			stringicmp(name, "shared") ||
			stringicmp(name, "precise") ||
			stringicmp(name, "register") ||
			stringicmp(name, "texture") ||
			stringicmp(name, "unorm") ||
			stringicmp(name, "triangle") ||
			stringicmp(name, "triangleadj") ||
			stringicmp(name, "out") ||
			stringicmp(name, "vector"))
			// This is guaranteed to not clash with user defined names, since those starting with an underscore are filtered out in 'define_name'
			name = '_' + name;

		return name;
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
			write_type<true>(code, member.type); // HLSL allows interpolation attributes on struct members, so handle this like a parameter
			code += ' ' + member.name;

			if (member.type.is_array())
				code += '[' + std::to_string(member.type.array_length) + ']';

			if (!member.semantic.empty())
				code += " : " + convert_semantic(member.semantic, std::max(1u, member.type.components() / 4) * std::max(1u, member.type.array_length));

			code += ";\n";
		}

		code += "};\n";

		return res;
	}
	id   define_texture(const location &, texture &info) override
	{
		const id res = info.id = make_id();

		_module.textures.push_back(info);

		return res;
	}
	id   define_sampler(const location &loc, const texture &tex_info, sampler &info) override
	{
		const id res = info.id = create_block();
		define_name<naming::unique>(res, info.unique_name);

		std::string &code = _blocks.at(res);

		// Default to a register index equivalent to the entry in the sampler list (this is later overwritten in 'finalize_code_for_entry_point' to a more optimal placement)
		const uint32_t default_binding = static_cast<uint32_t>(_module.samplers.size());
		uint32_t sampler_state_binding = 0;

		if (_shader_model >= 40)
		{
#if 0
			// Try and reuse a sampler binding with the same sampler description
			const auto existing_sampler_it = std::find_if(_sampler_lookup.begin(), _sampler_lookup.end(),
				[&info](const sampler_desc &existing_info) {
					return
						existing_info.filter == info.filter &&
						existing_info.address_u == info.address_u &&
						existing_info.address_v == info.address_v &&
						existing_info.address_w == info.address_w &&
						existing_info.min_lod == info.min_lod &&
						existing_info.max_lod == info.max_lod &&
						existing_info.lod_bias == info.lod_bias;
				});
			if (existing_sampler_it != _sampler_lookup.end())
			{
				sampler_state_binding = existing_sampler_it->binding;
			}
			else
			{
				sampler_state_binding = static_cast<uint32_t>(_sampler_lookup.size());

				sampler_binding s;
				s.filter = info.filter;
				s.address_u = info.address_u;
				s.address_v = info.address_v;
				s.address_w = info.address_w;
				s.min_lod = info.min_lod;
				s.max_lod = info.max_lod;
				s.lod_bias = info.lod_bias;
				s.binding = sampler_state_binding;
				_sampler_lookup.push_back(std::move(s));

				if (_shader_model >= 60)
					_blocks.at(0) += "[[vk::binding(" + std::to_string(sampler_state_binding) + ", 1)]] "; // Descriptor set 1

				_blocks.at(0) += "SamplerState __s" + std::to_string(sampler_state_binding) + " : register(s" + std::to_string(sampler_state_binding) + ");\n";
			}

			if (_shader_model >= 60)
				code += "[[vk::binding(" + std::to_string(default_binding) + ", 2)]] "; // Descriptor set 2

			code += "Texture";
			code += to_digit(static_cast<unsigned int>(tex_info.type));
			code += "D<";
			write_texture_format(code, tex_info.format);
			code += "> __" + info.unique_name + "_t : register(t" + std::to_string(default_binding) + "); \n";

			write_location(code, loc);

			code += "static const ";
			write_type(code, info.type);
			code += ' ' + id_to_name(res) + " = { __" + info.unique_name + "_t, __s" + std::to_string(sampler_state_binding) + " };\n";
#else
			code += "Texture";
			code += to_digit(static_cast<unsigned int>(tex_info.type));
			code += "D<";
			write_texture_format(code, tex_info.format);
			code += "> __" + info.unique_name + "_t : register(t" + std::to_string(default_binding) + "); \n";

			code += "SamplerState __" + info.unique_name + "_s : register(s" + std::to_string(default_binding) + "); \n";

			write_location(code, loc);

			code += "static const ";
			write_type(code, info.type);
			code += ' ' + id_to_name(res) + " = { __" + info.unique_name + "_t, __" + info.unique_name + "_s };\n";
#endif
		}
		else
		{
			const unsigned int texture_dimension = info.type.texture_dimension();

			code += "sampler";
			code += to_digit(texture_dimension);
			code += "D __" + info.unique_name + "_s : register(s" + std::to_string(default_binding) + ");\n";

			write_location(code, loc);

			code += "static const ";
			write_type(code, info.type);
			code += ' ' + id_to_name(res) + " = { __" + info.unique_name + "_s, float" + to_digit(texture_dimension) + '(';

			if (tex_info.semantic.empty())
			{
					code +=   "1.0 / " + std::to_string(tex_info.width);
				if (texture_dimension >= 2)
					code += ", 1.0 / " + std::to_string(tex_info.height);
				if (texture_dimension >= 3)
					code += ", 1.0 / " + std::to_string(tex_info.depth);
			}
			else
			{
				// Expect application to set inverse texture size via a define if it is not known here
				code += tex_info.semantic + "_PIXEL_SIZE";
			}

			code += ") }; \n";
		}

		_module.samplers.push_back(info);

		return res;
	}
	id   define_storage(const location &loc, const texture &, storage &info) override
	{
		const id res = info.id = create_block();
		define_name<naming::unique>(res, info.unique_name);

		// Default to a register index equivalent to the entry in the storage list (this is later overwritten in 'finalize_code_for_entry_point' to a more optimal placement)
		const uint32_t default_binding = static_cast<uint32_t>(_module.storages.size());

		if (_shader_model >= 50)
		{
			std::string &code = _blocks.at(res);

			write_location(code, loc);

#if 0
			if (_shader_model >= 60)
				code += "[[vk::binding(" + std::to_string(default_binding) + ", 3)]] "; // Descriptor set 3
#endif

			write_type(code, info.type);
			code += ' ' + info.unique_name + " : register(u" + std::to_string(default_binding) + ");\n";
		}

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

			code += "static const ";
			write_type(code, info.type);
			code += ' ' + id_to_name(res) + " = ";
			if (!info.type.is_scalar())
				write_type<false, false>(code, info.type);
			code += "(SPEC_CONSTANT_" + info.name + ");\n";

			_module.spec_constants.push_back(info);
		}
		else
		{
			if (info.type.is_matrix())
				info.size = align_up(info.type.cols * 4, 16, info.type.rows);
			else // Vectors are column major (1xN), matrices are row major (NxM)
				info.size = info.type.rows * 4;
			// Arrays are not packed in HLSL by default, each element is stored in a four-component vector (16 bytes)
			if (info.type.is_array())
				info.size = align_up(info.size, 16, info.type.array_length);

			if (_shader_model < 40)
				_module.total_uniform_size /= 4;

			// Data is packed into 4-byte boundaries (see https://docs.microsoft.com/windows/win32/direct3dhlsl/dx-graphics-hlsl-packing-rules)
			// This is already guaranteed, since all types are at least 4-byte in size
			info.offset = _module.total_uniform_size;
			// Additionally, HLSL packs data so that it does not cross a 16-byte boundary
			const uint32_t remaining = 16 - (info.offset & 15);
			if (remaining != 16 && info.size > remaining)
				info.offset += remaining;
			_module.total_uniform_size = info.offset + info.size;

			write_location<true>(_cbuffer_block, loc);

			if (_shader_model >= 40)
				_cbuffer_block += '\t';
			if (info.type.is_matrix()) // Force row major matrices
				_cbuffer_block += "row_major ";

			type type = info.type;
			if (_shader_model < 40)
			{
				// The HLSL compiler tries to evaluate boolean values with temporary registers, which breaks branches, so force it to use constant float registers
				if (type.is_boolean())
					type.base = type::t_float;

				// Simply put each uniform into a separate constant register in shader model 3 for now
				info.offset *= 4;
				_module.total_uniform_size *= 4;
			}

			write_type(_cbuffer_block, type);
			_cbuffer_block += ' ' + id_to_name(res);

			if (info.type.is_array())
				_cbuffer_block += '[' + std::to_string(info.type.array_length) + ']';

			if (_shader_model < 40)
			{
				// Every constant register is 16 bytes wide, so divide memory offset by 16 to get the constant register index
				// Note: All uniforms are floating-point in shader model 3, even if the uniform type says different!!
				_cbuffer_block += " : register(c" + std::to_string(info.offset / 16) + ')';
			}

			_cbuffer_block += ";\n";

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
		define_name<naming::unique>(res, info.unique_name);

		assert(_current_block == 0 && (_current_function_declaration.empty() || info.type != shader_type::unknown));
		std::string &code = _current_function_declaration;

		write_location(code, loc);

		write_type(code, info.return_type);
		code += ' ' + id_to_name(res) + '(';

		for (member_type &param : info.parameter_list)
		{
			param.id = make_id();
			define_name<naming::unique>(param.id, param.name);

			code += '\n';
			write_location(code, param.location);
			code += '\t';
			write_type<true>(code, param.type);
			code += ' ' + id_to_name(param.id);

			if (param.type.is_array())
				code += '[' + std::to_string(param.type.array_length) + ']';

			if (!param.semantic.empty())
				code += " : " + convert_semantic(param.semantic, std::max(1u, param.type.cols / 4u) * std::max(1u, param.type.array_length));

			code += ',';
		}

		// Remove trailing comma
		if (!info.parameter_list.empty())
			code.pop_back();

		code += ')';

		if (!info.return_semantic.empty())
			code += " : " + convert_semantic(info.return_semantic);

		code += '\n';

		_functions.push_back(std::make_unique<function>(info));
		_current_function = _functions.back().get();

		return res;
	}

	void define_entry_point(function &func) override
	{
		// Modify entry point name since a new function is created for it below
		assert(!func.unique_name.empty() && func.unique_name[0] == 'F');
		if (_shader_model < 40 || func.type == shader_type::compute)
			func.unique_name[0] = 'E';

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

		// Only have to rewrite the entry point function signature in shader model 3 and for compute (to write "numthreads" attribute)
		if (_shader_model >= 40 && func.type != shader_type::compute)
			return;

		function entry_point = func;
		entry_point.referenced_functions.push_back(func.id);

		const auto is_color_semantic = [](const std::string &semantic) {
			return semantic.compare(0, 9, "SV_TARGET") == 0 || semantic.compare(0, 5, "COLOR") == 0; };
		const auto is_position_semantic = [](const std::string &semantic) {
			return semantic == "SV_POSITION" || semantic == "POSITION"; };

		const id ret = make_id();
		define_name<naming::general>(ret, "ret");

		std::string position_variable_name;
		{
			if (func.type == shader_type::vertex && func.return_type.is_struct())
			{
				// If this function returns a struct which contains a position output, keep track of its member name
				for (const member_type &member : get_struct(func.return_type.struct_definition).member_list)
					if (is_position_semantic(member.semantic))
						position_variable_name = id_to_name(ret) + '.' + member.name;
			}

			if (is_color_semantic(func.return_semantic))
			{
				// The COLOR output semantic has to be a four-component vector in shader model 3, so enforce that
				entry_point.return_type.rows = 4;
			}
			if (is_position_semantic(func.return_semantic))
			{
				if (func.type == shader_type::vertex)
					// Keep track of the position output variable
					position_variable_name = id_to_name(ret);
			}
		}
		for (member_type &param : entry_point.parameter_list)
		{
			if (func.type == shader_type::vertex && param.type.is_struct())
			{
				for (const member_type &member : get_struct(param.type.struct_definition).member_list)
					if (is_position_semantic(member.semantic))
						position_variable_name = id_to_name(param.id) + '.' + member.name;
			}

			if (is_color_semantic(param.semantic))
			{
				param.type.rows = 4;
			}
			if (is_position_semantic(param.semantic))
			{
				if (func.type == shader_type::vertex)
					// Keep track of the position output variable
					position_variable_name = id_to_name(param.id);
				else if (func.type == shader_type::pixel)
					// Change the position input semantic in pixel shaders
					param.semantic = "VPOS";
			}
		}

		assert(_current_function_declaration.empty());
		if (func.type == shader_type::compute)
			_current_function_declaration += "[numthreads(" +
				std::to_string(func.num_threads[0]) + ", " +
				std::to_string(func.num_threads[1]) + ", " +
				std::to_string(func.num_threads[2]) + ")]\n";

		define_function({}, entry_point);
		enter_block(create_block());

		std::string &code = _blocks.at(_current_block);

		// Clear all color output parameters so no component is left uninitialized
		for (const member_type &param : entry_point.parameter_list)
		{
			if (is_color_semantic(param.semantic))
				code += '\t' + id_to_name(param.id) + " = float4(0.0, 0.0, 0.0, 0.0);\n";
		}

		code += '\t';
		if (is_color_semantic(func.return_semantic))
		{
			code += "const float4 " + id_to_name(ret) + " = float4(";
		}
		else if (!func.return_type.is_void())
		{
			write_type(code, func.return_type);
			code += ' ' + id_to_name(ret) + " = ";
		}

		// Call the function this entry point refers to
		code += id_to_name(func.id) + '(';

		for (size_t i = 0; i < func.parameter_list.size(); ++i)
		{
			code += id_to_name(entry_point.parameter_list[i].id);

			const member_type &param = func.parameter_list[i];

			if (is_color_semantic(param.semantic))
			{
				code += '.';
				for (unsigned int c = 0; c < param.type.rows; c++)
					code += "xyzw"[c];
			}

			code += ", ";
		}

		// Remove trailing ", "
		if (!entry_point.parameter_list.empty())
			code.erase(code.size() - 2);

		code += ')';

		// Cast the output value to a four-component vector
		if (is_color_semantic(func.return_semantic))
		{
			for (unsigned int c = 0; c < (4 - func.return_type.rows); c++)
				code += ", 0.0";
			code += ')';
		}

		code += ";\n";

		// Shift everything by half a viewport pixel to workaround the different half-pixel offset in D3D9 (https://aras-p.info/blog/2016/04/08/solving-dx9-half-pixel-offset/)
		if (func.type == shader_type::vertex && !position_variable_name.empty()) // Check if we are in a vertex shader definition
			code += '\t' + position_variable_name + ".xy += __TEXEL_SIZE__ * " + position_variable_name + ".ww;\n";

		leave_block_and_return(func.return_type.is_void() ? 0 : ret);
		leave_function();
	}

	id   emit_load(const expression &exp, bool force_new_id) override
	{
		if (exp.is_constant)
			return emit_constant(exp.type, exp.constant);
		else if (exp.chain.empty() && !force_new_id) // Can refer to values without access chain directly
			return exp.base;

		const id res = make_id();

		static const char s_matrix_swizzles[16][5] = {
			"_m00", "_m01", "_m02", "_m03",
			"_m10", "_m11", "_m12", "_m13",
			"_m20", "_m21", "_m22", "_m23",
			"_m30", "_m31", "_m32", "_m33"
		};

		std::string type, expr_code = id_to_name(exp.base);

		for (const expression::operation &op : exp.chain)
		{
			switch (op.op)
			{
			case expression::operation::op_cast:
				type.clear();
				write_type<false, false>(type, op.to);
				// Cast is in parentheses so that a subsequent operation operates on the casted value
				expr_code = "((" + type + ')' + expr_code + ')';
				break;
			case expression::operation::op_member:
				expr_code += '.';
				expr_code += get_struct(op.from.struct_definition).member_list[op.index].name;
				break;
			case expression::operation::op_dynamic_index:
				expr_code += '[' + id_to_name(op.index) + ']';
				break;
			case expression::operation::op_constant_index:
				if (op.from.is_vector() && !op.from.is_array())
					expr_code += '.',
					expr_code += "xyzw"[op.index];
				else
					expr_code += '[' + std::to_string(op.index) + ']';
				break;
			case expression::operation::op_swizzle:
				expr_code += '.';
				for (int i = 0; i < 4 && op.swizzle[i] >= 0; ++i)
					if (op.from.is_matrix())
						expr_code += s_matrix_swizzles[op.swizzle[i]];
					else
						expr_code += "xyzw"[op.swizzle[i]];
				break;
			}
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
		std::string &code = _blocks.at(_current_block);

		write_location(code, exp.location);

		code += '\t' + id_to_name(exp.base);

		static const char s_matrix_swizzles[16][5] = {
			"_m00", "_m01", "_m02", "_m03",
			"_m10", "_m11", "_m12", "_m13",
			"_m20", "_m21", "_m22", "_m23",
			"_m30", "_m31", "_m32", "_m33"
		};

		for (const expression::operation &op : exp.chain)
		{
			switch (op.op)
			{
			case expression::operation::op_member:
				code += '.';
				code += get_struct(op.from.struct_definition).member_list[op.index].name;
				break;
			case expression::operation::op_dynamic_index:
				code += '[' + id_to_name(op.index) + ']';
				break;
			case expression::operation::op_constant_index:
				code += '[' + std::to_string(op.index) + ']';
				break;
			case expression::operation::op_swizzle:
				code += '.';
				for (int i = 0; i < 4 && op.swizzle[i] >= 0; ++i)
					if (op.from.is_matrix())
						code += s_matrix_swizzles[op.swizzle[i]];
					else
						code += "xyzw"[op.swizzle[i]];
				break;
			}
		}

		code += " = " + id_to_name(value) + ";\n";
	}

	id   emit_constant(const type &data_type, const constant &data) override
	{
		const id res = make_id();

		if (data_type.is_array())
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
			else
				_constant_lookup.push_back({ data_type, data, res });

			// Put constant variable into global scope, so that it can be reused in different blocks
			std::string &code = _blocks.at(0);

			// Array constants need to be stored in a constant variable as they cannot be used in-place
			code += "static const ";
			write_type<false, false>(code, data_type);
			code += ' ' + id_to_name(res);
			code += '[' + std::to_string(data_type.array_length) + ']';
			code += " = ";
			write_constant(code, data_type, data);
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

		if (_shader_model < 40 && op == tokenid::tilde)
			code += "0xFFFFFFFF - "; // Emulate bitwise not operator on shader model 3
		else
			code += char(op);

		code += id_to_name(val) + ";\n";

		return res;
	}
	id   emit_binary_op(const location &loc, tokenid op, const type &res_type, const type &, id lhs, id rhs) override
	{
		const id res = make_id();

		std::string &code = _blocks.at(_current_block);

		write_location(code, loc);

		code += '\t';
		write_type(code, res_type);
		code += ' ' + id_to_name(res) + " = ";

		if (_shader_model < 40)
		{
			// See bitwise shift operator emulation below
			if (op == tokenid::less_less || op == tokenid::less_less_equal)
				code += '(';
			else if (op == tokenid::greater_greater || op == tokenid::greater_greater_equal)
				code += "floor(";
		}

		code += id_to_name(lhs) + ' ';

		switch (op)
		{
		case tokenid::plus:
		case tokenid::plus_plus:
		case tokenid::plus_equal:
			code += '+';
			break;
		case tokenid::minus:
		case tokenid::minus_minus:
		case tokenid::minus_equal:
			code += '-';
			break;
		case tokenid::star:
		case tokenid::star_equal:
			code += '*';
			break;
		case tokenid::slash:
		case tokenid::slash_equal:
			code += '/';
			break;
		case tokenid::percent:
		case tokenid::percent_equal:
			code += '%';
			break;
		case tokenid::caret:
		case tokenid::caret_equal:
			code += '^';
			break;
		case tokenid::pipe:
		case tokenid::pipe_equal:
			code += '|';
			break;
		case tokenid::ampersand:
		case tokenid::ampersand_equal:
			code += '&';
			break;
		case tokenid::less_less:
		case tokenid::less_less_equal:
			code += _shader_model >= 40 ? "<<" : ") * exp2("; // Emulate bitwise shift operators on shader model 3
			break;
		case tokenid::greater_greater:
		case tokenid::greater_greater_equal:
			code += _shader_model >= 40 ? ">>" : ") / exp2(";
			break;
		case tokenid::pipe_pipe:
			code += "||";
			break;
		case tokenid::ampersand_ampersand:
			code += "&&";
			break;
		case tokenid::less:
			code += '<';
			break;
		case tokenid::less_equal:
			code += "<=";
			break;
		case tokenid::greater:
			code += '>';
			break;
		case tokenid::greater_equal:
			code += ">=";
			break;
		case tokenid::equal_equal:
			code += "==";
			break;
		case tokenid::exclaim_equal:
			code += "!=";
			break;
		default:
			assert(false);
		}

		code += ' ' + id_to_name(rhs);

		if (_shader_model < 40)
		{
			// See bitwise shift operator emulation above
			if (op == tokenid::less_less || op == tokenid::less_less_equal ||
				op == tokenid::greater_greater || op == tokenid::greater_greater_equal)
				code += ')';
		}

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

		code += " = " + id_to_name(condition) + " ? " + id_to_name(true_value) + " : " + id_to_name(false_value) + ";\n";

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

		enum
		{
		#define IMPLEMENT_INTRINSIC_HLSL(name, i, code) name##i,
			#include "effect_symbol_table_intrinsics.inl"
		};

		write_location(code, loc);

		code += '\t';

		if (!res_type.is_void())
		{
			write_type(code, res_type);
			code += ' ' + id_to_name(res) + " = ";
		}

		switch (intrinsic)
		{
		#define IMPLEMENT_INTRINSIC_HLSL(name, i, code) case name##i: code break;
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

		if (res_type.is_array())
			code += "{ ";
		else
			write_type<false, false>(code, res_type), code += '(';

		for (const expression &arg : args)
		{
			code += id_to_name(arg.base);
			code += ", ";
		}

		// Remove trailing ", "
		if (!args.empty())
			code.erase(code.size() - 2);

		if (res_type.is_array())
			code += " }";
		else
			code += ')';

		code += ";\n";

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

		code += '\t';

		if (flags & 0x1) code += "[flatten] ";
		if (flags & 0x2) code += "[branch] ";

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
		if (flags & 0x1)
			attributes += "[unroll] ";
		if (flags & 0x2)
			attributes += _shader_model >= 40 ? "[fastopt] " : "[loop] ";

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

			code += '\t' + attributes;
			code += "do\n\t{\n\t\t{\n";
			code += loop_data; // Encapsulate loop body into another scope, so not to confuse any local variables with the current iteration variable accessed in the continue block below
			code += "\t\t}\n";
			code += continue_data;
			code += "\t}\n\twhile (" + condition_name + ");\n";
		}
		else
		{
			std::string &condition_data = _blocks.at(condition_block);

			// Work around D3DCompiler putting uniform variables that are used as the loop count register into integer registers (only in SM3)
			// Only applies to dynamic loops with uniform variables in the condition, where it generates a loop instruction like "rep i0", but then expects the "i0" register to be set externally
			// Moving the loop condition into the loop body forces it to move the uniform variable into a constant register instead and geneates a fixed number of loop iterations with "defi i0, 255, ..."
			// Check 'condition_name' instead of 'condition_value' here to also catch cases where a constant boolean expression was passed in as loop condition
			bool use_break_statement_for_condition = (_shader_model < 40 && condition_name != "true") &&
				std::find_if(_module.uniforms.begin(), _module.uniforms.end(),
					[&](const uniform &info) {
						return condition_data.find(info.name) != std::string::npos || condition_name.find(info.name) != std::string::npos;
					}) != _module.uniforms.end();

			// If the condition data is just a single line, then it is a simple expression, which we can just put into the loop condition as-is
			if (!use_break_statement_for_condition && std::count(condition_data.begin(), condition_data.end(), '\n') == 1)
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

			write_location(code, loc);

			code += '\t' + attributes;
			if (use_break_statement_for_condition)
				code += "while (true)\n\t{\n\t\tif (" + condition_name + ")\n\t\t{\n";
			else
				code += "while (" + condition_name + ")\n\t{\n\t\t{\n";
			code += loop_data;
			code += "\t\t}\n";
			if (use_break_statement_for_condition)
				code += "\t\telse break;\n";
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
	void emit_switch(const location &loc, id selector_value, id selector_block, id default_label, id default_block, const std::vector<id> &case_literal_and_labels, const std::vector<id> &case_blocks, unsigned int flags) override
	{
		assert(selector_value != 0 && selector_block != 0 && default_label != 0 && default_block != 0);
		assert(case_blocks.size() == case_literal_and_labels.size() / 2);

		std::string &code = _blocks.at(_current_block);

		code += _blocks.at(selector_block);

		if (_shader_model >= 40)
		{
			write_location(code, loc);

			code += '\t';

			if (flags & 0x1) code += "[flatten] ";
			if (flags & 0x2) code += "[branch] ";
			if (flags & 0x4) code += "[forcecase] ";
			if (flags & 0x8) code += "[call] ";

			code += "switch (" + id_to_name(selector_value) + ")\n\t{\n";

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
		}
		else // Switch statements do not work correctly in SM3 if a constant is used as selector value (this is a D3DCompiler bug), so replace them with if statements
		{
			write_location(code, loc);

			code += "\t[unroll] do { "; // This dummy loop makes "break" statements work

			if (flags & 0x1) code += "[flatten] ";
			if (flags & 0x2) code += "[branch] ";

			std::vector<id> labels = case_literal_and_labels;
			for (size_t i = 0; i < labels.size(); i += 2)
			{
				if (labels[i + 1] == 0)
					continue; // Happens if a case was already handled, see below

				code += "if (" + id_to_name(selector_value) + " == " + std::to_string(labels[i]);

				for (size_t k = i + 2; k < labels.size(); k += 2)
				{
					if (labels[k + 1] == 0 || labels[k + 1] != labels[i + 1])
						continue;

					code += " || " + id_to_name(selector_value) + " == " + std::to_string(labels[k]);
					labels[k + 1] = 0;
				}

				assert(case_blocks[i / 2] != 0);
				std::string &case_data = _blocks.at(case_blocks[i / 2]);

				increase_indentation_level(case_data);

				code += ")\n\t{\n";
				code += case_data;
				code += "\t}\n\telse\n\t";
			}

			code += "{\n";

			if (default_block != _current_block)
			{
				std::string &default_data = _blocks.at(default_block);

				increase_indentation_level(default_data);

				code += default_data;

				_blocks.erase(default_block);
			}

			code += "\t} } while (false);\n";
		}

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
			// HLSL compiler doesn't handle discard like a shader kill
			// Add a return statement to exit functions in case discard is the last control flow statement
			// See https://docs.microsoft.com/windows/win32/direct3dhlsl/discard--sm4---asm-
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

codegen *reshadefx::create_codegen_hlsl(unsigned int shader_model, bool debug_info, bool uniforms_to_spec_constants)
{
	return new codegen_hlsl(shader_model, debug_info, uniforms_to_spec_constants);
}
