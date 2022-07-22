#pragma once
#include "../types.h"
#include "loader.h"
#include <string_view>
#include <vector>

namespace GL {
class Program
{
public:
  Program();
  Program(const Program&) = delete;
  Program(Program&& prog);
  ~Program();

  static GLuint CompileShader(GLenum type, const std::string_view source);
  static void ResetLastProgram();

  bool IsVaild() const { return m_program_id != 0; }
  bool IsBound() const { return s_last_program_id == m_program_id; }

  bool Compile(const std::string_view vertex_shader, const std::string_view geometry_shader,
               const std::string_view fragment_shader);

  bool CreateFromBinary(const void* data, u32 data_length, u32 data_format);

  bool GetBinary(std::vector<u8>* out_data, u32* out_data_format);
  void SetBinaryRetrievableHint();

  void BindAttribute(GLuint index, const char* name);
  void BindDefaultAttributes();

  void BindFragData(GLuint index = 0, const char* name = "o_col0");
  void BindFragDataIndexed(GLuint color_number = 0, const char* name = "o_col0");

  bool Link();

  void Bind() const;

  void Destroy();

  int RegisterUniform(const char* name);
  void Uniform1ui(int index, u32 x) const;
  void Uniform2ui(int index, u32 x, u32 y) const;
  void Uniform3ui(int index, u32 x, u32 y, u32 z) const;
  void Uniform4ui(int index, u32 x, u32 y, u32 z, u32 w) const;
  void Uniform1i(int index, s32 x) const;
  void Uniform2i(int index, s32 x, s32 y) const;
  void Uniform3i(int index, s32 x, s32 y, s32 z) const;
  void Uniform4i(int index, s32 x, s32 y, s32 z, s32 w) const;
  void Uniform1f(int index, float x) const;
  void Uniform2f(int index, float x, float y) const;
  void Uniform3f(int index, float x, float y, float z) const;
  void Uniform4f(int index, float x, float y, float z, float w) const;
  void Uniform2uiv(int index, const u32* v) const;
  void Uniform3uiv(int index, const u32* v) const;
  void Uniform4uiv(int index, const u32* v) const;
  void Uniform2iv(int index, const s32* v) const;
  void Uniform3iv(int index, const s32* v) const;
  void Uniform4iv(int index, const s32* v) const;
  void Uniform2fv(int index, const float* v) const;
  void Uniform3fv(int index, const float* v) const;
  void Uniform4fv(int index, const float* v) const;

  void Uniform1ui(const char* name, u32 x) const;
  void Uniform2ui(const char* name, u32 x, u32 y) const;
  void Uniform3ui(const char* name, u32 x, u32 y, u32 z) const;
  void Uniform4ui(const char* name, u32 x, u32 y, u32 z, u32 w) const;
  void Uniform1i(const char* name, s32 x) const;
  void Uniform2i(const char* name, s32 x, s32 y) const;
  void Uniform3i(const char* name, s32 x, s32 y, s32 z) const;
  void Uniform4i(const char* name, s32 x, s32 y, s32 z, s32 w) const;
  void Uniform1f(const char* name, float x) const;
  void Uniform2f(const char* name, float x, float y) const;
  void Uniform3f(const char* name, float x, float y, float z) const;
  void Uniform4f(const char* name, float x, float y, float z, float w) const;
  void Uniform2uiv(const char* name, const u32* v) const;
  void Uniform3uiv(const char* name, const u32* v) const;
  void Uniform4uiv(const char* name, const u32* v) const;
  void Uniform2iv(const char* name, const s32* v) const;
  void Uniform3iv(const char* name, const s32* v) const;
  void Uniform4iv(const char* name, const s32* v) const;
  void Uniform2fv(const char* name, const float* v) const;
  void Uniform3fv(const char* name, const float* v) const;
  void Uniform4fv(const char* name, const float* v) const;

  void BindUniformBlock(const char* name, u32 index);

  Program& operator=(const Program&) = delete;
  Program& operator=(Program&& prog);

private:
  static u32 s_last_program_id;

  GLuint m_program_id = 0;
  GLuint m_vertex_shader_id = 0;
  GLuint m_fragment_shader_id = 0;

  std::vector<GLint> m_uniform_locations;
};

} // namespace GL