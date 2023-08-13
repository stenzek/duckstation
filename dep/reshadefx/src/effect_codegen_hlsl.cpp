/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include <cmath> // std::signbit, std::isinf, std::isnan
#include <cctype> // std::tolower
#include <cstdio> // std::snprintf
#include <cassert>
#include <cstring> // stricmp
#include <algorithm> // std::find_if, std::max

using namespace reshadefx;

class codegen_hlsl final : public codegen
{
public:
	codegen_hlsl(unsigned int shader_model, bool debug_info, bool uniforms_to_spec_constants)
		: _shader_model(shader_model), _debug_info(debug_info), _uniforms_to_spec_constants(uniforms_to_spec_constants)
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

	std::string _cbuffer_block;
	std::string _current_location;
	std::unordered_map<id, std::string> _names;
	std::unordered_map<id, std::string> _blocks;
	unsigned int _shader_model = 0;
	bool _debug_info = false;
	bool _uniforms_to_spec_constants = false;
	std::unordered_map<std::string, std::string> _remapped_semantics;

	// Only write compatibility intrinsics to result if they are actually in use
	bool _uses_bitwise_cast = false;

	void write_result(module &module) override
	{
		module = std::move(_module);

		std::string preamble;

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

			if (!_cbuffer_block.empty())
			{
				if (_shader_model >= 60)
					preamble += "[[vk::binding(0, 0)]] "; // Descriptor set 0

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
					"int __asint(float v) {"
					"	if (v == 0) return 0;" // Zero (does not handle negative zero)
					//	if (isinf(v)) return v < 0 ? 4286578688 : 2139095040; // Infinity
					//	if (isnan(v)) return 2147483647; // NaN (does not handle negative NaN)
					"	float e = 0;"
					"	float f = frexp(v, e) * 2 - 1;" // frexp does not include sign bit in HLSL, so can use as is
					"	float m = ldexp(f, 23);"
					"	return (v < 0 ? 2147483648 : 0) + ldexp(e + 126, 23) + m;"
					"}\n"
					"int2 __asint(float2 v) { return int2(__asint(v.x), __asint(v.y)); }\n"
					"int3 __asint(float3 v) { return int3(__asint(v.x), __asint(v.y), __asint(v.z)); }\n"
					"int4 __asint(float4 v) { return int4(__asint(v.x), __asint(v.y), __asint(v.z), __asint(v.w)); }\n"

					"int __asuint(float v) { return __asint(v); }\n"
					"int2 __asuint(float2 v) { return int2(__asint(v.x), __asint(v.y)); }\n"
					"int3 __asuint(float3 v) { return int3(__asint(v.x), __asint(v.y), __asint(v.z)); }\n"
					"int4 __asuint(float4 v) { return int4(__asint(v.x), __asint(v.y), __asint(v.z), __asint(v.w)); }\n"

					"float __asfloat(int v) {"
					"	float m = v % exp2(23);"
					"	float f = ldexp(m, -23);"
					"	float e = floor(ldexp(v, -23) % 256);"
					"	return (v > 2147483647 ? -1 : 1) * ("
					//		e == 0 ? ldexp(f, -126) : // Denormalized
					//		e == 255 ? (m == 0 ? 1.#INF : -1.#IND) : // Infinity and NaN
					"		ldexp(1 + f, e - 127));"
					"}\n"
					"float2 __asfloat(int2 v) { return float2(__asfloat(v.x), __asfloat(v.y)); }\n"
					"float3 __asfloat(int3 v) { return float3(__asfloat(v.x), __asfloat(v.y), __asfloat(v.z)); }\n"
					"float4 __asfloat(int4 v) { return float4(__asfloat(v.x), __asfloat(v.y), __asfloat(v.z), __asfloat(v.w)); }\n";

			if (!_cbuffer_block.empty())
				preamble += _cbuffer_block;

			// Offsets were multiplied in 'define_uniform', so adjust total size here accordingly
			module.total_uniform_size *= 4;
		}

		module.code.assign(preamble.begin(), preamble.end());

		const std::string &main_block = _blocks.at(0);
		module.code.insert(module.code.end(), main_block.begin(), main_block.end());
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
			s += id_to_name(type.definition);
			return;
		case type::t_sampler1d_int:
			s += "__sampler1D";
			if (_shader_model >= 40)
				s += "_int" + (type.rows > 1 ? std::to_string(type.rows) : std::string());
			return;
		case type::t_sampler2d_int:
			s += "__sampler2D";
			if (_shader_model >= 40)
				s += "_int" + (type.rows > 1 ? std::to_string(type.rows) : std::string());
			return;
		case type::t_sampler3d_int:
			s += "__sampler3D";
			if (_shader_model >= 40)
				s += "_int" + (type.rows > 1 ? std::to_string(type.rows) : std::string());
			return;
		case type::t_sampler1d_uint:
			s += "__sampler1D";
			if (_shader_model >= 40)
				s += "_uint" + (type.rows > 1 ? std::to_string(type.rows) : std::string());
			return;
		case type::t_sampler2d_uint:
			s += "__sampler2D";
			if (_shader_model >= 40)
				s += "_uint" + (type.rows > 1 ? std::to_string(type.rows) : std::string());
			return;
		case type::t_sampler3d_uint:
			s += "__sampler3D";
			if (_shader_model >= 40)
				s += "_uint" + (type.rows > 1 ? std::to_string(type.rows) : std::string());
			return;
		case type::t_sampler1d_float:
			s += "__sampler1D";
			if (_shader_model >= 40)
				s += "_float" + (type.rows > 1 ? std::to_string(type.rows) : std::string());
			return;
		case type::t_sampler2d_float:
			s += "__sampler2D";
			if (_shader_model >= 40)
				s += "_float" + (type.rows > 1 ? std::to_string(type.rows) : std::string());
			return;
		case type::t_sampler3d_float:
			s += "__sampler3D";
			if (_shader_model >= 40)
				s += "_float" + (type.rows > 1 ? std::to_string(type.rows) : std::string());
			return;
		case type::t_storage1d_int:
			s += "RWTexture1D<int" + (type.rows > 1 ? std::to_string(type.rows) : std::string()) + '>';
			return;
		case type::t_storage2d_int:
			s += "RWTexture2D<int" + (type.rows > 1 ? std::to_string(type.rows) : std::string()) + '>';
			return;
		case type::t_storage3d_int:
			s += "RWTexture3D<int" + (type.rows > 1 ? std::to_string(type.rows) : std::string()) + '>';
			return;
		case type::t_storage1d_uint:
			s += "RWTexture1D<uint" + (type.rows > 1 ? std::to_string(type.rows) : std::string()) + '>';
			return;
		case type::t_storage2d_uint:
			s += "RWTexture2D<uint" + (type.rows > 1 ? std::to_string(type.rows) : std::string()) + '>';
			return;
		case type::t_storage3d_uint:
			s += "RWTexture3D<uint" + (type.rows > 1 ? std::to_string(type.rows) : std::string()) + '>';
			return;
		case type::t_storage1d_float:
			s += "RWTexture1D<float" + (type.rows > 1 ? std::to_string(type.rows) : std::string()) + '>';
			return;
		case type::t_storage2d_float:
			s += "RWTexture2D<float" + (type.rows > 1 ? std::to_string(type.rows) : std::string()) + '>';
			return;
		case type::t_storage3d_float:
			s += "RWTexture3D<float" + (type.rows > 1 ? std::to_string(type.rows) : std::string()) + '>';
			return;
		default:
			assert(false);
			return;
		}

