// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "media_capture.h"
#include "gpu_device.h"
#include "host.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/gsvector.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/threading.h"

#include "IconsFontAwesome5.h"
#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include "common/windows_headers.h"

#include <Mferror.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#pragma comment(lib, "mfuuid")
#endif

#ifndef __ANDROID__

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244) // warning C4244: 'return': conversion from 'int' to 'uint8_t', possible loss of data
#endif

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavcodec/version.h"
#include "libavformat/avformat.h"
#include "libavformat/version.h"
#include "libavutil/dict.h"
#include "libavutil/opt.h"
#include "libavutil/version.h"
#include "libswresample/swresample.h"
#include "libswresample/version.h"
#include "libswscale/swscale.h"
#include "libswscale/version.h"
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif

Log_SetChannel(MediaCapture);

namespace {

static constexpr u32 VIDEO_WIDTH_ALIGNMENT = 8;
static constexpr u32 VIDEO_HEIGHT_ALIGNMENT = 8;

class ALIGN_TO_CACHE_LINE MediaCaptureBase : public MediaCapture
{
public:
  static constexpr u32 NUM_FRAMES_IN_FLIGHT = 3;
  static constexpr u32 MAX_PENDING_FRAMES = NUM_FRAMES_IN_FLIGHT * 2;
  static constexpr u32 AUDIO_CHANNELS = 2;

  virtual ~MediaCaptureBase() override;

  bool BeginCapture(float fps, float aspect, u32 width, u32 height, GPUTexture::Format texture_format, u32 sample_rate,
                    std::string path, bool capture_video, std::string_view video_codec, u32 video_bitrate,
                    std::string_view video_codec_args, bool capture_audio, std::string_view audio_codec,
                    u32 audio_bitrate, std::string_view audio_codec_args, Error* error) override final;

  const std::string& GetPath() const override final;
  std::string GetNextCapturePath() const override final;
  u32 GetVideoWidth() const override final;
  u32 GetVideoHeight() const override final;
  float GetVideoFPS() const override final;

  float GetCaptureThreadUsage() const override final;
  float GetCaptureThreadTime() const override final;
  void UpdateCaptureThreadUsage(double pct_divider, double time_divider) override final;

  GPUTexture* GetRenderTexture() override final;
  bool DeliverVideoFrame(GPUTexture* stex) override final;
  bool DeliverAudioFrames(const s16* frames, u32 num_frames) override final;
  bool EndCapture(Error* error) override final;
  void Flush() override final;

protected:
  struct PendingFrame
  {
    enum class State
    {
      Unused,
      NeedsMap,
      NeedsEncoding
    };

    std::unique_ptr<GPUDownloadTexture> tex;
    s64 pts;
    State state;
  };

  ALWAYS_INLINE u32 GetAudioBufferSizeInFrames() const
  {
    return (static_cast<u32>(m_audio_buffer.size()) / AUDIO_CHANNELS);
  }

  void ProcessFramePendingMap(std::unique_lock<std::mutex>& lock);
  void ProcessAllInFlightFrames(std::unique_lock<std::mutex>& lock);
  void EncoderThreadEntryPoint();
  void StartEncoderThread();
  void StopEncoderThread(std::unique_lock<std::mutex>& lock);
  void DeleteOutputFile();

  virtual void ClearState();
  virtual bool SendFrame(const PendingFrame& pf, Error* error) = 0;
  virtual bool ProcessAudioPackets(s64 video_pts, Error* error) = 0;

  virtual bool InternalBeginCapture(float fps, float aspect, u32 sample_rate, bool capture_video,
                                    std::string_view video_codec, u32 video_bitrate, std::string_view video_codec_args,
                                    bool capture_audio, std::string_view audio_codec, u32 audio_bitrate,
                                    std::string_view audio_codec_args, Error* error) = 0;
  virtual bool InternalEndCapture(std::unique_lock<std::mutex>& lock, Error* error);

  mutable std::mutex m_lock;
  std::string m_path;
  std::atomic_bool m_capturing{false};
  std::atomic_bool m_encoding_error{false};

  GPUTexture::Format m_video_render_texture_format = GPUTexture::Format::Unknown;
  u32 m_video_width = 0;
  u32 m_video_height = 0;
  float m_video_fps = 0;
  s64 m_next_video_pts = 0;
  std::unique_ptr<GPUTexture> m_render_texture;

  s64 m_next_audio_pts = 0;
  u32 m_audio_frame_pos = 0;
  u32 m_audio_frame_size = 0;

  Threading::Thread m_encoder_thread;
  u64 m_encoder_thread_last_time = 0;
  float m_encoder_thread_usage = 0.0f;
  float m_encoder_thread_time = 0.0f;

  std::condition_variable m_frame_ready_cv;
  std::condition_variable m_frame_encoded_cv;
  std::array<PendingFrame, MAX_PENDING_FRAMES> m_pending_frames = {};
  u32 m_pending_frames_pos = 0;
  u32 m_frames_pending_map = 0;
  u32 m_frames_map_consume_pos = 0;
  u32 m_frames_pending_encode = 0;
  u32 m_frames_encode_consume_pos = 0;

  DynamicHeapArray<s16> m_audio_buffer;
  std::atomic<u32> m_audio_buffer_size{0};
  u32 m_audio_buffer_write_pos = 0;
  ALIGN_TO_CACHE_LINE u32 m_audio_buffer_read_pos = 0;

  // Shared across all backends.
  [[maybe_unused]] static inline std::mutex s_load_mutex;
};

MediaCaptureBase::~MediaCaptureBase() = default;

bool MediaCaptureBase::BeginCapture(float fps, float aspect, u32 width, u32 height, GPUTexture::Format texture_format,
                                    u32 sample_rate, std::string path, bool capture_video, std::string_view video_codec,
                                    u32 video_bitrate, std::string_view video_codec_args, bool capture_audio,
                                    std::string_view audio_codec, u32 audio_bitrate, std::string_view audio_codec_args,
                                    Error* error)
{
  m_video_render_texture_format = texture_format;
  m_video_width = width;
  m_video_height = height;
  m_video_fps = fps;

  if (path.empty())
  {
    Error::SetStringView(error, "No path specified.");
    return false;
  }
  else if (capture_video &&
           (fps == 0.0f || m_video_width == 0 || !Common::IsAlignedPow2(m_video_width, VIDEO_WIDTH_ALIGNMENT) ||
            m_video_height == 0 || !Common::IsAlignedPow2(m_video_height, VIDEO_HEIGHT_ALIGNMENT)))
  {
    Error::SetStringView(error, "Invalid video dimensions/rate.");
    return false;
  }

  m_path = std::move(path);
  m_capturing.store(true, std::memory_order_release);

  // allocate audio buffer, dynamic based on sample rate
  if (capture_audio)
    m_audio_buffer.resize(sample_rate * MAX_PENDING_FRAMES * AUDIO_CHANNELS);

  INFO_LOG("Initializing capture:");
  if (capture_video)
  {
    INFO_LOG("  Video: {}x{} FPS={}, Aspect={}, Codec={}, Bitrate={}, Args={}", width, height, fps, aspect, video_codec,
             video_bitrate, video_codec_args);
  }
  if (capture_audio)
  {
    INFO_LOG("  Audio: SampleRate={}, Codec={}, Bitrate={}, Args={}", sample_rate, audio_codec, audio_bitrate,
             audio_codec_args);
  }

  if (!InternalBeginCapture(fps, aspect, sample_rate, capture_video, video_codec, video_bitrate, video_codec_args,
                            capture_audio, audio_codec, audio_bitrate, audio_codec_args, error))
  {
    ClearState();
    return false;
  }

  StartEncoderThread();
  return true;
}

GPUTexture* MediaCaptureBase::GetRenderTexture()
{
  if (m_render_texture) [[likely]]
    return m_render_texture.get();

  m_render_texture = g_gpu_device->CreateTexture(m_video_width, m_video_height, 1, 1, 1, GPUTexture::Type::RenderTarget,
                                                 m_video_render_texture_format);
  if (!m_render_texture) [[unlikely]]
  {
    ERROR_LOG("Failed to create {}x{} render texture.", m_video_width, m_video_height);
    return nullptr;
  }

  return m_render_texture.get();
}

bool MediaCaptureBase::DeliverVideoFrame(GPUTexture* stex)
{
  std::unique_lock<std::mutex> lock(m_lock);

  // If the encoder thread reported an error, stop the capture.
  if (m_encoding_error.load(std::memory_order_acquire))
    return false;

  if (m_frames_pending_map >= NUM_FRAMES_IN_FLIGHT)
    ProcessFramePendingMap(lock);

  PendingFrame& pf = m_pending_frames[m_pending_frames_pos];

  // It shouldn't be pending map, but the encode thread might be lagging.
  DebugAssert(pf.state != PendingFrame::State::NeedsMap);
  if (pf.state == PendingFrame::State::NeedsEncoding)
  {
    m_frame_encoded_cv.wait(lock, [&pf]() { return pf.state == PendingFrame::State::Unused; });
  }

  if (!pf.tex || pf.tex->GetWidth() != static_cast<u32>(stex->GetWidth()) ||
      pf.tex->GetHeight() != static_cast<u32>(stex->GetHeight()))
  {
    pf.tex.reset();
    pf.tex = g_gpu_device->CreateDownloadTexture(stex->GetWidth(), stex->GetHeight(), stex->GetFormat());
    if (!pf.tex)
    {
      ERROR_LOG("Failed to create {}x{} download texture", stex->GetWidth(), stex->GetHeight());
      return false;
    }

#ifdef _DEBUG
    GL_OBJECT_NAME_FMT(pf.tex, "GSCapture {}x{} Download Texture", stex->GetWidth(), stex->GetHeight());
#endif
  }

  pf.tex->CopyFromTexture(0, 0, stex, 0, 0, m_video_width, m_video_height, 0, 0);
  pf.pts = m_next_video_pts++;
  pf.state = PendingFrame::State::NeedsMap;

  m_pending_frames_pos = (m_pending_frames_pos + 1) % MAX_PENDING_FRAMES;
  m_frames_pending_map++;
  return true;
}

void MediaCaptureBase::ProcessFramePendingMap(std::unique_lock<std::mutex>& lock)
{
  DebugAssert(m_frames_pending_map > 0);

  PendingFrame& pf = m_pending_frames[m_frames_map_consume_pos];
  DebugAssert(pf.state == PendingFrame::State::NeedsMap);

  // Flushing is potentially expensive, so we leave it unlocked in case the encode thread
  // needs to pick up another thread while we're waiting.
  lock.unlock();

  if (pf.tex->NeedsFlush())
    pf.tex->Flush();

  // Even if the map failed, we need to kick it to the encode thread anyway, because
  // otherwise our queue indices will get desynchronized.
  if (!pf.tex->Map(0, 0, m_video_width, m_video_height))
    WARNING_LOG("Failed to map previously flushed frame.");

  lock.lock();

  // Kick to encoder thread!
  pf.state = PendingFrame::State::NeedsEncoding;
  m_frames_map_consume_pos = (m_frames_map_consume_pos + 1) % MAX_PENDING_FRAMES;
  m_frames_pending_map--;
  m_frames_pending_encode++;
  m_frame_ready_cv.notify_one();
}

void MediaCaptureBase::EncoderThreadEntryPoint()
{
  Threading::SetNameOfCurrentThread("Media Capture Encoding");

  Error error;
  std::unique_lock<std::mutex> lock(m_lock);

  for (;;)
  {
    m_frame_ready_cv.wait(
      lock, [this]() { return (m_frames_pending_encode > 0 || !m_capturing.load(std::memory_order_acquire)); });
    if (m_frames_pending_encode == 0 && !m_capturing.load(std::memory_order_acquire))
      break;

    PendingFrame& pf = m_pending_frames[m_frames_encode_consume_pos];
    DebugAssert(!IsCapturingVideo() || pf.state == PendingFrame::State::NeedsEncoding);

    lock.unlock();

    bool okay = !m_encoding_error;

    // If the frame failed to map, this will be false, and we'll just skip it.
    if (okay && IsCapturingVideo() && pf.tex->IsMapped())
      okay = SendFrame(pf, &error);

    // Encode as many audio frames while the video is ahead.
    if (okay && IsCapturingAudio())
      okay = ProcessAudioPackets(pf.pts, &error);

    lock.lock();

    // If we had an encoding error, tell the GS thread to shut down the capture (later).
    if (!okay) [[unlikely]]
    {
      ERROR_LOG("Encoding error: {}", error.GetDescription());
      m_encoding_error.store(true, std::memory_order_release);
    }

    // Done with this frame! Wait for the next.
    pf.state = PendingFrame::State::Unused;
    m_frames_encode_consume_pos = (m_frames_encode_consume_pos + 1) % MAX_PENDING_FRAMES;
    m_frames_pending_encode--;
    m_frame_encoded_cv.notify_all();
  }
}

void MediaCaptureBase::StartEncoderThread()
{
  INFO_LOG("Starting encoder thread.");
  DebugAssert(m_capturing.load(std::memory_order_acquire) && !m_encoder_thread.Joinable());
  m_encoder_thread.Start([this]() { EncoderThreadEntryPoint(); });
}

void MediaCaptureBase::StopEncoderThread(std::unique_lock<std::mutex>& lock)
{
  // Thread will exit when s_capturing is false.
  DebugAssert(!m_capturing.load(std::memory_order_acquire));

  if (m_encoder_thread.Joinable())
  {
    INFO_LOG("Stopping encoder thread.");

    // Might be sleeping, so wake it before joining.
    m_frame_ready_cv.notify_one();
    lock.unlock();
    m_encoder_thread.Join();
    lock.lock();
  }
}

void MediaCaptureBase::ProcessAllInFlightFrames(std::unique_lock<std::mutex>& lock)
{
  while (m_frames_pending_map > 0)
    ProcessFramePendingMap(lock);

  while (m_frames_pending_encode > 0)
  {
    m_frame_encoded_cv.wait(lock, [this]() { return (m_frames_pending_encode == 0 || m_encoding_error); });
  }
}

bool MediaCaptureBase::DeliverAudioFrames(const s16* frames, u32 num_frames)
{
  if (!IsCapturingAudio())
    return true;
  else if (!m_capturing.load(std::memory_order_acquire))
    return false;

  const u32 audio_buffer_size = GetAudioBufferSizeInFrames();
  if ((audio_buffer_size - m_audio_buffer_size.load(std::memory_order_acquire)) < num_frames)
  {
    // Need to wait for it to drain a bit.
    std::unique_lock<std::mutex> lock(m_lock);
    m_frame_encoded_cv.wait(lock, [this, &num_frames, &audio_buffer_size]() {
      return (!m_capturing.load(std::memory_order_acquire) ||
              ((audio_buffer_size - m_audio_buffer_size.load(std::memory_order_acquire)) >= num_frames));
    });
    if (!m_capturing.load(std::memory_order_acquire))
      return false;
  }

  for (u32 remaining_frames = num_frames;;)
  {
    const u32 contig_frames = std::min(audio_buffer_size - m_audio_buffer_write_pos, remaining_frames);
    std::memcpy(&m_audio_buffer[m_audio_buffer_write_pos * AUDIO_CHANNELS], frames,
                sizeof(s16) * AUDIO_CHANNELS * contig_frames);
    m_audio_buffer_write_pos = (m_audio_buffer_write_pos + contig_frames) % audio_buffer_size;
    remaining_frames -= contig_frames;
    if (remaining_frames == 0)
      break;
  }

  const u32 buffer_size = m_audio_buffer_size.fetch_add(num_frames, std::memory_order_release) + num_frames;
  if (!IsCapturingVideo() && buffer_size >= m_audio_frame_size)
  {
    // If we're not capturing video, push "frames" when we hit the audio packet size.
    std::unique_lock<std::mutex> lock(m_lock);
    if (!m_capturing.load(std::memory_order_acquire))
      return false;

    PendingFrame& pf = m_pending_frames[m_pending_frames_pos];
    pf.state = PendingFrame::State::NeedsEncoding;
    m_pending_frames_pos = (m_pending_frames_pos + 1) % MAX_PENDING_FRAMES;

    m_frames_pending_encode++;
    m_frame_ready_cv.notify_one();
  }

  return true;
}

bool MediaCaptureBase::InternalEndCapture(std::unique_lock<std::mutex>& lock, Error* error)
{
  DebugAssert(m_capturing.load(std::memory_order_acquire));

  const bool had_error = m_encoding_error.load(std::memory_order_acquire);
  if (!had_error)
    ProcessAllInFlightFrames(lock);

  m_capturing.store(false, std::memory_order_release);
  StopEncoderThread(lock);
  return !had_error;
}

void MediaCaptureBase::ClearState()
{
  m_next_video_pts = 0;
  m_next_audio_pts = 0;

  m_pending_frames = {};
  m_pending_frames_pos = 0;
  m_frames_pending_map = 0;
  m_frames_map_consume_pos = 0;
  m_frames_pending_encode = 0;
  m_frames_encode_consume_pos = 0;

  m_audio_buffer_read_pos = 0;
  m_audio_buffer_write_pos = 0;
  m_audio_buffer_size.store(0, std::memory_order_release);
  m_audio_frame_pos = 0;
  m_audio_buffer_size = 0;
  m_audio_buffer.deallocate();

  m_encoding_error.store(false, std::memory_order_release);
}

bool MediaCaptureBase::EndCapture(Error* error)
{
  std::unique_lock<std::mutex> lock(m_lock);
  if (!InternalEndCapture(lock, error))
  {
    DeleteOutputFile();
    ClearState();
    return false;
  }

  ClearState();
  return true;
}

const std::string& MediaCaptureBase::GetPath() const
{
  return m_path;
}

std::string MediaCaptureBase::GetNextCapturePath() const
{
  const std::string_view ext = Path::GetExtension(m_path);
  std::string_view name = Path::GetFileTitle(m_path);

  // Should end with a number.
  u32 partnum = 2;
  std::string_view::size_type pos = name.rfind("_part");
  if (pos != std::string_view::npos)
  {
    std::string_view::size_type cpos = pos + 5;
    for (; cpos < name.length(); cpos++)
    {
      if (name[cpos] < '0' || name[cpos] > '9')
        break;
    }
    if (cpos == name.length())
    {
      // Has existing part number, so add to it.
      partnum = StringUtil::FromChars<u32>(name.substr(pos + 5)).value_or(1) + 1;
      name = name.substr(0, pos);
    }
  }

  // If we haven't started a new file previously, add "_part2".
  return Path::BuildRelativePath(m_path, fmt::format("{}_part{:03d}.{}", name, partnum, ext));
}

u32 MediaCaptureBase::GetVideoWidth() const
{
  return m_video_width;
}

u32 MediaCaptureBase::GetVideoHeight() const
{
  return m_video_height;
}

float MediaCaptureBase::GetVideoFPS() const
{
  return m_video_fps;
}

float MediaCaptureBase::GetCaptureThreadUsage() const
{
  return m_encoder_thread_usage;
}

float MediaCaptureBase::GetCaptureThreadTime() const
{
  return m_encoder_thread_time;
}

void MediaCaptureBase::UpdateCaptureThreadUsage(double pct_divider, double time_divider)
{
  const u64 time = m_encoder_thread.GetCPUTime();
  const u64 delta = time - m_encoder_thread_last_time;
  m_encoder_thread_usage = static_cast<float>(static_cast<double>(delta) * pct_divider);
  m_encoder_thread_time = static_cast<float>(static_cast<double>(delta) * time_divider);
  m_encoder_thread_last_time = time;
}

void MediaCaptureBase::Flush()
{
  std::unique_lock<std::mutex> lock(m_lock);

  if (m_encoding_error)
    return;

  ProcessAllInFlightFrames(lock);

  if (IsCapturingAudio())
  {
    // Clear any buffered audio frames out, we don't want to delay the CPU thread.
    const u32 audio_frames = m_audio_buffer_size.load(std::memory_order_acquire);
    if (audio_frames > 0)
      WARNING_LOG("Dropping {} audio frames for buffer clear.", audio_frames);

    m_audio_buffer_read_pos = 0;
    m_audio_buffer_write_pos = 0;
    m_audio_buffer_size.store(0, std::memory_order_release);
  }
}

void MediaCaptureBase::DeleteOutputFile()
{
  if (m_path.empty())
    return;

  Error error;
  if (FileSystem::DeleteFile(m_path.c_str(), &error))
  {
    INFO_LOG("Deleted output file {}", Path::GetFileName(m_path));
    m_path = {};
  }
  else
  {
    ERROR_LOG("Failed to delete output file '{}': {}", Path::GetFileName(m_path), error.GetDescription());
  }
}

#ifdef _WIN32

#define VISIT_MFPLAT_IMPORTS(X)                                                                                        \
  X(MFCreateMediaType)                                                                                                 \
  X(MFCreateMemoryBuffer)                                                                                              \
  X(MFCreateSample)                                                                                                    \
  X(MFHeapFree)                                                                                                        \
  X(MFShutdown)                                                                                                        \
  X(MFStartup)                                                                                                         \
  X(MFTEnumEx)

#define VISIT_MFREADWRITE_IMPORTS(X) X(MFCreateSinkWriterFromURL)

#define VISIT_MF_IMPORTS(X) X(MFTranscodeGetAudioOutputAvailableTypes)

class MediaCaptureMF final : public MediaCaptureBase
{
  template<class T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  static constexpr u32 FRAME_RATE_NUMERATOR = 10 * 1000 * 1000;
  static constexpr DWORD INVALID_STREAM_INDEX = std::numeric_limits<DWORD>::max();
  static constexpr u32 AUDIO_BITS_PER_SAMPLE = sizeof(s16) * 8;

