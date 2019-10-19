#pragma once
#include "glad.h"
#include "types.h"
#include <vector>

namespace GL {
class Program
{
public:
  Program();
  ~Program();

  static GLuint CompileShader(GLenum type, const char* source);
  static void ResetLastProgram();

  bool IsVaild() const { return m_program_id != 0; }

  bool Compile(const char* vertex_shader, const char* fragment_shader);

  void BindAttribute(GLuint index, const char* name);
  void BindDefaultAttributes();

  void BindFragData(GLuint index = 0, const char* name = "o_col0");
  void BindFragDataIndexed(GLuint color_number = 0, const char* name = "o_col0");

  bool Link();

  void Bind() const;

  void Destroy();

  u32 RegisterUniform(const char* name);
  void Uniform1ui(u32 index, u32 x) const;
  void Uniform2ui(u32 index, u32 x, u32 y) const;
  void Uniform3ui(u32 index, u32 x, u32 y, u32 z) const;
  void Uniform4ui(u32 index, u32 x, u32 y, u32 z, u32 w) const;
  void Uniform1i(u32 index, s32 x) const;
  void Uniform2i(u32 index, s32 x, s32 y) const;
  void Uniform3i(u32 index, s32 x, s32 y, s32 z) const;
  void Uniform4i(u32 index, s32 x, s32 y, s32 z, s32 w) const;
  void Uniform1f(u32 index, float x) const;
  void Uniform2f(u32 index, float x, float y) const;
  void Uniform3f(u32 index, float x, float y, float z) const;
  void Uniform4f(u32 index, float x, float y, float z, float w) const;
  void Uniform2uiv(u32 index, const u32* v) const;
  void Uniform3uiv(u32 index, const u32* v) const;
  void Uniform4uiv(u32 index, const u32* v) const;
  void Uniform2iv(u32 index, const s32* v) const;
  void Uniform3iv(u32 index, const s32* v) const;
  void Uniform4iv(u32 index, const s32* v) const;
  void Uniform2fv(u32 index, const float* v) const;
  void Uniform3fv(u32 index, const float* v) const;
  void Uniform4fv(u32 index, const float* v) const;

private:
  GLuint m_program_id = 0;
  GLuint m_vertex_shader_id = 0;
  GLuint m_fragment_shader_id = 0;

  std::vector<GLint> m_uniform_locations;
};

} // namespace GL