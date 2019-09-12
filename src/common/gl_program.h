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

  bool IsVaild() const { return m_program_id != 0; }

  bool Compile(const char* vertex_shader, const char* fragment_shader);

  void BindAttribute(GLuint index, const char* name);
  void BindDefaultAttributes();

  void BindFragData(GLuint index = 0, const char* name = "ocol0");

  bool Link();

  void Bind();

  void Destroy();

  u32 RegisterUniform(const char* name);
  void Uniform1ui(u32 index, u32 value);
  void Uniform4f(u32 index, float x, float y, float z, float w);

private:
  GLuint m_program_id = 0;
  GLuint m_vertex_shader_id = 0;
  GLuint m_fragment_shader_id = 0;

  std::vector<GLint> m_uniform_locations;
};

} // namespace GL