// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "media_capture.h"
#include "gpu_device.h"
#include "host.h"

#include "common/align.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/gsvector.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/threading.h"

#include "IconsFontAwesome5.h"
#include "fmt/format.h"

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

#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")
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
  static constexpr u32 AUDIO_BITS_PER_SAMPLE = sizeof(s16) * 8;

  virtual ~MediaCaptureBase() override;

  bool BeginCapture(float fps, float aspect, u32 width, u32 height, GPUTexture::Format texture_format, u32 sample_rate,
                    std::string path, bool capture_video, std::string_view video_codec, u32 video_bitrate,
                    std::string_view video_codec_args, bool capture_audio, std::string_view audio_codec,
                    u32 audio_bitrate, std::string_view audio_codec_args, Error* error) override;

  const std::string& GetPath() const override;
  u32 GetVideoWidth() const override;
  u32 GetVideoHeight() const override;

  float GetCaptureThreadUsage() const override;
  float GetCaptureThreadTime() const override;
  void UpdateCaptureThreadUsage(double pct_divider, double time_divider) override;

  GPUTexture* GetRenderTexture() override;
  bool DeliverVideoFrame(GPUTexture* stex) override;
  bool DeliverAudioFrames(const s16* frames, u32 num_frames) override;
  bool EndCapture(Error* error) override;
  void Flush() override;

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

  u32 m_video_width = 0;
  u32 m_video_height = 0;
  GPUTexture::Format m_video_render_texture_format = GPUTexture::Format::Unknown;
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
};

MediaCaptureBase::~MediaCaptureBase() = default;