  static constexpr const GUID& AUDIO_INPUT_MEDIA_FORMAT = MFAudioFormat_PCM;
  static constexpr const GUID& VIDEO_RGB_MEDIA_FORMAT = MFVideoFormat_RGB32;
  static constexpr const GUID& VIDEO_YUV_MEDIA_FORMAT = MFVideoFormat_NV12;

public:
  ~MediaCaptureMF() override;

  static std::unique_ptr<MediaCapture> Create(Error* error);
  static ContainerList GetContainerList();
  static CodecList GetVideoCodecList(const char* container);
  static CodecList GetAudioCodecList(const char* container);

  bool IsCapturingAudio() const override;
  bool IsCapturingVideo() const override;
  time_t GetElapsedTime() const override;

protected:
  void ClearState() override;
  bool SendFrame(const PendingFrame& pf, Error* error) override;
  bool ProcessAudioPackets(s64 video_pts, Error* error) override;
  bool InternalBeginCapture(float fps, float aspect, u32 sample_rate, bool capture_video, std::string_view video_codec,
                            u32 video_bitrate, std::string_view video_codec_args, bool capture_audio,
                            std::string_view audio_codec, u32 audio_bitrate, std::string_view audio_codec_args,
                            Error* error) override;
  bool InternalEndCapture(std::unique_lock<std::mutex>& lock, Error* error) override;

private:
  // Media foundation works in units of 100 nanoseconds.
  static constexpr double ConvertFrequencyToMFDurationUnits(double frequency) { return ((1e+9 / frequency) / 100.0); }
  static constexpr LONGLONG ConvertPTSToTimestamp(s64 pts, double duration)
  {
    // Both of these use truncation, not rounding, so that the next sample lines up.
    return static_cast<LONGLONG>(static_cast<double>(pts) * duration);
  }
  static constexpr LONGLONG ConvertFramesToDuration(u32 frames, double duration)
  {
    return static_cast<LONGLONG>(static_cast<double>(frames) * duration);
  }
  static constexpr time_t ConvertPTSToSeconds(s64 pts, double duration)
  {
    return static_cast<time_t>((static_cast<double>(pts) * duration) / 1e+7);
  }

  ComPtr<IMFTransform> CreateVideoYUVTransform(ComPtr<IMFMediaType>* output_type, float fps, Error* error);
  ComPtr<IMFTransform> CreateVideoEncodeTransform(std::string_view codec, float fps, u32 bitrate,
                                                  IMFMediaType* input_type, ComPtr<IMFMediaType>* output_type,
                                                  bool* use_async_transform, Error* error);
  bool GetAudioTypes(std::string_view codec, ComPtr<IMFMediaType>* input_type, ComPtr<IMFMediaType>* output_type,
                     u32 sample_rate, u32 bitrate, Error* error);
  void ConvertVideoFrame(u8* dst, size_t dst_stride, const u8* src, size_t src_stride, u32 width, u32 height) const;

  bool ProcessVideoOutputSamples(Error* error); // synchronous
  bool ProcessVideoEvents(Error* error);        // asynchronous

  ComPtr<IMFSinkWriter> m_sink_writer;

  DWORD m_video_stream_index = INVALID_STREAM_INDEX;
  DWORD m_audio_stream_index = INVALID_STREAM_INDEX;

  double m_video_sample_duration = 0;
  double m_audio_sample_duration = 0;

  ComPtr<IMFTransform> m_video_yuv_transform;
  ComPtr<IMFSample> m_video_yuv_sample;
  ComPtr<IMFTransform> m_video_encode_transform;
  ComPtr<IMFMediaEventGenerator> m_video_encode_event_generator;
  std::deque<ComPtr<IMFSample>> m_pending_video_samples;
  ComPtr<IMFSample> m_video_output_sample;
  u32 m_wanted_video_samples = 0;
  DWORD m_video_sample_size = 0;

#define DECLARE_IMPORT(X) static inline decltype(X)* wrap_##X;
  VISIT_MFPLAT_IMPORTS(DECLARE_IMPORT);
  VISIT_MFREADWRITE_IMPORTS(DECLARE_IMPORT);
  VISIT_MF_IMPORTS(DECLARE_IMPORT);
#undef DECLARE_IMPORT

  static bool LoadMediaFoundation(Error* error);
  static void UnloadMediaFoundation();

