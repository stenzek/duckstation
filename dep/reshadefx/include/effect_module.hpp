/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "effect_expression.hpp"
#include <cstdint>

namespace reshadefx
{
	/// <summary>
	/// Describes an annotation attached to a variable.
	/// </summary>
	struct annotation
	{
		reshadefx::type type = {};
		std::string name;
		reshadefx::constant value = {};
	};

	/// <summary>
	/// Describes a struct member or parameter.
	/// </summary>
	struct member_type
	{
		reshadefx::type type = {};
		uint32_t id = 0;
		std::string name;
		std::string semantic;
		reshadefx::location location;
		bool has_default_value = false;
		reshadefx::constant default_value = {};
	};

	/// <summary>
	/// Describes a struct type defined in effect code.
	/// </summary>
	struct struct_type
	{
		uint32_t id = 0;
		std::string name;
		std::string unique_name;
		std::vector<member_type> member_list;
	};

	/// <summary>
	/// Available texture types.
	/// </summary>
	enum class texture_type : uint8_t
	{
		texture_1d = 1,
		texture_2d = 2,
		texture_3d = 3
	};

	/// <summary>
	/// Available texture formats.
	/// </summary>
	enum class texture_format : uint8_t
	{
		unknown,

		r8,
		r16,
		r16f,
		r32i,
		r32u,
		r32f,
		rg8,
		rg16,
		rg16f,
		rg32f,
		rgba8,
		rgba16,
		rgba16f,
		rgba32f,
		rgb10a2
	};

	/// <summary>
	/// Describes the properties of a <see cref="texture"/> object.
	/// </summary>
	struct texture_desc
	{
		uint32_t width = 1;
		uint32_t height = 1;
		uint16_t depth = 1;
		uint16_t levels = 1;
		texture_type type = texture_type::texture_2d;
		texture_format format = texture_format::rgba8;
	};

	/// <summary>
	/// Describes a texture object defined in effect code.
	/// </summary>
	struct texture : texture_desc
	{
		uint32_t id = 0;
		std::string name;
		std::string unique_name;
		std::string semantic;
		std::vector<annotation> annotations;
		bool render_target = false;
		bool storage_access = false;
	};

	/// <summary>
	/// Describes the binding of a <see cref="texture"/> object.
	/// </summary>
	struct texture_binding
	{
		uint32_t binding = 0;
		std::string texture_name;
		bool srgb = false;
	};

	/// <summary>
	/// Texture filtering modes available for texture sampling operations.
	/// </summary>
	enum class filter_mode : uint8_t
	{
		min_mag_mip_point = 0,
		min_mag_point_mip_linear = 0x1,
		min_point_mag_linear_mip_point = 0x4,
		min_point_mag_mip_linear = 0x5,
		min_linear_mag_mip_point = 0x10,
		min_linear_mag_point_mip_linear = 0x11,
		min_mag_linear_mip_point = 0x14,
		min_mag_mip_linear = 0x15,
		anisotropic = 0x55
	};

	/// <summary>
	/// Sampling behavior at texture coordinates outside the bounds of a texture resource.
	/// </summary>
	enum class texture_address_mode : uint8_t
	{
		wrap = 1,
		mirror = 2,
		clamp = 3,
		border = 4
	};

	/// <summary>
	/// Describes the properties of a <see cref="sampler"/> object.
	/// </summary>
	struct sampler_desc
	{
		filter_mode filter = filter_mode::min_mag_mip_linear;
		texture_address_mode address_u = texture_address_mode::clamp;
		texture_address_mode address_v = texture_address_mode::clamp;
		texture_address_mode address_w = texture_address_mode::clamp;
		float min_lod = -3.402823466e+38f;
		float max_lod = +3.402823466e+38f; // FLT_MAX
		float lod_bias = 0.0f;
	};

	/// <summary>
	/// Describes a texture sampler object defined in effect code.
	/// </summary>
	struct sampler : sampler_desc
	{
		reshadefx::type type = {};
		uint32_t id = 0;
		std::string name;
		std::string unique_name;
		std::string texture_name;
		std::vector<annotation> annotations;
		bool srgb = false;
	};

	/// <summary>
	/// Describes the binding of a <see cref="sampler"/> object.
	/// </summary>
	struct sampler_binding : sampler_desc
	{
		uint32_t binding = 0;
	};

	/// <summary>
	/// Describes the properties of a <see cref="storage"/> object.
	/// </summary>
	struct storage_desc
	{
		uint16_t level = 0;
	};

	/// <summary>
	/// Describes a texture storage object defined in effect code.
	/// </summary>
	struct storage : storage_desc
	{
		reshadefx::type type = {};
		uint32_t id = 0;
		std::string name;
		std::string unique_name;
		std::string texture_name;
	};

	/// <summary>
	/// Describes the binding of a <see cref="storage"/> object.
	/// </summary>
	struct storage_binding : storage_desc
	{
		uint32_t binding = 0;
		std::string texture_name;
	};

	/// <summary>
	/// Describes a uniform variable defined in effect code.
	/// </summary>
	struct uniform
	{
		reshadefx::type type = {};
		std::string name;
		uint32_t size = 0;
		uint32_t offset = 0;
		std::vector<annotation> annotations;
		bool has_initializer_value = false;
		reshadefx::constant initializer_value = {};
	};

	/// <summary>
	/// Type of a shader entry point.
	/// </summary>
	enum class shader_type
	{
		unknown,
		vertex,
		pixel,
		compute
	};

