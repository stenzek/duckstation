#include "gl_program.h"
#include "YBaseLib/Log.h"
#include "YBaseLib/String.h"
#include <array>
#include <fstream>
Log_SetChannel(GL);

static u32 s_next_bad_shader_id = 1;

namespace GL {

Program::Program() = default;

Program::~Program()
{
  Destroy();
}

GLuint Program::CompileShader(GLenum type, const char* source)
{
  GLuint id = glCreateShader(type);

  std::array<const GLchar*, 1> sources = {{source}};
  std::array<GLint, 1> source_lengths = {{static_cast<GLint>(std::strlen(source))}};
  glShaderSource(id, static_cast<GLsizei>(sources.size()), sources.data(), source_lengths.data());
  glCompileShader(id);

  GLint status = GL_FALSE;
  glGetShaderiv(id, GL_COMPILE_STATUS, &status);

  GLint info_log_length = 0;
  glGetShaderiv(id, GL_INFO_LOG_LENGTH, &info_log_length);

  if (status == GL_FALSE || info_log_length > 0)
  {
    std::string info_log;
    info_log.resize(info_log_length + 1);
    glGetShaderInfoLog(id, info_log_length, &info_log_length, &info_log[0]);

    if (status == GL_TRUE)
    {
      Log_ErrorPrintf("Shader compiled with warnings:\n%s", info_log.c_str());
    }
    else
    {
      Log_ErrorPrintf("Shader failed to compile:\n%s", info_log.c_str());

      std::ofstream ofs(SmallString::FromFormat("bad_shader_%u.txt", s_next_bad_shader_id++),
                        std::ofstream::out | std::ofstream::binary);
      if (ofs.is_open())
      {
        ofs.write(sources[0], source_lengths[0]);
        ofs << "\n\nCompile failed, info log:\n";
        ofs << info_log;
        ofs.close();
      }

      glDeleteShader(id);
      return 0;
    }
  }

  return id;
}

bool Program::Compile(const char* vertex_shader, const char* fragment_shader)
{
  GLuint vertex_shader_id = CompileShader(GL_VERTEX_SHADER, vertex_shader);
  if (vertex_shader_id == 0)
    return false;

  GLuint fragment_shader_id = CompileShader(GL_FRAGMENT_SHADER, fragment_shader);
  if (fragment_shader_id == 0)
  {
    glDeleteShader(vertex_shader_id);
    return false;
  }

  m_program_id = glCreateProgram();
  glAttachShader(m_program_id, vertex_shader_id);
  glAttachShader(m_program_id, fragment_shader_id);
  return true;
}

void Program::BindAttribute(GLuint index, const char* name)
{
  glBindAttribLocation(m_program_id, index, name);
}

void Program::BindDefaultAttributes()
{
  BindAttribute(0, "a_position");
  BindAttribute(1, "a_texcoord");
  BindAttribute(2, "a_color");
}

void Program::BindFragData(GLuint index /*= 0*/, const char* name /*= "o_col0"*/)
{
  glBindFragDataLocation(m_program_id, index, name);
}

bool Program::Link()
{
  glLinkProgram(m_program_id);

  glDeleteShader(m_vertex_shader_id);
  m_vertex_shader_id = 0;
  glDeleteShader(m_fragment_shader_id);
  m_fragment_shader_id = 0;

  GLint status = GL_FALSE;
  glGetProgramiv(m_program_id, GL_LINK_STATUS, &status);

  GLint info_log_length = 0;
  glGetProgramiv(m_program_id, GL_INFO_LOG_LENGTH, &info_log_length);

  if (status == GL_FALSE || info_log_length > 0)
  {
    std::string info_log;
    info_log.resize(info_log_length + 1);
    glGetProgramInfoLog(m_program_id, info_log_length, &info_log_length, &info_log[0]);

    if (status == GL_TRUE)
    {
      Log_ErrorPrintf("Program linked with warnings:\n%s", info_log.c_str());
    }
    else
    {
      Log_ErrorPrintf("Program failed to link:\n%s", info_log.c_str());
      glDeleteProgram(m_program_id);
      m_program_id = 0;
      return false;
    }
  }

  return true;
}

void Program::Bind() const
{
  glUseProgram(m_program_id);
}

void Program::Destroy()
{
  if (m_vertex_shader_id != 0)
  {
    glDeleteShader(m_vertex_shader_id);
    m_vertex_shader_id = 0;
  }
  if (m_fragment_shader_id != 0)
  {
    glDeleteShader(m_fragment_shader_id);
    m_fragment_shader_id = 0;
  }
  if (m_program_id != 0)
  {
    glDeleteProgram(m_program_id);
    m_program_id = 0;
  }
}

u32 Program::RegisterUniform(const char* name)
{
  u32 id = static_cast<u32>(m_uniform_locations.size());
  m_uniform_locations.push_back(glGetUniformLocation(m_program_id, name));
  return id;
}

void Program::Uniform1ui(u32 index, u32 x) const
{
  Assert(index < m_uniform_locations.size());
  const int location = m_uniform_locations[index];
  if (location >= 0)
    glUniform1ui(location, x);
}

void Program::Uniform2ui(u32 index, u32 x, u32 y) const
{
  Assert(index < m_uniform_locations.size());
  const int location = m_uniform_locations[index];
  if (location >= 0)
    glUniform2ui(location, x, y);
}

void Program::Uniform3ui(u32 index, u32 x, u32 y, u32 z) const
{
  Assert(index < m_uniform_locations.size());
  const int location = m_uniform_locations[index];
  if (location >= 0)
    glUniform3ui(location, x, y, z);
}

void Program::Uniform4ui(u32 index, u32 x, u32 y, u32 z, u32 w) const
{
  Assert(index < m_uniform_locations.size());
  const int location = m_uniform_locations[index];
  if (location >= 0)
    glUniform4ui(location, x, y, z, w);
}

void Program::Uniform1i(u32 index, s32 x) const
{
  Assert(index < m_uniform_locations.size());
  const int location = m_uniform_locations[index];
  if (location >= 0)
    glUniform1i(location, x);
}

void Program::Uniform2i(u32 index, s32 x, s32 y) const
{
  Assert(index < m_uniform_locations.size());
  const int location = m_uniform_locations[index];
  if (location >= 0)
    glUniform2i(location, x, y);
}

void Program::Uniform3i(u32 index, s32 x, s32 y, s32 z) const
{
  Assert(index < m_uniform_locations.size());
  const int location = m_uniform_locations[index];
  if (location >= 0)
    glUniform3i(location, x, y, z);
}

void Program::Uniform4i(u32 index, s32 x, s32 y, s32 z, s32 w) const
{
  Assert(index < m_uniform_locations.size());
  const int location = m_uniform_locations[index];
  if (location >= 0)
    glUniform4i(location, x, y, z, w);
}

void Program::Uniform1f(u32 index, float x) const
{
  Assert(index < m_uniform_locations.size());
  const int location = m_uniform_locations[index];
  if (location >= 0)
    glUniform1f(location, x);
}

void Program::Uniform2f(u32 index, float x, float y) const
{
  Assert(index < m_uniform_locations.size());
  const int location = m_uniform_locations[index];
  if (location >= 0)
    glUniform2f(location, x, y);
}

void Program::Uniform3f(u32 index, float x, float y, float z) const
{
  Assert(index < m_uniform_locations.size());
  const int location = m_uniform_locations[index];
  if (location >= 0)
    glUniform3f(location, x, y, z);
}

void Program::Uniform4f(u32 index, float x, float y, float z, float w) const
{
  Assert(index < m_uniform_locations.size());
  const int location = m_uniform_locations[index];
  if (location >= 0)
    glUniform4f(location, x, y, z, w);
}

} // namespace GL