// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "imgui_manager.h"
#include "translation.h"

#include "core/settings.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/string_util.h"

#include "cubeb/cubeb.h"
#include "fmt/format.h"

#include <mutex>
#include <string>

LOG_CHANNEL(AudioStream);

namespace {

struct CubebContextHolder
{
  std::mutex mutex;
  cubeb* context = nullptr;
  u32 reference_count = 0;
  std::string driver_name;
};

class CubebAudioStream final : public AudioStream
{
public:
  CubebAudioStream();
  ~CubebAudioStream() override;

  bool Initialize(u32 sample_rate, u32 channels, u32 output_latency_frames, bool output_latency_minimal,
                  std::string_view driver_name, std::string_view device_name, AudioStreamSource* source,
                  bool auto_start, Error* error);

  bool Start(Error* error) override;
  bool Stop(Error* error) override;

private:
  static long DataCallback(cubeb_stream* stm, void* user_ptr, const void* input_buffer, void* output_buffer,
                           long nframes);
  static void StateCallback(cubeb_stream* stream, void* user_ptr, cubeb_state state);

  cubeb* m_context = nullptr;
  cubeb_stream* stream = nullptr;
};
} // namespace

static CubebContextHolder s_cubeb_context;

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

static void CubebLogCallback(const char* fmt, ...)
{
  if (!Log::IsLogVisible(Log::Level::Dev, Log::Channel::AudioStream))
    return;

  LargeString str;
  std::va_list ap;
  va_start(ap, fmt);
  str.vsprintf(fmt, ap);
  va_end(ap);
  DEV_LOG(str);
}

static cubeb* GetCubebContext(std::string_view driver_name, Error* error)
{
  std::lock_guard<std::mutex> lock(s_cubeb_context.mutex);
  if (s_cubeb_context.context)
  {
    // Check if the requested driver/device matches the existing context.
    if (driver_name != s_cubeb_context.driver_name)
      ERROR_LOG("Cubeb context initialized with driver {}, but requested {}", driver_name, s_cubeb_context.driver_name);

    s_cubeb_context.reference_count++;
    return s_cubeb_context.context;
  }

  Assert(s_cubeb_context.reference_count == 0);

  INFO_LOG("Creating Cubeb context with {} driver...", driver_name.empty() ? std::string_view("default") : driver_name);
  cubeb_set_log_callback(CUBEB_LOG_NORMAL, CubebLogCallback);

  std::string driver_name_str = std::string(driver_name);
  const int rv =
    cubeb_init(&s_cubeb_context.context, "DuckStation", driver_name_str.empty() ? nullptr : driver_name_str.c_str());
  if (rv != CUBEB_OK)
  {
    FormatCubebError(error, "Could not initialize cubeb context: ", rv);
    return nullptr;
  }

  s_cubeb_context.driver_name = std::move(driver_name_str);
  s_cubeb_context.reference_count = 1;
  return s_cubeb_context.context;
}

static void ReleaseCubebContext(cubeb* ctx)
{
  std::lock_guard<std::mutex> lock(s_cubeb_context.mutex);
  AssertMsg(s_cubeb_context.context == ctx, "Cubeb context mismatch on release.");
  Assert(s_cubeb_context.reference_count > 0);
  s_cubeb_context.reference_count--;
  if (s_cubeb_context.reference_count > 0)
    return;

  VERBOSE_LOG("Destroying Cubeb context...");
  cubeb_destroy(s_cubeb_context.context);
  s_cubeb_context.context = nullptr;
  s_cubeb_context.driver_name = {};
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
    ReleaseCubebContext(m_context);
}

bool CubebAudioStream::Initialize(u32 sample_rate, u32 channels, u32 output_latency_frames, bool output_latency_minimal,
                                  std::string_view driver_name, std::string_view device_name, AudioStreamSource* source,
                                  bool auto_start, Error* error)
{
  m_context = GetCubebContext(driver_name, error);
  if (!m_context)
    return false;

  cubeb_stream_params params = {};
  params.format = CUBEB_SAMPLE_S16LE;
  params.rate = sample_rate;
  params.channels = channels;
  params.layout = CUBEB_LAYOUT_STEREO;
  params.prefs = CUBEB_STREAM_PREF_NONE;

  u32 min_latency_frames = 0;
  int rv = cubeb_get_min_latency(m_context, &params, &min_latency_frames);
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
  if (!device_name.empty())
  {
    rv = cubeb_enumerate_devices(m_context, CUBEB_DEVICE_TYPE_OUTPUT, &devices);
    devices_valid = (rv == CUBEB_OK);
    if (rv == CUBEB_OK)
    {
      for (size_t i = 0; i < devices.count; i++)
      {
        const cubeb_device_info& di = devices.device[i];
        if (di.device_id && device_name == di.device_id)
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

  rv =
    cubeb_stream_init(m_context, &stream, stream_name, nullptr, nullptr, selected_device, &params,
                      output_latency_frames, &CubebAudioStream::DataCallback, &CubebAudioStream::StateCallback, source);

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

std::unique_ptr<AudioStream> AudioStream::CreateCubebAudioStream(
  u32 sample_rate, u32 channels, u32 output_latency_frames, bool output_latency_minimal, std::string_view driver_name,
  std::string_view device_name, AudioStreamSource* source, bool auto_start, Error* error)
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

std::vector<AudioStream::DeviceInfo> AudioStream::GetCubebOutputDevices(std::string_view driver, u32 sample_rate)
{
  Error error;

  std::vector<AudioStream::DeviceInfo> ret;
  ret.emplace_back(std::string(), TRANSLATE_STR("AudioStream", "Default"), 0);

  cubeb* context;
  TinyString driver_str(driver);
  int rv = cubeb_init(&context, "DuckStation", driver_str.empty() ? nullptr : driver_str.c_str());
  if (rv != CUBEB_OK)
  {
    FormatCubebError(&error, "cubeb_init() failed: ", rv);
    ERROR_LOG(error.GetDescription());
    return ret;
  }

  cubeb_device_collection devices;
  rv = cubeb_enumerate_devices(context, CUBEB_DEVICE_TYPE_OUTPUT, &devices);
  if (rv != CUBEB_OK)
  {
    FormatCubebError(&error, "cubeb_enumerate_devices() failed: ", rv);
    ERROR_LOG(error.GetDescription());
    cubeb_destroy(context);
    return ret;
  }

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

  cubeb_device_collection_destroy(context, &devices);
  cubeb_destroy(context);
  return ret;
}