	/// <summary>
	/// Describes a function defined in effect code.
	/// </summary>
	struct function
	{
		reshadefx::type return_type = {};
		uint32_t id = 0;
		std::string name;
		std::string unique_name;
		std::string return_semantic;
		std::vector<member_type> parameter_list;
		shader_type type = shader_type::unknown;
		int num_threads[3] = {};
		std::vector<uint32_t> referenced_samplers;
		std::vector<uint32_t> referenced_storages;
		std::vector<uint32_t> referenced_functions;
	};

	/// <summary>
	/// Color or alpha blending operations.
	/// </summary>
	enum class blend_op : uint8_t
	{
		add = 1,
		subtract,
		reverse_subtract,
		min,
		max
	};

	/// <summary>
	/// Blend factors in color or alpha blending operations, which modulate values between the pixel shader output and render target.
	/// </summary>
	enum class blend_factor : uint8_t
	{
		zero = 0,
		one = 1,
		source_color,
		one_minus_source_color,
		dest_color,
		one_minus_dest_color,
		source_alpha,
		one_minus_source_alpha,
		dest_alpha,
		one_minus_dest_alpha
	};

	/// <summary>
	/// Stencil operations that can be performed during depth-stencil testing.
	/// </summary>
	enum class stencil_op : uint8_t
	{
		zero = 0,
		keep,
		replace,
		increment_saturate,
		decrement_saturate,
		invert,
		increment,
		decrement
	};

	/// <summary>
	/// Comparison operations for depth-stencil testing.
	/// </summary>
	enum class stencil_func : uint8_t
	{
		never,
		less,
		equal,
		less_equal,
		greater,
		not_equal,
		greater_equal,
		always
	};

	/// <summary>
	/// Specifies the possible primitives.
	/// </summary>
	enum class primitive_topology : uint8_t
	{
		point_list = 1,
		line_list,
		line_strip,
		triangle_list,
		triangle_strip
	};

	/// <summary>
	/// Describes a render pass with all its state info.
	/// </summary>
	struct pass
	{
		std::string name;
		std::string render_target_names[8] = {};
		std::string vs_entry_point;
		std::string ps_entry_point;
		std::string cs_entry_point;
		bool generate_mipmaps = true;
		bool clear_render_targets = false;
		bool blend_enable[8] = { false, false, false, false, false, false, false, false };
		blend_factor source_color_blend_factor[8] = { blend_factor::one, blend_factor::one, blend_factor::one, blend_factor::one, blend_factor::one, blend_factor::one, blend_factor::one, blend_factor::one };
		blend_factor dest_color_blend_factor[8] = { blend_factor::zero, blend_factor::zero, blend_factor::zero, blend_factor::zero, blend_factor::zero, blend_factor::zero, blend_factor::zero, blend_factor::zero };
		blend_op color_blend_op[8] = { blend_op::add, blend_op::add, blend_op::add, blend_op::add, blend_op::add, blend_op::add, blend_op::add, blend_op::add };
		blend_factor source_alpha_blend_factor[8] = { blend_factor::one, blend_factor::one, blend_factor::one, blend_factor::one, blend_factor::one, blend_factor::one, blend_factor::one, blend_factor::one };
		blend_factor dest_alpha_blend_factor[8] = { blend_factor::zero, blend_factor::zero, blend_factor::zero, blend_factor::zero, blend_factor::zero, blend_factor::zero, blend_factor::zero, blend_factor::zero };
		blend_op alpha_blend_op[8] = { blend_op::add, blend_op::add, blend_op::add, blend_op::add, blend_op::add, blend_op::add, blend_op::add, blend_op::add };
		bool srgb_write_enable = false;
		uint8_t render_target_write_mask[8] = { 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF };
		bool stencil_enable = false;
		uint8_t stencil_read_mask = 0xFF;
		uint8_t stencil_write_mask = 0xFF;
		stencil_func stencil_comparison_func = stencil_func::always;
		stencil_op stencil_pass_op = stencil_op::keep;
		stencil_op stencil_fail_op = stencil_op::keep;
		stencil_op stencil_depth_fail_op = stencil_op::keep;
		primitive_topology topology = primitive_topology::triangle_list;
		uint32_t stencil_reference_value = 0;
		uint32_t num_vertices = 3;
		uint32_t viewport_width = 0;
		uint32_t viewport_height = 0;
		uint32_t viewport_dispatch_z = 1;

		// Bindings specific for the code generation target (in case of combined texture and sampler, 'texture_bindings' and 'sampler_bindings' will be the same size and point to the same bindings, otherwise they are independent)
		std::vector<texture_binding> texture_bindings;
		std::vector<sampler_binding> sampler_bindings;
		std::vector<storage_binding> storage_bindings;
	};

	/// <summary>
	/// A collection of passes that make up an effect.
	/// </summary>
	struct technique
	{
		std::string name;
		std::vector<pass> passes;
		std::vector<annotation> annotations;
	};

	/// <summary>
	/// In-memory representation of an effect file.
	/// </summary>
	struct effect_module
	{
		std::vector<std::pair<std::string, shader_type>> entry_points;

		std::vector<texture> textures;
		std::vector<sampler> samplers;
		std::vector<storage> storages;

		std::vector<uniform> uniforms;
		std::vector<uniform> spec_constants;
		std::vector<technique> techniques;

		uint32_t total_uniform_size = 0;
		uint32_t num_texture_bindings = 0;
		uint32_t num_sampler_bindings = 0;
		uint32_t num_storage_bindings = 0;
	};
}
