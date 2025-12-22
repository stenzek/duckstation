// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "imgui_manager.h"
#include "translation.h"

#include "core/settings.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"

#include "cubeb/cubeb.h"
#include "fmt/format.h"

LOG_CHANNEL(AudioStream);

namespace {

class CubebAudioStream final : public AudioStream
{
public:
  CubebAudioStream();
  ~CubebAudioStream() override;

  bool Initialize(u32 sample_rate, u32 channels, u32 output_latency_frames, bool output_latency_minimal,
                  const char* driver_name, const char* device_name, AudioStreamSource* source, bool auto_start,
                  Error* error);

  bool Start(Error* error) override;
  bool Stop(Error* error) override;

private:
  static void LogCallback(const char* fmt, ...);
  static long DataCallback(cubeb_stream* stm, void* user_ptr, const void* input_buffer, void* output_buffer,
                           long nframes);
  static void StateCallback(cubeb_stream* stream, void* user_ptr, cubeb_state state);

  cubeb* m_context = nullptr;
  cubeb_stream* stream = nullptr;
};
} // namespace

static void FormatCubebError(Error* error, const char* prefix, int rv)
{
  const char* str;
  switch (rv)
  {
    // clang-format off
#define C(e) case e: str = #e; break
    // clang-format on

    C(CUBEB_OK);
    C(CUBEB_ERROR);
    C(CUBEB_ERROR_INVALID_FORMAT);
    C(CUBEB_ERROR_INVALID_PARAMETER);
    C(CUBEB_ERROR_NOT_SUPPORTED);
    C(CUBEB_ERROR_DEVICE_UNAVAILABLE);

    default:
      str = "CUBEB_ERROR_UNKNOWN";
      break;

#undef C
  }

  Error::SetStringFmt(error, "{}: {} ({})", prefix, str, rv);
}

CubebAudioStream::CubebAudioStream() = default;

CubebAudioStream::~CubebAudioStream()
{
  if (stream)
  {
    cubeb_stream_stop(stream);
    cubeb_stream_destroy(stream);
    stream = nullptr;
  }

  if (m_context)
  {
    cubeb_destroy(m_context);
    m_context = nullptr;
  }
}

void CubebAudioStream::LogCallback(const char* fmt, ...)
{
  LargeString str;
  std::va_list ap;
  va_start(ap, fmt);
  str.vsprintf(fmt, ap);
  va_end(ap);
  DEV_LOG(str);
}

bool CubebAudioStream::Initialize(u32 sample_rate, u32 channels, u32 output_latency_frames, bool output_latency_minimal,
                                  const char* driver_name, const char* device_name, AudioStreamSource* source,
                                  bool auto_start, Error* error)
{
  cubeb_set_log_callback(CUBEB_LOG_NORMAL, LogCallback);

  int rv = cubeb_init(&m_context, "DuckStation", (driver_name && *driver_name != '\0') ? driver_name : nullptr);
  if (rv != CUBEB_OK)
  {
    FormatCubebError(error, "Could not initialize cubeb context: ", rv);
    return false;
  }

  cubeb_stream_params params = {};
  params.format = CUBEB_SAMPLE_S16LE;
  params.rate = sample_rate;
  params.channels = channels;
  params.layout = CUBEB_LAYOUT_STEREO;
  params.prefs = CUBEB_STREAM_PREF_NONE;

  u32 min_latency_frames = 0;
  rv = cubeb_get_min_latency(m_context, &params, &min_latency_frames);
  if (rv == CUBEB_ERROR_NOT_SUPPORTED)
  {
    DEV_LOG("Cubeb backend does not support latency queries, using latency of {} ms ({} frames).",
            FramesToMS(sample_rate, output_latency_frames), output_latency_frames);
  }
  else
  {
    if (rv != CUBEB_OK)
    {
      FormatCubebError(error, "cubeb_get_min_latency() failed: {}", rv);
      return false;
    }

    if (output_latency_minimal)
    {
      // use minimum
      output_latency_frames = min_latency_frames;
    }
    else if (min_latency_frames > output_latency_frames)
    {
      WARNING_LOG("Minimum latency is above requested latency: {} vs {}, adjusting to compensate.", min_latency_frames,
                  output_latency_frames);
      output_latency_frames = min_latency_frames;
    }
  }

  DEV_LOG("Output latency: {} ms ({} audio frames)", FramesToMS(sample_rate, output_latency_frames),
          min_latency_frames);

  cubeb_devid selected_device = nullptr;
  cubeb_device_collection devices;
  bool devices_valid = false;
  if (device_name && *device_name != '\0')
  {
    rv = cubeb_enumerate_devices(m_context, CUBEB_DEVICE_TYPE_OUTPUT, &devices);
    devices_valid = (rv == CUBEB_OK);
    if (rv == CUBEB_OK)
    {
      for (size_t i = 0; i < devices.count; i++)
      {
        const cubeb_device_info& di = devices.device[i];
        if (di.device_id && std::strcmp(device_name, di.device_id) == 0)
        {
          INFO_LOG("Using output device '{}' ({}).", di.device_id, di.friendly_name ? di.friendly_name : di.device_id);
          selected_device = di.devid;
          break;
        }
      }

      if (!selected_device)
      {
        Host::AddOSDMessage(OSDMessageType::Error,
                            fmt::format("Requested audio output device '{}' not found, using default.", device_name));
      }
    }
    else
    {
      Error enumerate_error;
      FormatCubebError(&enumerate_error, "cubeb_enumerate_devices() failed: ", rv);
      WARNING_LOG("{}, using default device.", enumerate_error.GetDescription());
    }
  }

  char stream_name[32];
  std::snprintf(stream_name, sizeof(stream_name), "%p", this);

  rv = cubeb_stream_init(m_context, &stream, stream_name, nullptr, nullptr, selected_device, &params,
                         output_latency_frames, &CubebAudioStream::DataCallback, StateCallback, source);

  if (devices_valid)
    cubeb_device_collection_destroy(m_context, &devices);

  if (rv != CUBEB_OK)
  {
    FormatCubebError(error, "cubeb_stream_init() failed: ", rv);
    return false;
  }

  if (auto_start)
  {
    rv = cubeb_stream_start(stream);
    if (rv != CUBEB_OK)
    {
      FormatCubebError(error, "cubeb_stream_start() failed: ", rv);
      return false;
    }
  }

  return true;
}