  static inline DynamicLibrary s_mfplat_library;
  static inline DynamicLibrary s_mfreadwrite_library;
  static inline DynamicLibrary s_mf_library;
  static inline bool s_library_loaded = false;
};

struct MediaFoundationVideoCodec
{
  const char* name;
  const char* display_name;
  const GUID& guid;
  bool require_hardware;
};
struct MediaFoundationAudioCodec
{
  const char* name;
  const char* display_name;
  const GUID& guid;
  u32 min_bitrate;
  u32 max_bitrate;
};
static constexpr const MediaFoundationVideoCodec s_media_foundation_video_codecs[] = {
  {"h264", "H.264 with Software Encoding", MFVideoFormat_H264, false},
  {"h264_hw", "H.264 with Hardware Encoding", MFVideoFormat_H264, true},
  {"h265", "H.265 with Software Encoding", MFVideoFormat_H265, false},
  {"h265_hw", "H.265 with Hardware Encoding", MFVideoFormat_H265, true},
  {"hevc", "HEVC with Software Encoding", MFVideoFormat_HEVC, false},
  {"hevc_hw", "HEVC with Hardware Encoding", MFVideoFormat_HEVC, true},
  {"vp9", "VP9 with Software Encoding", MFVideoFormat_VP90, false},
  {"vp9_hw", "VP9 with Hardware Encoding", MFVideoFormat_VP90, true},
  {"av1", "AV1 with Software Encoding", MFVideoFormat_AV1, false},
  {"av1_hw", "AV1 with Hardware Encoding", MFVideoFormat_AV1, false},
};
static constexpr const MediaFoundationAudioCodec s_media_foundation_audio_codecs[] = {
  {"aac", "Advanced Audio Coding", MFAudioFormat_AAC, 64, 224},
  {"mp3", "MPEG-2 Audio Layer III", MFAudioFormat_MP3, 64, 320},
  {"pcm", "Uncompressed PCM", MFAudioFormat_PCM, 0, std::numeric_limits<u32>::max()},
};

bool MediaCaptureMF::LoadMediaFoundation(Error* error)
{
  std::unique_lock lock(s_load_mutex);
  if (s_library_loaded)
    return true;

  bool result = s_mfplat_library.Open("mfplat.dll", error);
  result = result && s_mfreadwrite_library.Open("mfreadwrite.dll", error);
  result = result && s_mf_library.Open("mf.dll", error);

#define RESOLVE_IMPORT(X) result = result && s_mfplat_library.GetSymbol(#X, &wrap_##X);
  VISIT_MFPLAT_IMPORTS(RESOLVE_IMPORT);
#undef RESOLVE_IMPORT

#define RESOLVE_IMPORT(X) result = result && s_mfreadwrite_library.GetSymbol(#X, &wrap_##X);
  VISIT_MFREADWRITE_IMPORTS(RESOLVE_IMPORT);
#undef RESOLVE_IMPORT

#define RESOLVE_IMPORT(X) result = result && s_mf_library.GetSymbol(#X, &wrap_##X);
  VISIT_MF_IMPORTS(RESOLVE_IMPORT);
#undef RESOLVE_IMPORT

  HRESULT hr;
  if (result && FAILED(hr = wrap_MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) [[unlikely]]
  {
    Error::SetHResult(error, "MFStartup() failed: ", hr);
    result = false;
  }

  if (result) [[likely]]
  {
    s_library_loaded = true;
    std::atexit(&MediaCaptureMF::UnloadMediaFoundation);
    return true;
  }

  UnloadMediaFoundation();

  Error::AddPrefix(error, TRANSLATE_SV("MediaCapture", "Failed to load Media Foundation libraries: "));
  return false;
}

void MediaCaptureMF::UnloadMediaFoundation()
{
#define CLEAR_IMPORT(X) wrap_##X = nullptr;
  VISIT_MF_IMPORTS(CLEAR_IMPORT);
  VISIT_MFREADWRITE_IMPORTS(CLEAR_IMPORT);
  VISIT_MFPLAT_IMPORTS(CLEAR_IMPORT);
#undef CLEAR_IMPORT

  s_mf_library.Close();
  s_mfreadwrite_library.Close();
  s_mfplat_library.Close();
  s_library_loaded = false;
}

#undef VISIT_MF_IMPORTS
#undef VISIT_MFREADWRITE_IMPORTS
#undef VISIT_MFPLAT_IMPORTS

MediaCaptureMF::~MediaCaptureMF() = default;

std::unique_ptr<MediaCapture> MediaCaptureMF::Create(Error* error)
{
  if (!LoadMediaFoundation(error))
    return nullptr;

  return std::make_unique<MediaCaptureMF>();
}

MediaCapture::ContainerList MediaCaptureMF::GetContainerList()
{
  return {
    {"avi", "Audio Video Interleave"},
    {"mp4", "MPEG-4 Part 14"},
    {"mp3", "MPEG-2 Audio Layer III"},
    {"wav", "Waveform Audio File Format"},
  };
}

MediaCapture::ContainerList MediaCaptureMF::GetAudioCodecList(const char* container)
{
  ContainerList ret;
  ret.reserve(std::size(s_media_foundation_audio_codecs));
  for (const MediaFoundationAudioCodec& codec : s_media_foundation_audio_codecs)
    ret.emplace_back(codec.name, codec.display_name);
  return ret;
}

MediaCapture::ContainerList MediaCaptureMF::GetVideoCodecList(const char* container)
{
  ContainerList ret;
  ret.reserve(std::size(s_media_foundation_video_codecs));
  for (const MediaFoundationVideoCodec& codec : s_media_foundation_video_codecs)
    ret.emplace_back(codec.name, codec.display_name);
  return ret;
}

bool MediaCaptureMF::IsCapturingVideo() const
{
  return (m_video_stream_index != INVALID_STREAM_INDEX);
}

bool MediaCaptureMF::IsCapturingAudio() const
{
  return (m_audio_stream_index != INVALID_STREAM_INDEX);
}

time_t MediaCaptureMF::GetElapsedTime() const
{
  if (IsCapturingVideo())
    return ConvertPTSToSeconds(m_next_video_pts, m_video_sample_duration);
  else
    return ConvertPTSToSeconds(m_next_audio_pts, m_audio_sample_duration);
}

bool MediaCaptureMF::InternalBeginCapture(float fps, float aspect, u32 sample_rate, bool capture_video,
                                          std::string_view video_codec, u32 video_bitrate,
                                          std::string_view video_codec_args, bool capture_audio,
                                          std::string_view audio_codec, u32 audio_bitrate,
                                          std::string_view audio_codec_args, Error* error)
{
  HRESULT hr;

  ComPtr<IMFMediaType> video_media_type;
  bool use_async_video_transform = false;

  if (capture_video)
  {
    m_video_sample_duration = ConvertFrequencyToMFDurationUnits(fps);

    ComPtr<IMFMediaType> yuv_media_type;
    if (!(m_video_yuv_transform = CreateVideoYUVTransform(&yuv_media_type, fps, error)) ||
        !(m_video_encode_transform = CreateVideoEncodeTransform(video_codec, fps, video_bitrate, yuv_media_type.Get(),
                                                                &video_media_type, &use_async_video_transform, error)))
    {
      return false;
    }
  }

  ComPtr<IMFMediaType> audio_input_type, audio_output_type;
  if (capture_audio)
  {
    if (!GetAudioTypes(audio_codec, &audio_input_type, &audio_output_type, sample_rate, audio_bitrate, error))
      return false;

    // only used when not capturing video
    m_audio_frame_size = static_cast<u32>(static_cast<float>(sample_rate) / fps);
    m_audio_sample_duration = ConvertFrequencyToMFDurationUnits(sample_rate);
  }

  if (FAILED(hr = wrap_MFCreateSinkWriterFromURL(StringUtil::UTF8StringToWideString(m_path).c_str(), nullptr, nullptr,
                                                 m_sink_writer.GetAddressOf())))
  {
    Error::SetHResult(error, "MFCreateSinkWriterFromURL() failed: ", hr);
    return false;
  }

  if (capture_video)
  {
    if (SUCCEEDED(hr) && FAILED(hr = m_sink_writer->AddStream(video_media_type.Get(), &m_video_stream_index)))
      [[unlikely]]
    {
      Error::SetHResult(error, "Video AddStream() failed: ", hr);
    }

    if (SUCCEEDED(hr) && FAILED(hr = m_sink_writer->SetInputMediaType(m_video_stream_index, video_media_type.Get(),
                                                                      nullptr))) [[unlikely]]
    {
      Error::SetHResult(error, "Video SetInputMediaType() failed: ", hr);
    }
  }

  if (capture_audio)
  {
    if (SUCCEEDED(hr) && FAILED(hr = m_sink_writer->AddStream(audio_output_type.Get(), &m_audio_stream_index)))
      [[unlikely]]
    {
      Error::SetHResult(error, "Audio AddStream() failed: ", hr);
    }

    if (SUCCEEDED(hr) && audio_input_type &&
        FAILED(hr = m_sink_writer->SetInputMediaType(m_audio_stream_index, audio_input_type.Get(), nullptr)))
      [[unlikely]]
    {
      Error::SetHResult(error, "Audio SetInputMediaType() failed: ", hr);
    }
  }

  if (SUCCEEDED(hr) && FAILED(hr = m_sink_writer->BeginWriting()))
    Error::SetHResult(error, "BeginWriting() failed: ", hr);

  if (use_async_video_transform)
  {
    if (SUCCEEDED(hr) && FAILED(hr = m_video_encode_transform.As(&m_video_encode_event_generator)))
      Error::SetHResult(error, "Getting video encode event generator failed: ", hr);
  }

  if (capture_video && SUCCEEDED(hr) &&
      FAILED(hr = m_video_encode_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
  {
    Error::SetHResult(error, "MFT_MESSAGE_NOTIFY_START_OF_STREAM failed: ", hr);
  }

  if (FAILED(hr))
  {
    m_sink_writer.Reset();
    DeleteOutputFile();
    return false;
  }

  return true;
}

bool MediaCaptureMF::InternalEndCapture(std::unique_lock<std::mutex>& lock, Error* error)
{
  HRESULT hr = MediaCaptureBase::InternalEndCapture(lock, error) ? S_OK : E_FAIL;

  // need to drain all input frames
  if (m_video_encode_transform)
  {
    if (SUCCEEDED(hr) && FAILED(hr = m_video_encode_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0)))
    {
      Error::SetHResult(error, "MFT_MESSAGE_NOTIFY_END_OF_STREAM failed: ", hr);
      return false;
    }

    if (m_video_encode_event_generator)
      hr = ProcessVideoEvents(error) ? S_OK : E_FAIL;
    else
      hr = ProcessVideoOutputSamples(error) ? S_OK : E_FAIL;
  }

  if (SUCCEEDED(hr) && FAILED(hr = m_sink_writer->Finalize())) [[unlikely]]
    Error::SetHResult(error, "Finalize() failed: ", hr);

  m_sink_writer.Reset();
  return SUCCEEDED(hr);
}

MediaCaptureMF::ComPtr<IMFTransform> MediaCaptureMF::CreateVideoYUVTransform(ComPtr<IMFMediaType>* output_type,
                                                                             float fps, Error* error)
{
  const MFT_REGISTER_TYPE_INFO input_type_info = {.guidMajorType = MFMediaType_Video,
                                                  .guidSubtype = VIDEO_RGB_MEDIA_FORMAT};
  const MFT_REGISTER_TYPE_INFO output_type_info = {.guidMajorType = MFMediaType_Video,
                                                   .guidSubtype = VIDEO_YUV_MEDIA_FORMAT};

  IMFActivate** transforms = nullptr;
  UINT32 num_transforms = 0;
  HRESULT hr = wrap_MFTEnumEx(MFT_CATEGORY_VIDEO_PROCESSOR, MFT_ENUM_FLAG_SORTANDFILTER, &input_type_info,
                              &output_type_info, &transforms, &num_transforms);
  if (FAILED(hr)) [[unlikely]]
  {
    Error::SetHResult(error, "YUV MFTEnumEx() failed: ", hr);
    return nullptr;
  }
  else if (num_transforms == 0) [[unlikely]]
  {
    Error::SetStringView(error, "No video processors found.");
    return nullptr;
  }

  ComPtr<IMFTransform> transform;
  hr = transforms[0]->ActivateObject(IID_PPV_ARGS(transform.GetAddressOf()));
  if (transforms)
    wrap_MFHeapFree(transforms);
  if (FAILED(hr)) [[unlikely]]
  {
    Error::SetHResult(error, "YUV ActivateObject() failed: ", hr);
    return nullptr;
  }

  ComPtr<IMFMediaType> input_type;
  if (FAILED(hr = wrap_MFCreateMediaType(input_type.GetAddressOf())) ||
      FAILED(hr = wrap_MFCreateMediaType(output_type->GetAddressOf()))) [[unlikely]]
  {
    Error::SetHResult(error, "YUV MFCreateMediaType() failed: ", hr);
    return nullptr;
  }

  if (FAILED(hr = input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
      FAILED(hr = input_type->SetGUID(MF_MT_SUBTYPE, VIDEO_RGB_MEDIA_FORMAT)) ||
      FAILED(hr = input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
      FAILED(hr = MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, m_video_width, m_video_height)) ||
      FAILED(hr = (*output_type)->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
      FAILED(hr = (*output_type)->SetGUID(MF_MT_SUBTYPE, VIDEO_YUV_MEDIA_FORMAT)) ||
      FAILED(hr = (*output_type)->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
      FAILED(hr = MFSetAttributeSize(output_type->Get(), MF_MT_FRAME_SIZE, m_video_width, m_video_height)) ||
      FAILED(hr = MFSetAttributeRatio(
               output_type->Get(), MF_MT_FRAME_RATE,
               static_cast<UINT32>(static_cast<double>(fps) * static_cast<double>(FRAME_RATE_NUMERATOR)),
               FRAME_RATE_NUMERATOR))) [[unlikely]]
  {
    Error::SetHResult(error, "YUV setting attributes failed: ", hr);
    return nullptr;
  }

  if (FAILED(hr = transform->SetOutputType(0, output_type->Get(), 0))) [[unlikely]]
  {
    Error::SetHResult(error, "YUV SetOutputType() failed: ", hr);
    return nullptr;
  }

  if (FAILED(hr = transform->SetInputType(0, input_type.Get(), 0))) [[unlikely]]
  {
    Error::SetHResult(error, "YUV SetInputType() failed: ", hr);
    return nullptr;
  }

  return transform;
}

MediaCaptureMF::ComPtr<IMFTransform> MediaCaptureMF::CreateVideoEncodeTransform(std::string_view codec, float fps,
                                                                                u32 bitrate, IMFMediaType* input_type,
                                                                                ComPtr<IMFMediaType>* output_type,
                                                                                bool* use_async_transform, Error* error)
{
  const MFT_REGISTER_TYPE_INFO input_type_info = {.guidMajorType = MFMediaType_Video,
                                                  .guidSubtype = VIDEO_YUV_MEDIA_FORMAT};
  MFT_REGISTER_TYPE_INFO output_type_info = {.guidMajorType = MFMediaType_Video, .guidSubtype = MFVideoFormat_H264};
  bool hardware = false;
  if (!codec.empty())
  {
    bool found = false;
    for (const MediaFoundationVideoCodec& tcodec : s_media_foundation_video_codecs)
    {
      if (StringUtil::EqualNoCase(codec, tcodec.name))
      {
        output_type_info.guidSubtype = tcodec.guid;
        hardware = tcodec.require_hardware;
        found = true;
        break;
      }
    }
    if (!found)
    {
      Error::SetStringFmt(error, "Unknown video codec '{}'", codec);
      return nullptr;
    }
  }

  IMFActivate** transforms = nullptr;
  UINT32 num_transforms = 0;
  HRESULT hr =
    wrap_MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, (hardware ? MFT_ENUM_FLAG_HARDWARE : 0) | MFT_ENUM_FLAG_SORTANDFILTER,
                   &input_type_info, &output_type_info, &transforms, &num_transforms);
  if (FAILED(hr)) [[unlikely]]
  {
    Error::SetHResult(error, "Encoder MFTEnumEx() failed: ", hr);
    return nullptr;
  }
  else if (num_transforms == 0) [[unlikely]]
  {
    Error::SetStringView(error, "No video encoders found.");
    return nullptr;
  }

  ComPtr<IMFTransform> transform;
  hr = transforms[0]->ActivateObject(IID_PPV_ARGS(transform.GetAddressOf()));
  if (transforms)
    wrap_MFHeapFree(transforms);
  if (FAILED(hr)) [[unlikely]]
  {
    Error::SetHResult(error, "Encoder ActivateObject() failed: ", hr);
    return nullptr;
  }

  *use_async_transform = false;
  if (hardware)
  {
    ComPtr<IMFAttributes> attributes;
    if (FAILED(transform->GetAttributes(attributes.GetAddressOf()))) [[unlikely]]
    {
      Error::SetHResult(error, "YUV GetAttributes() failed: ", hr);
      return nullptr;
    }
    UINT32 async_supported;
    *use_async_transform =
      (SUCCEEDED(hr = attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async_supported)) && async_supported == TRUE &&
       SUCCEEDED(hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, 1)));
    if (use_async_transform)
      INFO_LOG("Using async video transform.");
  }

  if (FAILED(hr = wrap_MFCreateMediaType(output_type->GetAddressOf()))) [[unlikely]]
  {
    Error::SetHResult(error, "Encoder MFCreateMediaType() failed: ", hr);
    return nullptr;
  }

  constexpr u32 par_numerator = 1;
  constexpr u32 par_denominator = 1;

  u32 profile = 0;
  if (output_type_info.guidSubtype == MFVideoFormat_H264)
    profile = eAVEncH264VProfile_Main;
  else if (output_type_info.guidSubtype == MFVideoFormat_H265)
    profile = eAVEncH265VProfile_Main_420_8;
  else if (output_type_info.guidSubtype == MFVideoFormat_VP90)
    profile = eAVEncVP9VProfile_420_8;

  if (FAILED(hr = (*output_type)->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
      FAILED(hr = (*output_type)->SetGUID(MF_MT_SUBTYPE, output_type_info.guidSubtype)) ||
      FAILED(hr = (*output_type)->SetUINT32(MF_MT_AVG_BITRATE, bitrate * 1000)) ||
      FAILED(hr = (*output_type)->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
      FAILED(hr = (*output_type)->SetUINT32(MF_MT_MPEG2_PROFILE, profile)) ||
      FAILED(hr = MFSetAttributeSize(output_type->Get(), MF_MT_FRAME_SIZE, m_video_width, m_video_height)) ||
      FAILED(hr = MFSetAttributeRatio(
               output_type->Get(), MF_MT_FRAME_RATE,
               static_cast<UINT32>(static_cast<double>(fps) * static_cast<double>(FRAME_RATE_NUMERATOR)),
               FRAME_RATE_NUMERATOR)) ||
      FAILED(hr = MFSetAttributeRatio(output_type->Get(), MF_MT_PIXEL_ASPECT_RATIO, par_numerator, par_denominator)))
    [[unlikely]]
  {
    Error::SetHResult(error, "Encoder setting attributes failed: ", hr);
    return nullptr;
  }

  if (FAILED(hr = transform->SetOutputType(0, output_type->Get(), 0))) [[unlikely]]
  {
    Error::SetHResult(error, "Encoder SetOutputType() failed: ", hr);
    return nullptr;
  }

  if (FAILED(hr = transform->SetInputType(0, input_type, 0))) [[unlikely]]
  {
    Error::SetHResult(error, "Encoder SetInputType() failed: ", hr);
    return nullptr;
  }

  MFT_OUTPUT_STREAM_INFO osi;
  if (FAILED(hr = transform->GetOutputStreamInfo(0, &osi))) [[unlikely]]
  {
    Error::SetHResult(error, "Encoder GetOutputStreamInfo() failed: ", hr);
    return nullptr;
  }

  if (!(osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES))
  {
    if (osi.cbSize == 0)
    {
      Error::SetStringFmt(error, "Invalid sample size for non-output-providing stream");
      return nullptr;
    }

    m_video_sample_size = osi.cbSize;
  }

  INFO_LOG("Video sample size: {}", m_video_sample_size);
  return transform;
}

ALWAYS_INLINE_RELEASE void MediaCaptureMF::ConvertVideoFrame(u8* dst, size_t dst_stride, const u8* src,
                                                             size_t src_stride, u32 width, u32 height) const
{
  if (!g_gpu_device->UsesLowerLeftOrigin())
  {
    src += src_stride * (height - 1);
    src_stride = static_cast<size_t>(-static_cast<std::make_signed_t<size_t>>(src_stride));
  }

  if (m_video_render_texture_format == GPUTexture::Format::RGBA8)
  {
    // need to convert rgba -> bgra, as well as flipping vertically
    const u32 vector_width = 4;
    const u32 aligned_width = Common::AlignDownPow2(width, vector_width);
    for (u32 remaining_rows = height;;)
    {
      const u8* row_src = src;
      u8* row_dst = dst;

      u32 x = 0;
      for (; x < aligned_width; x += vector_width)
      {
        static constexpr GSVector4i mask = GSVector4i::cxpr8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
        GSVector4i::store<false>(row_dst, GSVector4i::load<false>(row_src).shuffle8(mask));
        row_src += vector_width * sizeof(u32);
        row_dst += vector_width * sizeof(u32);
      }

      for (; x < width; x++)
      {
        row_dst[0] = row_src[2];
        row_dst[1] = row_src[1];
        row_dst[2] = row_src[0];
        row_dst[3] = row_src[3];
        row_src += sizeof(u32);
        row_dst += sizeof(u32);
      }

      src += src_stride;
      dst += dst_stride;

      remaining_rows--;
      if (remaining_rows == 0)
        break;
    }
  }
  else
  {
    // only flip
    const u32 copy_width = sizeof(u32) * width;
    for (u32 remaining_rows = height;;)
    {
      const u8* row_src = src;
      u8* row_dst = dst;
      std::memcpy(row_dst, row_src, copy_width);
      src += src_stride;
      dst += dst_stride;

      remaining_rows--;
      if (remaining_rows == 0)
        break;
    }
  }
}

void MediaCaptureMF::ClearState()
{
  MediaCaptureBase::ClearState();

  m_sink_writer.Reset();

  m_video_stream_index = INVALID_STREAM_INDEX;
  m_audio_stream_index = INVALID_STREAM_INDEX;

  m_video_sample_duration = 0;
  m_audio_sample_duration = 0;

  m_video_yuv_transform.Reset();
  m_video_yuv_sample.Reset();
  m_video_encode_transform.Reset();
  m_video_encode_event_generator.Reset();
  m_pending_video_samples.clear();
  m_video_output_sample.Reset();
  m_wanted_video_samples = 0;
  m_video_sample_size = 0;
}

bool MediaCaptureMF::SendFrame(const PendingFrame& pf, Error* error)
{
  const u32 buffer_stride = m_video_width * sizeof(u32);
  const u32 buffer_size = buffer_stride * m_video_height;

  HRESULT hr;
  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(hr = wrap_MFCreateMemoryBuffer(buffer_size, buffer.GetAddressOf()))) [[unlikely]]
  {
    Error::SetHResult(error, "MFCreateMemoryBuffer() failed: ", hr);
    return false;
  }

  BYTE* buffer_data;
  if (FAILED(hr = buffer->Lock(&buffer_data, nullptr, nullptr))) [[unlikely]]
  {
    Error::SetHResult(error, "Lock() failed: ", hr);
    return false;
  }

  ConvertVideoFrame(buffer_data, buffer_stride, pf.tex->GetMapPointer(), pf.tex->GetMapPitch(), m_video_width,
                    m_video_height);
  buffer->Unlock();

  if (FAILED(hr = buffer->SetCurrentLength(buffer_size))) [[unlikely]]
  {
    Error::SetHResult(error, "SetCurrentLength() failed: ", hr);
    return false;
  }

  ComPtr<IMFSample> sample;
  if (FAILED(hr = wrap_MFCreateSample(sample.GetAddressOf()))) [[unlikely]]
  {
    Error::SetHResult(error, "MFCreateSample() failed: ", hr);
    return false;
  }

  if (FAILED(hr = sample->AddBuffer(buffer.Get()))) [[unlikely]]
  {
    Error::SetHResult(error, "AddBuffer() failed: ", hr);
    return false;
  }

  if (FAILED(hr = sample->SetSampleTime(ConvertPTSToTimestamp(pf.pts, m_video_sample_duration)))) [[unlikely]]
  {
    Error::SetHResult(error, "SetSampleTime() failed: ", hr);
    return false;
  }

  if (FAILED(hr = sample->SetSampleDuration(static_cast<LONGLONG>(m_video_sample_duration)))) [[unlikely]]
  {
    Error::SetHResult(error, "SetSampleDuration() failed: ", hr);
    return false;
  }

  //////////////////////////////////////////////////////////////////////////
  // RGB -> YUV
  //////////////////////////////////////////////////////////////////////////

  if (FAILED(hr = m_video_yuv_transform->ProcessInput(0, sample.Get(), 0))) [[unlikely]]
  {
    Error::SetHResult(error, "YUV ProcessInput() failed: ", hr);
    return false;
  }

  for (;;)
  {
    if (!m_video_yuv_sample)
    {
      ComPtr<IMFMediaBuffer> yuv_membuf;
      if (FAILED(hr = wrap_MFCreateMemoryBuffer(buffer_size, yuv_membuf.GetAddressOf()))) [[unlikely]]
      {
        Error::SetHResult(error, "YUV MFCreateMemoryBuffer() failed: ", hr);
        return false;
      }

      if (FAILED(hr = wrap_MFCreateSample(m_video_yuv_sample.GetAddressOf()))) [[unlikely]]
      {
        Error::SetHResult(error, "YUV MFCreateSample() failed: ", hr);
        return false;
      }
      if (FAILED(hr = m_video_yuv_sample->AddBuffer(yuv_membuf.Get()))) [[unlikely]]
      {
        Error::SetHResult(error, "YUV AddBuffer() failed: ", hr);
        return false;
      }
    }

    DWORD status;
    MFT_OUTPUT_DATA_BUFFER yuv_buf = {.pSample = m_video_yuv_sample.Get()};
    hr = m_video_yuv_transform->ProcessOutput(0, 1, &yuv_buf, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
      break;

    if (FAILED(hr)) [[unlikely]]
    {
      Error::SetHResult(error, "YUV ProcessOutput() failed: ", hr);
      return false;
    }
    if (yuv_buf.pEvents)
      yuv_buf.pEvents->Release();

    m_pending_video_samples.push_back(std::move(m_video_yuv_sample));

    if (m_video_encode_event_generator)
    {
      if (!ProcessVideoEvents(error)) [[unlikely]]
        return false;
    }
    else
    {
      if (!ProcessVideoOutputSamples(error)) [[unlikely]]
        return false;
    }
  }

  return true;
}

bool MediaCaptureMF::ProcessVideoOutputSamples(Error* error)
{
  HRESULT hr;

  for (;;)
  {
    while (!m_pending_video_samples.empty())
    {
      if (FAILED(hr = m_video_encode_transform->ProcessInput(0, m_pending_video_samples.front().Get(), 0))) [[unlikely]]
      {
        Error::SetHResult(error, "Video ProcessInput() failed: ", hr);
        return false;
      }
      m_pending_video_samples.pop_front();
    }

    if (m_video_sample_size > 0 && !m_video_output_sample)
    {
      ComPtr<IMFMediaBuffer> video_membuf;
      if (FAILED(hr = wrap_MFCreateMemoryBuffer(m_video_sample_size, video_membuf.GetAddressOf()))) [[unlikely]]
      {
        Error::SetHResult(error, "YUV MFCreateMemoryBuffer() failed: ", hr);
        return false;
      }

      if (FAILED(hr = wrap_MFCreateSample(m_video_output_sample.GetAddressOf()))) [[unlikely]]
      {
        Error::SetHResult(error, "YUV MFCreateSample() failed: ", hr);
        return false;
      }
      if (FAILED(hr = m_video_output_sample->AddBuffer(video_membuf.Get()))) [[unlikely]]
      {
        Error::SetHResult(error, "YUV AddBuffer() failed: ", hr);
        return false;
      }
    }

    MFT_OUTPUT_DATA_BUFFER video_buf = {.pSample = m_video_output_sample.Get()};
    DWORD status;
    hr = m_video_encode_transform->ProcessOutput(0, 1, &video_buf, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
      break;

    if (FAILED(hr)) [[unlikely]]
    {
      Error::SetHResult(error, "Video ProcessOutput() failed: ", hr);
      return false;
    }
    if (video_buf.pEvents)
      video_buf.pEvents->Release();

    hr = m_sink_writer->WriteSample(m_video_stream_index, video_buf.pSample);
    if (FAILED(hr)) [[unlikely]]
    {
      Error::SetHResult(error, "Video WriteSample() failed: ", hr);
      return false;
    }

    // might be transform-provided
    if (m_video_output_sample)
      m_video_output_sample.Reset();
    else
      video_buf.pSample->Release();
  }

  return true;
}

bool MediaCaptureMF::ProcessVideoEvents(Error* error)
{
  HRESULT hr;

  for (;;)
  {
    // push any wanted input
    while (m_wanted_video_samples > 0)
    {
      if (m_pending_video_samples.empty())
        break;

      if (FAILED(hr = m_video_encode_transform->ProcessInput(0, m_pending_video_samples.front().Get(), 0))) [[unlikely]]
      {
        Error::SetHResult(error, "Video ProcessInput() failed: ", hr);
        return false;
      }
      m_pending_video_samples.pop_front();

      m_wanted_video_samples--;
    }

    ComPtr<IMFMediaEvent> event;
    hr = m_video_encode_event_generator->GetEvent(MF_EVENT_FLAG_NO_WAIT, event.GetAddressOf());
    if (hr == MF_E_NO_EVENTS_AVAILABLE)
      return true;

    if (FAILED(hr)) [[unlikely]]
    {
      Error::SetHResult(error, "GetEvent() failed: ", hr);
      return false;
    }

    MediaEventType type;
    if (FAILED(hr = event->GetType(&type))) [[unlikely]]
    {
      Error::SetHResult(error, "GetEvent() failed: ", hr);
      return false;
    }

    UINT32 stream_id = 0;
    if (type == METransformNeedInput || type == METransformHaveOutput)
    {
      if (FAILED(hr = event->GetUINT32(MF_EVENT_MFT_INPUT_STREAM_ID, &stream_id)))
      {
        Error::SetHResult(error, "Get stream ID failed: ", hr);
        return false;
      }
      else if (stream_id != 0)
      {
        Error::SetStringFmt(error, "Unexpected stream ID {}", stream_id);
        return false;
      }
    }

    switch (type)
    {
      case METransformNeedInput:
      {
        m_wanted_video_samples++;
      }
      break;

      case METransformHaveOutput:
      {
        if (m_video_sample_size > 0 && !m_video_output_sample)
        {
          ComPtr<IMFMediaBuffer> video_membuf;
          if (FAILED(hr = wrap_MFCreateMemoryBuffer(m_video_sample_size, video_membuf.GetAddressOf()))) [[unlikely]]
          {
            Error::SetHResult(error, "YUV MFCreateMemoryBuffer() failed: ", hr);
            return false;
          }

          if (FAILED(hr = wrap_MFCreateSample(m_video_output_sample.GetAddressOf()))) [[unlikely]]
          {
            Error::SetHResult(error, "YUV MFCreateSample() failed: ", hr);
            return false;
          }
          if (FAILED(hr = m_video_output_sample->AddBuffer(video_membuf.Get()))) [[unlikely]]
          {
            Error::SetHResult(error, "YUV AddBuffer() failed: ", hr);
            return false;
          }
        }

        MFT_OUTPUT_DATA_BUFFER video_buf = {.pSample = m_video_output_sample.Get()};
        DWORD status;
        if (FAILED(hr = m_video_encode_transform->ProcessOutput(0, 1, &video_buf, &status))) [[unlikely]]
        {
          Error::SetHResult(error, "Video ProcessOutput() failed: ", hr);
          return false;
        }
        if (video_buf.pEvents)
          video_buf.pEvents->Release();

        hr = m_sink_writer->WriteSample(m_video_stream_index, video_buf.pSample);
        if (FAILED(hr)) [[unlikely]]
        {
          Error::SetHResult(error, "Video WriteSample() failed: ", hr);
          return false;
        }

        // might be transform-provided
        if (m_video_output_sample)
          m_video_output_sample.Reset();
        else
          video_buf.pSample->Release();
      }
      break;

      default:
        WARNING_LOG("Unhandled video event {}", static_cast<u32>(type));
        break;
    }
  }
}

bool MediaCaptureMF::GetAudioTypes(std::string_view codec, ComPtr<IMFMediaType>* input_type,
                                   ComPtr<IMFMediaType>* output_type, u32 sample_rate, u32 bitrate, Error* error)
{
  GUID output_subtype = MFAudioFormat_AAC;
  if (!codec.empty())
  {
    bool found = false;
    for (const MediaFoundationAudioCodec& tcodec : s_media_foundation_audio_codecs)
    {
      if (StringUtil::EqualNoCase(codec, tcodec.name))
      {
        output_subtype = tcodec.guid;
        bitrate = std::clamp(bitrate, tcodec.min_bitrate, tcodec.max_bitrate);
        found = true;
        break;
      }
    }
    if (!found)
    {
      Error::SetStringFmt(error, "Unknown audio codec '{}'", codec);
      return false;
    }
  }

  HRESULT hr;
  if (FAILED(hr = wrap_MFCreateMediaType(input_type->GetAddressOf()))) [[unlikely]]
  {
    Error::SetHResult(error, "Audio MFCreateMediaType() failed: ", hr);
    return false;
  }

  const u32 block_align = AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8);
  const u32 bytes_per_second = block_align * sample_rate;

  if (FAILED(hr = (*input_type)->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
      FAILED(hr = (*input_type)->SetGUID(MF_MT_SUBTYPE, AUDIO_INPUT_MEDIA_FORMAT)) ||
      FAILED(hr = (*input_type)->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AUDIO_CHANNELS)) ||
      FAILED(hr = (*input_type)->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, AUDIO_BITS_PER_SAMPLE)) ||
      FAILED(hr = (*input_type)->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate)) ||
      FAILED(hr = (*input_type)->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align)) ||
      FAILED(hr = (*input_type)->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytes_per_second)) ||
      FAILED(hr = (*input_type)->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE))) [[unlikely]]
  {
    Error::SetHResult(error, "Audio setting attributes failed: ", hr);
    return false;
  }

