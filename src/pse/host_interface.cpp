#include "host_interface.h"
#include "YBaseLib/ByteStream.h"
#include "system.h"

HostInterface::HostInterface() = default;

HostInterface::~HostInterface() = default;

bool HostInterface::LoadState(const char* filename)
{
  ByteStream* stream;
  if (!ByteStream_OpenFileStream(filename, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED, &stream))
    return false;

  ReportMessage(SmallString::FromFormat("Loading state from %s...", filename));

  const bool result = m_system->LoadState(stream);
  if (!result)
  {
    ReportMessage(SmallString::FromFormat("Loading state from %s failed. Resetting.", filename));
    m_system->Reset();
  }

  stream->Release();
  return result;
}

bool HostInterface::SaveState(const char* filename)
{
  ByteStream* stream;
  if (!ByteStream_OpenFileStream(filename,
                                 BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_WRITE | BYTESTREAM_OPEN_TRUNCATE |
                                   BYTESTREAM_OPEN_ATOMIC_UPDATE | BYTESTREAM_OPEN_STREAMED,
                                 &stream))
  {
    return false;
  }

  const bool result = m_system->SaveState(stream);
  if (!result)
  {
    ReportMessage(SmallString::FromFormat("Saving state to %s failed.", filename));
    stream->Discard();
  }
  else
  {
    ReportMessage(SmallString::FromFormat("State saved to %s.", filename));
    stream->Commit();
  }

  stream->Release();
  return result;
}
