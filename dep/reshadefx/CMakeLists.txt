add_library(reshadefx
  include/effect_codegen.hpp
  include/effect_expression.hpp
  include/effect_lexer.hpp
  include/effect_module.hpp
  include/effect_parser.hpp
  include/effect_preprocessor.hpp
  include/effect_symbol_table.hpp
  include/effect_token.hpp
  src/effect_codegen_glsl.cpp
  src/effect_codegen_hlsl.cpp
  src/effect_codegen_spirv.cpp
  src/effect_expression.cpp
  src/effect_lexer.cpp
  src/effect_parser_exp.cpp
  src/effect_parser_stmt.cpp
  src/effect_preprocessor.cpp
  src/effect_symbol_table.cpp
)

target_include_directories(reshadefx PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_include_directories(reshadefx INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# reshadefx is not C++20-compatible.
set_property(TARGET reshadefx PROPERTY CXX_STANDARD 17)