  // If our input type is PCM, no need for an input type, it's the same as output.
  if (output_subtype == AUDIO_INPUT_MEDIA_FORMAT)
  {
    *output_type = std::move(*input_type);
    return true;
  }

  ComPtr<IMFCollection> output_types_collection;
  DWORD output_types_collection_size = 0;
  hr = wrap_MFTranscodeGetAudioOutputAvailableTypes(output_subtype, 0, nullptr, output_types_collection.GetAddressOf());
  if (FAILED(hr) || FAILED(hr = output_types_collection->GetElementCount(&output_types_collection_size))) [[unlikely]]
  {
    Error::SetHResult(error, "MFTranscodeGetAudioOutputAvailableTypes() failed: ", hr);
    return false;
  }

  std::vector<std::pair<ComPtr<IMFMediaType>, u32>> output_types;
  for (DWORD i = 0; i < output_types_collection_size; i++)
  {
    ComPtr<IUnknown> current_output_type;
    ComPtr<IMFMediaType> current_output_type_c;
    if (SUCCEEDED(hr = output_types_collection->GetElement(i, current_output_type.GetAddressOf())) &&
        SUCCEEDED(current_output_type.As(&current_output_type_c)))
    {
      UINT32 current_channel_count, current_sample_rate;
      if (SUCCEEDED(current_output_type_c->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &current_channel_count)) &&
          SUCCEEDED(current_output_type_c->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &current_sample_rate)) &&
          current_channel_count == AUDIO_CHANNELS && current_sample_rate == sample_rate)
      {
        u32 current_bitrate;
        if (SUCCEEDED(current_output_type_c->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &current_bitrate)))
          current_bitrate *= 8;
        else if (FAILED(current_output_type_c->GetUINT32(MF_MT_AVG_BITRATE, &current_bitrate)))
          continue;

