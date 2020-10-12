#include "state_wrapper.h"
#include "log.h"
#include "string.h"
#include <cinttypes>
#include <cstring>
Log_SetChannel(StateWrapper);

StateWrapper::StateWrapper(ByteStream* stream, Mode mode, u32 version)
  : m_stream(stream), m_mode(mode), m_version(version)
{
}

StateWrapper::~StateWrapper() = default;

void StateWrapper::DoBytes(void* data, size_t length)
{
  if (m_mode == Mode::Read)
  {
    if (m_error || (m_error |= !m_stream->Read2(data, static_cast<u32>(length))) == true)
      std::memset(data, 0, length);
  }
  else
  {
    if (!m_error)
      m_error |= !m_stream->Write2(data, static_cast<u32>(length));
  }
}

void StateWrapper::Do(bool* value_ptr)
{
  if (m_mode == Mode::Read)
  {
    u8 value = 0;
    if (!m_error)
      m_error |= !m_stream->ReadByte(&value);
    *value_ptr = (value != 0);
  }
  else
  {
    u8 value = static_cast<u8>(*value_ptr);
    if (!m_error)
      m_error |= !m_stream->WriteByte(value);
  }
}

void StateWrapper::Do(std::string* value_ptr)
{
  u32 length = static_cast<u32>(value_ptr->length());
  Do(&length);
  if (m_mode == Mode::Read)
    value_ptr->resize(length);
  DoBytes(&(*value_ptr)[0], length);
  value_ptr->resize(std::strlen(&(*value_ptr)[0]));
}

void StateWrapper::Do(String* value_ptr)
{
  u32 length = static_cast<u32>(value_ptr->GetLength());
  Do(&length);
  if (m_mode == Mode::Read)
    value_ptr->Resize(length);
  DoBytes(value_ptr->GetWriteableCharArray(), length);
  value_ptr->UpdateSize();
}

bool StateWrapper::DoMarker(const char* marker)
{
  SmallString file_value(marker);
  Do(&file_value);
  if (m_error)
    return false;

  if (m_mode == Mode::Write || file_value.Compare(marker))
    return true;

  Log_ErrorPrintf("Marker mismatch at offset %" PRIu64 ": found '%s' expected '%s'", m_stream->GetPosition(),
                  file_value.GetCharArray(), marker);

  return false;
}
