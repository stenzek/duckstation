// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "state_wrapper.h"
#include "common/log.h"
#include "common/small_string.h"
#include <cinttypes>
#include <cstring>
Log_SetChannel(StateWrapper);

StateWrapper::StateWrapper(std::span<u8> data, Mode mode, u32 version)
  : m_data(data.data()), m_size(data.size()), m_mode(mode), m_version(version)
{
}

StateWrapper::StateWrapper(std::span<const u8> data, Mode mode, u32 version)
  : m_data(const_cast<u8*>(data.data())), m_size(data.size()), m_mode(mode), m_version(version)
{
  Assert(mode == Mode::Read);
}

StateWrapper::~StateWrapper() = default;

void StateWrapper::DoBytes(void* data, size_t length)
{
  if (m_mode == Mode::Read)
  {
    if (!ReadData(data, length))
      std::memset(data, 0, length);
  }
  else
  {
    WriteData(data, length);
  }
}

void StateWrapper::DoBytesEx(void* data, size_t length, u32 version_introduced, const void* default_value)
{
  if (m_mode == Mode::Read && m_version < version_introduced)
  {
    std::memcpy(data, default_value, length);
    return;
  }

  DoBytes(data, length);
}

void StateWrapper::Do(bool* value_ptr)
{
  if (m_mode == Mode::Read)
  {
    u8 value = 0;
    if (!(m_error = m_error || (m_pos + 1) > m_size)) [[likely]]
      value = m_data[m_pos++];
    *value_ptr = (value != 0);
  }
  else
  {
    if (!(m_error = m_error || (m_pos + 1) > m_size)) [[likely]]
      m_data[m_pos++] = static_cast<u8>(*value_ptr);
  }
}

void StateWrapper::Do(std::string* value_ptr)
{
  u32 length = static_cast<u32>(value_ptr->length());
  Do(&length);
  if (m_mode == Mode::Read)
  {
    if ((m_error = (m_error || ((m_pos + length) > m_size)))) [[unlikely]]
      return;
    value_ptr->resize(length);
  }
  DoBytes(&(*value_ptr)[0], length);
  value_ptr->resize(std::strlen(&(*value_ptr)[0]));
}

void StateWrapper::Do(SmallStringBase* value_ptr)
{
  u32 length = static_cast<u32>(value_ptr->length());
  Do(&length);
  if (m_mode == Mode::Read)
  {
    if ((m_error = (m_error || ((m_pos + length) > m_size)))) [[unlikely]]
      return;
    value_ptr->resize(length);
  }
  DoBytes(value_ptr->data(), length);
  value_ptr->update_size();
}

void StateWrapper::Do(std::string_view* value_ptr)
{
  Assert(m_mode == Mode::Write);
  u32 length = static_cast<u32>(value_ptr->length());
  Do(&length);
  DoBytes(const_cast<char*>(value_ptr->data()), length);
}

bool StateWrapper::DoMarker(const char* marker)
{
  SmallString file_value(marker);
  Do(&file_value);
  if (m_error)
    return false;

  if (m_mode == Mode::Write || file_value.equals(marker))
    return true;

  ERROR_LOG("Marker mismatch at offset {}: found '{}' expected '{}'", m_pos, file_value, marker);
  return false;
}

bool StateWrapper::ReadData(void* buf, size_t size)
{
  if ((m_error = (m_error || (m_pos + size) > m_size))) [[unlikely]]
    return false;

  std::memcpy(buf, &m_data[m_pos], size);
  m_pos += size;
  return true;
}

bool StateWrapper::WriteData(const void* buf, size_t size)
{
  if ((m_error = (m_error || (m_pos + size) > m_size))) [[unlikely]]
    return false;

  std::memcpy(&m_data[m_pos], buf, size);
  m_pos += size;
  return true;
}