        output_types.emplace_back(std::move(current_output_type_c), current_bitrate);
      }
    }
  }

  // pick the closest bitrate
  const u32 bitrate_kbps = bitrate * 1000;
  std::pair<ComPtr<IMFMediaType>, u32>* selected_output_type = nullptr;
  for (auto it = output_types.begin(); it != output_types.end(); ++it)
  {
    if (it->second >= bitrate_kbps &&
        (!selected_output_type || (selected_output_type->second - bitrate_kbps) > (it->second - bitrate_kbps)))
    {
      selected_output_type = &(*it);
    }
  }
  if (!selected_output_type)
  {
    Error::SetStringView(error, "Unable to find a matching audio output type.");
    return false;
  }

  *output_type = std::move(selected_output_type->first);
  return true;
}

bool MediaCaptureMF::ProcessAudioPackets(s64 video_pts, Error* error)
{
  const u32 max_audio_buffer_size = GetAudioBufferSizeInFrames();
  HRESULT hr;

  u32 pending_frames = m_audio_buffer_size.load(std::memory_order_acquire);
  while (pending_frames > 0 && (!IsCapturingVideo() ||
                                ((m_next_audio_pts * m_audio_sample_duration) < (video_pts * m_video_sample_duration))))
  {
    // Grab as many source frames as we can.
    const u32 contig_frames = std::min(pending_frames, max_audio_buffer_size - m_audio_buffer_read_pos);
    DebugAssert(contig_frames > 0);

    const u32 buffer_size = contig_frames * sizeof(s16) * AUDIO_CHANNELS;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(hr = wrap_MFCreateMemoryBuffer(buffer_size, buffer.GetAddressOf()))) [[unlikely]]
    {
      Error::SetHResult(error, "Audio MFCreateMemoryBuffer() failed: ", hr);
      return false;
    }

    BYTE* buffer_data;
    if (FAILED(hr = buffer->Lock(&buffer_data, nullptr, nullptr))) [[unlikely]]
    {
      Error::SetHResult(error, "Audio Lock() failed: ", hr);
      return false;
    }

    std::memcpy(buffer_data, &m_audio_buffer[m_audio_buffer_read_pos * AUDIO_CHANNELS], buffer_size);
    buffer->Unlock();

    if (FAILED(hr = buffer->SetCurrentLength(buffer_size))) [[unlikely]]
    {
      Error::SetHResult(error, "Audio SetCurrentLength() failed: ", hr);
      return false;
    }

    ComPtr<IMFSample> sample;
    if (FAILED(hr = wrap_MFCreateSample(sample.GetAddressOf()))) [[unlikely]]
    {
      Error::SetHResult(error, "Audio MFCreateSample() failed: ", hr);
      return false;
    }

    if (FAILED(hr = sample->AddBuffer(buffer.Get()))) [[unlikely]]
    {
      Error::SetHResult(error, "Audio AddBuffer() failed: ", hr);
      return false;
    }

    if (FAILED(hr = sample->SetSampleTime(ConvertPTSToTimestamp(m_next_audio_pts, m_audio_sample_duration))))
      [[unlikely]]
    {
      Error::SetHResult(error, "Audio SetSampleTime() failed: ", hr);
      return false;
    }

    if (FAILED(hr = sample->SetSampleDuration(ConvertFramesToDuration(contig_frames, m_audio_sample_duration))))
      [[unlikely]]
    {
      Error::SetHResult(error, "Audio SetSampleDuration() failed: ", hr);
      return false;
    }

    m_next_audio_pts += contig_frames;

    hr = m_sink_writer->WriteSample(m_audio_stream_index, sample.Get());
    if (FAILED(hr)) [[unlikely]]
    {
      Error::SetHResult(error, "Audio WriteSample() failed: ", hr);
      return false;
    }

    m_audio_buffer_read_pos = (m_audio_buffer_read_pos + contig_frames) % max_audio_buffer_size;
    m_audio_buffer_size.fetch_sub(contig_frames, std::memory_order_acq_rel);
    m_audio_frame_pos += contig_frames;
    pending_frames -= contig_frames;
  }

  return true;
}

#endif

#ifndef __ANDROID__

// We're using deprecated fields because we're targeting multiple ffmpeg versions.
#if defined(_MSC_VER)
#pragma warning(disable : 4996) // warning C4996: 'AVCodecContext::channels': was declared deprecated
#elif defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

// Compatibility with both ffmpeg 4.x and 5.x.
#if (LIBAVFORMAT_VERSION_MAJOR < 59)
#define ff_const59
#else
#define ff_const59 const
#endif

#define VISIT_AVCODEC_IMPORTS(X)                                                                                       \
  X(avcodec_find_encoder_by_name)                                                                                      \
  X(avcodec_find_encoder)                                                                                              \
  X(avcodec_alloc_context3)                                                                                            \
  X(avcodec_open2)                                                                                                     \
  X(avcodec_free_context)                                                                                              \
  X(avcodec_send_frame)                                                                                                \
  X(avcodec_receive_packet)                                                                                            \
  X(avcodec_parameters_from_context)                                                                                   \
  X(avcodec_get_hw_config)                                                                                             \
  X(av_codec_iterate)                                                                                                  \
  X(av_packet_alloc)                                                                                                   \
  X(av_packet_free)                                                                                                    \
  X(av_packet_rescale_ts)                                                                                              \
  X(av_packet_unref)

#define VISIT_AVFORMAT_IMPORTS(X)                                                                                      \
  X(avformat_alloc_output_context2)                                                                                    \
  X(avformat_new_stream)                                                                                               \
  X(avformat_write_header)                                                                                             \
  X(av_guess_format)                                                                                                   \
  X(av_interleaved_write_frame)                                                                                        \
  X(av_write_trailer)                                                                                                  \
  X(avformat_free_context)                                                                                             \
  X(avformat_query_codec)                                                                                              \
  X(avio_open)                                                                                                         \
  X(avio_closep)

#if LIBAVUTIL_VERSION_MAJOR < 57
#define AVUTIL_57_IMPORTS(X)
#else
#define AVUTIL_57_IMPORTS(X)                                                                                           \
  X(av_channel_layout_default)                                                                                         \
  X(av_channel_layout_copy)                                                                                            \
  X(av_opt_set_chlayout)
#endif

#define VISIT_AVUTIL_IMPORTS(X)                                                                                        \
  AVUTIL_57_IMPORTS(X)                                                                                                 \
  X(av_frame_alloc)                                                                                                    \
  X(av_frame_get_buffer)                                                                                               \
  X(av_frame_free)                                                                                                     \
  X(av_frame_make_writable)                                                                                            \
  X(av_strerror)                                                                                                       \
  X(av_reduce)                                                                                                         \
  X(av_dict_parse_string)                                                                                              \
  X(av_dict_get)                                                                                                       \
  X(av_dict_free)                                                                                                      \
  X(av_opt_set_int)                                                                                                    \
  X(av_opt_set_sample_fmt)                                                                                             \
  X(av_compare_ts)                                                                                                     \
  X(av_get_bytes_per_sample)                                                                                           \
  X(av_sample_fmt_is_planar)                                                                                           \
  X(av_d2q)                                                                                                            \
  X(av_hwdevice_get_type_name)                                                                                         \
  X(av_hwdevice_ctx_create)                                                                                            \
  X(av_hwframe_ctx_alloc)                                                                                              \
  X(av_hwframe_ctx_init)                                                                                               \
  X(av_hwframe_transfer_data)                                                                                          \
  X(av_hwframe_get_buffer)                                                                                             \
  X(av_buffer_ref)                                                                                                     \
  X(av_buffer_unref)

#define VISIT_SWSCALE_IMPORTS(X)                                                                                       \
  X(sws_getCachedContext)                                                                                              \
  X(sws_scale)                                                                                                         \
  X(sws_freeContext)

#define VISIT_SWRESAMPLE_IMPORTS(X)                                                                                    \
  X(swr_alloc)                                                                                                         \
  X(swr_init)                                                                                                          \
  X(swr_free)                                                                                                          \
  X(swr_convert)                                                                                                       \
  X(swr_next_pts)

