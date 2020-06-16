set(SRCS
	glslang/CInterface/glslang_c_interface.cpp
	glslang/GenericCodeGen/CodeGen.cpp
	glslang/GenericCodeGen/Link.cpp
	glslang/MachineIndependent/attribute.cpp
	glslang/MachineIndependent/Constant.cpp
	glslang/MachineIndependent/glslang_tab.cpp
	glslang/MachineIndependent/InfoSink.cpp
	glslang/MachineIndependent/Initialize.cpp
	glslang/MachineIndependent/Intermediate.cpp
	glslang/MachineIndependent/intermOut.cpp
	glslang/MachineIndependent/IntermTraverse.cpp
	glslang/MachineIndependent/iomapper.cpp
	glslang/MachineIndependent/limits.cpp
	glslang/MachineIndependent/linkValidate.cpp
	glslang/MachineIndependent/parseConst.cpp
	glslang/MachineIndependent/ParseContextBase.cpp
	glslang/MachineIndependent/ParseHelper.cpp
	glslang/MachineIndependent/PoolAlloc.cpp
	glslang/MachineIndependent/preprocessor/Pp.cpp
	glslang/MachineIndependent/preprocessor/PpAtom.cpp
	glslang/MachineIndependent/preprocessor/PpContext.cpp
	glslang/MachineIndependent/preprocessor/PpScanner.cpp
	glslang/MachineIndependent/preprocessor/PpTokens.cpp
	glslang/MachineIndependent/propagateNoContraction.cpp
	glslang/MachineIndependent/reflection.cpp
	glslang/MachineIndependent/RemoveTree.cpp
	glslang/MachineIndependent/Scan.cpp
	glslang/MachineIndependent/ShaderLang.cpp
	glslang/MachineIndependent/SymbolTable.cpp
	glslang/MachineIndependent/Versions.cpp
	OGLCompilersDLL/InitializeDll.cpp
	SPIRV/disassemble.cpp
	SPIRV/doc.cpp
	SPIRV/GlslangToSpv.cpp
	SPIRV/InReadableOrder.cpp
	SPIRV/Logger.cpp
	SPIRV/SpvBuilder.cpp
	SPIRV/SpvPostProcess.cpp
	SPIRV/SPVRemapper.cpp
	SPIRV/SpvTools.cpp
	StandAlone/ResourceLimits.cpp
	StandAlone/resource_limits_c.cpp
)

add_library(glslang ${SRCS})

if(WIN32)
  target_sources(glslang PRIVATE glslang/OSDependent/Windows/ossource.cpp)
else()
  target_sources(glslang PRIVATE glslang/OSDependent/Unix/ossource.cpp)
endif()

target_include_directories(glslang PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

