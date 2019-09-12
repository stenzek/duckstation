#include "gl_program.h"
#include "YBaseLib/Log.h"
#include <array>
Log_SetChannel(GL);

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

void Program::BindFragData(GLuint index /*= 0*/, const char* name /*= "ocol0"*/)
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

void Program::Bind()
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

void Program::Uniform1ui(u32 index, u32 value)
{
  Assert(index < m_uniform_locations.size());
  const int location = m_uniform_locations[index];
  if (location >= 0)
    glUniform1ui(location, value);
}

void Program::Uniform4f(u32 index, float x, float y, float z, float w)
{
  Assert(index < m_uniform_locations.size());
  const int location = m_uniform_locations[index];
  if (location >= 0)
    glUniform4f(location, x, y, z, w);
}

} // namespace GL