class MediaCaptureFFmpeg final : public MediaCaptureBase
{
public:
  ~MediaCaptureFFmpeg() override = default;

  static std::unique_ptr<MediaCapture> Create(Error* error);
  static ContainerList GetContainerList();
  static CodecList GetVideoCodecList(const char* container);
  static CodecList GetAudioCodecList(const char* container);

  bool IsCapturingAudio() const override;
  bool IsCapturingVideo() const override;
  time_t GetElapsedTime() const override;

protected:
  void ClearState() override;
  bool SendFrame(const PendingFrame& pf, Error* error) override;
  bool ProcessAudioPackets(s64 video_pts, Error* error) override;
  bool InternalBeginCapture(float fps, float aspect, u32 sample_rate, bool capture_video, std::string_view video_codec,
                            u32 video_bitrate, std::string_view video_codec_args, bool capture_audio,
                            std::string_view audio_codec, u32 audio_bitrate, std::string_view audio_codec_args,
                            Error* error) override;
  bool InternalEndCapture(std::unique_lock<std::mutex>& lock, Error* error) override;

private:
  static void SetAVError(Error* error, std::string_view prefix, int errnum);
  static CodecList GetCodecListForContainer(const char* container, AVMediaType type);

  bool IsUsingHardwareVideoEncoding();

  bool ReceivePackets(AVCodecContext* codec_context, AVStream* stream, AVPacket* packet, Error* error);

  AVFormatContext* m_format_context = nullptr;

  AVCodecContext* m_video_codec_context = nullptr;
  AVStream* m_video_stream = nullptr;
  AVFrame* m_converted_video_frame = nullptr; // YUV
  AVFrame* m_hw_video_frame = nullptr;
  AVPacket* m_video_packet = nullptr;
  SwsContext* m_sws_context = nullptr;
  AVDictionary* m_video_codec_arguments = nullptr;
  AVBufferRef* m_video_hw_context = nullptr;
  AVBufferRef* m_video_hw_frames = nullptr;

  AVCodecContext* m_audio_codec_context = nullptr;
  AVStream* m_audio_stream = nullptr;
  AVFrame* m_converted_audio_frame = nullptr;
  AVPacket* m_audio_packet = nullptr;
  SwrContext* m_swr_context = nullptr;
  AVDictionary* m_audio_codec_arguments = nullptr;

  AVPixelFormat m_video_pixel_format = AV_PIX_FMT_NONE;
  u32 m_audio_frame_bps = 0;
  bool m_audio_frame_planar = false;

#define DECLARE_IMPORT(X) static inline decltype(X)* wrap_##X;
  VISIT_AVCODEC_IMPORTS(DECLARE_IMPORT);
  VISIT_AVFORMAT_IMPORTS(DECLARE_IMPORT);
  VISIT_AVUTIL_IMPORTS(DECLARE_IMPORT);
  VISIT_SWSCALE_IMPORTS(DECLARE_IMPORT);
  VISIT_SWRESAMPLE_IMPORTS(DECLARE_IMPORT);
#undef DECLARE_IMPORT

  static bool LoadFFmpeg(Error* error);
  static void UnloadFFmpeg();

  static inline DynamicLibrary s_avcodec_library;
  static inline DynamicLibrary s_avformat_library;
  static inline DynamicLibrary s_avutil_library;
  static inline DynamicLibrary s_swscale_library;
  static inline DynamicLibrary s_swresample_library;
  static inline bool s_library_loaded = false;
};

bool MediaCaptureFFmpeg::LoadFFmpeg(Error* error)
{
  std::unique_lock lock(s_load_mutex);
  if (s_library_loaded)
    return true;

  static constexpr auto open_dynlib = [](DynamicLibrary& lib, const char* name, int major_version, Error* error) {
    std::string full_name(DynamicLibrary::GetVersionedFilename(name, major_version));
    return lib.Open(full_name.c_str(), error);
  };

  bool result = true;

  result = result && open_dynlib(s_avutil_library, "avutil", LIBAVUTIL_VERSION_MAJOR, error);
  result = result && open_dynlib(s_avcodec_library, "avcodec", LIBAVCODEC_VERSION_MAJOR, error);
  result = result && open_dynlib(s_avformat_library, "avformat", LIBAVFORMAT_VERSION_MAJOR, error);
  result = result && open_dynlib(s_swscale_library, "swscale", LIBSWSCALE_VERSION_MAJOR, error);
  result = result && open_dynlib(s_swresample_library, "swresample", LIBSWRESAMPLE_VERSION_MAJOR, error);

#define RESOLVE_IMPORT(X) result = result && s_avcodec_library.GetSymbol(#X, &wrap_##X);
  VISIT_AVCODEC_IMPORTS(RESOLVE_IMPORT);
#undef RESOLVE_IMPORT

#define RESOLVE_IMPORT(X) result = result && s_avformat_library.GetSymbol(#X, &wrap_##X);
  VISIT_AVFORMAT_IMPORTS(RESOLVE_IMPORT);
#undef RESOLVE_IMPORT

#define RESOLVE_IMPORT(X) result = result && s_avutil_library.GetSymbol(#X, &wrap_##X);
  VISIT_AVUTIL_IMPORTS(RESOLVE_IMPORT);
#undef RESOLVE_IMPORT

#define RESOLVE_IMPORT(X) result = result && s_swscale_library.GetSymbol(#X, &wrap_##X);
  VISIT_SWSCALE_IMPORTS(RESOLVE_IMPORT);
#undef RESOLVE_IMPORT

#define RESOLVE_IMPORT(X) result = result && s_swresample_library.GetSymbol(#X, &wrap_##X);
  VISIT_SWRESAMPLE_IMPORTS(RESOLVE_IMPORT);
#undef RESOLVE_IMPORT

  if (result)
  {
    s_library_loaded = true;
    std::atexit(&MediaCaptureFFmpeg::UnloadFFmpeg);
    return true;
  }

  UnloadFFmpeg();

  Error::SetStringFmt(
    error,
    TRANSLATE_FS(
      "MediaCapture",
      "You may be missing one or more files, or are using the incorrect version. This build of DuckStation requires:\n"
      "  libavcodec: {}\n"
      "  libavformat: {}\n"
      "  libavutil: {}\n"
      "  libswscale: {}\n"
      "  libswresample: {}\n"),
    LIBAVCODEC_VERSION_MAJOR, LIBAVFORMAT_VERSION_MAJOR, LIBAVUTIL_VERSION_MAJOR, LIBSWSCALE_VERSION_MAJOR,
    LIBSWRESAMPLE_VERSION_MAJOR);
  return false;
}

void MediaCaptureFFmpeg::UnloadFFmpeg()
{
#define CLEAR_IMPORT(X) wrap_##X = nullptr;
  VISIT_AVCODEC_IMPORTS(CLEAR_IMPORT);
  VISIT_AVFORMAT_IMPORTS(CLEAR_IMPORT);
  VISIT_AVUTIL_IMPORTS(CLEAR_IMPORT);
  VISIT_SWSCALE_IMPORTS(CLEAR_IMPORT);
  VISIT_SWRESAMPLE_IMPORTS(CLEAR_IMPORT);
#undef CLEAR_IMPORT

  s_swresample_library.Close();
  s_swscale_library.Close();
  s_avutil_library.Close();
  s_avformat_library.Close();
  s_avcodec_library.Close();
  s_library_loaded = false;
}

#undef VISIT_AVCODEC_IMPORTS
#undef VISIT_AVFORMAT_IMPORTS
#undef VISIT_AVUTIL_IMPORTS
#undef VISIT_SWSCALE_IMPORTS
#undef VISIT_SWRESAMPLE_IMPORTS

void MediaCaptureFFmpeg::SetAVError(Error* error, std::string_view prefix, int errnum)
{
  char errbuf[128];
  wrap_av_strerror(errnum, errbuf, sizeof(errbuf));

  Error::SetStringFmt(error, "{} {}", prefix, errbuf);
}

bool MediaCaptureFFmpeg::IsCapturingAudio() const
{
  return (m_audio_stream != nullptr);
}

bool MediaCaptureFFmpeg::IsCapturingVideo() const
{
  return (m_video_stream != nullptr);
}

time_t MediaCaptureFFmpeg::GetElapsedTime() const
{
  std::unique_lock<std::mutex> lock(m_lock);
  s64 seconds;
  if (m_video_stream)
  {
    seconds = (m_next_video_pts * static_cast<s64>(m_video_codec_context->time_base.num)) /
              static_cast<s64>(m_video_codec_context->time_base.den);
  }
  else
  {
    DebugAssert(IsCapturingAudio());
    seconds = (m_next_audio_pts * static_cast<s64>(m_audio_codec_context->time_base.num)) /
              static_cast<s64>(m_audio_codec_context->time_base.den);
  }

  return seconds;
}

bool MediaCaptureFFmpeg::IsUsingHardwareVideoEncoding()
{
  return (m_video_hw_context != nullptr);
}