bool MediaCaptureBase::BeginCapture(float fps, float aspect, u32 width, u32 height, GPUTexture::Format texture_format,
                                    u32 sample_rate, std::string path, bool capture_video, std::string_view video_codec,
                                    u32 video_bitrate, std::string_view video_codec_args, bool capture_audio,
                                    std::string_view audio_codec, u32 audio_bitrate, std::string_view audio_codec_args,
                                    Error* error)
{
  m_video_width = width;
  m_video_height = height;
  m_video_render_texture_format = texture_format;

  if (path.empty())
  {
    Error::SetStringView(error, "No path specified.");
    return false;
  }
  else if (fps == 0.0f || m_video_width == 0 || !Common::IsAlignedPow2(m_video_width, VIDEO_WIDTH_ALIGNMENT) ||
           m_video_height == 0 || !Common::IsAlignedPow2(m_video_height, VIDEO_HEIGHT_ALIGNMENT))
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
    INFO_LOG("  Video: FPS={}, Aspect={}, Codec={}, Bitrate={}, Args={}", fps, aspect, video_codec, video_bitrate,
             video_codec_args);
  }
  if (capture_audio)
  {
    INFO_LOG("  Audio: SampleRate={}, Codec={}, Bitrate={}, Args={}", sample_rate, audio_codec, audio_bitrate,
             audio_codec_args);
  }

  if (!InternalBeginCapture(fps, aspect, sample_rate, capture_video, video_codec, video_bitrate, video_codec_args,
                            capture_audio, audio_codec, audio_bitrate, audio_codec_args, error))
  {
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
    DebugAssert(pf.state == PendingFrame::State::NeedsEncoding);

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

u32 MediaCaptureBase::GetVideoWidth() const
{
  return m_video_width;
}

u32 MediaCaptureBase::GetVideoHeight() const
{
  return m_video_height;
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

class MediaCaptureMF final : public MediaCaptureBase
{
  template<class T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  static constexpr u32 TEN_NANOSECONDS = 10 * 1000 * 1000;
  static constexpr DWORD INVALID_STREAM_INDEX = std::numeric_limits<DWORD>::max();

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
  ComPtr<IMFTransform> CreateVideoYUVTransform(ComPtr<IMFMediaType>* output_type, Error* error);
  ComPtr<IMFTransform> CreateVideoEncodeTransform(std::string_view codec, u32 bitrate, IMFMediaType* input_type,
                                                  ComPtr<IMFMediaType>* output_type, bool* use_async_transform,
                                                  Error* error);
  bool GetAudioTypes(std::string_view codec, ComPtr<IMFMediaType>* input_type, ComPtr<IMFMediaType>* output_type,
                     u32 sample_rate, u32 bitrate, Error* error);
  static void ConvertVideoFrame(u8* dst, size_t dst_stride, const u8* src, size_t src_stride, u32 width, u32 height);

  bool ProcessVideoOutputSamples(Error* error); // synchronous
  bool ProcessVideoEvents(Error* error);        // asynchronous

  ComPtr<IMFSinkWriter> m_sink_writer;

  DWORD m_video_stream_index = INVALID_STREAM_INDEX;
  DWORD m_audio_stream_index = INVALID_STREAM_INDEX;

  LONGLONG m_video_sample_duration = 0;
  LONGLONG m_audio_sample_duration = 0;

  u32 m_frame_rate_numerator = 0;

  ComPtr<IMFTransform> m_video_yuv_transform;
  ComPtr<IMFSample> m_video_yuv_sample;
  ComPtr<IMFTransform> m_video_encode_transform;
  ComPtr<IMFMediaEventGenerator> m_video_encode_event_generator;
  std::deque<ComPtr<IMFSample>> m_pending_video_samples;
  ComPtr<IMFSample> m_video_output_sample;
  u32 m_wanted_video_samples = 0;
  DWORD m_video_sample_size = 0;
};

static std::once_flag s_media_foundation_initialized_flag;
static HRESULT s_media_foundation_initialized = S_OK;

struct MediaFoundationCodec
{
  const char* name;
  const char* display_name;
  const GUID& guid;
  bool require_hardware;
};
static constexpr const MediaFoundationCodec s_media_foundation_audio_codecs[] = {
  {"aac", "Advanced Audio Coding", MFAudioFormat_AAC},
  {"mp3", "MPEG-2 Audio Layer III", MFAudioFormat_MP3},
  {"pcm", "Uncompressed PCM", MFAudioFormat_PCM},
};
static constexpr const MediaFoundationCodec s_media_foundation_video_codecs[] = {
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

static bool InitializeMediaFoundation(Error* error)
{
  std::call_once(s_media_foundation_initialized_flag, []() {
    s_media_foundation_initialized = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (SUCCEEDED(s_media_foundation_initialized))
      std::atexit([]() { MFShutdown(); });
  });
  if (FAILED(s_media_foundation_initialized)) [[unlikely]]
  {
    Error::SetHResult(error, "MFStartup() failed: ", s_media_foundation_initialized);
    return false;
  }

  return true;
}

MediaCaptureMF::~MediaCaptureMF() = default;

std::unique_ptr<MediaCapture> MediaCaptureMF::Create(Error* error)
{
  if (!InitializeMediaFoundation(error))
    return nullptr;

  return std::make_unique<MediaCaptureMF>();
}

MediaCapture::ContainerList MediaCaptureMF::GetContainerList()
{
  return {
    {"avi", "Audio Video Interleave"},     {"mp4", "MPEG-4 Part 14"},
    {"mkv", "Matroska Media Container"},   {"mp3", "MPEG-2 Audio Layer III"},
    {"wav", "Waveform Audio File Format"},
  };
}

MediaCapture::ContainerList MediaCaptureMF::GetAudioCodecList(const char* container)
{
  ContainerList ret;
  ret.reserve(std::size(s_media_foundation_audio_codecs));
  for (const MediaFoundationCodec& codec : s_media_foundation_audio_codecs)
    ret.emplace_back(codec.name, codec.display_name);
  return ret;
}

MediaCapture::ContainerList MediaCaptureMF::GetVideoCodecList(const char* container)
{
  ContainerList ret;
  ret.reserve(std::size(s_media_foundation_video_codecs));
  for (const MediaFoundationCodec& codec : s_media_foundation_video_codecs)
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
    return static_cast<time_t>(static_cast<LONGLONG>(m_next_video_pts * m_video_sample_duration) / TEN_NANOSECONDS);
  else
    return static_cast<time_t>(static_cast<LONGLONG>(m_next_audio_pts * m_audio_sample_duration) / TEN_NANOSECONDS);
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
    m_frame_rate_numerator = static_cast<u32>(fps * TEN_NANOSECONDS);
    m_video_sample_duration = static_cast<LONGLONG>(static_cast<double>(TEN_NANOSECONDS) / static_cast<double>(fps));

    ComPtr<IMFMediaType> yuv_media_type;
    if (!(m_video_yuv_transform = CreateVideoYUVTransform(&yuv_media_type, error)) ||
        !(m_video_encode_transform = CreateVideoEncodeTransform(video_codec, video_bitrate, yuv_media_type.Get(),
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

    m_audio_sample_duration =
      static_cast<LONGLONG>(static_cast<double>(TEN_NANOSECONDS) / static_cast<double>(sample_rate));
  }

  if (FAILED(hr = MFCreateSinkWriterFromURL(StringUtil::UTF8StringToWideString(m_path).c_str(), nullptr, nullptr,
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

    if (SUCCEEDED(hr) && FAILED(hr = m_sink_writer->SetInputMediaType(m_audio_stream_index, audio_input_type.Get(),
                                                                      nullptr))) [[unlikely]]
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

  if (SUCCEEDED(hr) && FAILED(hr = m_video_encode_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
    Error::SetHResult(error, "MFT_MESSAGE_NOTIFY_START_OF_STREAM failed: ", hr);

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
                                                                             Error* error)
{
  const MFT_REGISTER_TYPE_INFO input_type_info = {.guidMajorType = MFMediaType_Video,
                                                  .guidSubtype = VIDEO_RGB_MEDIA_FORMAT};
  const MFT_REGISTER_TYPE_INFO output_type_info = {.guidMajorType = MFMediaType_Video,
                                                   .guidSubtype = VIDEO_YUV_MEDIA_FORMAT};

  IMFActivate** transforms = nullptr;
  UINT32 num_transforms = 0;
  HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_PROCESSOR, MFT_ENUM_FLAG_SORTANDFILTER, &input_type_info, &output_type_info,
                         &transforms, &num_transforms);
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
    MFHeapFree(transforms);
  if (FAILED(hr)) [[unlikely]]
  {
    Error::SetHResult(error, "YUV ActivateObject() failed: ", hr);
    return nullptr;
  }

  ComPtr<IMFMediaType> input_type;
  if (FAILED(hr = MFCreateMediaType(input_type.GetAddressOf())) ||
      FAILED(hr = MFCreateMediaType(output_type->GetAddressOf()))) [[unlikely]]
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
      FAILED(hr = MFSetAttributeRatio(output_type->Get(), MF_MT_FRAME_RATE, m_frame_rate_numerator, TEN_NANOSECONDS)))
    [[unlikely]]
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

MediaCaptureMF::ComPtr<IMFTransform> MediaCaptureMF::CreateVideoEncodeTransform(std::string_view codec, u32 bitrate,
                                                                                IMFMediaType* input_type,
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
    for (const MediaFoundationCodec& tcodec : s_media_foundation_video_codecs)
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
    MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, (hardware ? MFT_ENUM_FLAG_HARDWARE : 0) | MFT_ENUM_FLAG_SORTANDFILTER,
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
    MFHeapFree(transforms);
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

  if (FAILED(hr = MFCreateMediaType(output_type->GetAddressOf()))) [[unlikely]]
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
      FAILED(hr = MFSetAttributeRatio(output_type->Get(), MF_MT_FRAME_RATE, m_frame_rate_numerator, TEN_NANOSECONDS)) ||
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
                                                             size_t src_stride, u32 width, u32 height)
{
  // need to convert rgba -> bgra, as well as flipping vertically
  const u32 vector_width = 4;
  const u32 aligned_width = Common::AlignDownPow2(width, vector_width);
  src += src_stride * (height - 1);

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

    src -= src_stride;
    dst += dst_stride;

    remaining_rows--;
    if (remaining_rows == 0)
      break;
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
  m_frame_rate_numerator = 0;

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
  if (FAILED(hr = MFCreateMemoryBuffer(buffer_size, buffer.GetAddressOf()))) [[unlikely]]
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
  if (FAILED(hr = MFCreateSample(sample.GetAddressOf()))) [[unlikely]]
  {
    Error::SetHResult(error, "MFCreateSample() failed: ", hr);
    return false;
  }

  if (FAILED(hr = sample->AddBuffer(buffer.Get()))) [[unlikely]]
  {
    Error::SetHResult(error, "AddBuffer() failed: ", hr);
    return false;
  }

  const LONGLONG timestamp = static_cast<LONGLONG>(pf.pts) * m_video_sample_duration;
  if (FAILED(hr = sample->SetSampleTime(timestamp))) [[unlikely]]
  {
    Error::SetHResult(error, "SetSampleTime() failed: ", hr);
    return false;
  }

  if (FAILED(hr = sample->SetSampleDuration(m_video_sample_duration))) [[unlikely]]
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
      if (FAILED(hr = MFCreateMemoryBuffer(buffer_size, yuv_membuf.GetAddressOf()))) [[unlikely]]
      {
        Error::SetHResult(error, "YUV MFCreateMemoryBuffer() failed: ", hr);
        return false;
      }

      if (FAILED(hr = MFCreateSample(m_video_yuv_sample.GetAddressOf()))) [[unlikely]]
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
      if (FAILED(hr = MFCreateMemoryBuffer(m_video_sample_size, video_membuf.GetAddressOf()))) [[unlikely]]
      {
        Error::SetHResult(error, "YUV MFCreateMemoryBuffer() failed: ", hr);
        return false;
      }

      if (FAILED(hr = MFCreateSample(m_video_output_sample.GetAddressOf()))) [[unlikely]]
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
          if (FAILED(hr = MFCreateMemoryBuffer(m_video_sample_size, video_membuf.GetAddressOf()))) [[unlikely]]
          {
            Error::SetHResult(error, "YUV MFCreateMemoryBuffer() failed: ", hr);
            return false;
          }

          if (FAILED(hr = MFCreateSample(m_video_output_sample.GetAddressOf()))) [[unlikely]]
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
  HRESULT hr;
  if (FAILED(hr = MFCreateMediaType(input_type->GetAddressOf())) ||
      FAILED(hr = MFCreateMediaType(output_type->GetAddressOf()))) [[unlikely]]
  {
    Error::SetHResult(error, "Audio MFCreateMediaType() failed: ", hr);
    return false;
  }

  GUID output_subtype = MFAudioFormat_AAC;
  if (!codec.empty())
  {
    bool found = false;
    for (const MediaFoundationCodec& tcodec : s_media_foundation_audio_codecs)
    {
      if (StringUtil::EqualNoCase(codec, tcodec.name))
      {
        output_subtype = tcodec.guid;
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

  if (FAILED(hr = (*input_type)->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
      FAILED(hr = (*input_type)->SetGUID(MF_MT_SUBTYPE, AUDIO_INPUT_MEDIA_FORMAT)) ||
      FAILED(hr = (*input_type)->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AUDIO_CHANNELS)) ||
      FAILED(hr = (*input_type)->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, AUDIO_BITS_PER_SAMPLE)) ||
      FAILED(hr = (*input_type)->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate)) ||

      FAILED(hr = (*output_type)->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
      FAILED(hr = (*output_type)->SetGUID(MF_MT_SUBTYPE, output_subtype)) ||
      FAILED(hr = (*output_type)->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AUDIO_CHANNELS)) ||
      FAILED(hr = (*output_type)->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, AUDIO_BITS_PER_SAMPLE)) ||
      FAILED(hr = (*output_type)->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate)) ||
      FAILED(hr = (*output_type)->SetUINT32(MF_MT_AVG_BITRATE, bitrate * 1000))) [[unlikely]]
  {
    Error::SetHResult(error, "Audio setting attributes failed: ", hr);
    return false;
  }

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
    if (FAILED(hr = MFCreateMemoryBuffer(buffer_size, buffer.GetAddressOf()))) [[unlikely]]
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
    if (FAILED(hr = MFCreateSample(sample.GetAddressOf()))) [[unlikely]]
    {
      Error::SetHResult(error, "Audio MFCreateSample() failed: ", hr);
      return false;
    }

    if (FAILED(hr = sample->AddBuffer(buffer.Get()))) [[unlikely]]
    {
      Error::SetHResult(error, "Audio AddBuffer() failed: ", hr);
      return false;
    }

    const LONGLONG timestamp = static_cast<LONGLONG>(m_next_audio_pts) * m_audio_sample_duration;
    if (FAILED(hr = sample->SetSampleTime(timestamp))) [[unlikely]]
    {
      Error::SetHResult(error, "Audio SetSampleTime() failed: ", hr);
      return false;
    }

    const LONGLONG duration = static_cast<LONGLONG>(contig_frames) * m_audio_sample_duration;
    if (FAILED(hr = sample->SetSampleDuration(duration))) [[unlikely]]
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

} // namespace

static constexpr const std::array s_backend_names = {
#ifdef _WIN32
  "MediaFoundation",
#endif
#ifndef __ANDROID__
  "FFMPEG",
#endif
};
static constexpr const std::array s_backend_display_names = {
#ifdef _WIN32
  TRANSLATE_NOOP("MediaCapture", "Media Foundation"),
#endif
#ifndef __ANDROID__
  TRANSLATE_NOOP("MediaCapture", "FFMPEG"),
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
  return Host::TranslateToCString("MediaCapture", s_backend_display_names[static_cast<size_t>(backend)]);
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
    case MediaCaptureBackend::FFMPEG:
      // ret = MediaCaptureFFMPEG::GetContainerList();
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
    case MediaCaptureBackend::FFMPEG:
      // ret = MediaCaptureFFMPEG::GetVideoCodecList(container);
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
    case MediaCaptureBackend::FFMPEG:
      // ret = MediaCaptureFFMPEG::GetAudioCodecList(container);
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
    case MediaCaptureBackend::FFMPEG:
      return nullptr;
#endif
    default:
      return nullptr;
  }
}