		if (type.rows > 1)
			s += std::to_string(type.rows);
		if (type.cols > 1)
			s += 'x' + std::to_string(type.cols);
	}
	void write_constant(std::string &s, const type &type, const constant &data) const
	{
		if (type.is_array())
		{
			auto elem_type = type;
			elem_type.array_length = 0;

			s += "{ ";

			for (int i = 0; i < type.array_length; ++i)
			{
				write_constant(s, elem_type, i < static_cast<int>(data.array_data.size()) ? data.array_data[i] : constant());

				if (i < type.array_length - 1)
					s += ", ";
			}

			s += " }";
			return;
		}

		if (type.is_struct())
		{
			// The can only be zero initializer struct constants
			assert(data.as_uint[0] == 0);

			s += '(' + id_to_name(type.definition) + ")0";
			return;
		}

		// There can only be numeric constants
		assert(type.is_numeric());

		if (!type.is_scalar())
			write_type<false, false>(s, type), s += '(';

		for (unsigned int i = 0, components = type.components(); i < components; ++i)
		{
			switch (type.base)
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
				char temp[64]; // Will be null-terminated by snprintf
				std::snprintf(temp, sizeof(temp), "%1.8e", data.as_float[i]);
				s += temp;
				break;
			default:
				assert(false);
			}

			if (i < components - 1)
				s += ", ";
		}

		if (!type.is_scalar())
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
		case texture_format::r8:
		case texture_format::r16:
		case texture_format::r16f:
		case texture_format::r32f:
			s += "float";
			break;
		default:
			assert(false);
			[[fallthrough]];
		case texture_format::unknown:
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
			if (std::find_if(_names.begin(), _names.end(), [&name](const auto &it) { return it.second == name; }) != _names.end())
				name += '_' + std::to_string(id); // Append a numbered suffix if the name already exists
		_names[id] = std::move(name);
	}

	std::string convert_semantic(const std::string &semantic)
	{
		if (_shader_model < 40)
		{
			if (semantic == "SV_POSITION")
				return "POSITION"; // For pixel shaders this has to be "VPOS", so need to redefine that in post
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

			if (semantic != "VPOS" &&
				semantic.compare(0, 5, "COLOR") != 0 &&
				semantic.compare(0, 6, "NORMAL") != 0 &&
				semantic.compare(0, 7, "TANGENT") != 0)
			{
				// Shader model 3 only supports a selected list of semantic names, so need to remap custom ones to that
				if (const auto it = _remapped_semantics.find(semantic);
					it != _remapped_semantics.end())
					return it->second;

				// Legal semantic indices are between 0 and 15
				if (_remapped_semantics.size() < 15)
				{
					const std::string remapped_semantic = "TEXCOORD" + std::to_string(_remapped_semantics.size()) + " /* " + semantic + " */";
					_remapped_semantics.emplace(semantic, remapped_semantic);
					return remapped_semantic;
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
			stringicmp(name, "technique"))
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

	id   define_struct(const location &loc, struct_info &info) override
	{
		info.definition = make_id();
		define_name<naming::unique>(info.definition, info.unique_name);

		_structs.push_back(info);

		std::string &code = _blocks.at(_current_block);

		write_location(code, loc);

		code += "struct " + id_to_name(info.definition) + "\n{\n";

		for (const struct_member_info &member : info.member_list)
		{
			code += '\t';
			write_type<true>(code, member.type); // HLSL allows interpolation attributes on struct members, so handle this like a parameter
			code += ' ' + member.name;
			if (member.type.is_array())
				code += '[' + std::to_string(member.type.array_length) + ']';
			if (!member.semantic.empty())
				code += " : " + convert_semantic(member.semantic);
			code += ";\n";
		}

		code += "};\n";

		return info.definition;
	}
	id   define_texture(const location &loc, texture_info &info) override
	{
		info.id = make_id();
		info.binding = ~0u;

		define_name<naming::unique>(info.id, info.unique_name);

		if (_shader_model >= 40)
		{
			info.binding = _module.num_texture_bindings;
			_module.num_texture_bindings += 2;

			std::string &code = _blocks.at(_current_block);

			write_location(code, loc);

			if (_shader_model >= 60)
				code += "[[vk::binding(" + std::to_string(info.binding + 0) + ", 2)]] "; // Descriptor set 2

			code += "Texture" + std::to_string(static_cast<unsigned int>(info.type)) + "D<";
			write_texture_format(code, info.format);
			code += "> __"     + info.unique_name + " : register(t" + std::to_string(info.binding + 0) + "); \n";

			if (_shader_model >= 60)
				code += "[[vk::binding(" + std::to_string(info.binding + 1) + ", 2)]] "; // Descriptor set 2

			code += "Texture" + std::to_string(static_cast<unsigned int>(info.type)) + "D<";
			write_texture_format(code, info.format);
			code += "> __srgb" + info.unique_name + " : register(t" + std::to_string(info.binding + 1) + "); \n";
		}

		_module.textures.push_back(info);

		return info.id;
	}
	id   define_sampler(const location &loc, const texture_info &tex_info, sampler_info &info) override
	{
		info.id = make_id();

		define_name<naming::unique>(info.id, info.unique_name);

		std::string &code = _blocks.at(_current_block);

		if (_shader_model >= 40)
		{
			// Try and reuse a sampler binding with the same sampler description
			const auto existing_sampler = std::find_if(_module.samplers.begin(), _module.samplers.end(),
				[&info](const auto &it) {
					return it.filter == info.filter && it.address_u == info.address_u && it.address_v == info.address_v && it.address_w == info.address_w && it.min_lod == info.min_lod && it.max_lod == info.max_lod && it.lod_bias == info.lod_bias;
				});

			if (existing_sampler != _module.samplers.end())
			{
				info.binding = existing_sampler->binding;
			}
			else
			{
				info.binding = _module.num_sampler_bindings++;

				if (_shader_model >= 60)
					code += "[[vk::binding(" + std::to_string(info.binding) + ", 1)]] "; // Descriptor set 1

				code += "SamplerState __s" + std::to_string(info.binding) + " : register(s" + std::to_string(info.binding) + ");\n";
			}

			assert(info.srgb == 0 || info.srgb == 1);
			info.texture_binding = tex_info.binding + info.srgb; // Offset binding by one to choose the SRGB variant

			write_location(code, loc);

			code += "static const ";
			write_type(code, info.type);
			code += ' ' + id_to_name(info.id) + " = { " + (info.srgb ? "__srgb" : "__") + info.texture_name + ", __s" + std::to_string(info.binding) + " };\n";
		}
		else
		{
			info.binding = _module.num_sampler_bindings++;
			info.texture_binding = ~0u; // Unset texture binding

			const unsigned int texture_dimension = info.type.texture_dimension();

			code += "sampler" + std::to_string(texture_dimension) + "D __" + info.unique_name + "_s : register(s" + std::to_string(info.binding) + ");\n";

			write_location(code, loc);

			code += "static const ";
			write_type(code, info.type);
			code += ' ' + id_to_name(info.id) + " = { __" + info.unique_name + "_s, float" + std::to_string(texture_dimension) + '(';

			if (tex_info.semantic.empty())
			{
				code += "1.0 / " + std::to_string(tex_info.width);
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

		return info.id;
	}
	id   define_storage(const location &loc, const texture_info &, storage_info &info) override
	{
		info.id = make_id();
		info.binding = ~0u;

		define_name<naming::unique>(info.id, info.unique_name);

		if (_shader_model >= 50)
		{
			info.binding = _module.num_storage_bindings++;

			std::string &code = _blocks.at(_current_block);

			write_location(code, loc);

			if (_shader_model >= 60)
				code += "[[vk::binding(" + std::to_string(info.binding) + ", 3)]] "; // Descriptor set 3

			write_type(code, info.type);
			code += ' ' + info.unique_name + " : register(u" + std::to_string(info.binding) + ");\n";
		}

		_module.storages.push_back(info);

		return info.id;
	}
	id   define_uniform(const location &loc, uniform_info &info) override
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
		const id res = make_id();

		if (!name.empty())
			define_name<naming::general>(res, name);

		std::string &code = _blocks.at(_current_block);

		write_location(code, loc);

		if (!global)
			code += '\t';

		if (initializer_value != 0 && type.has(type::q_const))
			code += "const ";

		write_type(code, type);
		code += ' ' + id_to_name(res);

		if (type.is_array())
			code += '[' + std::to_string(type.array_length) + ']';

		if (initializer_value != 0)
			code += " = " + id_to_name(initializer_value);

		code += ";\n";

		return res;
	}
	id   define_function(const location &loc, function_info &info) override
	{
		info.definition = make_id();

		define_name<naming::unique>(info.definition, info.unique_name);

		std::string &code = _blocks.at(_current_block);

		write_location(code, loc);

		write_type(code, info.return_type);
		code += ' ' + id_to_name(info.definition) + '(';

		for (size_t i = 0, num_params = info.parameter_list.size(); i < num_params; ++i)
		{
			auto &param = info.parameter_list[i];

			param.definition = make_id();
			define_name<naming::unique>(param.definition, param.name);

			code += '\n';
			write_location(code, param.location);
			code += '\t';
			write_type<true>(code, param.type);
			code += ' ' + id_to_name(param.definition);

			if (param.type.is_array())
				code += '[' + std::to_string(param.type.array_length) + ']';

			if (!param.semantic.empty())
				code += " : " + convert_semantic(param.semantic);

			if (i < num_params - 1)
				code += ',';
		}

		code += ')';

		if (!info.return_semantic.empty())
			code += " : " + convert_semantic(info.return_semantic);

		code += '\n';

		_functions.push_back(std::make_unique<function_info>(info));

		return info.definition;
	}

	void define_entry_point(function_info &func, shader_type stype, int num_threads[3]) override
	{
		// Modify entry point name since a new function is created for it below
		if (stype == shader_type::cs)
			func.unique_name = 'E' + func.unique_name +
				'_' + std::to_string(num_threads[0]) +
				'_' + std::to_string(num_threads[1]) +
				'_' + std::to_string(num_threads[2]);
		else if (_shader_model < 40)
			func.unique_name = 'E' + func.unique_name;

		if (const auto it = std::find_if(_module.entry_points.begin(), _module.entry_points.end(),
				[&func](const auto &ep) { return ep.name == func.unique_name; });
			it != _module.entry_points.end())
			return;

		_module.entry_points.push_back({ func.unique_name, stype });

		// Only have to rewrite the entry point function signature in shader model 3 and for compute (to write "numthreads" attribute)
		if (_shader_model >= 40 && stype != shader_type::cs)
			return;

		auto entry_point = func;

		const auto is_color_semantic = [](const std::string &semantic) {
			return semantic.compare(0, 9, "SV_TARGET") == 0 || semantic.compare(0, 5, "COLOR") == 0; };
		const auto is_position_semantic = [](const std::string &semantic) {
			return semantic == "SV_POSITION" || semantic == "POSITION"; };

		const auto ret = make_id();
		define_name<naming::general>(ret, "ret");

		std::string position_variable_name;
		{
			if (func.return_type.is_struct() && stype == shader_type::vs)
			{
				// If this function returns a struct which contains a position output, keep track of its member name
				for (const struct_member_info &member : get_struct(func.return_type.definition).member_list)
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
				if (stype == shader_type::vs)
					// Keep track of the position output variable
					position_variable_name = id_to_name(ret);
			}
		}
		for (struct_member_info &param : entry_point.parameter_list)
		{
			if (param.type.is_struct() && stype == shader_type::vs)
			{
				for (const struct_member_info &member : get_struct(param.type.definition).member_list)
					if (is_position_semantic(member.semantic))
						position_variable_name = param.name + '.' + member.name;
			}

			if (is_color_semantic(param.semantic))
			{
				param.type.rows = 4;
			}
			if (is_position_semantic(param.semantic))
			{
				if (stype == shader_type::vs)
					// Keep track of the position output variable
					position_variable_name = param.name;
				else if (stype == shader_type::ps)
					// Change the position input semantic in pixel shaders
					param.semantic = "VPOS";
			}
		}

		if (stype == shader_type::cs)
			_blocks.at(_current_block) += "[numthreads(" +
				std::to_string(num_threads[0]) + ", " +
				std::to_string(num_threads[1]) + ", " +
				std::to_string(num_threads[2]) + ")]\n";

		define_function({}, entry_point);
		enter_block(create_block());

		std::string &code = _blocks.at(_current_block);

		// Clear all color output parameters so no component is left uninitialized
		for (struct_member_info &param : entry_point.parameter_list)
		{
			if (is_color_semantic(param.semantic))
				code += '\t' + param.name + " = float4(0.0, 0.0, 0.0, 0.0);\n";
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
		code += id_to_name(func.definition) + '(';

		for (size_t i = 0, num_params = func.parameter_list.size(); i < num_params; ++i)
		{
			code += func.parameter_list[i].name;

			if (is_color_semantic(func.parameter_list[i].semantic))
			{
				code += '.';
				for (unsigned int k = 0; k < func.parameter_list[i].type.rows; k++)
					code += "xyzw"[k];
			}

			if (i < num_params - 1)
				code += ", ";
		}

		code += ')';

		// Cast the output value to a four-component vector
		if (is_color_semantic(func.return_semantic))
		{
			for (unsigned int i = 0; i < 4 - func.return_type.rows; i++)
				code += ", 0.0";
			code += ')';
		}

		code += ";\n";

		// Shift everything by half a viewport pixel to workaround the different half-pixel offset in D3D9 (https://aras-p.info/blog/2016/04/08/solving-dx9-half-pixel-offset/)
		if (!position_variable_name.empty() && stype == shader_type::vs) // Check if we are in a vertex shader definition
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

		for (const auto &op : exp.chain)
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
				expr_code += get_struct(op.from.definition).member_list[op.index].name;
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
				for (unsigned int i = 0; i < 4 && op.swizzle[i] >= 0; ++i)
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

		for (const auto &op : exp.chain)
		{
			switch (op.op)
			{
			case expression::operation::op_member:
				code += '.';
				code += get_struct(op.from.definition).member_list[op.index].name;
				break;
			case expression::operation::op_dynamic_index:
				code += '[' + id_to_name(op.index) + ']';
				break;
			case expression::operation::op_constant_index:
				code += '[' + std::to_string(op.index) + ']';
				break;
			case expression::operation::op_swizzle:
				code += '.';
				for (unsigned int i = 0; i < 4 && op.swizzle[i] >= 0; ++i)
					if (op.from.is_matrix())
						code += s_matrix_swizzles[op.swizzle[i]];
					else
						code += "xyzw"[op.swizzle[i]];
				break;
			}
		}

		code += " = " + id_to_name(value) + ";\n";
	}

	id   emit_constant(const type &type, const constant &data) override
	{
		const id res = make_id();

		if (type.is_array())
		{
			assert(type.has(type::q_const));

			std::string &code = _blocks.at(_current_block);

			// Array constants need to be stored in a constant variable as they cannot be used in-place
			code += '\t';
			code += "const ";
			write_type(code, type);
			code += ' ' + id_to_name(res);
			code += '[' + std::to_string(type.array_length) + ']';
			code += " = ";
			write_constant(code, type, data);
			code += ";\n";
			return res;
		}

		std::string code;
		write_constant(code, type, data);
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

		for (size_t i = 0, num_args = args.size(); i < num_args; ++i)
		{
			code += id_to_name(args[i].base);

			if (i < num_args - 1)
				code += ", ";
		}

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

		if (_shader_model >= 40 && (
			(intrinsic >= tex1Dsize0 && intrinsic <= tex3Dsize2) ||
			(intrinsic >= atomicAdd0 && intrinsic <= atomicCompareExchange1) ||
			(!(res_type.is_floating_point() || _shader_model >= 67) && (intrinsic >= tex1D0 && intrinsic <= tex3Dlod1))))
		{
			// Implementation of the 'tex2Dsize' intrinsic passes the result variable into 'GetDimensions' as output argument
			// Same with the atomic intrinsics, which use the last parameter to return the previous value of the target
			write_type(code, res_type);
			code += ' ' + id_to_name(res) + "; ";
		}
		else if (!res_type.is_void())
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
	id   emit_construct(const location &loc, const type &type, const std::vector<expression> &args) override
	{
#ifndef NDEBUG
		for (const auto &arg : args)
			assert((arg.type.is_scalar() || type.is_array()) && arg.chain.empty() && arg.base != 0);
#endif

		const id res = make_id();

		std::string &code = _blocks.at(_current_block);

		write_location(code, loc);

		code += '\t';
		write_type(code, type);
		code += ' ' + id_to_name(res);

		if (type.is_array())
			code += '[' + std::to_string(type.array_length) + ']';

		code += " = ";

		if (type.is_array())
			code += "{ ";
		else
			write_type<false, false>(code, type), code += '(';

		for (size_t i = 0, num_args = args.size(); i < num_args; ++i)
		{
			code += id_to_name(args[i].base);

			if (i < num_args - 1)
				code += ", ";
		}

		if (type.is_array())
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
	id   emit_phi(const location &loc, id condition_value, id condition_block, id true_value, id true_statement_block, id false_value, id false_statement_block, const type &type) override
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
		write_type(code, type);
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
			auto pos_assign = continue_data.rfind(condition_name);
			auto pos_prev_assign = continue_data.rfind('\t', pos_assign);
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
					[&](const uniform_info &info) {
						return condition_data.find(info.name) != std::string::npos || condition_name.find(info.name) != std::string::npos;
					}) != _module.uniforms.end();

			// If the condition data is just a single line, then it is a simple expression, which we can just put into the loop condition as-is
			if (!use_break_statement_for_condition && std::count(condition_data.begin(), condition_data.end(), '\n') == 1)
			{
				// Convert SSA variable initializer back to a condition expression
				auto pos_assign = condition_data.find('=');
				condition_data.erase(0, pos_assign + 2);
				auto pos_semicolon = condition_data.rfind(';');
				condition_data.erase(pos_semicolon);

				condition_name = std::move(condition_data);
				assert(condition_data.empty());
			}
			else
			{
				code += condition_data;

				increase_indentation_level(condition_data);

				// Convert the last SSA variable initializer to an assignment statement
				auto pos_assign = condition_data.rfind(condition_name);
				auto pos_prev_assign = condition_data.rfind('\t', pos_assign);
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

		const auto &return_type = _functions.back()->return_type;
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
		if (!_functions.back()->return_type.is_void() && value == 0)
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
		assert(_last_block != 0);

		_blocks.at(0) += "{\n" + _blocks.at(_last_block) + "}\n";
	}
};

codegen *reshadefx::create_codegen_hlsl(unsigned int shader_model, bool debug_info, bool uniforms_to_spec_constants)
{
	return new codegen_hlsl(shader_model, debug_info, uniforms_to_spec_constants);
}