bool MediaCaptureFFmpeg::InternalBeginCapture(float fps, float aspect, u32 sample_rate, bool capture_video,
                                              std::string_view video_codec, u32 video_bitrate,
                                              std::string_view video_codec_args, bool capture_audio,
                                              std::string_view audio_codec, u32 audio_bitrate,
                                              std::string_view audio_codec_args, Error* error)
{
  ff_const59 AVOutputFormat* output_format = wrap_av_guess_format(nullptr, m_path.c_str(), nullptr);
  if (!output_format)
  {
    Error::SetStringFmt(error, "Failed to get output format for '{}'", Path::GetFileName(m_path));
    return false;
  }

  int res = wrap_avformat_alloc_output_context2(&m_format_context, output_format, nullptr, m_path.c_str());
  if (res < 0)
  {
    SetAVError(error, "avformat_alloc_output_context2() failed: ", res);
    return false;
  }

  // find the codec id
  if (capture_video)
  {
    const AVCodec* vcodec = nullptr;
    if (!video_codec.empty())
    {
      vcodec = wrap_avcodec_find_encoder_by_name(TinyString(video_codec).c_str());
      if (!vcodec)
      {
        Error::SetStringFmt(error, "Video codec {} not found.", video_codec);
        return false;
      }
    }

    // FFmpeg decides whether mp4, mkv, etc should use h264 or mpeg4 as their default codec by whether x264 was enabled
    // But there's a lot of other h264 encoders (e.g. hardware encoders) we may want to use instead
    if (!vcodec && wrap_avformat_query_codec(output_format, AV_CODEC_ID_H264, FF_COMPLIANCE_NORMAL))
      vcodec = wrap_avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!vcodec)
      vcodec = wrap_avcodec_find_encoder(output_format->video_codec);

    if (!vcodec)
    {
      Error::SetStringView(error, "Failed to find video encoder.");
      return false;
    }

    m_video_codec_context = wrap_avcodec_alloc_context3(vcodec);
    if (!m_video_codec_context)
    {
      Error::SetStringView(error, "Failed to allocate video codec context.");
      return false;
    }

    m_video_codec_context->codec_type = AVMEDIA_TYPE_VIDEO;
    m_video_codec_context->bit_rate = video_bitrate * 1000;
    m_video_codec_context->width = m_video_width;
    m_video_codec_context->height = m_video_height;
    m_video_codec_context->sample_aspect_ratio = wrap_av_d2q(aspect, 100000);
    wrap_av_reduce(&m_video_codec_context->time_base.num, &m_video_codec_context->time_base.den, 10000,
                   static_cast<s64>(static_cast<double>(fps) * 10000.0), std::numeric_limits<s32>::max());

    // Map input pixel format.
    static constexpr const std::pair<GPUTexture::Format, AVPixelFormat> texture_pf_mapping[] = {
      {GPUTexture::Format::RGBA8, AV_PIX_FMT_RGBA},
      {GPUTexture::Format::BGRA8, AV_PIX_FMT_BGRA},
    };
    if (const auto pf_mapping =
          std::find_if(std::begin(texture_pf_mapping), std::end(texture_pf_mapping),
                       [this](const auto& it) { return (it.first == m_video_render_texture_format); });
        pf_mapping != std::end(texture_pf_mapping))
    {
      m_video_pixel_format = pf_mapping->second;
    }
    else
    {
      Error::SetStringFmt(error, "Unhandled input pixel format {}",
                          GPUTexture::GetFormatName(m_video_render_texture_format));
      return false;
    }

    // Default to YUV 4:2:0 if the codec doesn't specify a pixel format.
    AVPixelFormat sw_pix_fmt = AV_PIX_FMT_YUV420P;
    if (vcodec->pix_fmts)
    {
      // Prefer YUV420 given the choice, but otherwise fall back to whatever it supports.
      sw_pix_fmt = vcodec->pix_fmts[0];
      for (u32 i = 0; vcodec->pix_fmts[i] != AV_PIX_FMT_NONE; i++)
      {
        if (vcodec->pix_fmts[i] == AV_PIX_FMT_YUV420P)
        {
          sw_pix_fmt = vcodec->pix_fmts[i];
          break;
        }
      }
    }
    m_video_codec_context->pix_fmt = sw_pix_fmt;

    // Can we use hardware encoding?
    const AVCodecHWConfig* hwconfig = wrap_avcodec_get_hw_config(vcodec, 0);
    if (hwconfig && hwconfig->pix_fmt != AV_PIX_FMT_NONE && hwconfig->pix_fmt != sw_pix_fmt)
    {
      // First index isn't our preferred pixel format, try the others, but fall back if one doesn't exist.
      int index = 1;
      while (const AVCodecHWConfig* next_hwconfig = wrap_avcodec_get_hw_config(vcodec, index++))
      {
        if (next_hwconfig->pix_fmt == sw_pix_fmt)
        {
          hwconfig = next_hwconfig;
          break;
        }
      }
    }

    if (hwconfig)
    {
      Error hw_error;

      INFO_LOG("Trying to use {} hardware device for video encoding.",
               wrap_av_hwdevice_get_type_name(hwconfig->device_type));
      res = wrap_av_hwdevice_ctx_create(&m_video_hw_context, hwconfig->device_type, nullptr, nullptr, 0);
      if (res < 0)
      {
        SetAVError(&hw_error, "av_hwdevice_ctx_create() failed: ", res);
        ERROR_LOG(hw_error.GetDescription());
      }
      else
      {
        m_video_hw_frames = wrap_av_hwframe_ctx_alloc(m_video_hw_context);
        if (!m_video_hw_frames)
        {
          ERROR_LOG("s_video_hw_frames() failed");
          wrap_av_buffer_unref(&m_video_hw_context);
        }
        else
        {
          AVHWFramesContext* frames_ctx = reinterpret_cast<AVHWFramesContext*>(m_video_hw_frames->data);
          frames_ctx->format = (hwconfig->pix_fmt != AV_PIX_FMT_NONE) ? hwconfig->pix_fmt : sw_pix_fmt;
          frames_ctx->sw_format = sw_pix_fmt;
          frames_ctx->width = m_video_codec_context->width;
          frames_ctx->height = m_video_codec_context->height;
          res = wrap_av_hwframe_ctx_init(m_video_hw_frames);
          if (res < 0)
          {
            SetAVError(&hw_error, "av_hwframe_ctx_init() failed: ", res);
            ERROR_LOG(hw_error.GetDescription());
            wrap_av_buffer_unref(&m_video_hw_frames);
            wrap_av_buffer_unref(&m_video_hw_context);
          }
          else
          {
            m_video_codec_context->hw_frames_ctx = wrap_av_buffer_ref(m_video_hw_frames);
            if (hwconfig->pix_fmt != AV_PIX_FMT_NONE)
              m_video_codec_context->pix_fmt = hwconfig->pix_fmt;
          }
        }
      }

      if (!m_video_hw_context)
      {
        ERROR_LOG("Failed to create hardware encoder, using software encoding.");
        hwconfig = nullptr;
      }
    }

    if (!video_codec_args.empty())
    {
      res = wrap_av_dict_parse_string(&m_video_codec_arguments, SmallString(video_codec_args).c_str(), "=", ":", 0);
      if (res < 0)
      {
        SetAVError(error, "av_dict_parse_string() for video failed: ", res);
        return false;
      }
    }

    if (output_format->flags & AVFMT_GLOBALHEADER)
      m_video_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    bool has_pixel_format_override = wrap_av_dict_get(m_video_codec_arguments, "pixel_format", nullptr, 0);

    res = wrap_avcodec_open2(m_video_codec_context, vcodec, &m_video_codec_arguments);
    if (res < 0)
    {
      SetAVError(error, "avcodec_open2() for video failed: ", res);
      return false;
    }

    // If the user overrode the pixel format, get that now
    if (has_pixel_format_override)
      sw_pix_fmt = m_video_codec_context->pix_fmt;

    m_converted_video_frame = wrap_av_frame_alloc();
    m_hw_video_frame = IsUsingHardwareVideoEncoding() ? wrap_av_frame_alloc() : nullptr;
    if (!m_converted_video_frame || (IsUsingHardwareVideoEncoding() && !m_hw_video_frame))
    {
      SetAVError(error, "Failed to allocate frame: ", AVERROR(ENOMEM));
      return false;
    }

    m_converted_video_frame->format = sw_pix_fmt;
    m_converted_video_frame->width = m_video_codec_context->width;
    m_converted_video_frame->height = m_video_codec_context->height;
    res = wrap_av_frame_get_buffer(m_converted_video_frame, 0);
    if (res < 0)
    {
      SetAVError(error, "av_frame_get_buffer() for converted frame failed: ", res);
      return false;
    }

    if (IsUsingHardwareVideoEncoding())
    {
      m_hw_video_frame->format = m_video_codec_context->pix_fmt;
      m_hw_video_frame->width = m_video_codec_context->width;
      m_hw_video_frame->height = m_video_codec_context->height;
      res = wrap_av_hwframe_get_buffer(m_video_hw_frames, m_hw_video_frame, 0);
      if (res < 0)
      {
        SetAVError(error, "av_frame_get_buffer() for HW frame failed: ", res);
        return false;
      }
    }

    m_video_stream = wrap_avformat_new_stream(m_format_context, vcodec);
    if (!m_video_stream)
    {
      SetAVError(error, "avformat_new_stream() for video failed: ", res);
      return false;
    }

    res = wrap_avcodec_parameters_from_context(m_video_stream->codecpar, m_video_codec_context);
    if (res < 0)
    {
      SetAVError(error, "avcodec_parameters_from_context() for video failed: ", AVERROR(ENOMEM));
      return false;
    }

    m_video_stream->time_base = m_video_codec_context->time_base;
    m_video_stream->sample_aspect_ratio = m_video_codec_context->sample_aspect_ratio;

    m_video_packet = wrap_av_packet_alloc();
    if (!m_video_packet)
    {
      SetAVError(error, "av_packet_alloc() for video failed: ", AVERROR(ENOMEM));
      return false;
    }
  }

  if (capture_audio)
  {
    const AVCodec* acodec = nullptr;
    if (!audio_codec.empty())
    {
      acodec = wrap_avcodec_find_encoder_by_name(TinyString(audio_codec).c_str());
      if (!acodec)
      {
        Error::SetStringFmt(error, "Audio codec {} not found.", video_codec);
        return false;
      }
    }
    if (!acodec)
      acodec = wrap_avcodec_find_encoder(output_format->audio_codec);
    if (!acodec)
    {
      Error::SetStringView(error, "Failed to find audio encoder.");
      return false;
    }

    m_audio_codec_context = wrap_avcodec_alloc_context3(acodec);
    if (!m_audio_codec_context)
    {
      Error::SetStringView(error, "Failed to allocate audio codec context.");
      return false;
    }

    m_audio_codec_context->codec_type = AVMEDIA_TYPE_AUDIO;
    m_audio_codec_context->bit_rate = audio_bitrate * 1000;
    m_audio_codec_context->sample_fmt = AV_SAMPLE_FMT_S16;
    m_audio_codec_context->sample_rate = sample_rate;
    m_audio_codec_context->time_base = {1, static_cast<int>(sample_rate)};
#if LIBAVUTIL_VERSION_MAJOR < 57
    m_audio_codec_context->channels = AUDIO_CHANNELS;
    m_audio_codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
#else
    wrap_av_channel_layout_default(&m_audio_codec_context->ch_layout, AUDIO_CHANNELS);
#endif

    bool supports_format = false;
    for (const AVSampleFormat* p = acodec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; p++)
    {
      if (*p == m_audio_codec_context->sample_fmt)
      {
        supports_format = true;
        break;
      }
    }
    if (!supports_format)
    {
      WARNING_LOG("Audio codec '{}' does not support S16 samples, using default.", acodec->name);
      m_audio_codec_context->sample_fmt = acodec->sample_fmts[0];
      m_swr_context = wrap_swr_alloc();
      if (!m_swr_context)
      {
        SetAVError(error, "swr_alloc() failed: ", AVERROR(ENOMEM));
        return false;
      }

      wrap_av_opt_set_int(m_swr_context, "in_channel_count", AUDIO_CHANNELS, 0);
      wrap_av_opt_set_int(m_swr_context, "in_sample_rate", sample_rate, 0);
      wrap_av_opt_set_sample_fmt(m_swr_context, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
      wrap_av_opt_set_int(m_swr_context, "out_channel_count", AUDIO_CHANNELS, 0);
      wrap_av_opt_set_int(m_swr_context, "out_sample_rate", sample_rate, 0);
      wrap_av_opt_set_sample_fmt(m_swr_context, "out_sample_fmt", m_audio_codec_context->sample_fmt, 0);

#if LIBAVUTIL_VERSION_MAJOR >= 59
      wrap_av_opt_set_chlayout(m_swr_context, "in_chlayout", &m_audio_codec_context->ch_layout, 0);
      wrap_av_opt_set_chlayout(m_swr_context, "out_chlayout", &m_audio_codec_context->ch_layout, 0);
#endif

      res = wrap_swr_init(m_swr_context);
      if (res < 0)
      {
        SetAVError(error, "swr_init() failed: ", res);
        return false;
      }
    }

    // TODO: Check channel layout support

    if (!audio_codec_args.empty())
    {
      res = wrap_av_dict_parse_string(&m_audio_codec_arguments, SmallString(audio_codec_args).c_str(), "=", ":", 0);
      if (res < 0)
      {
        SetAVError(error, "av_dict_parse_string() for audio failed: ", res);
        return false;
      }
    }

    if (output_format->flags & AVFMT_GLOBALHEADER)
      m_audio_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    res = wrap_avcodec_open2(m_audio_codec_context, acodec, &m_audio_codec_arguments);
    if (res < 0)
    {
      SetAVError(error, "avcodec_open2() for audio failed: ", res);
      return false;
    }

    // Use packet size for frame if it supports it... but most don't.
    if (acodec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
      m_audio_frame_size = static_cast<u32>(static_cast<float>(sample_rate) / fps);
    else
      m_audio_frame_size = m_audio_codec_context->frame_size;
    if (m_audio_frame_size >= m_audio_buffer.size())
    {
      SetAVError(error,
                 TinyString::from_format("Audio frame size {} exceeds buffer size {}", m_audio_frame_size,
                                         m_audio_buffer.size()),
                 AVERROR(EINVAL));
      return false;
    }

    m_audio_frame_bps = wrap_av_get_bytes_per_sample(m_audio_codec_context->sample_fmt);
    m_audio_frame_planar = (wrap_av_sample_fmt_is_planar(m_audio_codec_context->sample_fmt) != 0);

    m_converted_audio_frame = wrap_av_frame_alloc();
    if (!m_converted_audio_frame)
    {
      SetAVError(error, "Failed to allocate audio frame: ", AVERROR(ENOMEM));
      return false;
    }

    m_converted_audio_frame->format = m_audio_codec_context->sample_fmt;
    m_converted_audio_frame->nb_samples = m_audio_frame_size;
#if LIBAVUTIL_VERSION_MAJOR < 57
    m_converted_audio_frame->channels = AUDIO_CHANNELS;
    m_converted_audio_frame->channel_layout = m_audio_codec_context->channel_layout;
#else
    wrap_av_channel_layout_copy(&m_converted_audio_frame->ch_layout, &m_audio_codec_context->ch_layout);
#endif
    res = wrap_av_frame_get_buffer(m_converted_audio_frame, 0);
    if (res < 0)
    {
      SetAVError(error, "av_frame_get_buffer() for audio frame failed: ", res);
      return false;
    }

    m_audio_stream = wrap_avformat_new_stream(m_format_context, acodec);
    if (!m_audio_stream)
    {
      SetAVError(error, "avformat_new_stream() for audio failed: ", AVERROR(ENOMEM));
      return false;
    }

    res = wrap_avcodec_parameters_from_context(m_audio_stream->codecpar, m_audio_codec_context);
    if (res < 0)
    {
      SetAVError(error, "avcodec_parameters_from_context() for audio failed: ", res);
      return false;
    }

    m_audio_stream->time_base = m_audio_codec_context->time_base;

    m_audio_packet = wrap_av_packet_alloc();
    if (!m_audio_packet)
    {
      SetAVError(error, "av_packet_alloc() for audio failed: ", AVERROR(ENOMEM));
      return false;
    }
  }

  res = wrap_avio_open(&m_format_context->pb, m_path.c_str(), AVIO_FLAG_WRITE);
  if (res < 0)
  {
    SetAVError(error, "avio_open() failed: ", res);
    return false;
  }

  res = wrap_avformat_write_header(m_format_context, nullptr);
  if (res < 0)
  {
    SetAVError(error, "avformat_write_header() failed: ", res);
    return false;
  }

  return true;
}

bool MediaCaptureFFmpeg::InternalEndCapture(std::unique_lock<std::mutex>& lock, Error* error)
{
  int res = MediaCaptureBase::InternalEndCapture(lock, error) ? 0 : -1;
  if (res == 0)
  {
    // end of stream
    if (m_video_stream)
    {
      res = wrap_avcodec_send_frame(m_video_codec_context, nullptr);
      if (res < 0)
        SetAVError(error, "avcodec_send_frame() for video EOS failed: ", res);
      else
        res = ReceivePackets(m_video_codec_context, m_video_stream, m_video_packet, error) ? 0 : -1;
    }
    if (m_audio_stream)
    {
      res = wrap_avcodec_send_frame(m_audio_codec_context, nullptr);
      if (res < 0)
        SetAVError(error, "avcodec_send_frame() for audio EOS failed: ", res);
      else
        res = ReceivePackets(m_audio_codec_context, m_audio_stream, m_audio_packet, error) ? 0 : -1;
    }

    // end of file!
    if (res == 0)
    {
      res = wrap_av_write_trailer(m_format_context);
      if (res < 0)
        SetAVError(error, "av_write_trailer() failed: ", res);
    }
  }

  return (res == 0);
}

void MediaCaptureFFmpeg::ClearState()
{
  if (m_format_context)
  {
    int res = wrap_avio_closep(&m_format_context->pb);
    if (res < 0) [[unlikely]]
    {
      Error close_error;
      SetAVError(&close_error, "avio_closep() failed: ", res);
      ERROR_LOG(close_error.GetDescription());
    }
  }

  if (m_sws_context)
  {
    wrap_sws_freeContext(m_sws_context);
    m_sws_context = nullptr;
  }
  if (m_video_packet)
    wrap_av_packet_free(&m_video_packet);
  if (m_converted_video_frame)
    wrap_av_frame_free(&m_converted_video_frame);
  if (m_hw_video_frame)
    wrap_av_frame_free(&m_hw_video_frame);
  if (m_video_hw_frames)
    wrap_av_buffer_unref(&m_video_hw_frames);
  if (m_video_hw_context)
    wrap_av_buffer_unref(&m_video_hw_context);
  if (m_video_codec_context)
    wrap_avcodec_free_context(&m_video_codec_context);
  m_video_stream = nullptr;

  if (m_swr_context)
    wrap_swr_free(&m_swr_context);
  if (m_audio_packet)
    wrap_av_packet_free(&m_audio_packet);
  if (m_converted_audio_frame)
    wrap_av_frame_free(&m_converted_audio_frame);
  if (m_audio_codec_context)
    wrap_avcodec_free_context(&m_audio_codec_context);
  m_audio_stream = nullptr;

  if (m_format_context)
  {
    wrap_avformat_free_context(m_format_context);
    m_format_context = nullptr;
  }
  if (m_video_codec_arguments)
    wrap_av_dict_free(&m_video_codec_arguments);
  if (m_audio_codec_arguments)
    wrap_av_dict_free(&m_audio_codec_arguments);
}

bool MediaCaptureFFmpeg::ReceivePackets(AVCodecContext* codec_context, AVStream* stream, AVPacket* packet, Error* error)
{
  for (;;)
  {
    int res = wrap_avcodec_receive_packet(codec_context, packet);
    if (res == AVERROR(EAGAIN) || res == AVERROR_EOF)
    {
      // no more data available
      break;
    }
    else if (res < 0) [[unlikely]]
    {
      SetAVError(error, "avcodec_receive_packet() failed: ", res);
      return false;
    }

    packet->stream_index = stream->index;

    // in case the frame rate changed...
    wrap_av_packet_rescale_ts(packet, codec_context->time_base, stream->time_base);

    res = wrap_av_interleaved_write_frame(m_format_context, packet);
    if (res < 0) [[unlikely]]
    {
      SetAVError(error, "av_interleaved_write_frame() failed: ", res);
      return false;
    }

    wrap_av_packet_unref(packet);
  }

  return true;
}

bool MediaCaptureFFmpeg::SendFrame(const PendingFrame& pf, Error* error)
{
  const u8* source_ptr = pf.tex->GetMapPointer();
  const int source_width = static_cast<int>(pf.tex->GetWidth());
  const int source_height = static_cast<int>(pf.tex->GetHeight());

  // OpenGL lower-left flip.
  int source_pitch = static_cast<int>(pf.tex->GetMapPitch());
  if (g_gpu_device->UsesLowerLeftOrigin())
  {
    source_ptr = source_ptr + static_cast<size_t>(source_pitch) * static_cast<u32>(source_height - 1);
    source_pitch = -source_pitch;
  }

  // In case a previous frame is still using the frame.
  wrap_av_frame_make_writable(m_converted_video_frame);

  m_sws_context = wrap_sws_getCachedContext(m_sws_context, source_width, source_height, m_video_pixel_format,
                                            m_converted_video_frame->width, m_converted_video_frame->height,
                                            static_cast<AVPixelFormat>(m_converted_video_frame->format), SWS_BICUBIC,
                                            nullptr, nullptr, nullptr);
  if (!m_sws_context) [[unlikely]]
  {
    Error::SetStringView(error, "sws_getCachedContext() failed");
    return false;
  }

  wrap_sws_scale(m_sws_context, reinterpret_cast<const u8**>(&source_ptr), &source_pitch, 0, source_height,
                 m_converted_video_frame->data, m_converted_video_frame->linesize);

  AVFrame* frame_to_send = m_converted_video_frame;
  if (IsUsingHardwareVideoEncoding())
  {
    // Need to transfer the frame to hardware.
    const int res = wrap_av_hwframe_transfer_data(m_hw_video_frame, m_converted_video_frame, 0);
    if (res < 0) [[unlikely]]
    {
      SetAVError(error, "av_hwframe_transfer_data() failed: ", res);
      return false;
    }

    frame_to_send = m_hw_video_frame;
  }

  // Set the correct PTS before handing it off.
  frame_to_send->pts = pf.pts;

  const int res = wrap_avcodec_send_frame(m_video_codec_context, frame_to_send);
  if (res < 0) [[unlikely]]
  {
    SetAVError(error, "avcodec_send_frame() failed: ", res);
    return false;
  }

  return ReceivePackets(m_video_codec_context, m_video_stream, m_video_packet, error);
}

bool MediaCaptureFFmpeg::ProcessAudioPackets(s64 video_pts, Error* error)
{
  const u32 max_audio_buffer_size = GetAudioBufferSizeInFrames();

  u32 pending_frames = m_audio_buffer_size.load(std::memory_order_acquire);
  while (pending_frames > 0 &&
         (!m_video_codec_context || wrap_av_compare_ts(video_pts, m_video_codec_context->time_base, m_next_audio_pts,
                                                       m_audio_codec_context->time_base) > 0))
  {
    // In case the encoder is still using it.
    if (m_audio_frame_pos == 0)
      wrap_av_frame_make_writable(m_converted_audio_frame);

    // Grab as many source frames as we can.
    const u32 contig_frames = std::min(pending_frames, max_audio_buffer_size - m_audio_buffer_read_pos);
    const u32 this_batch = std::min(m_audio_frame_size - m_audio_frame_pos, contig_frames);

    // Do we need to convert the sample format?
    if (!m_swr_context)
    {
      // No, just copy frames out of staging buffer.
      if (m_audio_frame_planar)
      {
        // This is slow. Hopefully doesn't happen in too many configurations.
        for (u32 i = 0; i < AUDIO_CHANNELS; i++)
        {
          u8* output = m_converted_audio_frame->data[i] + m_audio_frame_pos * m_audio_frame_bps;
          const u8* input = reinterpret_cast<u8*>(&m_audio_buffer[m_audio_buffer_read_pos * AUDIO_CHANNELS + i]);
          for (u32 j = 0; j < this_batch; j++)
          {
            std::memcpy(output, input, sizeof(s16));
            input += sizeof(s16) * AUDIO_CHANNELS;
            output += m_audio_frame_bps;
          }
        }
      }
      else
      {
        // Direct copy - optimal.
        std::memcpy(m_converted_audio_frame->data[0] + m_audio_frame_pos * m_audio_frame_bps * AUDIO_CHANNELS,
                    &m_audio_buffer[m_audio_buffer_read_pos * AUDIO_CHANNELS],
                    this_batch * sizeof(s16) * AUDIO_CHANNELS);
      }
    }
    else
    {
      // Use swresample to convert.
      const u8* input = reinterpret_cast<u8*>(&m_audio_buffer[m_audio_buffer_read_pos * AUDIO_CHANNELS]);

      // Might be planar, so offset both buffers.
      u8* output[AUDIO_CHANNELS];
      if (m_audio_frame_planar)
      {
        for (u32 i = 0; i < AUDIO_CHANNELS; i++)
          output[i] = m_converted_audio_frame->data[i] + (m_audio_frame_pos * m_audio_frame_bps);
      }
      else
      {
        output[0] = m_converted_audio_frame->data[0] + (m_audio_frame_pos * m_audio_frame_bps * AUDIO_CHANNELS);
      }

      const int res = wrap_swr_convert(m_swr_context, output, this_batch, &input, this_batch);
      if (res < 0)
      {
        SetAVError(error, "swr_convert() failed: ", res);
        return false;
      }
    }

    m_audio_buffer_read_pos = (m_audio_buffer_read_pos + this_batch) % max_audio_buffer_size;
    m_audio_buffer_size.fetch_sub(this_batch);
    m_audio_frame_pos += this_batch;
    pending_frames -= this_batch;

    // Do we have a complete frame?
    if (m_audio_frame_pos == m_audio_frame_size)
    {
      m_audio_frame_pos = 0;

      if (!m_swr_context)
      {
        // PTS is simply frames.
        m_converted_audio_frame->pts = m_next_audio_pts;
      }
      else
      {
        m_converted_audio_frame->pts = wrap_swr_next_pts(m_swr_context, m_next_audio_pts);
      }

      // Increment PTS.
      m_next_audio_pts += m_audio_frame_size;

      // Send off for encoding.
      int res = wrap_avcodec_send_frame(m_audio_codec_context, m_converted_audio_frame);
      if (res < 0) [[unlikely]]
      {
        SetAVError(error, "avcodec_send_frame() for audio failed: ", res);
        return false;
      }

      // Write any packets back to the output file.
      if (!ReceivePackets(m_audio_codec_context, m_audio_stream, m_audio_packet, error)) [[unlikely]]
        return false;
    }
  }

  return true;
}

std::unique_ptr<MediaCapture> MediaCaptureFFmpeg::Create(Error* error)
{
  if (!LoadFFmpeg(error))
    return nullptr;

  return std::make_unique<MediaCaptureFFmpeg>();
}

MediaCapture::ContainerList MediaCaptureFFmpeg::GetContainerList()
{
  return {
    {"avi", "Audio Video Interleave"}, {"mp4", "MPEG-4 Part 14"},         {"mkv", "Matroska Media Container"},
    {"mov", "QuickTime File Format"},  {"mp3", "MPEG-2 Audio Layer III"}, {"wav", "Waveform Audio File Format"},
  };
}

MediaCaptureBase::CodecList MediaCaptureFFmpeg::GetCodecListForContainer(const char* container, AVMediaType type)
{
  CodecList ret;

  Error error;
  if (!LoadFFmpeg(&error))
  {
    ERROR_LOG("FFmpeg load failed: {}", error.GetDescription());
    return ret;
  }

  const AVOutputFormat* output_format =
    wrap_av_guess_format(nullptr, fmt::format("video.{}", container ? container : "mp4").c_str(), nullptr);
  if (!output_format)
  {
    ERROR_LOG("av_guess_format() failed");
    return ret;
  }

  void* iter = nullptr;
  const AVCodec* codec;
  while ((codec = wrap_av_codec_iterate(&iter)) != nullptr)
  {
    // only get audio codecs
    if (codec->type != type || !wrap_avcodec_find_encoder(codec->id) || !wrap_avcodec_find_encoder_by_name(codec->name))
      continue;

    if (!wrap_avformat_query_codec(output_format, codec->id, FF_COMPLIANCE_NORMAL))
      continue;

    if (std::find_if(ret.begin(), ret.end(), [codec](const auto& it) { return it.first == codec->name; }) != ret.end())
      continue;

    ret.emplace_back(codec->name, codec->long_name ? codec->long_name : codec->name);
  }

  return ret;
}

MediaCapture::CodecList MediaCaptureFFmpeg::GetVideoCodecList(const char* container)
{
  return GetCodecListForContainer(container, AVMEDIA_TYPE_VIDEO);
}

MediaCapture::CodecList MediaCaptureFFmpeg::GetAudioCodecList(const char* container)
{
  return GetCodecListForContainer(container, AVMEDIA_TYPE_AUDIO);
}

#endif

} // namespace

