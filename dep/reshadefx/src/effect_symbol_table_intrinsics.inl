/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if defined(__INTELLISENSE__) || !defined(DEFINE_INTRINSIC)
#define DEFINE_INTRINSIC(name, i, ret_type, ...)
#endif
#if defined(__INTELLISENSE__) || !defined(IMPLEMENT_INTRINSIC_GLSL)
#define IMPLEMENT_INTRINSIC_GLSL(name, i, code)
#endif
#if defined(__INTELLISENSE__) || !defined(IMPLEMENT_INTRINSIC_HLSL)
#define IMPLEMENT_INTRINSIC_HLSL(name, i, code)
#endif
#if defined(__INTELLISENSE__) || !defined(IMPLEMENT_INTRINSIC_SPIRV)
#define IMPLEMENT_INTRINSIC_SPIRV(name, i, code)
#endif

// ret abs(x)
DEFINE_INTRINSIC(abs, 0, int, int)
DEFINE_INTRINSIC(abs, 0, int2, int2)
DEFINE_INTRINSIC(abs, 0, int3, int3)
DEFINE_INTRINSIC(abs, 0, int4, int4)
DEFINE_INTRINSIC(abs, 1, float, float)
DEFINE_INTRINSIC(abs, 1, float2, float2)
DEFINE_INTRINSIC(abs, 1, float3, float3)
DEFINE_INTRINSIC(abs, 1, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(abs, 0, {
	code += "abs(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(abs, 1, {
	code += "abs(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(abs, 0, {
	code += "abs(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(abs, 1, {
	code += "abs(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(abs, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450SAbs)
		.add(args[0].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(abs, 1, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450FAbs)
		.add(args[0].base);
	})

// ret all(x)
DEFINE_INTRINSIC(all, 0, bool, bool)
DEFINE_INTRINSIC(all, 1, bool, bool2)
DEFINE_INTRINSIC(all, 1, bool, bool3)
DEFINE_INTRINSIC(all, 1, bool, bool4)
IMPLEMENT_INTRINSIC_GLSL(all, 0, {
	code += id_to_name(args[0].base);
	})
IMPLEMENT_INTRINSIC_GLSL(all, 1, {
	code += "all(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(all, 0, {
	code += id_to_name(args[0].base);
	})
IMPLEMENT_INTRINSIC_HLSL(all, 1, {
	code += "all(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(all, 0, {
	return args[0].base;
	})
IMPLEMENT_INTRINSIC_SPIRV(all, 1, {
	return
	add_instruction(spv::OpAll, convert_type(res_type))
		.add(args[0].base);
	})

// ret any(x)
DEFINE_INTRINSIC(any, 0, bool, bool)
DEFINE_INTRINSIC(any, 1, bool, bool2)
DEFINE_INTRINSIC(any, 1, bool, bool3)
DEFINE_INTRINSIC(any, 1, bool, bool4)
IMPLEMENT_INTRINSIC_GLSL(any, 0, {
	code += id_to_name(args[0].base);
	})
IMPLEMENT_INTRINSIC_GLSL(any, 1, {
	code += "any(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(any, 0, {
	code += id_to_name(args[0].base);
	})
IMPLEMENT_INTRINSIC_HLSL(any, 1, {
	code += "any(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(any, 0, {
	return args[0].base;
	})
IMPLEMENT_INTRINSIC_SPIRV(any, 1, {
	return
	add_instruction(spv::OpAny, convert_type(res_type))
		.add(args[0].base);
	})

// ret asin(x)
DEFINE_INTRINSIC(asin, 0, float, float)
DEFINE_INTRINSIC(asin, 0, float2, float2)
DEFINE_INTRINSIC(asin, 0, float3, float3)
DEFINE_INTRINSIC(asin, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(asin, 0, {
	code += "asin(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(asin, 0, {
	code += "asin(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(asin, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Asin)
		.add(args[0].base);
	})

// ret acos(x)
DEFINE_INTRINSIC(acos, 0, float, float)
DEFINE_INTRINSIC(acos, 0, float2, float2)
DEFINE_INTRINSIC(acos, 0, float3, float3)
DEFINE_INTRINSIC(acos, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(acos, 0, {
	code += "acos(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(acos, 0, {
	code += "acos(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(acos, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Acos)
		.add(args[0].base);
	})

// ret atan(x)
DEFINE_INTRINSIC(atan, 0, float, float)
DEFINE_INTRINSIC(atan, 0, float2, float2)
DEFINE_INTRINSIC(atan, 0, float3, float3)
DEFINE_INTRINSIC(atan, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(atan, 0, {
	code += "atan(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atan, 0, {
	code += "atan(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(atan, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Atan)
		.add(args[0].base);
	})

// ret atan2(x, y)
DEFINE_INTRINSIC(atan2, 0, float, float, float)
DEFINE_INTRINSIC(atan2, 0, float2, float2, float2)
DEFINE_INTRINSIC(atan2, 0, float3, float3, float3)
DEFINE_INTRINSIC(atan2, 0, float4, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(atan2, 0, {
	code += "atan(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atan2, 0, {
	code += "atan2(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(atan2, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Atan2)
		.add(args[0].base)
		.add(args[1].base);
	})

// ret sin(x)
DEFINE_INTRINSIC(sin, 0, float, float)
DEFINE_INTRINSIC(sin, 0, float2, float2)
DEFINE_INTRINSIC(sin, 0, float3, float3)
DEFINE_INTRINSIC(sin, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(sin, 0, {
	code += "sin(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(sin, 0, {
	code += "sin(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(sin, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Sin)
		.add(args[0].base);
	})

// ret sinh(x)
DEFINE_INTRINSIC(sinh, 0, float, float)
DEFINE_INTRINSIC(sinh, 0, float2, float2)
DEFINE_INTRINSIC(sinh, 0, float3, float3)
DEFINE_INTRINSIC(sinh, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(sinh, 0, {
	code += "sinh(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(sinh, 0, {
	code += "sinh(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(sinh, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Sinh)
		.add(args[0].base);
	})

// ret cos(x)
DEFINE_INTRINSIC(cos, 0, float, float)
DEFINE_INTRINSIC(cos, 0, float2, float2)
DEFINE_INTRINSIC(cos, 0, float3, float3)
DEFINE_INTRINSIC(cos, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(cos, 0, {
	code += "cos(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(cos, 0, {
	code += "cos(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(cos, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Cos)
		.add(args[0].base);
	})

// ret cosh(x)
DEFINE_INTRINSIC(cosh, 0, float, float)
DEFINE_INTRINSIC(cosh, 0, float2, float2)
DEFINE_INTRINSIC(cosh, 0, float3, float3)
DEFINE_INTRINSIC(cosh, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(cosh, 0, {
	code += "cosh(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(cosh, 0, {
	code += "cosh(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(cosh, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Cosh)
		.add(args[0].base);
	})

// ret tan(x)
DEFINE_INTRINSIC(tan, 0, float, float)
DEFINE_INTRINSIC(tan, 0, float2, float2)
DEFINE_INTRINSIC(tan, 0, float3, float3)
DEFINE_INTRINSIC(tan, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(tan, 0, {
	code += "tan(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(tan, 0, {
	code += "tan(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(tan, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Tan)
		.add(args[0].base);
	})

// ret tanh(x)
DEFINE_INTRINSIC(tanh, 0, float, float)
DEFINE_INTRINSIC(tanh, 0, float2, float2)
DEFINE_INTRINSIC(tanh, 0, float3, float3)
DEFINE_INTRINSIC(tanh, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(tanh, 0, {
	code += "tanh(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(tanh, 0, {
	code += "tanh(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(tanh, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Tanh)
		.add(args[0].base);
	})

// sincos(x, out s, out c)
DEFINE_INTRINSIC(sincos, 0, void, float, out_float, out_float)
DEFINE_INTRINSIC(sincos, 0, void, float2, out_float2, out_float2)
DEFINE_INTRINSIC(sincos, 0, void, float3, out_float3, out_float3)
DEFINE_INTRINSIC(sincos, 0, void, float4, out_float4, out_float4)
IMPLEMENT_INTRINSIC_GLSL(sincos, 0, {
	code += id_to_name(args[1].base) + " = sin(" + id_to_name(args[0].base) + "), " + id_to_name(args[2].base) + " = cos(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(sincos, 0, {
	code += "sincos(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(sincos, 0, {
	const spv::Id sin_result = add_instruction(spv::OpExtInst, convert_type(args[0].type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Sin)
		.add(args[0].base);
	const spv::Id cos_result = add_instruction(spv::OpExtInst, convert_type(args[0].type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Cos)
		.add(args[0].base);

	add_instruction_without_result(spv::OpStore)
		.add(args[1].base)
		.add(sin_result);
	add_instruction_without_result(spv::OpStore)
		.add(args[2].base)
		.add(cos_result);

	return 0;
	})

// ret asint(x)
DEFINE_INTRINSIC(asint, 0, int, float)
DEFINE_INTRINSIC(asint, 0, int2, float2)
DEFINE_INTRINSIC(asint, 0, int3, float3)
DEFINE_INTRINSIC(asint, 0, int4, float4)
IMPLEMENT_INTRINSIC_GLSL(asint, 0, {
	code += "floatBitsToInt(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(asint, 0, {
	_uses_bitwise_cast = true;
	if (_shader_model < 40)
		code += "__";
	code += "asint(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(asint, 0, {
	return
	add_instruction(spv::OpBitcast, convert_type(res_type))
		.add(args[0].base);
	})

// ret asuint(x)
DEFINE_INTRINSIC(asuint, 0, uint, float)
DEFINE_INTRINSIC(asuint, 0, uint2, float2)
DEFINE_INTRINSIC(asuint, 0, uint3, float3)
DEFINE_INTRINSIC(asuint, 0, uint4, float4)
IMPLEMENT_INTRINSIC_GLSL(asuint, 0, {
	code += "floatBitsToUint(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(asuint, 0, {
	_uses_bitwise_cast = true;
	if (_shader_model < 40)
		code += "__";
	code += "asuint(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(asuint, 0, {
	return
	add_instruction(spv::OpBitcast, convert_type(res_type))
		.add(args[0].base);
	})

// ret asfloat(x)
DEFINE_INTRINSIC(asfloat, 0, float, int)
DEFINE_INTRINSIC(asfloat, 0, float2, int2)
DEFINE_INTRINSIC(asfloat, 0, float3, int3)
DEFINE_INTRINSIC(asfloat, 0, float4, int4)
DEFINE_INTRINSIC(asfloat, 1, float, uint)
DEFINE_INTRINSIC(asfloat, 1, float2, uint2)
DEFINE_INTRINSIC(asfloat, 1, float3, uint3)
DEFINE_INTRINSIC(asfloat, 1, float4, uint4)
IMPLEMENT_INTRINSIC_GLSL(asfloat, 0, {
	code += "intBitsToFloat(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(asfloat, 1, {
	code += "uintBitsToFloat(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(asfloat, 0, {
	_uses_bitwise_cast = true;
	if (_shader_model < 40)
		code += "__";
	code += "asfloat(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(asfloat, 1, {
	_uses_bitwise_cast = true;
	if (_shader_model < 40)
		code += "__";
	code += "asfloat(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(asfloat, 0, {
	return
	add_instruction(spv::OpBitcast, convert_type(res_type))
		.add(args[0].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(asfloat, 1, {
	return
	add_instruction(spv::OpBitcast, convert_type(res_type))
		.add(args[0].base);
	})

// ret f16tof32(x)
DEFINE_INTRINSIC(f16tof32, 0, float, uint)
DEFINE_INTRINSIC(f16tof32, 0, float2, uint2)
DEFINE_INTRINSIC(f16tof32, 0, float3, uint3)
DEFINE_INTRINSIC(f16tof32, 0, float4, uint4)
IMPLEMENT_INTRINSIC_GLSL(f16tof32, 0, {
	if (args[0].type.rows > 1)
		code += "vec" + std::to_string(args[0].type.rows) + '(';
	for (unsigned int i = 0; i < args[0].type.rows; ++i)
	{
		assert(i < 4);
		code += "unpackHalf2x16(" + id_to_name(args[0].base) + '.' + "xyzw"[i] + ").x";
		if (i < args[0].type.rows - 1)
			code += ", ";
	}
	if (args[0].type.rows > 1)
		code += ')';
	})
IMPLEMENT_INTRINSIC_HLSL(f16tof32, 0, {
	code += "f16tof32(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(f16tof32, 0, {
	type res_scalar_type = res_type;
	res_scalar_type.rows = 1;
	type res_vector_type = res_type;
	res_vector_type.rows = 2;
	type arg_scalar_type = args[0].type;
	arg_scalar_type.rows = 1;

	spv::Id res[4] = {};
	for (unsigned int i = 0; i < args[0].type.rows; ++i)
	{
		assert(i < 4);

		spv::Id arg_scalar = args[0].base;
		if (args[0].type.rows > 1)
			arg_scalar = add_instruction(spv::OpCompositeExtract, convert_type(arg_scalar_type))
				.add(arg_scalar)
				.add(i);

		spv::Id arg_vector = add_instruction(spv::OpExtInst, convert_type(res_vector_type))
			.add(_glsl_ext)
			.add(spv::GLSLstd450UnpackHalf2x16)
			.add(arg_scalar);

		res[i] = add_instruction(spv::OpCompositeExtract, convert_type(res_scalar_type))
			.add(arg_vector)
			.add(0u);
	}

	if (res_type.rows > 1)
		return
		add_instruction(spv::OpCompositeConstruct, convert_type(res_type))
			.add(res, res + res_type.rows);
	else
		return res[0];
	})

// ret f32tof16(x)
DEFINE_INTRINSIC(f32tof16, 0, uint, float)
DEFINE_INTRINSIC(f32tof16, 0, uint2, float2)
DEFINE_INTRINSIC(f32tof16, 0, uint3, float3)
DEFINE_INTRINSIC(f32tof16, 0, uint4, float4)
IMPLEMENT_INTRINSIC_GLSL(f32tof16, 0, {
	if (args[0].type.rows > 1)
		code += "uvec" + std::to_string(args[0].type.rows) + '(';
	for (unsigned int i = 0; i < args[0].type.rows; ++i)
	{
		assert(i < 4);
		code += "packHalf2x16(vec2(" + id_to_name(args[0].base) + '.' + "xyzw"[i] + ", 0.0))";
		if (i < args[0].type.rows - 1)
			code += ", ";
	}
	if (args[0].type.rows > 1)
		code += ')';
	})
IMPLEMENT_INTRINSIC_HLSL(f32tof16, 0, {
	code += "f32tof16(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(f32tof16, 0, {
	type res_scalar_type = res_type;
	res_scalar_type.rows = 1;
	type arg_scalar_type = args[0].type;
	arg_scalar_type.rows = 1;
	type arg_vector_type = args[0].type;
	arg_vector_type.rows = 2;

	const spv::Id constant_zero = emit_constant(arg_scalar_type, 0u);

	spv::Id res[4] = {};
	for (unsigned int i = 0; i < args[0].type.rows; ++i)
	{
		assert(i < 4);

		spv::Id arg_scalar = args[0].base;
		if (args[0].type.rows > 1)
			arg_scalar = add_instruction(spv::OpCompositeExtract, convert_type(arg_scalar_type))
				.add(arg_scalar)
				.add(i);

		spv::Id arg_vector = add_instruction(spv::OpCompositeConstruct, convert_type(arg_vector_type))
			.add(arg_scalar)
			.add(constant_zero);

		res[i] = add_instruction(spv::OpExtInst, convert_type(res_scalar_type))
			.add(_glsl_ext)
			.add(spv::GLSLstd450PackHalf2x16)
			.add(arg_vector);
	}

	if (res_type.rows > 1)
		return
		add_instruction(spv::OpCompositeConstruct, convert_type(res_type))
			.add(res, res + res_type.rows);
	else
		return res[0];
	})

// ret firstbitlow
DEFINE_INTRINSIC(firstbitlow, 0, uint, uint)
DEFINE_INTRINSIC(firstbitlow, 0, uint2, uint2)
DEFINE_INTRINSIC(firstbitlow, 0, uint3, uint3)
DEFINE_INTRINSIC(firstbitlow, 0, uint4, uint4)
IMPLEMENT_INTRINSIC_GLSL(firstbitlow, 0, {
	code += "findLSB(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(firstbitlow, 0, {
	_uses_bitwise_intrinsics = true;
	if (_shader_model < 50)
		code += "__";
	code += "firstbitlow(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(firstbitlow, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450FindILsb)
		.add(args[0].base);
	})

// ret firstbithigh
DEFINE_INTRINSIC(firstbithigh, 0, int, int)
DEFINE_INTRINSIC(firstbithigh, 0, int2, int2)
DEFINE_INTRINSIC(firstbithigh, 0, int3, int3)
DEFINE_INTRINSIC(firstbithigh, 0, int4, int4)
DEFINE_INTRINSIC(firstbithigh, 1, uint, uint)
DEFINE_INTRINSIC(firstbithigh, 1, uint2, uint2)
DEFINE_INTRINSIC(firstbithigh, 1, uint3, uint3)
DEFINE_INTRINSIC(firstbithigh, 1, uint4, uint4)
IMPLEMENT_INTRINSIC_GLSL(firstbithigh, 0, {
	code += "findMSB(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(firstbithigh, 1, {
	code += "findMSB(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(firstbithigh, 0, {
	_uses_bitwise_intrinsics = true;
	if (_shader_model < 50)
		code += "__";
	code += "firstbithigh(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(firstbithigh, 1, {
	_uses_bitwise_intrinsics = true;
	if (_shader_model < 50)
		code += "__";
	code += "firstbithigh(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(firstbithigh, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450FindSMsb)
		.add(args[0].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(firstbithigh, 1, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450FindUMsb)
		.add(args[0].base);
	})

// ret countbits
DEFINE_INTRINSIC(countbits, 0, uint, uint)
DEFINE_INTRINSIC(countbits, 0, uint2, uint2)
DEFINE_INTRINSIC(countbits, 0, uint3, uint3)
DEFINE_INTRINSIC(countbits, 0, uint4, uint4)
IMPLEMENT_INTRINSIC_GLSL(countbits, 0, {
	code += "bitCount(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(countbits, 0, {
	_uses_bitwise_intrinsics = true;
	if (_shader_model < 50)
		code += "__";
	code += "countbits(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(countbits, 0, {
	return
	add_instruction(spv::OpBitCount, convert_type(res_type))
		.add(args[0].base);
	})

// ret reversebits
DEFINE_INTRINSIC(reversebits, 0, uint, uint)
DEFINE_INTRINSIC(reversebits, 0, uint2, uint2)
DEFINE_INTRINSIC(reversebits, 0, uint3, uint3)
DEFINE_INTRINSIC(reversebits, 0, uint4, uint4)
IMPLEMENT_INTRINSIC_GLSL(reversebits, 0, {
	code += "bitfieldReverse(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(reversebits, 0, {
	_uses_bitwise_intrinsics = true;
	if (_shader_model < 50)
		code += "__";
	code += "reversebits(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(reversebits, 0, {
	return
	add_instruction(spv::OpBitReverse, convert_type(res_type))
		.add(args[0].base);
	})

// ret ceil(x)
DEFINE_INTRINSIC(ceil, 0, float, float)
DEFINE_INTRINSIC(ceil, 0, float2, float2)
DEFINE_INTRINSIC(ceil, 0, float3, float3)
DEFINE_INTRINSIC(ceil, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(ceil, 0, {
	code += "ceil(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(ceil, 0, {
	code += "ceil(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(ceil, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Ceil)
		.add(args[0].base);
	})

// ret floor(x)
DEFINE_INTRINSIC(floor, 0, float, float)
DEFINE_INTRINSIC(floor, 0, float2, float2)
DEFINE_INTRINSIC(floor, 0, float3, float3)
DEFINE_INTRINSIC(floor, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(floor, 0, {
	code += "floor(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(floor, 0, {
	if (_shader_model >= 40)
		code += "floor(" + id_to_name(args[0].base) + ')';
	else // Using the floor intrinsic sometimes causes the SM3 D3DCompiler to generate wrong code, so replace it with a custom implementation
		code += id_to_name(args[0].base) + " - frac(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(floor, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Floor)
		.add(args[0].base);
	})

// ret clamp(x, min, max)
DEFINE_INTRINSIC(clamp, 0, int, int, int, int)
DEFINE_INTRINSIC(clamp, 0, int2, int2, int2, int2)
DEFINE_INTRINSIC(clamp, 0, int3, int3, int3, int3)
DEFINE_INTRINSIC(clamp, 0, int4, int4, int4, int4)
DEFINE_INTRINSIC(clamp, 1, uint, uint, uint, uint)
DEFINE_INTRINSIC(clamp, 1, uint2, uint2, uint2, uint2)
DEFINE_INTRINSIC(clamp, 1, uint3, uint3, uint3, uint3)
DEFINE_INTRINSIC(clamp, 1, uint4, uint4, uint4, uint4)
DEFINE_INTRINSIC(clamp, 2, float, float, float, float)
DEFINE_INTRINSIC(clamp, 2, float2, float2, float2, float2)
DEFINE_INTRINSIC(clamp, 2, float3, float3, float3, float3)
DEFINE_INTRINSIC(clamp, 2, float4, float4, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(clamp, 0, {
	code += "clamp(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(clamp, 1, {
	code += "clamp(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(clamp, 2, {
	code += "clamp(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(clamp, 0, {
	code += "clamp(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(clamp, 1, {
	code += "clamp(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(clamp, 2, {
	code += "clamp(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(clamp, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450SClamp)
		.add(args[0].base)
		.add(args[1].base)
		.add(args[2].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(clamp, 1, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450UClamp)
		.add(args[0].base)
		.add(args[1].base)
		.add(args[2].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(clamp, 2, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450FClamp)
		.add(args[0].base)
		.add(args[1].base)
		.add(args[2].base);
	})

// ret saturate(x)
DEFINE_INTRINSIC(saturate, 0, float, float)
DEFINE_INTRINSIC(saturate, 0, float2, float2)
DEFINE_INTRINSIC(saturate, 0, float3, float3)
DEFINE_INTRINSIC(saturate, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(saturate, 0, {
	code += "clamp(" + id_to_name(args[0].base) + ", 0.0, 1.0)";
	})
IMPLEMENT_INTRINSIC_HLSL(saturate, 0, {
	code += "saturate(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(saturate, 0, {
	const spv::Id constant_one = emit_constant(args[0].type, 1u);
	const spv::Id constant_zero = emit_constant(args[0].type, 0u);

	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450FClamp)
		.add(args[0].base)
		.add(constant_zero)
		.add(constant_one);
	})

// ret mad(mvalue, avalue, bvalue)
DEFINE_INTRINSIC(mad, 0, float, float, float, float)
DEFINE_INTRINSIC(mad, 0, float2, float2, float2, float2)
DEFINE_INTRINSIC(mad, 0, float3, float3, float3, float3)
DEFINE_INTRINSIC(mad, 0, float4, float4, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(mad, 0, {
	code += "fma(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(mad, 0, {
	if (_shader_model >= 50)
		code += "mad(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	else
		code += id_to_name(args[0].base) + " * " + id_to_name(args[1].base) + " + " + id_to_name(args[2].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(mad, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Fma)
		.add(args[0].base)
		.add(args[1].base)
		.add(args[2].base);
	})

// ret rcp(x)
DEFINE_INTRINSIC(rcp, 0, float, float)
DEFINE_INTRINSIC(rcp, 0, float2, float2)
DEFINE_INTRINSIC(rcp, 0, float3, float3)
DEFINE_INTRINSIC(rcp, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(rcp, 0, {
	code += "1.0 / " + id_to_name(args[0].base);
	})
IMPLEMENT_INTRINSIC_HLSL(rcp, 0, {
	if (_shader_model >= 50)
		code += "rcp(" + id_to_name(args[0].base) + ')';
	else
		code += "1.0 / " + id_to_name(args[0].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(rcp, 0, {
	const spv::Id constant_one = emit_constant(args[0].type, 1u);

	return
	add_instruction(spv::OpFDiv, convert_type(res_type))
		.add(constant_one)
		.add(args[0].base);
	})

// ret pow(x, y)
DEFINE_INTRINSIC(pow, 0, float, float, float)
DEFINE_INTRINSIC(pow, 0, float2, float2, float2)
DEFINE_INTRINSIC(pow, 0, float3, float3, float3)
DEFINE_INTRINSIC(pow, 0, float4, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(pow, 0, {
	code += "pow(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(pow, 0, {
	code += "pow(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(pow, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Pow)
		.add(args[0].base)
		.add(args[1].base);
	})

// ret exp(x)
DEFINE_INTRINSIC(exp, 0, float, float)
DEFINE_INTRINSIC(exp, 0, float2, float2)
DEFINE_INTRINSIC(exp, 0, float3, float3)
DEFINE_INTRINSIC(exp, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(exp, 0, {
	code += "exp(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(exp, 0, {
	code += "exp(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(exp, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Exp)
		.add(args[0].base);
	})

// ret exp2(x)
DEFINE_INTRINSIC(exp2, 0, float, float)
DEFINE_INTRINSIC(exp2, 0, float2, float2)
DEFINE_INTRINSIC(exp2, 0, float3, float3)
DEFINE_INTRINSIC(exp2, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(exp2, 0, {
	code += "exp2(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(exp2, 0, {
	code += "exp2(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(exp2, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Exp2)
		.add(args[0].base);
	})

// ret log(x)
DEFINE_INTRINSIC(log, 0, float, float)
DEFINE_INTRINSIC(log, 0, float2, float2)
DEFINE_INTRINSIC(log, 0, float3, float3)
DEFINE_INTRINSIC(log, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(log, 0, {
	code += "log(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(log, 0, {
	code += "log(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(log, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Log)
		.add(args[0].base);
	})

// ret log2(x)
DEFINE_INTRINSIC(log2, 0, float, float)
DEFINE_INTRINSIC(log2, 0, float2, float2)
DEFINE_INTRINSIC(log2, 0, float3, float3)
DEFINE_INTRINSIC(log2, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(log2, 0, {
	code += "log2(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(log2, 0, {
	code += "log2(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(log2, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Log2)
		.add(args[0].base);
	})

// ret log10(x)
DEFINE_INTRINSIC(log10, 0, float, float)
DEFINE_INTRINSIC(log10, 0, float2, float2)
DEFINE_INTRINSIC(log10, 0, float3, float3)
DEFINE_INTRINSIC(log10, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(log10, 0, {
	code += "(log2(" + id_to_name(args[0].base) + ") / log2(10.0))";
	})
IMPLEMENT_INTRINSIC_HLSL(log10, 0, {
	code += "(log2(" + id_to_name(args[0].base) + ") / log2(10.0))";
	})
IMPLEMENT_INTRINSIC_SPIRV(log10, 0, {
	const spv::Id log2 = add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Log2)
		.add(args[0].base);

	const spv::Id log10 = emit_constant(args[0].type, /* log2(10) */
		constant { { 3.321928f, 3.321928f, 3.321928f, 3.321928f } });

	return
	add_instruction(spv::OpFDiv, convert_type(res_type))
		.add(log2)
		.add(log10); })

// ret sign(x)
DEFINE_INTRINSIC(sign, 0, int, int)
DEFINE_INTRINSIC(sign, 0, int2, int2)
DEFINE_INTRINSIC(sign, 0, int3, int3)
DEFINE_INTRINSIC(sign, 0, int4, int4)
DEFINE_INTRINSIC(sign, 1, float, float)
DEFINE_INTRINSIC(sign, 1, float2, float2)
DEFINE_INTRINSIC(sign, 1, float3, float3)
DEFINE_INTRINSIC(sign, 1, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(sign, 0, {
	code += "sign(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(sign, 1, {
	code += "sign(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(sign, 0, {
	code += "sign(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(sign, 1, {
	code += "sign(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(sign, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450SSign)
		.add(args[0].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(sign, 1, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450FSign)
		.add(args[0].base);
	})

// ret sqrt(x)
DEFINE_INTRINSIC(sqrt, 0, float, float)
DEFINE_INTRINSIC(sqrt, 0, float2, float2)
DEFINE_INTRINSIC(sqrt, 0, float3, float3)
DEFINE_INTRINSIC(sqrt, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(sqrt, 0, {
	code += "sqrt(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(sqrt, 0, {
	code += "sqrt(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(sqrt, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Sqrt)
		.add(args[0].base);
	})

// ret rsqrt(x)
DEFINE_INTRINSIC(rsqrt, 0, float, float)
DEFINE_INTRINSIC(rsqrt, 0, float2, float2)
DEFINE_INTRINSIC(rsqrt, 0, float3, float3)
DEFINE_INTRINSIC(rsqrt, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(rsqrt, 0, {
	code += "inversesqrt(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(rsqrt, 0, {
	code += "rsqrt(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(rsqrt, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450InverseSqrt)
		.add(args[0].base);
	})

// ret lerp(x, y, s)
DEFINE_INTRINSIC(lerp, 0, float, float, float, float)
DEFINE_INTRINSIC(lerp, 0, float2, float2, float2, float2)
DEFINE_INTRINSIC(lerp, 0, float3, float3, float3, float3)
DEFINE_INTRINSIC(lerp, 0, float4, float4, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(lerp, 0, {
	code += "mix(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(lerp, 0, {
	code += "lerp(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(lerp, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450FMix)
		.add(args[0].base)
		.add(args[1].base)
		.add(args[2].base);
	})

// ret step(y, x)
DEFINE_INTRINSIC(step, 0, float, float, float)
DEFINE_INTRINSIC(step, 0, float2, float2, float2)
DEFINE_INTRINSIC(step, 0, float3, float3, float3)
DEFINE_INTRINSIC(step, 0, float4, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(step, 0, {
	code += "step(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(step, 0, {
	code += "step(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(step, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Step)
		.add(args[0].base)
		.add(args[1].base);
	})

// ret smoothstep(min, max, x)
DEFINE_INTRINSIC(smoothstep, 0, float, float, float, float)
DEFINE_INTRINSIC(smoothstep, 0, float2, float2, float2, float2)
DEFINE_INTRINSIC(smoothstep, 0, float3, float3, float3, float3)
DEFINE_INTRINSIC(smoothstep, 0, float4, float4, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(smoothstep, 0, {
	code += "smoothstep(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(smoothstep, 0, {
	code += "smoothstep(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(smoothstep, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(args[2].type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450SmoothStep)
		.add(args[0].base)
		.add(args[1].base)
		.add(args[2].base);
	})

// ret frac(x)
DEFINE_INTRINSIC(frac, 0, float, float)
DEFINE_INTRINSIC(frac, 0, float2, float2)
DEFINE_INTRINSIC(frac, 0, float3, float3)
DEFINE_INTRINSIC(frac, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(frac, 0, {
	code += "fract(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(frac, 0, {
	code += "frac(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(frac, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Fract)
		.add(args[0].base);
	})

// ret ldexp(x, exp)
DEFINE_INTRINSIC(ldexp, 0, float, float, int)
DEFINE_INTRINSIC(ldexp, 0, float2, float2, int2)
DEFINE_INTRINSIC(ldexp, 0, float3, float3, int3)
DEFINE_INTRINSIC(ldexp, 0, float4, float4, int4)
IMPLEMENT_INTRINSIC_GLSL(ldexp, 0, {
	code += "ldexp(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(ldexp, 0, {
	code += "ldexp(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(ldexp, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Ldexp)
		.add(args[0].base)
		.add(args[1].base);
	})

// ret modf(x, out ip)
DEFINE_INTRINSIC(modf, 0, float, float, out_float)
DEFINE_INTRINSIC(modf, 0, float2, float2, out_float2)
DEFINE_INTRINSIC(modf, 0, float3, float3, out_float3)
DEFINE_INTRINSIC(modf, 0, float4, float4, out_float4)
IMPLEMENT_INTRINSIC_GLSL(modf, 0, {
	code += "modf(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(modf, 0, {
	code += "modf(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(modf, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Modf)
		.add(args[0].base)
		.add(args[1].base);
	})

// ret frexp(x, out exp)
DEFINE_INTRINSIC(frexp, 0, float, float, out_int)
DEFINE_INTRINSIC(frexp, 0, float2, float2, out_int2)
DEFINE_INTRINSIC(frexp, 0, float3, float3, out_int3)
DEFINE_INTRINSIC(frexp, 0, float4, float4, out_int4)
IMPLEMENT_INTRINSIC_GLSL(frexp, 0, {
	code += "frexp(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(frexp, 0, {
	code += "frexp(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(frexp, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Frexp)
		.add(args[0].base)
		.add(args[1].base);
	})

// ret trunc(x)
DEFINE_INTRINSIC(trunc, 0, float, float)
DEFINE_INTRINSIC(trunc, 0, float2, float2)
DEFINE_INTRINSIC(trunc, 0, float3, float3)
DEFINE_INTRINSIC(trunc, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(trunc, 0, {
	code += "trunc(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(trunc, 0, {
	code += "trunc(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(trunc, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Trunc)
		.add(args[0].base);
	})

// ret round(x)
DEFINE_INTRINSIC(round, 0, float, float)
DEFINE_INTRINSIC(round, 0, float2, float2)
DEFINE_INTRINSIC(round, 0, float3, float3)
DEFINE_INTRINSIC(round, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(round, 0, {
	code += "round(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(round, 0, {
	code += "round(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(round, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Round)
		.add(args[0].base);
	})

// ret min(x, y)
DEFINE_INTRINSIC(min, 0, int, int, int)
DEFINE_INTRINSIC(min, 0, int2, int2, int2)
DEFINE_INTRINSIC(min, 0, int3, int3, int3)
DEFINE_INTRINSIC(min, 0, int4, int4, int4)
DEFINE_INTRINSIC(min, 1, float, float, float)
DEFINE_INTRINSIC(min, 1, float2, float2, float2)
DEFINE_INTRINSIC(min, 1, float3, float3, float3)
DEFINE_INTRINSIC(min, 1, float4, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(min, 0, {
	code += "min(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(min, 1, {
	code += "min(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(min, 0, {
	code += "min(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(min, 1, {
	code += "min(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(min, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450SMin)
		.add(args[0].base)
		.add(args[1].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(min, 1, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450FMin)
		.add(args[0].base)
		.add(args[1].base);
	})

// ret max(x, y)
DEFINE_INTRINSIC(max, 0, int, int, int)
DEFINE_INTRINSIC(max, 0, int2, int2, int2)
DEFINE_INTRINSIC(max, 0, int3, int3, int3)
DEFINE_INTRINSIC(max, 0, int4, int4, int4)
DEFINE_INTRINSIC(max, 1, float, float, float)
DEFINE_INTRINSIC(max, 1, float2, float2, float2)
DEFINE_INTRINSIC(max, 1, float3, float3, float3)
DEFINE_INTRINSIC(max, 1, float4, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(max, 0, {
	code += "max(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(max, 1, {
	code += "max(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(max, 0, {
	code += "max(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(max, 1, {
	code += "max(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(max, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450SMax)
		.add(args[0].base)
		.add(args[1].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(max, 1, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450FMax)
		.add(args[0].base)
		.add(args[1].base);
	})

// ret degrees(x)
DEFINE_INTRINSIC(degrees, 0, float, float)
DEFINE_INTRINSIC(degrees, 0, float2, float2)
DEFINE_INTRINSIC(degrees, 0, float3, float3)
DEFINE_INTRINSIC(degrees, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(degrees, 0, {
	code += "degrees(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(degrees, 0, {
	code += "degrees(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(degrees, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Degrees)
		.add(args[0].base);
	})

// ret radians(x)
DEFINE_INTRINSIC(radians, 0, float, float)
DEFINE_INTRINSIC(radians, 0, float2, float2)
DEFINE_INTRINSIC(radians, 0, float3, float3)
DEFINE_INTRINSIC(radians, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(radians, 0, {
	code += "radians(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(radians, 0, {
	code += "radians(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(radians, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Radians)
		.add(args[0].base);
	})

// ret ddx(x)
DEFINE_INTRINSIC(ddx, 0, float, float)
DEFINE_INTRINSIC(ddx, 0, float2, float2)
DEFINE_INTRINSIC(ddx, 0, float3, float3)
DEFINE_INTRINSIC(ddx, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(ddx, 0, {
	code += "dFdx(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(ddx, 0, {
	code += "ddx(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(ddx, 0, {
	return
	add_instruction(spv::OpDPdx, convert_type(res_type))
		.add(args[0].base);
	})
// ret ddx_coarse(x)
DEFINE_INTRINSIC(ddx_coarse, 0, float, float)
DEFINE_INTRINSIC(ddx_coarse, 0, float2, float2)
DEFINE_INTRINSIC(ddx_coarse, 0, float3, float3)
DEFINE_INTRINSIC(ddx_coarse, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(ddx_coarse, 0, {
	code += "dFdxCoarse(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(ddx_coarse, 0, {
	code += (_shader_model >= 50 ? "ddx_coarse(" : "ddx(") + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(ddx_coarse, 0, {
	return
	add_instruction(spv::OpDPdxCoarse, convert_type(res_type))
		.add(args[0].base);
	})
// ret ddx_fine(x)
DEFINE_INTRINSIC(ddx_fine, 0, float, float)
DEFINE_INTRINSIC(ddx_fine, 0, float2, float2)
DEFINE_INTRINSIC(ddx_fine, 0, float3, float3)
DEFINE_INTRINSIC(ddx_fine, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(ddx_fine, 0, {
	code += "dFdxFine(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(ddx_fine, 0, {
	code += (_shader_model >= 50 ? "ddx_fine(" : "ddx(") + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(ddx_fine, 0, {
	return
	add_instruction(spv::OpDPdxFine, convert_type(res_type))
		.add(args[0].base);
	})

// ret ddy(x)
DEFINE_INTRINSIC(ddy, 0, float, float)
DEFINE_INTRINSIC(ddy, 0, float2, float2)
DEFINE_INTRINSIC(ddy, 0, float3, float3)
DEFINE_INTRINSIC(ddy, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(ddy, 0, {
	code += "dFdy(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(ddy, 0, {
	code += "ddy(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(ddy, 0, {
	return
	add_instruction(spv::OpDPdy, convert_type(res_type))
		.add(args[0].base);
	})
// ret ddy_coarse(x)
DEFINE_INTRINSIC(ddy_coarse, 0, float, float)
DEFINE_INTRINSIC(ddy_coarse, 0, float2, float2)
DEFINE_INTRINSIC(ddy_coarse, 0, float3, float3)
DEFINE_INTRINSIC(ddy_coarse, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(ddy_coarse, 0, {
	code += "dFdyCoarse(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(ddy_coarse, 0, {
	code += (_shader_model >= 50 ? "ddy_coarse(" : "ddy(") + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(ddy_coarse, 0, {
	return
	add_instruction(spv::OpDPdyCoarse, convert_type(res_type))
		.add(args[0].base);
	})
// ret ddy_fine(x)
DEFINE_INTRINSIC(ddy_fine, 0, float, float)
DEFINE_INTRINSIC(ddy_fine, 0, float2, float2)
DEFINE_INTRINSIC(ddy_fine, 0, float3, float3)
DEFINE_INTRINSIC(ddy_fine, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(ddy_fine, 0, {
	code += "dFdyFine(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(ddy_fine, 0, {
	code += (_shader_model >= 50 ? "ddy_fine(" : "ddy(") + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(ddy_fine, 0, {
	return
	add_instruction(spv::OpDPdyFine, convert_type(res_type))
		.add(args[0].base);
	})

// ret fwidth(x)
DEFINE_INTRINSIC(fwidth, 0, float, float)
DEFINE_INTRINSIC(fwidth, 0, float2, float2)
DEFINE_INTRINSIC(fwidth, 0, float3, float3)
DEFINE_INTRINSIC(fwidth, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(fwidth, 0, {
	code += "fwidth(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(fwidth, 0, {
	code += "fwidth(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(fwidth, 0, {
	return
	add_instruction(spv::OpFwidth, convert_type(res_type))
		.add(args[0].base);
	})

// ret dot(x, y)
DEFINE_INTRINSIC(dot, 0, float, float, float)
DEFINE_INTRINSIC(dot, 1, float, float2, float2)
DEFINE_INTRINSIC(dot, 1, float, float3, float3)
DEFINE_INTRINSIC(dot, 1, float, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(dot, 0, {
	code += '(' + id_to_name(args[0].base) + " * " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(dot, 1, {
	code += "dot(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(dot, 0, {
	code += '(' + id_to_name(args[0].base) + " * " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(dot, 1, {
	code += "dot(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(dot, 0, {
	return
	add_instruction(spv::OpFMul, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(dot, 1, {
	return
	add_instruction(spv::OpDot, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base);
	})

// ret cross(x, y)
DEFINE_INTRINSIC(cross, 0, float3, float3, float3)
IMPLEMENT_INTRINSIC_GLSL(cross, 0, {
	code += "cross(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(cross, 0, {
	code += "cross(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(cross, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Cross)
		.add(args[0].base)
		.add(args[1].base);
	})

// ret length(x)
DEFINE_INTRINSIC(length, 0, float, float)
DEFINE_INTRINSIC(length, 0, float, float2)
DEFINE_INTRINSIC(length, 0, float, float3)
DEFINE_INTRINSIC(length, 0, float, float4)
IMPLEMENT_INTRINSIC_GLSL(length, 0, {
	code += "length(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(length, 0, {
	code += "length(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(length, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Length)
		.add(args[0].base);
	})

// ret distance(x, y)
DEFINE_INTRINSIC(distance, 0, float, float, float)
DEFINE_INTRINSIC(distance, 0, float, float2, float2)
DEFINE_INTRINSIC(distance, 0, float, float3, float3)
DEFINE_INTRINSIC(distance, 0, float, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(distance, 0, {
	code += "distance(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(distance, 0, {
	code += "distance(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(distance, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Distance)
		.add(args[0].base)
		.add(args[1].base);
	})

// ret normalize(x)
DEFINE_INTRINSIC(normalize, 0, float2, float2)
DEFINE_INTRINSIC(normalize, 0, float3, float3)
DEFINE_INTRINSIC(normalize, 0, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(normalize, 0, {
	code += "normalize(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(normalize, 0, {
	code += "normalize(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(normalize, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Normalize)
		.add(args[0].base);
	})

// ret transpose(x)
DEFINE_INTRINSIC(transpose, 0, float2x2, float2x2)
DEFINE_INTRINSIC(transpose, 0, float2x3, float3x2)
DEFINE_INTRINSIC(transpose, 0, float2x4, float4x2)
DEFINE_INTRINSIC(transpose, 0, float3x2, float2x3)
DEFINE_INTRINSIC(transpose, 0, float3x3, float3x3)
DEFINE_INTRINSIC(transpose, 0, float3x4, float4x3)
DEFINE_INTRINSIC(transpose, 0, float4x2, float2x4)
DEFINE_INTRINSIC(transpose, 0, float4x3, float3x4)
DEFINE_INTRINSIC(transpose, 0, float4x4, float4x4)
IMPLEMENT_INTRINSIC_GLSL(transpose, 0, {
	code += "transpose(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(transpose, 0, {
	code += "transpose(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(transpose, 0, {
	return
	add_instruction(spv::OpTranspose, convert_type(res_type))
		.add(args[0].base);
	})

// ret determinant(m)
DEFINE_INTRINSIC(determinant, 0, float, float2x2)
DEFINE_INTRINSIC(determinant, 0, float, float3x3)
DEFINE_INTRINSIC(determinant, 0, float, float4x4)
IMPLEMENT_INTRINSIC_GLSL(determinant, 0, {
	code += "determinant(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(determinant, 0, {
	code += "determinant(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(determinant, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Determinant)
		.add(args[0].base);
	})

// ret reflect(i, n)
DEFINE_INTRINSIC(reflect, 0, float2, float2, float2)
DEFINE_INTRINSIC(reflect, 0, float3, float3, float3)
DEFINE_INTRINSIC(reflect, 0, float4, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(reflect, 0, {
	code += "reflect(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(reflect, 0, {
	code += "reflect(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(reflect, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Reflect)
		.add(args[0].base)
		.add(args[1].base);
	})

// ret refract(i, n, eta)
DEFINE_INTRINSIC(refract, 0, float2, float2, float2, float)
DEFINE_INTRINSIC(refract, 0, float3, float3, float3, float)
DEFINE_INTRINSIC(refract, 0, float4, float4, float4, float)
IMPLEMENT_INTRINSIC_GLSL(refract, 0, {
	code += "refract(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(refract, 0, {
	code += "refract(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(refract, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450Refract)
		.add(args[0].base)
		.add(args[1].base)
		.add(args[2].base);
	})

// ret faceforward(n, i, ng)
DEFINE_INTRINSIC(faceforward, 0, float, float, float, float)
DEFINE_INTRINSIC(faceforward, 0, float2, float2, float2, float2)
DEFINE_INTRINSIC(faceforward, 0, float3, float3, float3, float3)
DEFINE_INTRINSIC(faceforward, 0, float4, float4, float4, float4)
IMPLEMENT_INTRINSIC_GLSL(faceforward, 0, {
	code += "faceforward(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(faceforward, 0, {
	code += "faceforward(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(faceforward, 0, {
	return
	add_instruction(spv::OpExtInst, convert_type(res_type))
		.add(_glsl_ext)
		.add(spv::GLSLstd450FaceForward)
		.add(args[0].base)
		.add(args[1].base)
		.add(args[2].base);
	})

// ret mul(x, y)
DEFINE_INTRINSIC(mul, 0, int2, int, int2)
DEFINE_INTRINSIC(mul, 0, int3, int, int3)
DEFINE_INTRINSIC(mul, 0, int4, int, int4)
DEFINE_INTRINSIC(mul, 0, float2, float, float2)
DEFINE_INTRINSIC(mul, 0, float3, float, float3)
DEFINE_INTRINSIC(mul, 0, float4, float, float4)
IMPLEMENT_INTRINSIC_GLSL(mul, 0, {
	code += '(' + id_to_name(args[0].base) + " * " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(mul, 0, {
	code += '(' + id_to_name(args[0].base) + " * " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(mul, 0, {
	return
	add_instruction(spv::OpVectorTimesScalar, convert_type(res_type))
		.add(args[1].base)
		.add(args[0].base);
	})
DEFINE_INTRINSIC(mul, 1, int2, int2, int)
DEFINE_INTRINSIC(mul, 1, int3, int3, int)
DEFINE_INTRINSIC(mul, 1, int4, int4, int)
DEFINE_INTRINSIC(mul, 1, float2, float2, float)
DEFINE_INTRINSIC(mul, 1, float3, float3, float)
DEFINE_INTRINSIC(mul, 1, float4, float4, float)
IMPLEMENT_INTRINSIC_GLSL(mul, 1, {
	code += '(' + id_to_name(args[0].base) + " * " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(mul, 1, {
	code += '(' + id_to_name(args[0].base) + " * " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(mul, 1, {
	return
	add_instruction(spv::OpVectorTimesScalar, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base);
	})

DEFINE_INTRINSIC(mul, 2, int2x2, int, int2x2)
DEFINE_INTRINSIC(mul, 2, int2x3, int, int2x3)
DEFINE_INTRINSIC(mul, 2, int2x4, int, int2x4)
DEFINE_INTRINSIC(mul, 2, int3x2, int, int3x2)
DEFINE_INTRINSIC(mul, 2, int3x3, int, int3x3)
DEFINE_INTRINSIC(mul, 2, int3x4, int, int3x4)
DEFINE_INTRINSIC(mul, 2, int4x2, int, int4x2)
DEFINE_INTRINSIC(mul, 2, int4x3, int, int4x3)
DEFINE_INTRINSIC(mul, 2, int4x4, int, int4x4)
DEFINE_INTRINSIC(mul, 2, float2x2, float, float2x2)
DEFINE_INTRINSIC(mul, 2, float2x3, float, float2x3)
DEFINE_INTRINSIC(mul, 2, float2x4, float, float2x4)
DEFINE_INTRINSIC(mul, 2, float3x2, float, float3x2)
DEFINE_INTRINSIC(mul, 2, float3x3, float, float3x3)
DEFINE_INTRINSIC(mul, 2, float3x4, float, float3x4)
DEFINE_INTRINSIC(mul, 2, float4x2, float, float4x2)
DEFINE_INTRINSIC(mul, 2, float4x3, float, float4x3)
DEFINE_INTRINSIC(mul, 2, float4x4, float, float4x4)
IMPLEMENT_INTRINSIC_GLSL(mul, 2, {
	code += '(' + id_to_name(args[0].base) + " * " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(mul, 2, {
	code += '(' + id_to_name(args[0].base) + " * " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(mul, 2, {
	return
	add_instruction(spv::OpMatrixTimesScalar, convert_type(res_type))
		.add(args[1].base)
		.add(args[0].base);
	})
DEFINE_INTRINSIC(mul, 3, int2x2, int2x2, int)
DEFINE_INTRINSIC(mul, 3, int2x3, int2x3, int)
DEFINE_INTRINSIC(mul, 3, int2x4, int2x4, int)
DEFINE_INTRINSIC(mul, 3, int3x2, int3x2, int)
DEFINE_INTRINSIC(mul, 3, int3x3, int3x3, int)
DEFINE_INTRINSIC(mul, 3, int3x4, int3x4, int)
DEFINE_INTRINSIC(mul, 3, int4x2, int4x2, int)
DEFINE_INTRINSIC(mul, 3, int4x3, int4x3, int)
DEFINE_INTRINSIC(mul, 3, int4x4, int4x4, int)
DEFINE_INTRINSIC(mul, 3, float2x2, float2x2, float)
DEFINE_INTRINSIC(mul, 3, float2x3, float2x3, float)
DEFINE_INTRINSIC(mul, 3, float2x4, float2x4, float)
DEFINE_INTRINSIC(mul, 3, float3x2, float3x2, float)
DEFINE_INTRINSIC(mul, 3, float3x3, float3x3, float)
DEFINE_INTRINSIC(mul, 3, float3x4, float3x4, float)
DEFINE_INTRINSIC(mul, 3, float4x2, float4x2, float)
DEFINE_INTRINSIC(mul, 3, float4x3, float4x3, float)
DEFINE_INTRINSIC(mul, 3, float4x4, float4x4, float)
IMPLEMENT_INTRINSIC_GLSL(mul, 3, {
	code += '(' + id_to_name(args[0].base) + " * " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(mul, 3, {
	code += '(' + id_to_name(args[0].base) + " * " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(mul, 3, {
	return
	add_instruction(spv::OpMatrixTimesScalar, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base);
	})

DEFINE_INTRINSIC(mul, 4, int2, int2, int2x2)
DEFINE_INTRINSIC(mul, 4, int3, int2, int2x3)
DEFINE_INTRINSIC(mul, 4, int4, int2, int2x4)
DEFINE_INTRINSIC(mul, 4, int2, int3, int3x2)
DEFINE_INTRINSIC(mul, 4, int3, int3, int3x3)
DEFINE_INTRINSIC(mul, 4, int4, int3, int3x4)
DEFINE_INTRINSIC(mul, 4, int2, int4, int4x2)
DEFINE_INTRINSIC(mul, 4, int3, int4, int4x3)
DEFINE_INTRINSIC(mul, 4, int4, int4, int4x4)
DEFINE_INTRINSIC(mul, 4, float2, float2, float2x2)
DEFINE_INTRINSIC(mul, 4, float3, float2, float2x3)
DEFINE_INTRINSIC(mul, 4, float4, float2, float2x4)
DEFINE_INTRINSIC(mul, 4, float2, float3, float3x2)
DEFINE_INTRINSIC(mul, 4, float3, float3, float3x3)
DEFINE_INTRINSIC(mul, 4, float4, float3, float3x4)
DEFINE_INTRINSIC(mul, 4, float2, float4, float4x2)
DEFINE_INTRINSIC(mul, 4, float3, float4, float4x3)
DEFINE_INTRINSIC(mul, 4, float4, float4, float4x4)
IMPLEMENT_INTRINSIC_GLSL(mul, 4, {
	// Flip inputs because matrices are column-wise
	code += '(' + id_to_name(args[1].base) + " * " + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(mul, 4, {
	code += "mul(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(mul, 4, {
	return
	add_instruction(spv::OpMatrixTimesVector, convert_type(res_type))
		.add(args[1].base) // Flip inputs because matrices are column-wise
		.add(args[0].base);
	})
DEFINE_INTRINSIC(mul, 5, int2, int2x2, int2)
DEFINE_INTRINSIC(mul, 5, int2, int2x3, int3)
DEFINE_INTRINSIC(mul, 5, int2, int2x4, int4)
DEFINE_INTRINSIC(mul, 5, int3, int3x2, int2)
DEFINE_INTRINSIC(mul, 5, int3, int3x3, int3)
DEFINE_INTRINSIC(mul, 5, int3, int3x4, int4)
DEFINE_INTRINSIC(mul, 5, int4, int4x2, int2)
DEFINE_INTRINSIC(mul, 5, int4, int4x3, int3)
DEFINE_INTRINSIC(mul, 5, int4, int4x4, int4)
DEFINE_INTRINSIC(mul, 5, float2, float2x2, float2)
DEFINE_INTRINSIC(mul, 5, float2, float2x3, float3)
DEFINE_INTRINSIC(mul, 5, float2, float2x4, float4)
DEFINE_INTRINSIC(mul, 5, float3, float3x2, float2)
DEFINE_INTRINSIC(mul, 5, float3, float3x3, float3)
DEFINE_INTRINSIC(mul, 5, float3, float3x4, float4)
DEFINE_INTRINSIC(mul, 5, float4, float4x2, float2)
DEFINE_INTRINSIC(mul, 5, float4, float4x3, float3)
DEFINE_INTRINSIC(mul, 5, float4, float4x4, float4)
IMPLEMENT_INTRINSIC_GLSL(mul, 5, {
	// Flip inputs because matrices are column-wise
	code += '(' + id_to_name(args[1].base) + " * " + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(mul, 5, {
	code += "mul(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(mul, 5, {
	return
	add_instruction(spv::OpVectorTimesMatrix, convert_type(res_type))
		.add(args[1].base) // Flip inputs because matrices are column-wise
		.add(args[0].base);
	})

DEFINE_INTRINSIC(mul, 6, int2x2, int2x2, int2x2)
DEFINE_INTRINSIC(mul, 6, int2x3, int2x2, int2x3)
DEFINE_INTRINSIC(mul, 6, int2x4, int2x2, int2x4)
DEFINE_INTRINSIC(mul, 6, int2x2, int2x3, int3x2)
DEFINE_INTRINSIC(mul, 6, int2x3, int2x3, int3x3)
DEFINE_INTRINSIC(mul, 6, int2x4, int2x3, int3x4)
DEFINE_INTRINSIC(mul, 6, int2x2, int2x4, int4x2)
DEFINE_INTRINSIC(mul, 6, int2x3, int2x4, int4x3)
DEFINE_INTRINSIC(mul, 6, int2x4, int2x4, int4x4)
DEFINE_INTRINSIC(mul, 6, int3x2, int3x2, int2x2)
DEFINE_INTRINSIC(mul, 6, int3x3, int3x2, int2x3)
DEFINE_INTRINSIC(mul, 6, int3x4, int3x2, int2x4)
DEFINE_INTRINSIC(mul, 6, int3x2, int3x3, int3x2)
DEFINE_INTRINSIC(mul, 6, int3x3, int3x3, int3x3)
DEFINE_INTRINSIC(mul, 6, int3x4, int3x3, int3x4)
DEFINE_INTRINSIC(mul, 6, int3x2, int3x4, int4x2)
DEFINE_INTRINSIC(mul, 6, int3x3, int3x4, int4x3)
DEFINE_INTRINSIC(mul, 6, int3x4, int3x4, int4x4)
DEFINE_INTRINSIC(mul, 6, int4x2, int4x2, int2x2)
DEFINE_INTRINSIC(mul, 6, int4x3, int4x2, int2x3)
DEFINE_INTRINSIC(mul, 6, int4x4, int4x2, int2x4)
DEFINE_INTRINSIC(mul, 6, int4x2, int4x3, int3x2)
DEFINE_INTRINSIC(mul, 6, int4x3, int4x3, int3x3)
DEFINE_INTRINSIC(mul, 6, int4x4, int4x3, int3x4)
DEFINE_INTRINSIC(mul, 6, int4x2, int4x4, int4x2)
DEFINE_INTRINSIC(mul, 6, int4x3, int4x4, int4x3)
DEFINE_INTRINSIC(mul, 6, int4x4, int4x4, int4x4)
DEFINE_INTRINSIC(mul, 6, float2x2, float2x2, float2x2)
DEFINE_INTRINSIC(mul, 6, float2x3, float2x2, float2x3)
DEFINE_INTRINSIC(mul, 6, float2x4, float2x2, float2x4)
DEFINE_INTRINSIC(mul, 6, float2x2, float2x3, float3x2)
DEFINE_INTRINSIC(mul, 6, float2x3, float2x3, float3x3)
DEFINE_INTRINSIC(mul, 6, float2x4, float2x3, float3x4)
DEFINE_INTRINSIC(mul, 6, float2x2, float2x4, float4x2)
DEFINE_INTRINSIC(mul, 6, float2x3, float2x4, float4x3)
DEFINE_INTRINSIC(mul, 6, float2x4, float2x4, float4x4)
DEFINE_INTRINSIC(mul, 6, float3x2, float3x2, float2x2)
DEFINE_INTRINSIC(mul, 6, float3x3, float3x2, float2x3)
DEFINE_INTRINSIC(mul, 6, float3x4, float3x2, float2x4)
DEFINE_INTRINSIC(mul, 6, float3x2, float3x3, float3x2)
DEFINE_INTRINSIC(mul, 6, float3x3, float3x3, float3x3)
DEFINE_INTRINSIC(mul, 6, float3x4, float3x3, float3x4)
DEFINE_INTRINSIC(mul, 6, float3x2, float3x4, float4x2)
DEFINE_INTRINSIC(mul, 6, float3x3, float3x4, float4x3)
DEFINE_INTRINSIC(mul, 6, float3x4, float3x4, float4x4)
DEFINE_INTRINSIC(mul, 6, float4x2, float4x2, float2x2)
DEFINE_INTRINSIC(mul, 6, float4x3, float4x2, float2x3)
DEFINE_INTRINSIC(mul, 6, float4x4, float4x2, float2x4)
DEFINE_INTRINSIC(mul, 6, float4x2, float4x3, float3x2)
DEFINE_INTRINSIC(mul, 6, float4x3, float4x3, float3x3)
DEFINE_INTRINSIC(mul, 6, float4x4, float4x3, float3x4)
DEFINE_INTRINSIC(mul, 6, float4x2, float4x4, float4x2)
DEFINE_INTRINSIC(mul, 6, float4x3, float4x4, float4x3)
DEFINE_INTRINSIC(mul, 6, float4x4, float4x4, float4x4)
IMPLEMENT_INTRINSIC_GLSL(mul, 6, {
	// Flip inputs because matrices are column-wise
	code += '(' + id_to_name(args[1].base) + " * " + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(mul, 6, {
	code += "mul(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(mul, 6, {
	return
	add_instruction(spv::OpMatrixTimesMatrix, convert_type(res_type))
		.add(args[1].base) // Flip inputs because matrices are column-wise
		.add(args[0].base);
	})

// ret isinf(x)
DEFINE_INTRINSIC(isinf, 0, bool, float)
DEFINE_INTRINSIC(isinf, 0, bool2, float2)
DEFINE_INTRINSIC(isinf, 0, bool3, float3)
DEFINE_INTRINSIC(isinf, 0, bool4, float4)
IMPLEMENT_INTRINSIC_GLSL(isinf, 0, {
	code += "isinf(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(isinf, 0, {
	code += "isinf(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(isinf, 0, {
	return
	add_instruction(spv::OpIsInf, convert_type(res_type))
		.add(args[0].base);
	})

// ret isnan(x)
DEFINE_INTRINSIC(isnan, 0, bool, float)
DEFINE_INTRINSIC(isnan, 0, bool2, float2)
DEFINE_INTRINSIC(isnan, 0, bool3, float3)
DEFINE_INTRINSIC(isnan, 0, bool4, float4)
IMPLEMENT_INTRINSIC_GLSL(isnan, 0, {
	code += "isnan(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(isnan, 0, {
	code += "isnan(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(isnan, 0, {
	return
	add_instruction(spv::OpIsNan, convert_type(res_type))
		.add(args[0].base);
	})

// ret tex1D(s, coords)
// ret tex1D(s, coords, offset)
DEFINE_INTRINSIC(tex1D, 0, int, sampler1d_int, float)
DEFINE_INTRINSIC(tex1D, 0, uint, sampler1d_uint, float)
DEFINE_INTRINSIC(tex1D, 0, float, sampler1d_float, float)
DEFINE_INTRINSIC(tex1D, 0, float4, sampler1d_float4, float)
DEFINE_INTRINSIC(tex1D, 1, int, sampler1d_int, float, int)
DEFINE_INTRINSIC(tex1D, 1, uint, sampler1d_uint, float, int)
DEFINE_INTRINSIC(tex1D, 1, float, sampler1d_float, float, int)
DEFINE_INTRINSIC(tex1D, 1, float4, sampler1d_float4, float, int)
IMPLEMENT_INTRINSIC_GLSL(tex1D, 0, {
	code += "texture(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	if (res_type.rows == 1)
		code += ".x"; // Collapse last argument from a 4-component vector
	})
IMPLEMENT_INTRINSIC_GLSL(tex1D, 1, {
	code += "textureOffset(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_HLSL(tex1D, 0, {
	if (_shader_model >= 40) { // SM4 and higher use a more object-oriented programming model for textures
		if (res_type.is_floating_point() || _shader_model >= 67)
			code += id_to_name(args[0].base) + ".t.Sample(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ')';
		else
			// Integer sampling is not supported until SM6.7, so emulate with a texture fetch
			code += "0; { "
				"float _dimensions; " +
				id_to_name(args[0].base) + ".t.GetDimensions(_dimensions); " +
				id_to_name(res) + " = " + id_to_name(args[0].base) + ".t.Load(int2(" + id_to_name(args[1].base) + " * _dimensions, 0)); }";
	}
	else {
		code += "tex1D(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ')';
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex1D, 1, {
	if (_shader_model >= 40) {
		if (res_type.is_floating_point() || _shader_model >= 67)
			code += id_to_name(args[0].base) + ".t.Sample(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
		else
			code += "0; { "
				"float _dimensions; " +
				id_to_name(args[0].base) + ".t.GetDimensions(_dimensions); " +
				id_to_name(res) + " = " + id_to_name(args[0].base) + ".t.Load(int2(" + id_to_name(args[1].base) + " * _dimensions, 0), " + id_to_name(args[2].base) + "); }";
	}
	else {
		code += "tex1D(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + " + " + id_to_name(args[2].base) + " * " + id_to_name(args[0].base) + ".pixelsize)";
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_SPIRV(tex1D, 0, {
	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleImplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(spv::ImageOperandsMaskNone);
	if (res_type.rows == 1)
		// Collapse last argument from a 4-component vector
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex1D, 1, {
	// Non-constant offset operand needs extended capability
	if (!args[2].is_constant)
		add_capability(spv::CapabilityImageGatherExtended);

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleImplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(args[2].is_constant ? spv::ImageOperandsConstOffsetMask : spv::ImageOperandsOffsetMask)
		.add(args[2].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})

// ret tex2D(s, coords)
// ret tex2D(s, coords, offset)
DEFINE_INTRINSIC(tex2D, 0, int, sampler2d_int, float2)
DEFINE_INTRINSIC(tex2D, 0, uint, sampler2d_uint, float2)
DEFINE_INTRINSIC(tex2D, 0, float, sampler2d_float, float2)
DEFINE_INTRINSIC(tex2D, 0, float4, sampler2d_float4, float2)
DEFINE_INTRINSIC(tex2D, 1, int, sampler2d_int, float2, int2)
DEFINE_INTRINSIC(tex2D, 1, uint, sampler2d_uint, float2, int2)
DEFINE_INTRINSIC(tex2D, 1, float, sampler2d_float, float2, int2)
DEFINE_INTRINSIC(tex2D, 1, float4, sampler2d_float4, float2, int2)
IMPLEMENT_INTRINSIC_GLSL(tex2D, 0, {
	code += "texture(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	if (res_type.rows == 1)
		code += ".x"; // Collapse last argument from a 4-component vector
	})
IMPLEMENT_INTRINSIC_GLSL(tex2D, 1, {
	code += "textureOffset(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2D, 0, {
	if (_shader_model >= 40) { // SM4 and higher use a more object-oriented programming model for textures
		if (res_type.is_floating_point() || _shader_model >= 67)
			code += id_to_name(args[0].base) + ".t.Sample(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ')';
		else
			// Integer sampling is not supported until SM6.7, so emulate with a texture fetch
			code += "0; { "
				"float2 _dimensions; " +
				id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y); " +
				id_to_name(res) + " = " + id_to_name(args[0].base) + ".t.Load(int3(" + id_to_name(args[1].base) + " * _dimensions, 0)); }";
	}
	else {
		code += "tex2D(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ')';
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex2D, 1, {
	if (_shader_model >= 40) {
		if (res_type.is_floating_point() || _shader_model >= 67)
			code += id_to_name(args[0].base) + ".t.Sample(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
		else
			code += "0; { "
				"float2 _dimensions; " +
				id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y); " +
				id_to_name(res) + " = " + id_to_name(args[0].base) + ".t.Load(int3(" + id_to_name(args[1].base) + " * _dimensions, 0), " + id_to_name(args[2].base) + "); }";
	}
	else {
		code += "tex2D(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + " + " + id_to_name(args[2].base) + " * " + id_to_name(args[0].base) + ".pixelsize)";
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2D, 0, {
	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleImplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(spv::ImageOperandsMaskNone);
	if (res_type.rows == 1)
		// Collapse last argument from a 4-component vector
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2D, 1, {
	// Non-constant offset operand needs extended capability
	if (!args[2].is_constant)
		add_capability(spv::CapabilityImageGatherExtended);

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleImplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(args[2].is_constant ? spv::ImageOperandsConstOffsetMask : spv::ImageOperandsOffsetMask)
		.add(args[2].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})

// ret tex3D(s, coords)
// ret tex3D(s, coords, offset)
DEFINE_INTRINSIC(tex3D, 0, int, sampler3d_int, float3)
DEFINE_INTRINSIC(tex3D, 0, uint, sampler3d_uint, float3)
DEFINE_INTRINSIC(tex3D, 0, float, sampler3d_float, float3)
DEFINE_INTRINSIC(tex3D, 0, float4, sampler3d_float4, float3)
DEFINE_INTRINSIC(tex3D, 1, int, sampler3d_int, float3, int3)
DEFINE_INTRINSIC(tex3D, 1, uint, sampler3d_uint, float3, int3)
DEFINE_INTRINSIC(tex3D, 1, float, sampler3d_float, float3, int3)
DEFINE_INTRINSIC(tex3D, 1, float4, sampler3d_float4, float3, int3)
IMPLEMENT_INTRINSIC_GLSL(tex3D, 0, {
	code += "texture(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	if (res_type.rows == 1)
		code += ".x"; // Collapse last argument from a 4-component vector
	})
IMPLEMENT_INTRINSIC_GLSL(tex3D, 1, {
	code += "textureOffset(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_HLSL(tex3D, 0, {
	if (_shader_model >= 40) { // SM4 and higher use a more object-oriented programming model for textures
		if (res_type.is_floating_point() || _shader_model >= 67)
			code += id_to_name(args[0].base) + ".t.Sample(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ')';
		else
			// Integer sampling is not supported until SM6.7, so emulate with a texture fetch
			code += "0; { "
				"float3 _dimensions; " +
				id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y, _dimensions.z); " +
				id_to_name(res) + " = " + id_to_name(args[0].base) + ".t.Load(int4(" + id_to_name(args[1].base) + " * _dimensions, 0)); }";
	}
	else {
		code += "tex3D(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ')';
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex3D, 1, {
	if (_shader_model >= 40) {
		if (res_type.is_floating_point() || _shader_model >= 67)
			code += id_to_name(args[0].base) + ".t.Sample(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
		else
			code += "0; { "
				"float3 _dimensions; " +
				id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y, _dimensions.z); " +
				id_to_name(res) + " = " + id_to_name(args[0].base) + ".t.Load(int4(" + id_to_name(args[1].base) + " * _dimensions, 0), " + id_to_name(args[2].base) + "); }";
	}
	else {
		code += "tex3D(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + " + " + id_to_name(args[2].base) + " * " + id_to_name(args[0].base) + ".pixelsize)";
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_SPIRV(tex3D, 0, {
	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleImplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(spv::ImageOperandsMaskNone);
	if (res_type.rows == 1)
		// Collapse last argument from a 4-component vector
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex3D, 1, {
	// Non-constant offset operand needs extended capability
	if (!args[2].is_constant)
		add_capability(spv::CapabilityImageGatherExtended);

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleImplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(args[2].is_constant ? spv::ImageOperandsConstOffsetMask : spv::ImageOperandsOffsetMask)
		.add(args[2].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})

// ret tex1Dgrad(s, coords, ddx, ddy)
// ret tex1Dgrad(s, coords, ddx, ddy, offset)
DEFINE_INTRINSIC(tex1Dgrad, 0, int, sampler1d_int, float, float, float)
DEFINE_INTRINSIC(tex1Dgrad, 0, uint, sampler1d_uint, float, float, float)
DEFINE_INTRINSIC(tex1Dgrad, 0, float, sampler1d_float, float, float, float)
DEFINE_INTRINSIC(tex1Dgrad, 0, float4, sampler1d_float4, float, float, float)
DEFINE_INTRINSIC(tex1Dgrad, 1, int, sampler1d_int, float, float, float, int)
DEFINE_INTRINSIC(tex1Dgrad, 1, uint, sampler1d_uint, float, float, float, int)
DEFINE_INTRINSIC(tex1Dgrad, 1, float, sampler1d_float, float, float, float, int)
DEFINE_INTRINSIC(tex1Dgrad, 1, float4, sampler1d_float4, float, float, float, int)
IMPLEMENT_INTRINSIC_GLSL(tex1Dgrad, 0, {
	code += "textureGrad(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ')';
	if (res_type.rows == 1)
		code += ".x"; // Collapse last argument from a 4-component vector
	})
IMPLEMENT_INTRINSIC_GLSL(tex1Dgrad, 1, {
	code += "textureGradOffset(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ", " + id_to_name(args[4].base) + ')';
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_HLSL(tex1Dgrad, 0, {
	if (_shader_model >= 40) {
		code += id_to_name(args[0].base) + ".t.SampleGrad(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ')';
	}
	else {
		code += "tex1Dgrad(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ')';
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex1Dgrad, 1, {
	if (_shader_model >= 40) {
		code += id_to_name(args[0].base) + ".t.SampleGrad(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ", " + id_to_name(args[4].base) + ')';
	}
	else {
		code += "tex1Dgrad(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + " + " + id_to_name(args[4].base) + " * " + id_to_name(args[0].base) + ".pixelsize, " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ')';
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_SPIRV(tex1Dgrad, 0, {
	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleExplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(spv::ImageOperandsGradMask)
		.add(args[2].base)
		.add(args[3].base);
	if (res_type.rows == 1)
		// Collapse last argument from a 4-component vector
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex1Dgrad, 1, {
	if (!args[4].is_constant)
		add_capability(spv::CapabilityImageGatherExtended);

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleExplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(spv::ImageOperandsGradMask | (args[4].is_constant ? spv::ImageOperandsConstOffsetMask : spv::ImageOperandsOffsetMask))
		.add(args[2].base)
		.add(args[3].base)
		.add(args[4].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})

// ret tex2Dgrad(s, coords, ddx, ddy)
// ret tex2Dgrad(s, coords, ddx, ddy, offset)
DEFINE_INTRINSIC(tex2Dgrad, 0, int, sampler2d_int, float2, float2, float2)
DEFINE_INTRINSIC(tex2Dgrad, 0, uint, sampler2d_uint, float2, float2, float2)
DEFINE_INTRINSIC(tex2Dgrad, 0, float, sampler2d_float, float2, float2, float2)
DEFINE_INTRINSIC(tex2Dgrad, 0, float4, sampler2d_float4, float2, float2, float2)
DEFINE_INTRINSIC(tex2Dgrad, 1, int, sampler2d_int, float2, float2, float2, int2)
DEFINE_INTRINSIC(tex2Dgrad, 1, uint, sampler2d_uint, float2, float2, float2, int2)
DEFINE_INTRINSIC(tex2Dgrad, 1, float, sampler2d_float, float2, float2, float2, int2)
DEFINE_INTRINSIC(tex2Dgrad, 1, float4, sampler2d_float4, float2, float2, float2, int2)
IMPLEMENT_INTRINSIC_GLSL(tex2Dgrad, 0, {
	code += "textureGrad(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ')';
	if (res_type.rows == 1)
		code += ".x"; // Collapse last argument from a 4-component vector
	})
IMPLEMENT_INTRINSIC_GLSL(tex2Dgrad, 1, {
	code += "textureGradOffset(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ", " + id_to_name(args[4].base) + ')';
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2Dgrad, 0, {
	if (_shader_model >= 40) {
		code += id_to_name(args[0].base) + ".t.SampleGrad(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ')';
	}
	else {
		code += "tex2Dgrad(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ')';
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex2Dgrad, 1, {
	if (_shader_model >= 40) {
		code += id_to_name(args[0].base) + ".t.SampleGrad(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ", " + id_to_name(args[4].base) + ')';
	}
	else {
		code += "tex2Dgrad(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + " + " + id_to_name(args[4].base) + " * " + id_to_name(args[0].base) + ".pixelsize, " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ')';
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2Dgrad, 0, {
	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleExplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(spv::ImageOperandsGradMask)
		.add(args[2].base)
		.add(args[3].base);
	if (res_type.rows == 1)
		// Collapse last argument from a 4-component vector
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2Dgrad, 1, {
	if (!args[4].is_constant)
		add_capability(spv::CapabilityImageGatherExtended);

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleExplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(spv::ImageOperandsGradMask | (args[4].is_constant ? spv::ImageOperandsConstOffsetMask : spv::ImageOperandsOffsetMask))
		.add(args[2].base)
		.add(args[3].base)
		.add(args[4].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})

// ret tex3Dgrad(s, coords, ddx, ddy)
// ret tex3Dgrad(s, coords, ddx, ddy, offset)
DEFINE_INTRINSIC(tex3Dgrad, 0, int, sampler3d_int, float3, float3, float3)
DEFINE_INTRINSIC(tex3Dgrad, 0, uint, sampler3d_uint, float3, float3, float3)
DEFINE_INTRINSIC(tex3Dgrad, 0, float, sampler3d_float, float3, float3, float3)
DEFINE_INTRINSIC(tex3Dgrad, 0, float4, sampler3d_float4, float3, float3, float3)
DEFINE_INTRINSIC(tex3Dgrad, 1, int, sampler3d_int, float3, float3, float3, int2)
DEFINE_INTRINSIC(tex3Dgrad, 1, uint, sampler3d_uint, float3, float3, float3, int2)
DEFINE_INTRINSIC(tex3Dgrad, 1, float, sampler3d_float, float3, float3, float3, int2)
DEFINE_INTRINSIC(tex3Dgrad, 1, float4, sampler3d_float4, float3, float3, float3, int2)
IMPLEMENT_INTRINSIC_GLSL(tex3Dgrad, 0, {
	code += "textureGrad(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ')';
	if (res_type.rows == 1)
		code += ".x"; // Collapse last argument from a 4-component vector
	})
IMPLEMENT_INTRINSIC_GLSL(tex3Dgrad, 1, {
	code += "textureGradOffset(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ", " + id_to_name(args[4].base) + ')';
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_HLSL(tex3Dgrad, 0, {
	if (_shader_model >= 40) {
		code += id_to_name(args[0].base) + ".t.SampleGrad(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ')';
	}
	else {
		code += "tex3Dgrad(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ')';
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex3Dgrad, 1, {
	if (_shader_model >= 40) {
		code += id_to_name(args[0].base) + ".t.SampleGrad(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ", " + id_to_name(args[4].base) + ')';
	}
	else {
		code += "tex3Dgrad(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + " + " + id_to_name(args[4].base) + " * " + id_to_name(args[0].base) + ".pixelsize, " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ')';
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_SPIRV(tex3Dgrad, 0, {
	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleExplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(spv::ImageOperandsGradMask)
		.add(args[2].base)
		.add(args[3].base);
	if (res_type.rows == 1)
		// Collapse last argument from a 4-component vector
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex3Dgrad, 1, {
	if (!args[4].is_constant)
		add_capability(spv::CapabilityImageGatherExtended);

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleExplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(spv::ImageOperandsGradMask | (args[4].is_constant ? spv::ImageOperandsConstOffsetMask : spv::ImageOperandsOffsetMask))
		.add(args[2].base)
		.add(args[3].base)
		.add(args[4].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})

// ret tex1Dlod(s, coords)
// ret tex1Dlod(s, coords, offset)
DEFINE_INTRINSIC(tex1Dlod, 0, int, sampler1d_int, float4)
DEFINE_INTRINSIC(tex1Dlod, 0, uint, sampler1d_uint, float4)
DEFINE_INTRINSIC(tex1Dlod, 0, float, sampler1d_float, float4)
DEFINE_INTRINSIC(tex1Dlod, 0, float4, sampler1d_float4, float4)
DEFINE_INTRINSIC(tex1Dlod, 1, int, sampler1d_int, float4, int)
DEFINE_INTRINSIC(tex1Dlod, 1, uint, sampler1d_uint, float4, int)
DEFINE_INTRINSIC(tex1Dlod, 1, float, sampler1d_float, float4, int)
DEFINE_INTRINSIC(tex1Dlod, 1, float4, sampler1d_float4, float4, int)
IMPLEMENT_INTRINSIC_GLSL(tex1Dlod, 0, {
	code += "textureLod(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ".x, " + id_to_name(args[1].base) + ".w)";
	if (res_type.rows == 1)
		code += ".x"; // Collapse last argument from a 4-component vector
	})
IMPLEMENT_INTRINSIC_GLSL(tex1Dlod, 1, {
	code += "textureLodOffset(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ".x, " + id_to_name(args[1].base) + ".w, " + id_to_name(args[2].base) + ')';
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_HLSL(tex1Dlod, 0, {
	if (_shader_model >= 40) {
		if (res_type.is_floating_point() || _shader_model >= 67)
			code += id_to_name(args[0].base) + ".t.SampleLevel(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ".x, " + id_to_name(args[1].base) + ".w)";
		else
			// Integer sampling is not supported until SM6.7, so emulate with a texture fetch
			code += "0; { "
				"float _dimensions; float _levels; " +
				id_to_name(args[0].base) + ".t.GetDimensions((int)" + id_to_name(args[1].base) + ".w, _dimensions, _levels); " +
				id_to_name(res) + " = " + id_to_name(args[0].base) + ".t.Load(int2(" + id_to_name(args[1].base) + ".x * _dimensions, (int)" + id_to_name(args[1].base) + ".w)); }";
	}
	else {
		code += "tex1Dlod(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ')';
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex1Dlod, 1, {
	if (_shader_model >= 40) {
		if (res_type.is_floating_point() || _shader_model >= 67)
			code += id_to_name(args[0].base) + ".t.SampleLevel(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ".x, " + id_to_name(args[1].base) + ".w, " + id_to_name(args[2].base) + ')';
		else
			code += "0; { "
				"float _dimensions; float _levels; " +
				id_to_name(args[0].base) + ".t.GetDimensions((int)" + id_to_name(args[1].base) + ".w, _dimensions, _levels); " +
				id_to_name(res) + " = " + id_to_name(args[0].base) + ".t.Load(int2(" + id_to_name(args[1].base) + ".x * _dimensions, (int)" + id_to_name(args[1].base) + ".w), " + id_to_name(args[2].base) + "); }";
	}
	else {
		code += "tex1Dlod(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + " + float4(" + id_to_name(args[2].base) + " * " + id_to_name(args[0].base) + ".pixelsize, 0, 0, 0))";
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_SPIRV(tex1Dlod, 0, {
	const spv::Id x = add_instruction(spv::OpCompositeExtract, convert_type({ type::t_float, 1, 1 }))
		.add(args[1].base)
		.add(0); // .x;
	const spv::Id lod = add_instruction(spv::OpCompositeExtract, convert_type({ type::t_float, 1, 1 }))
		.add(args[1].base)
		.add(3); // .w;

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleExplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(x)
		.add(spv::ImageOperandsLodMask)
		.add(lod);
	if (res_type.rows == 1)
		// Collapse last argument from a 4-component vector
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex1Dlod, 1, {
	if (!args[2].is_constant)
		add_capability(spv::CapabilityImageGatherExtended);

	const spv::Id x = add_instruction(spv::OpCompositeExtract, convert_type({ type::t_float, 1, 1 }))
		.add(args[1].base)
		.add(0); // .x;
	const spv::Id lod = add_instruction(spv::OpCompositeExtract, convert_type({ type::t_float, 1, 1 }))
		.add(args[1].base)
		.add(3); // .w;

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleExplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(x)
		.add(spv::ImageOperandsLodMask | (args[2].is_constant ? spv::ImageOperandsConstOffsetMask : spv::ImageOperandsOffsetMask))
		.add(lod)
		.add(args[2].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})

// ret tex2Dlod(s, coords)
// ret tex2Dlod(s, coords, offset)
DEFINE_INTRINSIC(tex2Dlod, 0, int, sampler2d_int, float4)
DEFINE_INTRINSIC(tex2Dlod, 0, uint, sampler2d_uint, float4)
DEFINE_INTRINSIC(tex2Dlod, 0, float, sampler2d_float, float4)
DEFINE_INTRINSIC(tex2Dlod, 0, float4, sampler2d_float4, float4)
DEFINE_INTRINSIC(tex2Dlod, 1, int, sampler2d_int, float4, int2)
DEFINE_INTRINSIC(tex2Dlod, 1, uint, sampler2d_uint, float4, int2)
DEFINE_INTRINSIC(tex2Dlod, 1, float, sampler2d_float, float4, int2)
DEFINE_INTRINSIC(tex2Dlod, 1, float4, sampler2d_float4, float4, int2)
IMPLEMENT_INTRINSIC_GLSL(tex2Dlod, 0, {
	code += "textureLod(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ".xy, " + id_to_name(args[1].base) + ".w)";
	if (res_type.rows == 1)
		code += ".x"; // Collapse last argument from a 4-component vector
	})
IMPLEMENT_INTRINSIC_GLSL(tex2Dlod, 1, {
	code += "textureLodOffset(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ".xy, " + id_to_name(args[1].base) + ".w, " + id_to_name(args[2].base) + ')';
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2Dlod, 0, {
	if (_shader_model >= 40) {
		if (res_type.is_floating_point() || _shader_model >= 67)
			code += id_to_name(args[0].base) + ".t.SampleLevel(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ".xy, " + id_to_name(args[1].base) + ".w)";
		else
			// Integer sampling is not supported until SM6.7, so emulate with a texture fetch
			code += "0; { "
				"float2 _dimensions; float _levels; " +
				id_to_name(args[0].base) + ".t.GetDimensions((int)" + id_to_name(args[1].base) + ".w, _dimensions.x, _dimensions.y, _levels); " +
				id_to_name(res) + " = " + id_to_name(args[0].base) + ".t.Load(int3(" + id_to_name(args[1].base) + ".xy * _dimensions, (int)" + id_to_name(args[1].base) + ".w)); }";
	}
	else {
		code += "tex2Dlod(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ')';
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex2Dlod, 1, {
	if (_shader_model >= 40) {
		if (res_type.is_floating_point() || _shader_model >= 67)
			code += id_to_name(args[0].base) + ".t.SampleLevel(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ".xy, " + id_to_name(args[1].base) + ".w, " + id_to_name(args[2].base) + ')';
		else
			code += "0; { "
				"float2 _dimensions; float _levels; " +
				id_to_name(args[0].base) + ".t.GetDimensions((int)" + id_to_name(args[1].base) + ".w, _dimensions.x, _dimensions.y, _levels); " +
				id_to_name(res) + " = " + id_to_name(args[0].base) + ".t.Load(int3(" + id_to_name(args[1].base) + ".xy * _dimensions, (int)" + id_to_name(args[1].base) + ".w), " + id_to_name(args[2].base) + "); }";
	}
	else {
		code += "tex2Dlod(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + " + float4(" + id_to_name(args[2].base) + " * " + id_to_name(args[0].base) + ".pixelsize, 0, 0))";
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2Dlod, 0, {
	const spv::Id xy = add_instruction(spv::OpVectorShuffle, convert_type({ type::t_float, 2, 1 }))
		.add(args[1].base)
		.add(args[1].base)
		.add(0)  // .x
		.add(1); // .y;
	const spv::Id lod = add_instruction(spv::OpCompositeExtract, convert_type({ type::t_float, 1, 1 }))
		.add(args[1].base)
		.add(3); // .w;

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleExplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(xy)
		.add(spv::ImageOperandsLodMask)
		.add(lod);
	if (res_type.rows == 1)
		// Collapse last argument from a 4-component vector
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2Dlod, 1, {
	if (!args[2].is_constant)
		add_capability(spv::CapabilityImageGatherExtended);

	const spv::Id xy = add_instruction(spv::OpVectorShuffle, convert_type({ type::t_float, 2, 1 }))
		.add(args[1].base)
		.add(args[1].base)
		.add(0)  // .x
		.add(1); // .y;
	const spv::Id lod = add_instruction(spv::OpCompositeExtract, convert_type({ type::t_float, 1, 1 }))
		.add(args[1].base)
		.add(3); // .w;

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleExplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(xy)
		.add(spv::ImageOperandsLodMask | (args[2].is_constant ? spv::ImageOperandsConstOffsetMask : spv::ImageOperandsOffsetMask))
		.add(lod)
		.add(args[2].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})

// ret tex3Dlod(s, coords)
// ret tex3Dlod(s, coords, offset)
DEFINE_INTRINSIC(tex3Dlod, 0, int, sampler3d_int, float4)
DEFINE_INTRINSIC(tex3Dlod, 0, uint, sampler3d_uint, float4)
DEFINE_INTRINSIC(tex3Dlod, 0, float, sampler3d_float, float4)
DEFINE_INTRINSIC(tex3Dlod, 0, float4, sampler3d_float4, float4)
DEFINE_INTRINSIC(tex3Dlod, 1, int, sampler3d_int, float4, int2)
DEFINE_INTRINSIC(tex3Dlod, 1, uint, sampler3d_uint, float4, int2)
DEFINE_INTRINSIC(tex3Dlod, 1, float, sampler3d_float, float4, int2)
DEFINE_INTRINSIC(tex3Dlod, 1, float4, sampler3d_float4, float4, int2)
IMPLEMENT_INTRINSIC_GLSL(tex3Dlod, 0, {
	code += "textureLod(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ".xyz, " + id_to_name(args[1].base) + ".w)";
	if (res_type.rows == 1)
		code += ".x"; // Collapse last argument from a 4-component vector
	})
IMPLEMENT_INTRINSIC_GLSL(tex3Dlod, 1, {
	code += "textureLodOffset(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ".xyz, " + id_to_name(args[1].base) + ".w, " + id_to_name(args[2].base) + ')';
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_HLSL(tex3Dlod, 0, {
	if (_shader_model >= 40) {
		if (res_type.is_floating_point() || _shader_model >= 67)
			code += id_to_name(args[0].base) + ".t.SampleLevel(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ".xyz, " + id_to_name(args[1].base) + ".w)";
		else
			// Integer sampling is not supported until SM6.7, so emulate with a texture fetch
			code += "0; { "
				"float3 _dimensions; float _levels; " +
				id_to_name(args[0].base) + ".t.GetDimensions((int)" + id_to_name(args[1].base) + ".w, _dimensions.x, _dimensions.y, _dimensions.z, _levels); " +
				id_to_name(res) + " = " + id_to_name(args[0].base) + ".t.Load(int4(" + id_to_name(args[1].base) + ".xyz * _dimensions, (int)" + id_to_name(args[1].base) + ".w)); }";
	}
	else {
		code += "tex3Dlod(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ')';
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex3Dlod, 1, {
	if (_shader_model >= 40) {
		if (res_type.is_floating_point() || _shader_model >= 67)
			code += id_to_name(args[0].base) + ".t.SampleLevel(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ".xyz, " + id_to_name(args[1].base) + ".w, " + id_to_name(args[2].base) + ')';
		else
			code += "0; { "
				"float3 _dimensions; float _levels; " +
				id_to_name(args[0].base) + ".t.GetDimensions((int)" + id_to_name(args[1].base) + ".w, _dimensions.x, _dimensions.y, _dimensions.z, levels); " +
				id_to_name(res) + " = " + id_to_name(args[0].base) + ".t.Load(int4(" + id_to_name(args[1].base) + ".xyz * _dimensions, (int)" + id_to_name(args[1].base) + ".w), " + id_to_name(args[2].base) + "); }";
	}
	else {
		code += "tex3Dlod(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + " + float4(" + id_to_name(args[2].base) + " * " + id_to_name(args[0].base) + ".pixelsize, 0))";
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_SPIRV(tex3Dlod, 0, {
	const spv::Id xyz = add_instruction(spv::OpVectorShuffle, convert_type({ type::t_float, 3, 1 }))
		.add(args[1].base)
		.add(args[1].base)
		.add(0)  // .x
		.add(1)  // .y
		.add(2); // .z;
	const spv::Id lod = add_instruction(spv::OpCompositeExtract, convert_type({ type::t_float, 1, 1 }))
		.add(args[1].base)
		.add(3); // .w;

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleExplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(xyz)
		.add(spv::ImageOperandsLodMask)
		.add(lod);
	if (res_type.rows == 1)
		// Collapse last argument from a 4-component vector
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex3Dlod, 1, {
	if (!args[2].is_constant)
		add_capability(spv::CapabilityImageGatherExtended);

	const spv::Id xyz = add_instruction(spv::OpVectorShuffle, convert_type({ type::t_float, 3, 1 }))
		.add(args[1].base)
		.add(args[1].base)
		.add(0)  // .x
		.add(1)  // .y
		.add(2); // .z;
	const spv::Id lod = add_instruction(spv::OpCompositeExtract, convert_type({ type::t_float, 1, 1 }))
		.add(args[1].base)
		.add(3); // .w;

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageSampleExplicitLod, convert_type(res_vector_type))
		.add(args[0].base)
		.add(xyz)
		.add(spv::ImageOperandsLodMask | (args[2].is_constant ? spv::ImageOperandsConstOffsetMask : spv::ImageOperandsOffsetMask))
		.add(lod)
		.add(args[2].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})

// ret tex1Dfetch(s, coords)
// ret tex1Dfetch(s, coords, lod)
DEFINE_INTRINSIC(tex1Dfetch, 0, int, sampler1d_int, int)
DEFINE_INTRINSIC(tex1Dfetch, 0, uint, sampler1d_uint, int)
DEFINE_INTRINSIC(tex1Dfetch, 0, float, sampler1d_float, int)
DEFINE_INTRINSIC(tex1Dfetch, 0, float4, sampler1d_float4, int)
DEFINE_INTRINSIC(tex1Dfetch, 1, int, sampler1d_int, int, int)
DEFINE_INTRINSIC(tex1Dfetch, 1, uint, sampler1d_uint, int, int)
DEFINE_INTRINSIC(tex1Dfetch, 1, float, sampler1d_float, int, int)
DEFINE_INTRINSIC(tex1Dfetch, 1, float4, sampler1d_float4, int, int)
DEFINE_INTRINSIC(tex1Dfetch, 2, int, storage1d_int, int)
DEFINE_INTRINSIC(tex1Dfetch, 2, uint, storage1d_uint, int)
DEFINE_INTRINSIC(tex1Dfetch, 2, float, storage1d_float, int)
DEFINE_INTRINSIC(tex1Dfetch, 2, float4, storage1d_float4, int)
IMPLEMENT_INTRINSIC_GLSL(tex1Dfetch, 0, {
	code += "texelFetch(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", 0)";
	if (res_type.rows == 1)
		code += ".x"; // Collapse last argument from a 4-component vector
	})
IMPLEMENT_INTRINSIC_GLSL(tex1Dfetch, 1, {
	code += "texelFetch(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_GLSL(tex1Dfetch, 2, {
	code += "imageLoad(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ")";
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_HLSL(tex1Dfetch, 0, {
	if (_shader_model >= 40)
		code += id_to_name(args[0].base) + ".t.Load(int2(" + id_to_name(args[1].base) + ", 0))";
	else {
		// SM3 does not have a fetch intrinsic, so emulate it by transforming coordinates into texture space ones
		// Also add a half-pixel offset to align texels with pixels
		//   (coords + 0.5) / size
		code += "tex1Dlod(" + id_to_name(args[0].base) + ".s, float4("
			"(" + id_to_name(args[1].base) + " + 0.5) * " + id_to_name(args[0].base) + ".pixelsize, 0, 0, 0))";
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex1Dfetch, 1, {
	if (_shader_model >= 40)
		code += id_to_name(args[0].base) + ".t.Load(int2(" + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + "))";
	else {
		code += "tex2Dlod(" + id_to_name(args[0].base) + ".s, float4("
			"(" + id_to_name(args[1].base) + " + 0.5) * " + id_to_name(args[0].base) + ".pixelsize * exp2(" + id_to_name(args[2].base) + "), 0, 0, " + id_to_name(args[2].base) + "))";
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex1Dfetch, 2, {
	code += id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + ']';
	})
IMPLEMENT_INTRINSIC_SPIRV(tex1Dfetch, 0, {
	const spv::Id image = add_instruction(spv::OpImage, convert_image_type(args[0].type))
		.add(args[0].base);

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageFetch, convert_type(res_vector_type))
		.add(image)
		.add(args[1].base);
	if (res_type.rows == 1)
		// Collapse last argument from a 4-component vector
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex1Dfetch, 1, {
	const spv::Id image = add_instruction(spv::OpImage, convert_image_type(args[0].type))
		.add(args[0].base);

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageFetch, convert_type(res_vector_type))
		.add(image)
		.add(args[1].base)
		.add(spv::ImageOperandsLodMask)
		.add(args[2].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex1Dfetch, 2, {
	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageRead, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})

// ret tex2Dfetch(s, coords)
// ret tex2Dfetch(s, coords, lod)
DEFINE_INTRINSIC(tex2Dfetch, 0, int, sampler2d_int, int2)
DEFINE_INTRINSIC(tex2Dfetch, 0, uint, sampler2d_uint, int2)
DEFINE_INTRINSIC(tex2Dfetch, 0, float, sampler2d_float, int2)
DEFINE_INTRINSIC(tex2Dfetch, 0, float4, sampler2d_float4, int2)
DEFINE_INTRINSIC(tex2Dfetch, 1, int, sampler2d_int, int2, int)
DEFINE_INTRINSIC(tex2Dfetch, 1, uint, sampler2d_uint, int2, int)
DEFINE_INTRINSIC(tex2Dfetch, 1, float, sampler2d_float, int2, int)
DEFINE_INTRINSIC(tex2Dfetch, 1, float4, sampler2d_float4, int2, int)
DEFINE_INTRINSIC(tex2Dfetch, 2, int, storage2d_int, int2)
DEFINE_INTRINSIC(tex2Dfetch, 2, uint, storage2d_uint, int2)
DEFINE_INTRINSIC(tex2Dfetch, 2, float, storage2d_float, int2)
DEFINE_INTRINSIC(tex2Dfetch, 2, float4, storage2d_float4, int2)
IMPLEMENT_INTRINSIC_GLSL(tex2Dfetch, 0, {
	code += "texelFetch(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", 0)";
	if (res_type.rows == 1)
		code += ".x"; // Collapse last argument from a 4-component vector
	})
IMPLEMENT_INTRINSIC_GLSL(tex2Dfetch, 1, {
	code += "texelFetch(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_GLSL(tex2Dfetch, 2, {
	code += "imageLoad(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2Dfetch, 0, {
	if (_shader_model >= 40)
		code += id_to_name(args[0].base) + ".t.Load(int3(" + id_to_name(args[1].base) + ", 0))";
	else {
		// SM3 does not have a fetch intrinsic, so emulate it by transforming coordinates into texture space ones
		// Also add a half-pixel offset to align texels with pixels
		//   (coords + 0.5) / size
		code += "tex2Dlod(" + id_to_name(args[0].base) + ".s, float4("
			"(" + id_to_name(args[1].base) + " + 0.5) * " + id_to_name(args[0].base) + ".pixelsize, 0, 0))";
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex2Dfetch, 1, {
	if (_shader_model >= 40)
		code += id_to_name(args[0].base) + ".t.Load(int3(" + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + "))";
	else {
		code += "tex2Dlod(" + id_to_name(args[0].base) + ".s, float4("
			"(" + id_to_name(args[1].base) + " + 0.5) * " + id_to_name(args[0].base) + ".pixelsize * exp2(" + id_to_name(args[2].base) + "), 0, " +	id_to_name(args[2].base) + "))";
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex2Dfetch, 2, {
	if (_shader_model >= 50)
		code += id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + ']';
	else
		code += "{}";
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2Dfetch, 0, {
	const spv::Id image = add_instruction(spv::OpImage, convert_image_type(args[0].type))
		.add(args[0].base);

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageFetch, convert_type(res_vector_type))
		.add(image)
		.add(args[1].base);
	if (res_type.rows == 1)
		// Collapse last argument from a 4-component vector
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2Dfetch, 1, {
	const spv::Id image = add_instruction(spv::OpImage, convert_image_type(args[0].type))
		.add(args[0].base);

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageFetch, convert_type(res_vector_type))
		.add(image)
		.add(args[1].base)
		.add(spv::ImageOperandsLodMask)
		.add(args[2].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2Dfetch, 2, {
	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageRead, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})

// ret tex3Dfetch(s, coords)
// ret tex3Dfetch(s, coords, lod)
DEFINE_INTRINSIC(tex3Dfetch, 0, int, sampler3d_int, int3)
DEFINE_INTRINSIC(tex3Dfetch, 0, uint, sampler3d_uint, int3)
DEFINE_INTRINSIC(tex3Dfetch, 0, float, sampler3d_float, int3)
DEFINE_INTRINSIC(tex3Dfetch, 0, float4, sampler3d_float4, int3)
DEFINE_INTRINSIC(tex3Dfetch, 1, int, sampler3d_int, int3, int)
DEFINE_INTRINSIC(tex3Dfetch, 1, uint, sampler3d_uint, int3, int)
DEFINE_INTRINSIC(tex3Dfetch, 1, float, sampler3d_float, int3, int)
DEFINE_INTRINSIC(tex3Dfetch, 1, float4, sampler3d_float4, int3, int)
DEFINE_INTRINSIC(tex3Dfetch, 2, int, storage3d_int, int3)
DEFINE_INTRINSIC(tex3Dfetch, 2, uint, storage3d_uint, int3)
DEFINE_INTRINSIC(tex3Dfetch, 2, float, storage3d_float, int3)
DEFINE_INTRINSIC(tex3Dfetch, 2, float4, storage3d_float4, int3)
IMPLEMENT_INTRINSIC_GLSL(tex3Dfetch, 0, {
	code += "texelFetch(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", 0)";
	if (res_type.rows == 1)
		code += ".x"; // Collapse last argument from a 4-component vector
	})
IMPLEMENT_INTRINSIC_GLSL(tex3Dfetch, 1, {
	code += "texelFetch(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_GLSL(tex3Dfetch, 2, {
	code += "imageLoad(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ")";
	if (res_type.rows == 1)
		code += ".x";
	})
IMPLEMENT_INTRINSIC_HLSL(tex3Dfetch, 0, {
	if (_shader_model >= 40)
		code += id_to_name(args[0].base) + ".t.Load(int4(" + id_to_name(args[1].base) + ", 0))";
	else {
		// SM3 does not have a fetch intrinsic, so emulate it by transforming coordinates into texture space ones
		// Also add a half-pixel offset to align texels with pixels
		//   (coords + 0.5) / size
		code += "tex3Dlod(" + id_to_name(args[0].base) + ".s, float4("
			"(" + id_to_name(args[1].base) + " + 0.5) * " + id_to_name(args[0].base) + ".pixelsize, 0))";
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex3Dfetch, 1, {
	if (_shader_model >= 40)
		code += id_to_name(args[0].base) + ".t.Load(int4(" + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + "))";
	else {
		code += "tex3Dlod(" + id_to_name(args[0].base) + ".s, float4("
			"(" + id_to_name(args[1].base) + " + 0.5) * " + id_to_name(args[0].base) + ".pixelsize * exp2(" + id_to_name(args[2].base) + "), " + id_to_name(args[2].base) + "))";
		if (res_type.rows == 1)
			code += ".x";
	}
	})
IMPLEMENT_INTRINSIC_HLSL(tex3Dfetch, 2, {
	code += id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + ']';
	})
IMPLEMENT_INTRINSIC_SPIRV(tex3Dfetch, 0, {
	const spv::Id image = add_instruction(spv::OpImage, convert_image_type(args[0].type))
		.add(args[0].base);

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageFetch, convert_type(res_vector_type))
		.add(image)
		.add(args[1].base);
	if (res_type.rows == 1)
		// Collapse last argument from a 4-component vector
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex3Dfetch, 1, {
	const spv::Id image = add_instruction(spv::OpImage, convert_image_type(args[0].type))
		.add(args[0].base);

	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageFetch, convert_type(res_vector_type))
		.add(image)
		.add(args[1].base)
		.add(spv::ImageOperandsLodMask)
		.add(args[2].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})
IMPLEMENT_INTRINSIC_SPIRV(tex3Dfetch, 2, {
	type res_vector_type = res_type;
	res_vector_type.rows = 4;

	const spv::Id res = add_instruction(spv::OpImageRead, convert_type(res_vector_type))
		.add(args[0].base)
		.add(args[1].base);
	if (res_type.rows == 1)
		return
		add_instruction(spv::OpCompositeExtract, convert_type(res_type))
			.add(res)
			.add(0u);
	else
		return res;
	})

// ret tex2DgatherR(s, coords)
// ret tex2DgatherR(s, coords, offset)
// ret tex2DgatherR(s, coords, offset0, offset1, offset2, offset3)
DEFINE_INTRINSIC(tex2DgatherR, 0, float4, sampler2d_float4, float2)
DEFINE_INTRINSIC(tex2DgatherR, 1, float4, sampler2d_float4, float2, int2)
DEFINE_INTRINSIC(tex2DgatherR, 2, float4, sampler2d_float4, float2, int2, int2, int2, int2)
IMPLEMENT_INTRINSIC_GLSL(tex2DgatherR, 0, {
	code += "textureGather(" + id_to_name(args[0].base) + ", " +
		id_to_name(args[1].base) + ", 0)";
	})
IMPLEMENT_INTRINSIC_GLSL(tex2DgatherR, 1, {
	code += "textureGatherOffset(" + id_to_name(args[0].base) + ", " +
		id_to_name(args[1].base) + ", " +
		id_to_name(args[2].base) + ", 0)";
	})
IMPLEMENT_INTRINSIC_GLSL(tex2DgatherR, 2, {
	code += "textureGatherOffsets(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " +
		"ivec2[]( " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ", " + id_to_name(args[4].base) + ", " + id_to_name(args[5].base) + "), 0)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2DgatherR, 0, {
	const std::string s = id_to_name(args[0].base);
	if (_shader_model >= 50)
		code += s + ".t.GatherRed(" + s + ".s, " + id_to_name(args[1].base) + ')';
	else if (_shader_model >= 40)
		// Emulate texture gather intrinsic by fetching each location separately (SM41 has 'Gather', but that only works on single component texture formats)
		// See also https://www.reedbeta.com/blog/texture-gathers-and-coordinate-precision/
		code += "0; { "
			"float2 _dimensions; " +
			id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y); "
			"int3 _location = int3(" + id_to_name(args[1].base) + " * _dimensions - 0.5 + 1.0 / 512.0, 0); " +
			id_to_name(res) + " = float4(" +
				s + ".t.Load(_location, int2(0, 1)).r, " +
				s + ".t.Load(_location, int2(1, 1)).r, " +
				s + ".t.Load(_location, int2(1, 0)).r, " +
				s + ".t.Load(_location, int2(0, 0)).r); }";
	else
		code += "float4("
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(0, 1) * " + s + ".pixelsize, 0, 0)).r, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(1, 1) * " + s + ".pixelsize, 0, 0)).r, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(1, 0) * " + s + ".pixelsize, 0, 0)).r, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(0, 0) * " + s + ".pixelsize, 0, 0)).r)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2DgatherR, 1, {
	const std::string s = id_to_name(args[0].base);
	if (_shader_model >= 50)
		code += s + ".t.GatherRed(" + s + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	else if (_shader_model >= 40)
		code += "0; { "
			"float2 _dimensions; " +
			id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y); "
			"int3 _location = int3(" + id_to_name(args[1].base) + " * _dimensions - 0.5 + 1.0 / 512.0, 0); " +
			id_to_name(res) + " = float4(" +
				s + ".t.Load(_location, int2(0, 1) + " + id_to_name(args[2].base) + ").r, " +
				s + ".t.Load(_location, int2(1, 1) + " + id_to_name(args[2].base) + ").r, " +
				s + ".t.Load(_location, int2(1, 0) + " + id_to_name(args[2].base) + ").r, " +
				s + ".t.Load(_location, int2(0, 0) + " + id_to_name(args[2].base) + ").r); }";
	else
		code += "float4("
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(0, 1)) * " + s + ".pixelsize, 0, 0)).r, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(1, 1)) * " + s + ".pixelsize, 0, 0)).r, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(1, 0)) * " + s + ".pixelsize, 0, 0)).r, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(0, 0)) * " + s + ".pixelsize, 0, 0)).r)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2DgatherR, 2, {
	const std::string s = id_to_name(args[0].base);
	if (_shader_model >= 50)
		code += s + ".t.GatherRed(" + s + ".s, " + id_to_name(args[1].base) + ", " +
			id_to_name(args[2].base) + " - int2(0, 1), " +
			id_to_name(args[3].base) + " - int2(1, 1), " +
			id_to_name(args[4].base) + " - int2(1, 0), " +
			id_to_name(args[5].base) + ')';
	else if (_shader_model >= 40)
		code += "0; { "
			"float2 _dimensions; " +
			id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y); "
			"int3 _location = int3(" + id_to_name(args[1].base) + " * _dimensions - 0.5 + 1.0 / 512.0, 0); " +
			id_to_name(res) + " = float4(" +
				s + ".t.Load(_location, int2(0, 1) + " + id_to_name(args[2].base) + ").r, " +
				s + ".t.Load(_location, int2(1, 1) + " + id_to_name(args[3].base) + ").r, " +
				s + ".t.Load(_location, int2(1, 0) + " + id_to_name(args[4].base) + ").r, " +
				s + ".t.Load(_location, int2(0, 0) + " + id_to_name(args[5].base) + ").r); }";
	else
		code += "float4("
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + ") * " + s + ".pixelsize, 0, 0)).r, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[3].base) + ") * " + s + ".pixelsize, 0, 0)).r, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[4].base) + ") * " + s + ".pixelsize, 0, 0)).r, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[5].base) + ") * " + s + ".pixelsize, 0, 0)).r)";
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2DgatherR, 0, {
	const spv::Id comp = emit_constant(0u);

	return
	add_instruction(spv::OpImageGather, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(comp)
		.add(spv::ImageOperandsMaskNone);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2DgatherR, 1, {
	if (!args[2].is_constant)
		add_capability(spv::CapabilityImageGatherExtended);

	const spv::Id comp = emit_constant(0u);

	return
	add_instruction(spv::OpImageGather, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(comp)
		.add(args[2].is_constant ? spv::ImageOperandsConstOffsetMask : spv::ImageOperandsOffsetMask)
		.add(args[2].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2DgatherR, 2, {
	add_capability(spv::CapabilityImageGatherExtended);

	const spv::Id comp = emit_constant(0u);
	const spv::Id offsets = add_instruction(spv::OpConstantComposite, convert_type({ reshadefx::type::t_int, 2, 1, 0, 4 }), _types_and_constants)
		.add(args[2].base)
		.add(args[3].base)
		.add(args[4].base)
		.add(args[5].base);

	return
	add_instruction(spv::OpImageGather, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(comp)
		.add(spv::ImageOperandsConstOffsetsMask)
		.add(offsets);
	})
// ret tex2DgatherG(s, coords)
// ret tex2DgatherG(s, coords, offset)
// ret tex2DgatherG(s, coords, offset0, offset1, offset2, offset3)
DEFINE_INTRINSIC(tex2DgatherG, 0, float4, sampler2d_float4, float2)
DEFINE_INTRINSIC(tex2DgatherG, 1, float4, sampler2d_float4, float2, int2)
DEFINE_INTRINSIC(tex2DgatherG, 2, float4, sampler2d_float4, float2, int2, int2, int2, int2)
IMPLEMENT_INTRINSIC_GLSL(tex2DgatherG, 0, {
	code += "textureGather(" + id_to_name(args[0].base) + ", " +
		id_to_name(args[1].base) + ", 1)";
	})
IMPLEMENT_INTRINSIC_GLSL(tex2DgatherG, 1, {
	code += "textureGatherOffset(" + id_to_name(args[0].base) + ", " +
		id_to_name(args[1].base) + ", " +
		id_to_name(args[2].base) + ", 1)";
	})
IMPLEMENT_INTRINSIC_GLSL(tex2DgatherG, 2, {
	code += "textureGatherOffsets(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " +
		"ivec2[]( " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ", " + id_to_name(args[4].base) + ", " + id_to_name(args[5].base) + "), 1)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2DgatherG, 0, {
	const std::string s = id_to_name(args[0].base);
	if (_shader_model >= 50)
		code += s + ".t.GatherGreen(" + id_to_name(args[0].base) + ".s, " + id_to_name(args[1].base) + ')';
	else if (_shader_model >= 40)
		code += "0; { "
			"float2 _dimensions; " +
			id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y); "
			"int3 _location = int3(" + id_to_name(args[1].base) + " * _dimensions - 0.5 + 1.0 / 512.0, 0); " +
			id_to_name(res) + " = float4(" +
				s + ".t.Load(_location, int2(0, 1)).g, " +
				s + ".t.Load(_location, int2(1, 1)).g, " +
				s + ".t.Load(_location, int2(1, 0)).g, " +
				s + ".t.Load(_location, int2(0, 0)).g); }";
	else
		code += "float4("
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(0, 1) * " + s + ".pixelsize, 0, 0)).g, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(1, 1) * " + s + ".pixelsize, 0, 0)).g, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(1, 0) * " + s + ".pixelsize, 0, 0)).g, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(0, 0) * " + s + ".pixelsize, 0, 0)).g)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2DgatherG, 1, {
	const std::string s = id_to_name(args[0].base);
	if (_shader_model >= 50)
		code += s + ".t.GatherGreen(" + s + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	else if (_shader_model >= 40)
		code += "0; { "
			"float2 _dimensions; " +
			id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y); "
			"int3 _location = int3(" + id_to_name(args[1].base) + " * _dimensions - 0.5 + 1.0 / 512.0, 0); " +
			id_to_name(res) + " = float4(" +
				s + ".t.Load(_location, int2(0, 1) + " + id_to_name(args[2].base) + ").g, " +
				s + ".t.Load(_location, int2(1, 1) + " + id_to_name(args[2].base) + ").g, " +
				s + ".t.Load(_location, int2(1, 0) + " + id_to_name(args[2].base) + ").g, " +
				s + ".t.Load(_location, int2(0, 0) + " + id_to_name(args[2].base) + ").g); }";
	else
		code += "float4("
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(0, 1)) * " + s + ".pixelsize, 0, 0)).g, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(1, 1)) * " + s + ".pixelsize, 0, 0)).g, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(1, 0)) * " + s + ".pixelsize, 0, 0)).g, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(0, 0)) * " + s + ".pixelsize, 0, 0)).g)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2DgatherG, 2, {
	const std::string s = id_to_name(args[0].base);
	if (_shader_model >= 50)
		code += s + ".t.GatherGreen(" + s + ".s, " + id_to_name(args[1].base) + ", " +
			id_to_name(args[2].base) + " - int2(0, 1), " +
			id_to_name(args[3].base) + " - int2(1, 1), " +
			id_to_name(args[4].base) + " - int2(1, 0), " +
			id_to_name(args[5].base) + ')';
	else if (_shader_model >= 40)
		code += "0; { "
			"float2 _dimensions; " +
			id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y); "
			"int3 _location = int3(" + id_to_name(args[1].base) + " * _dimensions - 0.5 + 1.0 / 512.0, 0); " +
			id_to_name(res) + " = float4(" +
				s + ".t.Load(_location, int2(0, 1) + " + id_to_name(args[2].base) + ").g, " +
				s + ".t.Load(_location, int2(1, 1) + " + id_to_name(args[3].base) + ").g, " +
				s + ".t.Load(_location, int2(1, 0) + " + id_to_name(args[4].base) + ").g, " +
				s + ".t.Load(_location, int2(0, 0) + " + id_to_name(args[5].base) + ").g); }";
	else
		code += "float4("
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + ") * " + s + ".pixelsize, 0, 0)).g, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[3].base) + ") * " + s + ".pixelsize, 0, 0)).g, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[4].base) + ") * " + s + ".pixelsize, 0, 0)).g, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[5].base) + ") * " + s + ".pixelsize, 0, 0)).g)";
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2DgatherG, 0, {
	const spv::Id comp = emit_constant(1u);

	return
	add_instruction(spv::OpImageGather, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(comp)
		.add(spv::ImageOperandsMaskNone);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2DgatherG, 1, {
	if (!args[2].is_constant)
		add_capability(spv::CapabilityImageGatherExtended);

	const spv::Id comp = emit_constant(1u);

	return
	add_instruction(spv::OpImageGather, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(comp)
		.add(args[2].is_constant ? spv::ImageOperandsConstOffsetMask : spv::ImageOperandsOffsetMask)
		.add(args[2].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2DgatherG, 2, {
	add_capability(spv::CapabilityImageGatherExtended);

	const spv::Id comp = emit_constant(1u);
	const spv::Id offsets = add_instruction(spv::OpConstantComposite, convert_type({ reshadefx::type::t_int, 2, 1, 0, 4 }), _types_and_constants)
		.add(args[2].base)
		.add(args[3].base)
		.add(args[4].base)
		.add(args[5].base);

	return
	add_instruction(spv::OpImageGather, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(comp)
		.add(spv::ImageOperandsConstOffsetsMask)
		.add(offsets);
	})
// ret tex2DgatherB(s, coords)
// ret tex2DgatherB(s, coords, offset)
// ret tex2DgatherB(s, coords, offset0, offset1, offset2, offset3)
DEFINE_INTRINSIC(tex2DgatherB, 0, float4, sampler2d_float4, float2)
DEFINE_INTRINSIC(tex2DgatherB, 1, float4, sampler2d_float4, float2, int2)
DEFINE_INTRINSIC(tex2DgatherB, 2, float4, sampler2d_float4, float2, int2, int2, int2, int2)
IMPLEMENT_INTRINSIC_GLSL(tex2DgatherB, 0, {
	code += "textureGather(" + id_to_name(args[0].base) + ", " +
		id_to_name(args[1].base) + ", 2)";
	})
IMPLEMENT_INTRINSIC_GLSL(tex2DgatherB, 1, {
	code += "textureGatherOffset(" + id_to_name(args[0].base) + ", " +
		id_to_name(args[1].base) + ", " +
		id_to_name(args[2].base) + ", 2)";
	})
IMPLEMENT_INTRINSIC_GLSL(tex2DgatherB, 2, {
	code += "textureGatherOffsets(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " +
		"ivec2[]( " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ", " + id_to_name(args[4].base) + ", " + id_to_name(args[5].base) + "), 2)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2DgatherB, 0, {
	const std::string s = id_to_name(args[0].base);
	if (_shader_model >= 50)
		code += s + ".t.GatherBlue(" + s + ".s, " + id_to_name(args[1].base) + ')';
	else if (_shader_model >= 40)
		code += "0; { "
			"float2 _dimensions; " +
			id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y); "
			"int3 _location = int3(" + id_to_name(args[1].base) + " * _dimensions - 0.5 + 1.0 / 512.0, 0); " +
			id_to_name(res) + " = float4(" +
				s + ".t.Load(_location, int2(0, 1)).b, " +
				s + ".t.Load(_location, int2(1, 1)).b, " +
				s + ".t.Load(_location, int2(1, 0)).b, " +
				s + ".t.Load(_location, int2(0, 0)).b); }";
	else
		code += "float4("
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(0, 1) * " + s + ".pixelsize, 0, 0)).b, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(1, 1) * " + s + ".pixelsize, 0, 0)).b, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(1, 0) * " + s + ".pixelsize, 0, 0)).b, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(0, 0) * " + s + ".pixelsize, 0, 0)).b)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2DgatherB, 1, {
	const std::string s = id_to_name(args[0].base);
	if (_shader_model >= 50)
		code += s + ".t.GatherBlue(" + s + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	else if (_shader_model >= 40)
		code += "0; { "
			"float2 _dimensions; " +
			id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y); "
			"int3 _location = int3(" + id_to_name(args[1].base) + " * _dimensions - 0.5 + 1.0 / 512.0, 0); " +
			id_to_name(res) + " = float4(" +
				s + ".t.Load(_location, int2(0, 1) + " + id_to_name(args[2].base) + ").b, " +
				s + ".t.Load(_location, int2(1, 1) + " + id_to_name(args[2].base) + ").b, " +
				s + ".t.Load(_location, int2(1, 0) + " + id_to_name(args[2].base) + ").b, " +
				s + ".t.Load(_location, int2(0, 0) + " + id_to_name(args[2].base) + ").b); }";
	else
		code += "float4("
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(0, 1)) * " + s + ".pixelsize, 0, 0)).b, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(1, 1)) * " + s + ".pixelsize, 0, 0)).b, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(1, 0)) * " + s + ".pixelsize, 0, 0)).b, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(0, 0)) * " + s + ".pixelsize, 0, 0)).b)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2DgatherB, 2, {
	const std::string s = id_to_name(args[0].base);
	if (_shader_model >= 50)
		code += s + ".t.GatherBlue(" + s + ".s, " + id_to_name(args[1].base) + ", " +
			id_to_name(args[2].base) + " - int2(0, 1), " +
			id_to_name(args[3].base) + " - int2(1, 1), " +
			id_to_name(args[4].base) + " - int2(1, 0), " +
			id_to_name(args[5].base) + ')';
	else if (_shader_model >= 40)
		code += "0; { "
			"float2 _dimensions; " +
			id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y); "
			"int3 _location = int3(" + id_to_name(args[1].base) + " * _dimensions - 0.5 + 1.0 / 512.0, 0); " +
			id_to_name(res) + " = float4(" +
				s + ".t.Load(_location, int2(0, 1) + " + id_to_name(args[2].base) + ").b, " +
				s + ".t.Load(_location, int2(1, 1) + " + id_to_name(args[3].base) + ").b, " +
				s + ".t.Load(_location, int2(1, 0) + " + id_to_name(args[4].base) + ").b, " +
				s + ".t.Load(_location, int2(0, 0) + " + id_to_name(args[5].base) + ").b); }";
	else
		code += "float4("
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + ") * " + s + ".pixelsize, 0, 0)).b, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[3].base) + ") * " + s + ".pixelsize, 0, 0)).b, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[4].base) + ") * " + s + ".pixelsize, 0, 0)).b, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[5].base) + ") * " + s + ".pixelsize, 0, 0)).b)";
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2DgatherB, 0, {
	const spv::Id comp = emit_constant(2u);

	return
	add_instruction(spv::OpImageGather, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(comp)
		.add(spv::ImageOperandsMaskNone);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2DgatherB, 1, {
	if (!args[2].is_constant)
		add_capability(spv::CapabilityImageGatherExtended);

	const spv::Id comp = emit_constant(2u);

	return
	add_instruction(spv::OpImageGather, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(comp)
		.add(args[2].is_constant ? spv::ImageOperandsConstOffsetMask : spv::ImageOperandsOffsetMask)
		.add(args[2].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2DgatherB, 2, {
	add_capability(spv::CapabilityImageGatherExtended);

	const spv::Id comp = emit_constant(2u);
	const spv::Id offsets = add_instruction(spv::OpConstantComposite, convert_type({ reshadefx::type::t_int, 2, 1, 0, 4 }), _types_and_constants)
		.add(args[2].base)
		.add(args[3].base)
		.add(args[4].base)
		.add(args[5].base);

	return
	add_instruction(spv::OpImageGather, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(comp)
		.add(spv::ImageOperandsConstOffsetsMask)
		.add(offsets);
	})
// ret tex2DgatherA(s, coords)
// ret tex2DgatherA(s, coords, offset)
// ret tex2DgatherA(s, coords, offset0, offset1, offset2, offset3)
DEFINE_INTRINSIC(tex2DgatherA, 0, float4, sampler2d_float4, float2)
DEFINE_INTRINSIC(tex2DgatherA, 1, float4, sampler2d_float4, float2, int2)
DEFINE_INTRINSIC(tex2DgatherA, 2, float4, sampler2d_float4, float2, int2, int2, int2, int2)
IMPLEMENT_INTRINSIC_GLSL(tex2DgatherA, 0, {
	code += "textureGather(" + id_to_name(args[0].base) + ", " +
		id_to_name(args[1].base) + ", 3)";
	})
IMPLEMENT_INTRINSIC_GLSL(tex2DgatherA, 1, {
	code += "textureGatherOffset(" + id_to_name(args[0].base) + ", " +
		id_to_name(args[1].base) + ", " +
		id_to_name(args[2].base) + ", 3)";
	})
IMPLEMENT_INTRINSIC_GLSL(tex2DgatherA, 2, {
	code += "textureGatherOffsets(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " +
		"ivec2[]( " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ", " + id_to_name(args[4].base) + ", " + id_to_name(args[5].base) + "), 3)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2DgatherA, 0, {
	const std::string s = id_to_name(args[0].base);
	if (_shader_model >= 50)
		code += s + ".t.GatherAlpha(" + s + ".s, " + id_to_name(args[1].base) + ')';
	else if (_shader_model >= 40)
		code += "0; { "
			"float2 _dimensions; " +
			id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y); "
			"int3 _location = int3(" + id_to_name(args[1].base) + " * _dimensions - 0.5 + 1.0 / 512.0, 0); " +
			id_to_name(res) + " = float4(" +
				s + ".t.Load(_location, int2(0, 1)).a, " +
				s + ".t.Load(_location, int2(1, 1)).a, " +
				s + ".t.Load(_location, int2(1, 0)).a, " +
				s + ".t.Load(_location, int2(0, 0)).a); }";
	else
		code += "float4("
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(0, 1) * " + s + ".pixelsize, 0, 0)).a, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(1, 1) * " + s + ".pixelsize, 0, 0)).a, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(1, 0) * " + s + ".pixelsize, 0, 0)).a, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + float2(0, 0) * " + s + ".pixelsize, 0, 0)).a)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2DgatherA, 1, {
	const std::string s = id_to_name(args[0].base);
	if (_shader_model >= 50)
		code += s + ".t.GatherAlpha(" + s + ".s, " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	else if (_shader_model >= 40)
		code += "0; { "
			"float2 _dimensions; " +
			id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y); "
			"int3 _location = int3(" + id_to_name(args[1].base) + " * _dimensions - 0.5 + 1.0 / 512.0, 0); " +
			id_to_name(res) + " = float4(" +
				s + ".t.Load(_location, int2(0, 1) + " + id_to_name(args[2].base) + ").a, " +
				s + ".t.Load(_location, int2(1, 1) + " + id_to_name(args[2].base) + ").a, " +
				s + ".t.Load(_location, int2(1, 0) + " + id_to_name(args[2].base) + ").a, " +
				s + ".t.Load(_location, int2(0, 0) + " + id_to_name(args[2].base) + ").a); }";
	else
		code += "float4("
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(0, 1)) * " + s + ".pixelsize, 0, 0)).a, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(1, 1)) * " + s + ".pixelsize, 0, 0)).a, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(1, 0)) * " + s + ".pixelsize, 0, 0)).a, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + " + float2(0, 0)) * " + s + ".pixelsize, 0, 0)).a)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2DgatherA, 2, {
	const std::string s = id_to_name(args[0].base);
	if (_shader_model >= 50)
		code += s + ".t.GatherAlpha(" + s + ".s, " + id_to_name(args[1].base) + ", " +
			id_to_name(args[2].base) + " - int2(0, 1), " +
			id_to_name(args[3].base) + " - int2(1, 1), " +
			id_to_name(args[4].base) + " - int2(1, 0), " +
			id_to_name(args[5].base) + ')';
	else if (_shader_model >= 40)
		code += "0; { "
			"float2 _dimensions; " +
			id_to_name(args[0].base) + ".t.GetDimensions(_dimensions.x, _dimensions.y); "
			"int3 _location = int3(" + id_to_name(args[1].base) + " * _dimensions - 0.5 + 1.0 / 512.0, 0); " +
			id_to_name(res) + " = float4(" +
				s + ".t.Load(_location, int2(0, 1) + " + id_to_name(args[2].base) + ").a, " +
				s + ".t.Load(_location, int2(1, 1) + " + id_to_name(args[3].base) + ").a, " +
				s + ".t.Load(_location, int2(1, 0) + " + id_to_name(args[4].base) + ").a, " +
				s + ".t.Load(_location, int2(0, 0) + " + id_to_name(args[5].base) + ").a); }";
	else
		code += "float4("
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[2].base) + ") * " + s + ".pixelsize, 0, 0)).a, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[3].base) + ") * " + s + ".pixelsize, 0, 0)).a, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[4].base) + ") * " + s + ".pixelsize, 0, 0)).a, "
			"tex2Dlod(" + s + ".s, float4(" + id_to_name(args[1].base) + " + (" + id_to_name(args[5].base) + ") * " + s + ".pixelsize, 0, 0)).a)";
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2DgatherA, 0, {
	const spv::Id comp = emit_constant(3u);

	return
	add_instruction(spv::OpImageGather, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(comp)
		.add(spv::ImageOperandsMaskNone);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2DgatherA, 1, {
	if (!args[2].is_constant)
		add_capability(spv::CapabilityImageGatherExtended);

	const spv::Id comp = emit_constant(3u);

	return
	add_instruction(spv::OpImageGather, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(comp)
		.add(args[2].is_constant ? spv::ImageOperandsConstOffsetMask : spv::ImageOperandsOffsetMask)
		.add(args[2].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2DgatherA, 2, {
	add_capability(spv::CapabilityImageGatherExtended);

	const spv::Id comp = emit_constant(3u);
	const spv::Id offsets = add_instruction(spv::OpConstantComposite, convert_type({ reshadefx::type::t_int, 2, 1, 0, 4 }), _types_and_constants)
		.add(args[2].base)
		.add(args[3].base)
		.add(args[4].base)
		.add(args[5].base);

	return
	add_instruction(spv::OpImageGather, convert_type(res_type))
		.add(args[0].base)
		.add(args[1].base)
		.add(comp)
		.add(spv::ImageOperandsConstOffsetsMask)
		.add(offsets);
	})

// tex1Dstore(s, coords, value)
DEFINE_INTRINSIC(tex1Dstore, 0, void, storage1d_int, int, int)
DEFINE_INTRINSIC(tex1Dstore, 0, void, storage1d_uint, int, uint)
DEFINE_INTRINSIC(tex1Dstore, 0, void, storage1d_float, int, float)
DEFINE_INTRINSIC(tex1Dstore, 0, void, storage1d_float4, int, float4)
IMPLEMENT_INTRINSIC_GLSL(tex1Dstore, 0, {
	code += "imageStore(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", ";
	if (args[2].type.rows == 1)
		code += '(';
	code += id_to_name(args[2].base);
	if (args[2].type.rows == 1)
		code += ").xxxx"; // Expand last argument to a 4-component vector
	code += ')';
	})
IMPLEMENT_INTRINSIC_HLSL(tex1Dstore, 0, {
	if (_shader_model >= 50)
		code += id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + "] = " + id_to_name(args[2].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex1Dstore, 0, {
	spv::Id data = args[2].base;
	if (args[2].type.rows == 1)
	{
		// Expand last argument to a 4-component vector
		auto comp_type = args[2].type;
		comp_type.rows = 4;

		data = add_instruction(spv::OpCompositeConstruct, convert_type(comp_type))
			.add(data)
			.add(data)
			.add(data)
			.add(data);
	}

	add_instruction_without_result(spv::OpImageWrite)
		.add(args[0].base)
		.add(args[1].base)
		.add(data);
	return 0;
	})

// tex2Dstore(s, coords, value)
DEFINE_INTRINSIC(tex2Dstore, 0, void, storage2d_int, int2, int)
DEFINE_INTRINSIC(tex2Dstore, 0, void, storage2d_uint, int2, uint)
DEFINE_INTRINSIC(tex2Dstore, 0, void, storage2d_float, int2, float)
DEFINE_INTRINSIC(tex2Dstore, 0, void, storage2d_float4, int2, float4)
IMPLEMENT_INTRINSIC_GLSL(tex2Dstore, 0, {
	code += "imageStore(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", ";
	if (args[2].type.rows == 1)
		code += '(';
	code += id_to_name(args[2].base);
	if (args[2].type.rows == 1)
		code += ").xxxx"; // Expand last argument to a 4-component vector
	code += ')';
	})
IMPLEMENT_INTRINSIC_HLSL(tex2Dstore, 0, {
	if (_shader_model >= 50)
		code += id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + "] = " + id_to_name(args[2].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2Dstore, 0, {
	spv::Id data = args[2].base;
	if (args[2].type.rows == 1)
	{
		// Expand last argument to a 4-component vector
		auto comp_type = args[2].type;
		comp_type.rows = 4;

		data = add_instruction(spv::OpCompositeConstruct, convert_type(comp_type))
			.add(data)
			.add(data)
			.add(data)
			.add(data);
	}

	add_instruction_without_result(spv::OpImageWrite)
		.add(args[0].base)
		.add(args[1].base)
		.add(data);
	return 0;
	})

// tex3Dstore(s, coords, value)
DEFINE_INTRINSIC(tex3Dstore, 0, void, storage3d_int, int3, int)
DEFINE_INTRINSIC(tex3Dstore, 0, void, storage3d_uint, int3, uint)
DEFINE_INTRINSIC(tex3Dstore, 0, void, storage3d_float, int3, float)
DEFINE_INTRINSIC(tex3Dstore, 0, void, storage3d_float4, int3, float4)
IMPLEMENT_INTRINSIC_GLSL(tex3Dstore, 0, {
	code += "imageStore(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", ";
	if (args[2].type.rows == 1)
		code += '(';
	code += id_to_name(args[2].base);
	if (args[2].type.rows == 1)
		code += ").xxxx"; // Expand last argument to a 4-component vector
	code += ')';
	})
IMPLEMENT_INTRINSIC_HLSL(tex3Dstore, 0, {
	if (_shader_model >= 50)
		code += id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + "] = " + id_to_name(args[2].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex3Dstore, 0, {
	spv::Id data = args[2].base;
	if (args[2].type.rows == 1)
	{
		// Expand last argument to a 4-component vector
		auto comp_type = args[2].type;
		comp_type.rows = 4;

		data = add_instruction(spv::OpCompositeConstruct, convert_type(comp_type))
			.add(data)
			.add(data)
			.add(data)
			.add(data);
	}

	add_instruction_without_result(spv::OpImageWrite)
		.add(args[0].base)
		.add(args[1].base)
		.add(data);
	return 0;
	})

// ret tex1Dsize(s)
// ret tex1Dsize(s, lod)
DEFINE_INTRINSIC(tex1Dsize, 0, int, sampler1d_int)
DEFINE_INTRINSIC(tex1Dsize, 0, int, sampler1d_uint)
DEFINE_INTRINSIC(tex1Dsize, 0, int, sampler1d_float)
DEFINE_INTRINSIC(tex1Dsize, 0, int, sampler1d_float4)
DEFINE_INTRINSIC(tex1Dsize, 1, int, sampler1d_int, int)
DEFINE_INTRINSIC(tex1Dsize, 1, int, sampler1d_uint, int)
DEFINE_INTRINSIC(tex1Dsize, 1, int, sampler1d_float, int)
DEFINE_INTRINSIC(tex1Dsize, 1, int, sampler1d_float4, int)
DEFINE_INTRINSIC(tex1Dsize, 2, int, storage1d_int)
DEFINE_INTRINSIC(tex1Dsize, 2, int, storage1d_uint)
DEFINE_INTRINSIC(tex1Dsize, 2, int, storage1d_float)
DEFINE_INTRINSIC(tex1Dsize, 2, int, storage1d_float4)
IMPLEMENT_INTRINSIC_GLSL(tex1Dsize, 0, {
	code += "textureSize(" + id_to_name(args[0].base) + ", 0)";
	})
IMPLEMENT_INTRINSIC_GLSL(tex1Dsize, 1, {
	code += "textureSize(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(tex1Dsize, 2, {
	code += "imageSize(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(tex1Dsize, 0, {
	if (_shader_model >= 40)
		code += "0; " + id_to_name(args[0].base) + ".t.GetDimensions(" + id_to_name(res) + ')';
	else
		code += "int(1.0 / " + id_to_name(args[0].base) + ".pixelsize)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex1Dsize, 1, {
	if (_shader_model >= 40)
		code += "0; { uint _levels; " + // Don't need the number of levels out value, so route that to a dummy variable
			id_to_name(args[0].base) + ".t.GetDimensions(" + id_to_name(args[1].base) + ", " + id_to_name(res) + ", _levels); }";
	else
		code += "int(1.0 / " + id_to_name(args[0].base) + ".pixelsize) / exp2(" + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(tex1Dsize, 2, {
	if (_shader_model >= 50)
		code += "0; " + id_to_name(args[0].base) + ".GetDimensions(" + id_to_name(res) + ')';
	else
		code += "0"; // Only supported on SM5+
	})
IMPLEMENT_INTRINSIC_SPIRV(tex1Dsize, 0, {
	add_capability(spv::CapabilityImageQuery);

	const spv::Id image = add_instruction(spv::OpImage, convert_image_type(args[0].type))
		.add(args[0].base);
	const spv::Id level = emit_constant(0u);

	return
	add_instruction(spv::OpImageQuerySizeLod, convert_type(res_type))
		.add(image)
		.add(level);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex1Dsize, 1, {
	add_capability(spv::CapabilityImageQuery);

	const spv::Id image = add_instruction(spv::OpImage, convert_image_type(args[0].type))
		.add(args[0].base);

	return
	add_instruction(spv::OpImageQuerySizeLod, convert_type(res_type))
		.add(image)
		.add(args[1].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex1Dsize, 2, {
	add_capability(spv::CapabilityImageQuery);

	return
	add_instruction(spv::OpImageQuerySize, convert_type(res_type))
		.add(args[0].base);
	})

// ret tex2Dsize(s)
// ret tex2Dsize(s, lod)
DEFINE_INTRINSIC(tex2Dsize, 0, int2, sampler2d_int)
DEFINE_INTRINSIC(tex2Dsize, 0, int2, sampler2d_uint)
DEFINE_INTRINSIC(tex2Dsize, 0, int2, sampler2d_float)
DEFINE_INTRINSIC(tex2Dsize, 0, int2, sampler2d_float4)
DEFINE_INTRINSIC(tex2Dsize, 1, int2, sampler2d_int, int)
DEFINE_INTRINSIC(tex2Dsize, 1, int2, sampler2d_uint, int)
DEFINE_INTRINSIC(tex2Dsize, 1, int2, sampler2d_float, int)
DEFINE_INTRINSIC(tex2Dsize, 1, int2, sampler2d_float4, int)
DEFINE_INTRINSIC(tex2Dsize, 2, int2, storage2d_int)
DEFINE_INTRINSIC(tex2Dsize, 2, int2, storage2d_uint)
DEFINE_INTRINSIC(tex2Dsize, 2, int2, storage2d_float)
DEFINE_INTRINSIC(tex2Dsize, 2, int2, storage2d_float4)
IMPLEMENT_INTRINSIC_GLSL(tex2Dsize, 0, {
	code += "textureSize(" + id_to_name(args[0].base) + ", 0)";
	})
IMPLEMENT_INTRINSIC_GLSL(tex2Dsize, 1, {
	code += "textureSize(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(tex2Dsize, 2, {
	code += "imageSize(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(tex2Dsize, 0, {
	if (_shader_model >= 40)
		code += "0; " + id_to_name(args[0].base) + ".t.GetDimensions(" + id_to_name(res) + ".x, " + id_to_name(res) + ".y)";
	else
		code += "int2(1.0 / " + id_to_name(args[0].base) + ".pixelsize)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex2Dsize, 1, {
	if (_shader_model >= 40)
		code += "0; { uint _levels; " + // Don't need the number of levels out value, so route that to a dummy variable
			id_to_name(args[0].base) + ".t.GetDimensions(" + id_to_name(args[1].base) + ", " + id_to_name(res) + ".x, " + id_to_name(res) + ".y, _levels); }";
	else
		code += "int2(1.0 / " + id_to_name(args[0].base) + ".pixelsize) / exp2(" + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(tex2Dsize, 2, {
	if (_shader_model >= 50)
		code += "0; " + id_to_name(args[0].base) + ".GetDimensions(" + id_to_name(res) + ".x, " + id_to_name(res) + ".y)";
	else
		code += "int2(0, 0)"; // Only supported on SM5+
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2Dsize, 0, {
	add_capability(spv::CapabilityImageQuery);

	const spv::Id image = add_instruction(spv::OpImage, convert_image_type(args[0].type))
		.add(args[0].base);
	const spv::Id level = emit_constant(0u);

	return
	add_instruction(spv::OpImageQuerySizeLod, convert_type(res_type))
		.add(image)
		.add(level);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2Dsize, 1, {
	add_capability(spv::CapabilityImageQuery);

	const spv::Id image = add_instruction(spv::OpImage, convert_image_type(args[0].type))
		.add(args[0].base);

	return
	add_instruction(spv::OpImageQuerySizeLod, convert_type(res_type))
		.add(image)
		.add(args[1].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex2Dsize, 2, {
	add_capability(spv::CapabilityImageQuery);

	return
	add_instruction(spv::OpImageQuerySize, convert_type(res_type))
		.add(args[0].base);
	})

// ret tex3Dsize(s)
// ret tex3Dsize(s, lod)
DEFINE_INTRINSIC(tex3Dsize, 0, int3, sampler3d_int)
DEFINE_INTRINSIC(tex3Dsize, 0, int3, sampler3d_uint)
DEFINE_INTRINSIC(tex3Dsize, 0, int3, sampler3d_float)
DEFINE_INTRINSIC(tex3Dsize, 0, int3, sampler3d_float4)
DEFINE_INTRINSIC(tex3Dsize, 1, int3, sampler3d_int, int)
DEFINE_INTRINSIC(tex3Dsize, 1, int3, sampler3d_uint, int)
DEFINE_INTRINSIC(tex3Dsize, 1, int3, sampler3d_float, int)
DEFINE_INTRINSIC(tex3Dsize, 1, int3, sampler3d_float4, int)
DEFINE_INTRINSIC(tex3Dsize, 2, int3, storage3d_int)
DEFINE_INTRINSIC(tex3Dsize, 2, int3, storage3d_uint)
DEFINE_INTRINSIC(tex3Dsize, 2, int3, storage3d_float)
DEFINE_INTRINSIC(tex3Dsize, 2, int3, storage3d_float4)
IMPLEMENT_INTRINSIC_GLSL(tex3Dsize, 0, {
	code += "textureSize(" + id_to_name(args[0].base) + ", 0)";
	})
IMPLEMENT_INTRINSIC_GLSL(tex3Dsize, 1, {
	code += "textureSize(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(tex3Dsize, 2, {
	code += "imageSize(" + id_to_name(args[0].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(tex3Dsize, 0, {
	if (_shader_model >= 40)
		code += "0; " + id_to_name(args[0].base) + ".t.GetDimensions(" + id_to_name(res) + ".x, " + id_to_name(res) + ".y, " + id_to_name(res) + ".z)";
	else
		code += "int3(1.0 / " + id_to_name(args[0].base) + ".pixelsize)";
	})
IMPLEMENT_INTRINSIC_HLSL(tex3Dsize, 1, {
	if (_shader_model >= 40)
		code += "0; { uint _levels; " + // Don't need the number of levels out value, so route that to a dummy variable
			id_to_name(args[0].base) + ".t.GetDimensions(" + id_to_name(args[1].base) + ", " + id_to_name(res) + ".x, " + id_to_name(res) + ".y, " + id_to_name(res) + ".z, _levels); }";
	else
		code += "int3(1.0 / " + id_to_name(args[0].base) + ".pixelsize) / exp2(" + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(tex3Dsize, 2, {
	if (_shader_model >= 50)
		code += "0; " + id_to_name(args[0].base) + ".GetDimensions(" + id_to_name(res) + ".x, " + id_to_name(res) + ".y, " + id_to_name(res) + ".z)";
	else
		code += "int3(0, 0, 0)"; // Only supported on SM5+
	})
IMPLEMENT_INTRINSIC_SPIRV(tex3Dsize, 0, {
	add_capability(spv::CapabilityImageQuery);

	const spv::Id image = add_instruction(spv::OpImage, convert_image_type(args[0].type))
		.add(args[0].base);
	const spv::Id level = emit_constant(0u);

	return
	add_instruction(spv::OpImageQuerySizeLod, convert_type(res_type))
		.add(image)
		.add(level);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex3Dsize, 1, {
	add_capability(spv::CapabilityImageQuery);

	const spv::Id image = add_instruction(spv::OpImage, convert_image_type(args[0].type))
		.add(args[0].base);

	return
	add_instruction(spv::OpImageQuerySizeLod, convert_type(res_type))
		.add(image)
		.add(args[1].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(tex3Dsize, 2, {
	add_capability(spv::CapabilityImageQuery);

	return
	add_instruction(spv::OpImageQuerySize, convert_type(res_type))
		.add(args[0].base);
	})

// barrier()
DEFINE_INTRINSIC(barrier, 0, void)
IMPLEMENT_INTRINSIC_GLSL(barrier, 0, {
	code += "barrier()";
	})
IMPLEMENT_INTRINSIC_HLSL(barrier, 0, {
	if (_shader_model >= 50)
		code += "GroupMemoryBarrierWithGroupSync()";
	})
IMPLEMENT_INTRINSIC_SPIRV(barrier, 0, {
	const spv::Id mem_scope = emit_constant(spv::ScopeWorkgroup);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsWorkgroupMemoryMask | spv::MemorySemanticsAcquireReleaseMask);

	add_instruction_without_result(spv::OpControlBarrier)
		.add(mem_scope) // Execution scope
		.add(mem_scope)
		.add(mem_semantics);
	return 0;
	})

// memoryBarrier()
DEFINE_INTRINSIC(memoryBarrier, 0, void)
IMPLEMENT_INTRINSIC_GLSL(memoryBarrier, 0, {
	code += "memoryBarrier()";
	})
IMPLEMENT_INTRINSIC_HLSL(memoryBarrier, 0, {
	if (_shader_model >= 50)
		code += "AllMemoryBarrier()";
	})
IMPLEMENT_INTRINSIC_SPIRV(memoryBarrier, 0, {
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsImageMemoryMask | spv::MemorySemanticsUniformMemoryMask | spv::MemorySemanticsWorkgroupMemoryMask | spv::MemorySemanticsAcquireReleaseMask);

	add_instruction_without_result(spv::OpMemoryBarrier)
		.add(mem_scope)
		.add(mem_semantics);
	return 0;
	})
// groupMemoryBarrier()
DEFINE_INTRINSIC(groupMemoryBarrier, 0, void)
IMPLEMENT_INTRINSIC_GLSL(groupMemoryBarrier, 0, {
	code += "groupMemoryBarrier()";
	})
IMPLEMENT_INTRINSIC_HLSL(groupMemoryBarrier, 0, {
	if (_shader_model >= 50)
		code += "GroupMemoryBarrier()";
	})
IMPLEMENT_INTRINSIC_SPIRV(groupMemoryBarrier, 0, {
	const spv::Id mem_scope = emit_constant(spv::ScopeWorkgroup);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsWorkgroupMemoryMask | spv::MemorySemanticsAcquireReleaseMask);

	add_instruction_without_result(spv::OpMemoryBarrier)
		.add(mem_scope)
		.add(mem_semantics);
	return 0;
	})

// ret atomicAdd(inout mem, data)
DEFINE_INTRINSIC(atomicAdd, 0, int, inout_int, int)
DEFINE_INTRINSIC(atomicAdd, 0, uint, inout_uint, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicAdd, 0, {
	code += "atomicAdd(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicAdd, 0, {
	if (_shader_model >= 50)
		code += "0; InterlockedAdd(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(res) + ')';
	else
		code += id_to_name(args[0].base) + "; " + id_to_name(args[0].base) + " += " + id_to_name(args[1].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicAdd, 0, {
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicIAdd, convert_type(res_type))
		.add(args[0].base)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[1].base);
	})
// ret atomicAdd(s, coords, data)
DEFINE_INTRINSIC(atomicAdd, 1, int, inout_storage1d_int, int, int)
DEFINE_INTRINSIC(atomicAdd, 1, int, inout_storage2d_int, int2, int)
DEFINE_INTRINSIC(atomicAdd, 1, int, inout_storage3d_int, int3, int)
DEFINE_INTRINSIC(atomicAdd, 1, uint, inout_storage1d_uint, int, uint)
DEFINE_INTRINSIC(atomicAdd, 1, uint, inout_storage2d_uint, int2, uint)
DEFINE_INTRINSIC(atomicAdd, 1, uint, inout_storage3d_uint, int3, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicAdd, 1, {
	code += "imageAtomicAdd(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicAdd, 1, {
	if (_shader_model >= 50)
		code += "0; InterlockedAdd(" + id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + ']' + ", " + id_to_name(args[2].base) + ", " + id_to_name(res) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicAdd, 1, {
	const spv::Id ms_sample = emit_constant(0u);

	const spv::Id texel = add_instruction(spv::OpImageTexelPointer, convert_type(res_type, true, spv::StorageClassImage))
		.add(args[0].base)
		.add(args[1].base)
		.add(ms_sample);
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicIAdd, convert_type(res_type))
		.add(texel)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[2].base);
	})

// ret atomicAnd(inout mem, data)
DEFINE_INTRINSIC(atomicAnd, 0, int, inout_int, int)
DEFINE_INTRINSIC(atomicAnd, 0, uint, inout_uint, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicAnd, 0, {
	code += "atomicAnd(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicAnd, 0, {
	if (_shader_model >= 50)
		code += "0; InterlockedAnd(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(res) + ')';
	else
		code += id_to_name(args[0].base) + "; " + id_to_name(args[0].base) + " &= " + id_to_name(args[1].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicAnd, 0, {
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicAnd, convert_type(res_type))
		.add(args[0].base)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[1].base);
	})
// ret atomicAnd(s, coords, data)
DEFINE_INTRINSIC(atomicAnd, 1, int, inout_storage1d_int, int, int)
DEFINE_INTRINSIC(atomicAnd, 1, int, inout_storage2d_int, int2, int)
DEFINE_INTRINSIC(atomicAnd, 1, int, inout_storage3d_int, int3, int)
DEFINE_INTRINSIC(atomicAnd, 1, uint, inout_storage1d_uint, int, uint)
DEFINE_INTRINSIC(atomicAnd, 1, uint, inout_storage2d_uint, int2, uint)
DEFINE_INTRINSIC(atomicAnd, 1, uint, inout_storage3d_uint, int3, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicAnd, 1, {
	code += "imageAtomicAnd(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicAnd, 1, {
	if (_shader_model >= 50)
		code += "InterlockedAnd(" + id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + ']' + ", " + id_to_name(args[2].base) + ", " + id_to_name(res) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicAnd, 1, {
	const spv::Id ms_sample = emit_constant(0u);

	const spv::Id texel = add_instruction(spv::OpImageTexelPointer, convert_type(res_type, true, spv::StorageClassImage))
		.add(args[0].base)
		.add(args[1].base)
		.add(ms_sample);
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicAnd, convert_type(res_type))
		.add(texel)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[2].base);
	})

// ret atomicOr(inout mem, data)
DEFINE_INTRINSIC(atomicOr, 0, int, inout_int, int)
DEFINE_INTRINSIC(atomicOr, 0, uint, inout_uint, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicOr, 0, {
	code += "atomicOr(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicOr, 0, {
	if (_shader_model >= 50)
		code += "0; InterlockedOr(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(res) + ')';
	else
		code += id_to_name(args[0].base) + "; " + id_to_name(args[0].base) + " |= " + id_to_name(args[1].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicOr, 0, {
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicOr, convert_type(res_type))
		.add(args[0].base)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[1].base);
	})
// ret atomicOr(s, coords, data)
DEFINE_INTRINSIC(atomicOr, 1, int, inout_storage1d_int, int, int)
DEFINE_INTRINSIC(atomicOr, 1, int, inout_storage2d_int, int2, int)
DEFINE_INTRINSIC(atomicOr, 1, int, inout_storage3d_int, int3, int)
DEFINE_INTRINSIC(atomicOr, 1, uint, inout_storage1d_uint, int, uint)
DEFINE_INTRINSIC(atomicOr, 1, uint, inout_storage2d_uint, int2, uint)
DEFINE_INTRINSIC(atomicOr, 1, uint, inout_storage3d_uint, int3, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicOr, 1, {
	code += "imageAtomicOr(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicOr, 1, {
	if (_shader_model >= 50)
		code += "0; InterlockedOr(" + id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + ']' + ", " + id_to_name(args[2].base) + ", " + id_to_name(res) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicOr, 1, {
	const spv::Id ms_sample = emit_constant(0u);

	const spv::Id texel = add_instruction(spv::OpImageTexelPointer, convert_type(res_type, true, spv::StorageClassImage))
		.add(args[0].base)
		.add(args[1].base)
		.add(ms_sample);
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicOr, convert_type(res_type))
		.add(texel)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[2].base);
	})

// ret atomicXor(inout mem, data)
DEFINE_INTRINSIC(atomicXor, 0, int, inout_int, int)
DEFINE_INTRINSIC(atomicXor, 0, uint, inout_uint, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicXor, 0, {
	code += "atomicXor(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicXor, 0, {
	if (_shader_model >= 50)
		code += "0; InterlockedXor(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(res) + ')';
	else
		code += id_to_name(args[0].base) + "; " + id_to_name(args[0].base) + " ^= " + id_to_name(args[1].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicXor, 0, {
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicXor, convert_type(res_type))
		.add(args[0].base)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[1].base);
	})
// ret atomicXor(s, coords, data)
DEFINE_INTRINSIC(atomicXor, 1, int, inout_storage1d_int, int, int)
DEFINE_INTRINSIC(atomicXor, 1, int, inout_storage2d_int, int2, int)
DEFINE_INTRINSIC(atomicXor, 1, int, inout_storage3d_int, int3, int)
DEFINE_INTRINSIC(atomicXor, 1, uint, inout_storage1d_uint, int, uint)
DEFINE_INTRINSIC(atomicXor, 1, uint, inout_storage2d_uint, int2, uint)
DEFINE_INTRINSIC(atomicXor, 1, uint, inout_storage3d_uint, int3, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicXor, 1, {
	code += "imageAtomicXor(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicXor, 1, {
	if (_shader_model >= 50)
		code += "0; InterlockedXor(" + id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + ']' + ", " + id_to_name(args[2].base) + ", " + id_to_name(res) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicXor, 1, {
	const spv::Id ms_sample = emit_constant(0u);

	const spv::Id texel = add_instruction(spv::OpImageTexelPointer, convert_type(res_type, true, spv::StorageClassImage))
		.add(args[0].base)
		.add(args[1].base)
		.add(ms_sample);
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicXor, convert_type(res_type))
		.add(texel)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[2].base);
	})

// ret atomicMin(inout mem, data)
DEFINE_INTRINSIC(atomicMin, 0, int, inout_int, int)
DEFINE_INTRINSIC(atomicMin, 1, uint, inout_uint, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicMin, 0, {
	code += "atomicMin(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(atomicMin, 1, {
	code += "atomicMin(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicMin, 0, {
	if (_shader_model >= 50)
		code += "0; InterlockedMin(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(res) + ')';
	else
		code += id_to_name(args[0].base) + "; " + id_to_name(args[0].base) + " = min(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicMin, 1, {
	if (_shader_model >= 50)
		code += "0; InterlockedMin(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(res) + ')';
	else
		code += id_to_name(args[0].base) + "; " + id_to_name(args[0].base) + " = min(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicMin, 0, {
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicSMin, convert_type(res_type))
		.add(args[0].base)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[1].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicMin, 1, {
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicUMin, convert_type(res_type))
		.add(args[0].base)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[1].base);
	})
// ret atomicMin(s, coords, data)
DEFINE_INTRINSIC(atomicMin, 2, int, inout_storage1d_int, int, int)
DEFINE_INTRINSIC(atomicMin, 2, int, inout_storage2d_int, int2, int)
DEFINE_INTRINSIC(atomicMin, 2, int, inout_storage3d_int, int3, int)
DEFINE_INTRINSIC(atomicMin, 3, uint, inout_storage1d_uint, int, uint)
DEFINE_INTRINSIC(atomicMin, 3, uint, inout_storage2d_uint, int2, uint)
DEFINE_INTRINSIC(atomicMin, 3, uint, inout_storage3d_uint, int3, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicMin, 2, {
	code += "imageAtomicMin(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(atomicMin, 3, {
	code += "imageAtomicMin(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicMin, 2, {
	if (_shader_model >= 50)
		code += "0; InterlockedMin(" + id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + ']' + ", " + id_to_name(args[2].base) + ", " + id_to_name(res) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicMin, 3, {
	if (_shader_model >= 50)
		code += "0; InterlockedMin(" + id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + ']' + ", " + id_to_name(args[2].base) + ", " + id_to_name(res) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicMin, 2, {
	const spv::Id ms_sample = emit_constant(0u);

	const spv::Id texel = add_instruction(spv::OpImageTexelPointer, convert_type(res_type, true, spv::StorageClassImage))
		.add(args[0].base)
		.add(args[1].base)
		.add(ms_sample);
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicSMin, convert_type(res_type))
		.add(texel)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[2].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicMin, 3, {
	const spv::Id ms_sample = emit_constant(0u);

	const spv::Id texel = add_instruction(spv::OpImageTexelPointer, convert_type(res_type, true, spv::StorageClassImage))
		.add(args[0].base)
		.add(args[1].base)
		.add(ms_sample);
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicUMin, convert_type(res_type))
		.add(texel)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[2].base);
	})

// ret atomicMax(inout mem, data)
DEFINE_INTRINSIC(atomicMax, 0, int, inout_int, int)
DEFINE_INTRINSIC(atomicMax, 1, uint, inout_uint, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicMax, 0, {
	code += "atomicMax(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(atomicMax, 1, {
	code += "atomicMax(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicMax, 0, {
	if (_shader_model >= 50)
		code += "0; InterlockedMax(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(res) + ')';
	else
		code += id_to_name(args[0].base) + "; " + id_to_name(args[0].base) + " = max(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicMax, 1, {
	if (_shader_model >= 50)
		code += "0; InterlockedMax(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(res) + ')';
	else
		code += id_to_name(args[0].base) + "; " + id_to_name(args[0].base) + " = max(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicMax, 0, {
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicSMax, convert_type(res_type))
		.add(args[0].base)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[1].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicMax, 1, {
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicUMax, convert_type(res_type))
		.add(args[0].base)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[1].base);
	})
// ret atomicMax(s, coords, data)
DEFINE_INTRINSIC(atomicMax, 2, int, inout_storage1d_int, int, int)
DEFINE_INTRINSIC(atomicMax, 2, int, inout_storage2d_int, int2, int)
DEFINE_INTRINSIC(atomicMax, 2, int, inout_storage3d_int, int3, int)
DEFINE_INTRINSIC(atomicMax, 3, uint, inout_storage1d_uint, int, uint)
DEFINE_INTRINSIC(atomicMax, 3, uint, inout_storage2d_uint, int2, uint)
DEFINE_INTRINSIC(atomicMax, 3, uint, inout_storage3d_uint, int3, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicMax, 2, {
	code += "imageAtomicMax(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_GLSL(atomicMax, 3, {
	code += "imageAtomicMax(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicMax, 2, {
	if (_shader_model >= 50)
		code += "0; InterlockedMax(" + id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + ']' + ", " + id_to_name(args[2].base) + ", " + id_to_name(res) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicMax, 3, {
	if (_shader_model >= 50)
		code += "0; InterlockedMax(" + id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + ']' + ", " + id_to_name(args[2].base) + ", " + id_to_name(res) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicMax, 2, {
	const spv::Id ms_sample = emit_constant(0u);

	const spv::Id texel = add_instruction(spv::OpImageTexelPointer, convert_type(res_type, true, spv::StorageClassImage))
		.add(args[0].base)
		.add(args[1].base)
		.add(ms_sample);
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicSMax, convert_type(res_type))
		.add(texel)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[2].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicMax, 3, {
	const spv::Id ms_sample = emit_constant(0u);

	const spv::Id texel = add_instruction(spv::OpImageTexelPointer, convert_type(res_type, true, spv::StorageClassImage))
		.add(args[0].base)
		.add(args[1].base)
		.add(ms_sample);
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicUMax, convert_type(res_type))
		.add(texel)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[2].base);
	})

// ret atomicExchange(inout mem, data)
DEFINE_INTRINSIC(atomicExchange, 0, int, inout_int, int)
DEFINE_INTRINSIC(atomicExchange, 0, uint, inout_uint, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicExchange, 0, {
	code += "atomicExchange(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicExchange, 0, {
	if (_shader_model >= 50)
		code += "0; InterlockedExchange(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(res) + ')';
	else
		code += id_to_name(args[0].base) + "; " + id_to_name(args[0].base) + " = " + id_to_name(args[1].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicExchange, 0, {
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicExchange, convert_type(res_type))
		.add(args[0].base)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[1].base);
	})
// ret atomicExchange(s, coords, data)
DEFINE_INTRINSIC(atomicExchange, 1, int, inout_storage1d_int, int, int)
DEFINE_INTRINSIC(atomicExchange, 1, int, inout_storage2d_int, int2, int)
DEFINE_INTRINSIC(atomicExchange, 1, int, inout_storage3d_int, int3, int)
DEFINE_INTRINSIC(atomicExchange, 1, uint, inout_storage1d_uint, int, uint)
DEFINE_INTRINSIC(atomicExchange, 1, uint, inout_storage2d_uint, int2, uint)
DEFINE_INTRINSIC(atomicExchange, 1, uint, inout_storage3d_uint, int3, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicExchange, 1, {
	code += "imageAtomicExchange(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicExchange, 1, {
	if (_shader_model >= 50)
		code += "0; InterlockedExchange(" + id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + ']' + ", " + id_to_name(args[2].base) + ", " + id_to_name(res) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicExchange, 1, {
	const spv::Id ms_sample = emit_constant(0u);

	const spv::Id texel = add_instruction(spv::OpImageTexelPointer, convert_type(res_type, true, spv::StorageClassImage))
		.add(args[0].base)
		.add(args[1].base)
		.add(ms_sample);
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicExchange, convert_type(res_type))
		.add(texel)
		.add(mem_scope)
		.add(mem_semantics)
		.add(args[2].base);
	})

// ret atomicCompareExchange(inout mem, compare, data)
DEFINE_INTRINSIC(atomicCompareExchange, 0, int, inout_int, int, int)
DEFINE_INTRINSIC(atomicCompareExchange, 0, uint, inout_uint, uint, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicCompareExchange, 0, {
	code += "atomicCompSwap(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicCompareExchange, 0, {
	if (_shader_model >= 50)
		code += "0; InterlockedCompareExchange(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(res) + ')';
	else
		code += id_to_name(args[0].base) + "; if (" + id_to_name(args[0].base) + " == " + id_to_name(args[1].base) + ") " + id_to_name(args[0].base) + " = " + id_to_name(args[2].base);
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicCompareExchange, 0, {
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicCompareExchange, convert_type(res_type))
		.add(args[0].base)
		.add(mem_scope)
		.add(mem_semantics)
		.add(mem_semantics)
		.add(args[2].base)
		.add(args[1].base);
	})
// ret atomicCompareExchange(s, coords, compare, data)
DEFINE_INTRINSIC(atomicCompareExchange, 1, int, inout_storage1d_int, int, int, int)
DEFINE_INTRINSIC(atomicCompareExchange, 1, int, inout_storage2d_int, int2, int, int)
DEFINE_INTRINSIC(atomicCompareExchange, 1, int, inout_storage3d_int, int3, int, int)
DEFINE_INTRINSIC(atomicCompareExchange, 1, uint, inout_storage1d_uint, int, uint, uint)
DEFINE_INTRINSIC(atomicCompareExchange, 1, uint, inout_storage2d_uint, int2, uint, uint)
DEFINE_INTRINSIC(atomicCompareExchange, 1, uint, inout_storage3d_uint, int3, uint, uint)
IMPLEMENT_INTRINSIC_GLSL(atomicCompareExchange, 1, {
	code += "imageAtomicCompSwap(" + id_to_name(args[0].base) + ", " + id_to_name(args[1].base) + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ')';
	})
IMPLEMENT_INTRINSIC_HLSL(atomicCompareExchange, 1, {
	if (_shader_model >= 50)
		code += "0; InterlockedCompareExchange(" + id_to_name(args[0].base) + '[' + id_to_name(args[1].base) + ']' + ", " + id_to_name(args[2].base) + ", " + id_to_name(args[3].base) + ", " + id_to_name(res) + ')';
	})
IMPLEMENT_INTRINSIC_SPIRV(atomicCompareExchange, 1, {
	const spv::Id ms_sample = emit_constant(0u);

	const spv::Id texel = add_instruction(spv::OpImageTexelPointer, convert_type(res_type, true, spv::StorageClassImage))
		.add(args[0].base)
		.add(args[1].base)
		.add(ms_sample);
	const spv::Id mem_scope = emit_constant(spv::ScopeDevice);
	const spv::Id mem_semantics = emit_constant(spv::MemorySemanticsMaskNone);

	return
	add_instruction(spv::OpAtomicCompareExchange, convert_type(res_type))
		.add(texel)
		.add(mem_scope)
		.add(mem_semantics)
		.add(mem_semantics)
		.add(args[3].base)
		.add(args[2].base);
	})

#undef DEFINE_INTRINSIC
#undef IMPLEMENT_INTRINSIC_GLSL
#undef IMPLEMENT_INTRINSIC_HLSL
#undef IMPLEMENT_INTRINSIC_SPIRV