void CubebAudioStream::StateCallback(cubeb_stream* stream, void* user_ptr, cubeb_state state)
{
  // noop
}

long CubebAudioStream::DataCallback(cubeb_stream* stm, void* user_ptr, const void* input_buffer, void* output_buffer,
                                    long nframes)
{
  static_cast<AudioStreamSource*>(user_ptr)->ReadFrames(static_cast<s16*>(output_buffer), static_cast<u32>(nframes));
  return nframes;
}

bool CubebAudioStream::Start(Error* error)
{
  const int rv = cubeb_stream_start(stream);
  if (rv != CUBEB_OK)
  {
    FormatCubebError(error, "cubeb_stream_start() failed: ", rv);
    return false;
  }

  return true;
}

bool CubebAudioStream::Stop(Error* error)
{
  const int rv = cubeb_stream_stop(stream);
  if (rv != CUBEB_OK)
  {
    FormatCubebError(error, "cubeb_stream_stop() failed: ", rv);
    return false;
  }

  return true;
}

std::unique_ptr<AudioStream> AudioStream::CreateCubebAudioStream(u32 sample_rate, u32 channels,
                                                                 u32 output_latency_frames, bool output_latency_minimal,
                                                                 const char* driver_name, const char* device_name,
                                                                 AudioStreamSource* source, bool auto_start,
                                                                 Error* error)
{
  std::unique_ptr<CubebAudioStream> stream = std::make_unique<CubebAudioStream>();
  if (!stream->Initialize(sample_rate, channels, output_latency_frames, output_latency_minimal, driver_name,
                          device_name, source, auto_start, error))
  {
    stream.reset();
  }

  return stream;
}

std::vector<std::pair<std::string, std::string>> AudioStream::GetCubebDriverNames()
{
  std::vector<std::pair<std::string, std::string>> names;
  names.emplace_back(std::string(), TRANSLATE_STR("AudioStream", "Default"));

  const char** cubeb_names = cubeb_get_backend_names();
  for (u32 i = 0; cubeb_names[i] != nullptr; i++)
    names.emplace_back(cubeb_names[i], cubeb_names[i]);
  return names;
}

std::vector<AudioStream::DeviceInfo> AudioStream::GetCubebOutputDevices(const char* driver, u32 sample_rate)
{
  Error error;

  std::vector<AudioStream::DeviceInfo> ret;
  ret.emplace_back(std::string(), TRANSLATE_STR("AudioStream", "Default"), 0);

  cubeb* context;
  int rv = cubeb_init(&context, "DuckStation", (driver && *driver) ? driver : nullptr);
  if (rv != CUBEB_OK)
  {
    FormatCubebError(&error, "cubeb_init() failed: ", rv);
    ERROR_LOG(error.GetDescription());
    return ret;
  }

  ScopedGuard context_cleanup([context]() { cubeb_destroy(context); });

  cubeb_device_collection devices;
  rv = cubeb_enumerate_devices(context, CUBEB_DEVICE_TYPE_OUTPUT, &devices);
  if (rv != CUBEB_OK)
  {
    FormatCubebError(&error, "cubeb_enumerate_devices() failed: ", rv);
    ERROR_LOG(error.GetDescription());
    return ret;
  }

  ScopedGuard devices_cleanup([context, &devices]() { cubeb_device_collection_destroy(context, &devices); });

  // we need stream parameters to query latency
  cubeb_stream_params params = {};
  params.format = CUBEB_SAMPLE_S16LE;
  params.rate = sample_rate;
  params.channels = 2;
  params.layout = CUBEB_LAYOUT_UNDEFINED;
  params.prefs = CUBEB_STREAM_PREF_NONE;

  u32 min_latency = 0;
  cubeb_get_min_latency(context, &params, &min_latency);
  ret[0].minimum_latency_frames = min_latency;

  for (size_t i = 0; i < devices.count; i++)
  {
    const cubeb_device_info& di = devices.device[i];
    if (!di.device_id)
      continue;

    ret.emplace_back(di.device_id, di.friendly_name ? di.friendly_name : di.device_id, min_latency);
  }

  return ret;
}