static constexpr const std::array s_backend_names = {
#ifdef _WIN32
  "MediaFoundation",
#endif
#ifndef __ANDROID__
  "FFmpeg",
#endif
};
static constexpr const std::array s_backend_display_names = {
#ifdef _WIN32
  TRANSLATE_DISAMBIG_NOOP("Settings", "Media Foundation", "MediaCaptureBackend"),
#endif
#ifndef __ANDROID__
  TRANSLATE_DISAMBIG_NOOP("Settings", "FFmpeg", "MediaCaptureBackend"),
#endif
};
static_assert(s_backend_names.size() == static_cast<size_t>(MediaCaptureBackend::MaxCount));
static_assert(s_backend_display_names.size() == static_cast<size_t>(MediaCaptureBackend::MaxCount));

MediaCapture::~MediaCapture() = default;

std::optional<MediaCaptureBackend> MediaCapture::ParseBackendName(const char* str)
{
  int index = 0;
  for (const char* name : s_backend_names)
  {
    if (std::strcmp(name, str) == 0)
      return static_cast<MediaCaptureBackend>(index);

    index++;
  }

  return std::nullopt;
}

const char* MediaCapture::GetBackendName(MediaCaptureBackend backend)
{
  return s_backend_names[static_cast<size_t>(backend)];
}

const char* MediaCapture::GetBackendDisplayName(MediaCaptureBackend backend)
{
  return Host::TranslateToCString("Settings", s_backend_display_names[static_cast<size_t>(backend)],
                                  "MediaCaptureBackend");
}

void MediaCapture::AdjustVideoSize(u32* width, u32* height)
{
  *width = Common::AlignUpPow2(*width, VIDEO_WIDTH_ALIGNMENT);
  *height = Common::AlignUpPow2(*height, VIDEO_HEIGHT_ALIGNMENT);
}

MediaCapture::ContainerList MediaCapture::GetContainerList(MediaCaptureBackend backend)
{
  ContainerList ret;
  switch (backend)
  {
#ifdef _WIN32
    case MediaCaptureBackend::MediaFoundation:
      ret = MediaCaptureMF::GetContainerList();
      break;
#endif
#ifndef __ANDROID__
    case MediaCaptureBackend::FFmpeg:
      ret = MediaCaptureFFmpeg::GetContainerList();
      break;
#endif
    default:
      break;
  }
  return ret;
}

MediaCapture::CodecList MediaCapture::GetVideoCodecList(MediaCaptureBackend backend, const char* container)
{
  CodecList ret;
  switch (backend)
  {
#ifdef _WIN32
    case MediaCaptureBackend::MediaFoundation:
      ret = MediaCaptureMF::GetVideoCodecList(container);
      break;
#endif
#ifndef __ANDROID__
    case MediaCaptureBackend::FFmpeg:
      ret = MediaCaptureFFmpeg::GetVideoCodecList(container);
      break;
#endif
    default:
      break;
  }
  return ret;
}

MediaCapture::CodecList MediaCapture::GetAudioCodecList(MediaCaptureBackend backend, const char* container)
{
  CodecList ret;
  switch (backend)
  {
#ifdef _WIN32
    case MediaCaptureBackend::MediaFoundation:
      ret = MediaCaptureMF::GetAudioCodecList(container);
      break;
#endif
#ifndef __ANDROID__
    case MediaCaptureBackend::FFmpeg:
      ret = MediaCaptureFFmpeg::GetAudioCodecList(container);
      break;
#endif
    default:
      break;
  }
  return ret;
}

std::unique_ptr<MediaCapture> MediaCapture::Create(MediaCaptureBackend backend, Error* error)
{
  switch (backend)
  {
#ifdef _WIN32
    case MediaCaptureBackend::MediaFoundation:
      return MediaCaptureMF::Create(error);
#endif
#ifndef __ANDROID__
    case MediaCaptureBackend::FFmpeg:
      return MediaCaptureFFmpeg::Create(error);
#endif
    default:
      return nullptr;
  }
}
