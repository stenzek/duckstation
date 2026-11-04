// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "util/core_audio_stream.h"

#include "common/xorshift_prng.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <vector>

namespace {

struct SimConfig
{
  AudioStretchMode mode = AudioStretchMode::TimeStretch;
  u16 buffer_ms = AudioStreamParameters::DEFAULT_BUFFER_MS;
  u32 callback_frames = 896;
  double seconds = 20.0;

  // nominal speed, and the speed the producer actually runs at relative to it
  float speed = 1.0f;
  double actual = 1.0;

  // consumer clock drift
  double drift_ppm = 0.0;

  // random variation of each producer frame time, either way
  double jitter_ms = 0.0;

  // one-off stall of the producer (emulation) or consumer (audio device)
  double producer_stall_at = -1.0;
  double producer_stall_ms = 0.0;
  double consumer_stall_at = -1.0;
  double consumer_stall_ms = 0.0;

  // nominal speed change at a given time
  double speed_change_at = -1.0;
  float speed_change_to = 1.0f;

  // stretch mode change at a given time
  double mode_change_at = -1.0;
  AudioStretchMode mode_change_to = AudioStretchMode::Off;
};

struct SimResult
{
  // after the initial fill
  u32 underruns = 0;

  // large sample-to-sample jumps in the output, i.e. discontinuities
  u32 clicks = 0;

  // output frames that weren't produced by the input signal
  u32 bad_frames = 0;

  // buffered latency after each callback, over the last half of the run
  double avg_latency_ms = 0.0;
  double max_latency_ms = 0.0;

  // fraction of the input that couldn't be written
  double dropped_input = 0.0;
};

std::ostream& operator<<(std::ostream& os, const SimResult& r)
{
  return os << "underruns=" << r.underruns << " clicks=" << r.clicks << " bad_frames=" << r.bad_frames
            << " avg_latency_ms=" << r.avg_latency_ms << " max_latency_ms=" << r.max_latency_ms
            << " dropped_input=" << r.dropped_input;
}

} // namespace

class CoreAudioStreamTest : public testing::Test
{
protected:
  using SampleType = CoreAudioStream::SampleType;

  static constexpr u32 SAMPLE_RATE = 44100;
  static constexpr u32 NUM_CHANNELS = CoreAudioStream::NUM_CHANNELS;

  // emulated audio per host frame
  static constexpr u32 FRAMES_PER_HOST_FRAME = SAMPLE_RATE / 60;

  // The right channel is the inverted left channel, so swapped or misaligned channels show up.
  static constexpr double SINE_FREQUENCY = 220.0;
  static constexpr double SINE_AMPLITUDE = 0.5 * 32767.0;

  // Well above the largest step of the sine, even when resampling at 4x.
  static constexpr int CLICK_THRESHOLD = static_cast<int>(SINE_AMPLITUDE * 0.25);

  // Sets up the stream as Initialize() would, but without an output, and unpaused so it accepts writes.
  void Create(AudioStretchMode mode, u16 buffer_ms = AudioStreamParameters::DEFAULT_BUFFER_MS)
  {
    m_stream.Destroy();
    m_stream.m_sample_rate = SAMPLE_RATE;
    m_stream.m_volume = 100;
    m_stream.m_parameters.stretch_mode = mode;
    m_stream.m_parameters.buffer_ms = buffer_ms;
    m_stream.m_filling = false;
    m_stream.m_paused = false;
    m_stream.AllocateBuffer();
    m_stream.StretchAllocate();
  }

  void SetPaused(bool paused) { m_stream.m_paused = paused; }

  void ReadFrames(SampleType* samples, u32 num_frames) { m_stream.ReadFrames(samples, num_frames); }

  const SampleType* GetBufferStart() const { return m_stream.m_buffer.get(); }
  u32 GetReadPos() const { return m_stream.m_rpos.load(std::memory_order_relaxed); }
  u32 GetWritePos() const { return m_stream.m_wpos.load(std::memory_order_relaxed); }
  bool IsFilling() const { return m_stream.m_filling; }

  // Writes frames the same way as the SPU, returns the number that were written.
  template<typename F>
  u32 WriteFrames(u32 num_frames, const F& generate)
  {
    u32 written = 0;
    while (num_frames > 0)
    {
      SampleType* buffer;
      u32 space;
      m_stream.BeginWrite(&buffer, &space);
      if (space == 0)
        break;

      const u32 count = std::min(num_frames, space);
      for (u32 i = 0; i < count; i++)
        generate(&buffer[i * NUM_CHANNELS]);
      m_stream.EndWrite(count);
      num_frames -= count;
      written += count;
    }

    return written;
  }

  // Runs a bursty producer, one host frame of audio at a time split into SPU-like batches, and a consumer callback
  // reading at the sample rate, in simulated time.
  SimResult RunSimulation(const SimConfig& config)
  {
    Create(config.mode, config.buffer_ms);
    m_stream.SetNominalRate(config.speed);

    SimResult result;
    XorShift128PlusPlus rng;
    std::vector<SampleType> output(config.callback_frames * NUM_CHANNELS);

    const double consumer_period =
      (static_cast<double>(config.callback_frames) / SAMPLE_RATE) * (1.0 + config.drift_ppm * 1e-6);
    const double phase_step = 2.0 * std::numbers::pi * SINE_FREQUENCY / SAMPLE_RATE;
    const double max_amplitude = SINE_AMPLITUDE * 1.1 + 2.0;
    const double latency_start = config.seconds * 0.5;

    double phase = 0.0;
    double speed = config.speed;
    double next_produce = 0.0;
    double next_consume = 0.0;
    bool producer_stalled = false;
    bool consumer_stalled = false;
    bool speed_changed = false;
    bool mode_changed = false;
    u64 produced_frames = 0;
    u64 dropped_frames = 0;
    double latency_sum = 0.0;
    u32 latency_count = 0;
    SampleType last_sample = 0;

    const auto generate = [&phase, phase_step](SampleType* frame) {
      const SampleType value = static_cast<SampleType>(std::lround(std::sin(phase) * SINE_AMPLITUDE));
      frame[0] = value;
      frame[1] = -value;
      phase += phase_step;
      if (phase >= 2.0 * std::numbers::pi)
        phase -= 2.0 * std::numbers::pi;
    };

    while (next_produce < config.seconds || next_consume < config.seconds)
    {
      if (next_produce <= next_consume)
      {
        const double time = next_produce;
        if (!speed_changed && config.speed_change_at >= 0.0 && time >= config.speed_change_at)
        {
          // same as System::UpdateSpeedLimiterState()
          m_stream.SetNominalRate(config.speed_change_to);
          if (config.speed_change_to < speed)
            m_stream.EmptyStretchBuffers();
          speed = config.speed_change_to;
          speed_changed = true;
        }
        if (!mode_changed && config.mode_change_at >= 0.0 && time >= config.mode_change_at)
        {
          AudioStreamParameters params = m_stream.m_parameters;
          params.stretch_mode = config.mode_change_to;
          m_stream.UpdateParameters(params);
          mode_changed = true;
        }

        // the SPU runs in variable-sized slices, depending on what the game is doing
        u32 remaining = FRAMES_PER_HOST_FRAME;
        while (remaining > 0)
        {
          const u32 batch = std::min(remaining, rng.NextRange<u32>(1, 257));
          const u32 written = WriteFrames(batch, generate);

          // the SPU writes what doesn't fit to a dummy buffer
          for (u32 i = written; i < batch; i++)
          {
            SampleType dummy[NUM_CHANNELS];
            generate(dummy);
          }

          dropped_frames += batch - written;
          remaining -= batch;
        }
        produced_frames += FRAMES_PER_HOST_FRAME;

        double frame_time = (1.0 / 60.0) / (speed * config.actual);
        if (config.jitter_ms > 0.0)
          frame_time += ((static_cast<double>(rng.Next() >> 11) * 0x1.0p-53) - 0.5) * config.jitter_ms * 1e-3;
        next_produce += std::max(frame_time, 0.0);

        if (!producer_stalled && config.producer_stall_at >= 0.0 && next_produce >= config.producer_stall_at)
        {
          next_produce += config.producer_stall_ms * 1e-3;
          producer_stalled = true;
        }
      }
      else
      {
        const double time = next_consume;
        const bool was_filling = IsFilling();
        ReadFrames(output.data(), config.callback_frames);
        if (!was_filling && IsFilling() && time >= 1.0)
          result.underruns++;

        for (u32 i = 0; i < config.callback_frames; i++)
        {
          const SampleType left = output[i * NUM_CHANNELS + 0];
          const SampleType right = output[i * NUM_CHANNELS + 1];
          if (std::abs(static_cast<int>(left) + static_cast<int>(right)) > 2 || std::abs(left) > max_amplitude)
            result.bad_frames++;
          if (std::abs(static_cast<int>(left) - static_cast<int>(last_sample)) > CLICK_THRESHOLD)
            result.clicks++;
          last_sample = left;
        }

        if (time >= latency_start)
        {
          const double latency_ms =
            static_cast<double>(m_stream.GetBufferedFramesRelaxed()) * 1000.0 / static_cast<double>(SAMPLE_RATE);
          latency_sum += latency_ms;
          latency_count++;
          result.max_latency_ms = std::max(result.max_latency_ms, latency_ms);
        }

        next_consume += consumer_period;
        if (!consumer_stalled && config.consumer_stall_at >= 0.0 && next_consume >= config.consumer_stall_at)
        {
          next_consume += config.consumer_stall_ms * 1e-3;
          consumer_stalled = true;
        }
      }
    }

    result.avg_latency_ms = (latency_count > 0) ? (latency_sum / latency_count) : 0.0;
    result.dropped_input = static_cast<double>(dropped_frames) / static_cast<double>(produced_frames);

    // for comparing changes
    if (std::getenv("CORE_AUDIO_STREAM_TEST_VERBOSE"))
      std::printf("%s\n", testing::PrintToString(result).c_str());

    return result;
  }

  CoreAudioStream m_stream;
};

//////////////////////////////////////////////////////////////////////////
// Ring buffer
//////////////////////////////////////////////////////////////////////////

TEST_F(CoreAudioStreamTest, WriteSpaceIsContiguous)
{
  Create(AudioStretchMode::Off);
  const u32 size = m_stream.GetBufferSize();
  std::vector<SampleType> output(size * NUM_CHANNELS);

  SampleType* buffer;
  u32 space;

  // one frame is always left free
  m_stream.BeginWrite(&buffer, &space);
  EXPECT_EQ(space, size - 1);
  m_stream.EndWrite(space);
  m_stream.BeginWrite(&buffer, &space);
  EXPECT_EQ(space, 0u);

  // only up to the end of the buffer
  ReadFrames(output.data(), 100);
  m_stream.BeginWrite(&buffer, &space);
  EXPECT_EQ(space, 1u);
  m_stream.EndWrite(space);
  EXPECT_EQ(GetWritePos(), 0u);

  // then from the start, up to just behind the read position
  m_stream.BeginWrite(&buffer, &space);
  EXPECT_EQ(buffer, GetBufferStart());
  EXPECT_EQ(space, 99u);
}

TEST_F(CoreAudioStreamTest, NoWriteSpaceWhenMutedOrPaused)
{
  Create(AudioStretchMode::TimeStretch);

  SampleType* buffer;
  u32 space;

  m_stream.SetOutputVolume(0);
  EXPECT_FALSE(m_stream.BeginWrite(&buffer, &space));

  m_stream.SetOutputVolume(100);
  SetPaused(true);
  EXPECT_FALSE(m_stream.BeginWrite(&buffer, &space));

  SetPaused(false);
  EXPECT_TRUE(m_stream.BeginWrite(&buffer, &space));
  EXPECT_NE(buffer, nullptr);
  EXPECT_GT(space, 0u);
}

// Frames written after BeginWrite() aren't visible to the reader until EndWrite().
TEST_F(CoreAudioStreamTest, UncommittedWritesAreInvisible)
{
  for (const AudioStretchMode mode : {AudioStretchMode::Off, AudioStretchMode::TimeStretch})
  {
    SCOPED_TRACE(CoreAudioStream::GetStretchModeName(mode));
    Create(mode);

    SampleType* buffer;
    u32 space;
    m_stream.BeginWrite(&buffer, &space);
    ASSERT_GE(space, 1024u);
    std::fill_n(buffer, 1024 * NUM_CHANNELS, static_cast<SampleType>(0x1234));

    std::vector<SampleType> output(512 * NUM_CHANNELS);
    ReadFrames(output.data(), 512);
    EXPECT_EQ(GetReadPos(), 0u);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](SampleType v) { return v == 0; }));

    m_stream.EndWrite(1024);
    EXPECT_EQ(GetWritePos(), 1024u);
  }
}

// Pass-through with no underruns should output exactly the input, through any combination of write and read sizes.
TEST_F(CoreAudioStreamTest, OffModeOutputMatchesInput)
{
  Create(AudioStretchMode::Off);

  // frame number, low half in the left channel, high half in the right
  u32 write_counter = 0;
  u32 read_counter = 0;
  const auto generate = [&write_counter](SampleType* frame) {
    frame[0] = static_cast<SampleType>(write_counter & 0xFFFFu);
    frame[1] = static_cast<SampleType>(write_counter >> 16);
    write_counter++;
  };

  XorShift128PlusPlus rng;
  std::vector<SampleType> output(m_stream.GetBufferSize() * NUM_CHANNELS);
  for (u32 iteration = 0; iteration < 20000; iteration++)
  {
    // fill to a random level, without dropping
    const u32 fill_to = rng.NextRange<u32>(1, m_stream.GetBufferSize());
    while ((write_counter - read_counter) < fill_to)
    {
      const u32 batch = std::min(rng.NextRange<u32>(1, 301), fill_to - (write_counter - read_counter));
      ASSERT_EQ(WriteFrames(batch, generate), batch);
    }

    // read without underrunning
    const u32 read_frames = rng.NextRange<u32>(1, write_counter - read_counter + 1);
    ReadFrames(output.data(), read_frames);
    ASSERT_FALSE(IsFilling());
    for (u32 i = 0; i < read_frames; i++, read_counter++)
    {
      const u32 value = static_cast<u16>(output[i * NUM_CHANNELS + 0]) |
                        (static_cast<u32>(static_cast<u16>(output[i * NUM_CHANNELS + 1])) << 16);
      ASSERT_EQ(value, read_counter) << "iteration " << iteration << " frame " << i;
    }
  }
}

// When the producer is faster than the consumer, input is dropped, but what's output stays in order.
TEST_F(CoreAudioStreamTest, OffModeOverflowDropsInput)
{
  Create(AudioStretchMode::Off);

  u32 write_counter = 1;
  const auto generate = [&write_counter](SampleType* frame) {
    frame[0] = static_cast<SampleType>(write_counter & 0xFFFFu);
    frame[1] = static_cast<SampleType>(write_counter >> 16);
    write_counter++;
  };

  XorShift128PlusPlus rng;
  u32 last_value = 0;
  u64 total_written = 0;
  u64 total_read = 0;
  std::vector<SampleType> output(512 * NUM_CHANNELS);
  for (u32 iteration = 0; iteration < 5000; iteration++)
  {
    // twice as much input as output on average
    for (u32 remaining = 2 * 256; remaining > 0;)
    {
      const u32 batch = std::min(remaining, rng.NextRange<u32>(1, 65));
      const u32 written = WriteFrames(batch, generate);
      total_written += written;
      for (u32 i = written; i < batch; i++)
        write_counter++;
      remaining -= batch;
    }

    const u32 buffered = (GetWritePos() + m_stream.GetBufferSize() - GetReadPos()) % m_stream.GetBufferSize();
    ASSERT_LT(buffered, m_stream.GetBufferSize());

    const u32 read_frames = rng.NextRange<u32>(1, 513);
    if (buffered < read_frames)
      continue;

    ReadFrames(output.data(), read_frames);
    total_read += read_frames;
    for (u32 i = 0; i < read_frames; i++)
    {
      const u32 value = static_cast<u16>(output[i * NUM_CHANNELS + 0]) |
                        (static_cast<u32>(static_cast<u16>(output[i * NUM_CHANNELS + 1])) << 16);
      ASSERT_GT(value, last_value) << "iteration " << iteration << " frame " << i;
      ASSERT_LT(value, write_counter);
      last_value = value;
    }
  }

  // everything that was written is either read or still buffered
  const u32 buffered = (GetWritePos() + m_stream.GetBufferSize() - GetReadPos()) % m_stream.GetBufferSize();
  EXPECT_EQ(total_written, total_read + buffered);
  EXPECT_LT(total_written, static_cast<u64>(write_counter - 1));
}

//////////////////////////////////////////////////////////////////////////
// Simulated playback
//////////////////////////////////////////////////////////////////////////

TEST_F(CoreAudioStreamTest, NormalSpeedOff)
{
  SimConfig config;
  config.mode = AudioStretchMode::Off;
  const SimResult result = RunSimulation(config);
  EXPECT_EQ(result.underruns, 0u) << result;
  EXPECT_EQ(result.clicks, 0u) << result;
  EXPECT_EQ(result.bad_frames, 0u) << result;
  EXPECT_LT(result.avg_latency_ms, 40.0) << result;
}

TEST_F(CoreAudioStreamTest, NormalSpeedResample)
{
  SimConfig config;
  config.mode = AudioStretchMode::Resample;
  const SimResult result = RunSimulation(config);
  EXPECT_EQ(result.underruns, 0u) << result;
  EXPECT_EQ(result.clicks, 0u) << result;
  EXPECT_EQ(result.bad_frames, 0u) << result;
  EXPECT_LT(result.avg_latency_ms, 70.0) << result;
}

TEST_F(CoreAudioStreamTest, NormalSpeedTimeStretch)
{
  SimConfig config;
  const SimResult result = RunSimulation(config);
  EXPECT_EQ(result.underruns, 0u) << result;
  EXPECT_EQ(result.clicks, 0u) << result;
  EXPECT_EQ(result.bad_frames, 0u) << result;
  EXPECT_LT(result.avg_latency_ms, 70.0) << result;
}

TEST_F(CoreAudioStreamTest, JitterTimeStretch)
{
  SimConfig config;
  config.jitter_ms = 16.0;
  const SimResult result = RunSimulation(config);
  EXPECT_EQ(result.clicks, 0u) << result;
  EXPECT_EQ(result.bad_frames, 0u) << result;
  EXPECT_LT(result.avg_latency_ms, 100.0) << result;
}

TEST_F(CoreAudioStreamTest, ClockDriftTimeStretch)
{
  for (const double drift_ppm : {-1000.0, 1000.0})
  {
    SCOPED_TRACE(drift_ppm);
    SimConfig config;
    config.drift_ppm = drift_ppm;
    config.seconds = 30.0;
    const SimResult result = RunSimulation(config);
    EXPECT_EQ(result.underruns, 0u) << result;
    EXPECT_EQ(result.bad_frames, 0u) << result;
    EXPECT_LT(result.avg_latency_ms, 70.0) << result;
  }
}

TEST_F(CoreAudioStreamTest, ClockDriftResample)
{
  for (const double drift_ppm : {-1000.0, 1000.0})
  {
    SCOPED_TRACE(drift_ppm);
    SimConfig config;
    config.mode = AudioStretchMode::Resample;
    config.drift_ppm = drift_ppm;
    config.seconds = 30.0;
    const SimResult result = RunSimulation(config);
    EXPECT_EQ(result.underruns, 0u) << result;
    EXPECT_EQ(result.bad_frames, 0u) << result;
    EXPECT_LT(result.avg_latency_ms, 70.0) << result;
  }
}

TEST_F(CoreAudioStreamTest, FastForwardTimeStretch)
{
  for (const float speed : {2.0f, 4.0f})
  {
    SCOPED_TRACE(speed);
    SimConfig config;
    config.speed = speed;
    const SimResult result = RunSimulation(config);
    EXPECT_EQ(result.underruns, 0u) << result;
    EXPECT_EQ(result.bad_frames, 0u) << result;
    EXPECT_LT(result.avg_latency_ms, 70.0) << result;
  }
}

TEST_F(CoreAudioStreamTest, FastForwardResample)
{
  SimConfig config;
  config.mode = AudioStretchMode::Resample;
  config.speed = 2.0f;
  const SimResult result = RunSimulation(config);
  EXPECT_EQ(result.underruns, 0u) << result;
  EXPECT_EQ(result.bad_frames, 0u) << result;
  EXPECT_LT(result.avg_latency_ms, 70.0) << result;
}

// Fast forward without a speed limit, the input arrives faster than the nominal rate.
TEST_F(CoreAudioStreamTest, UnthrottledTimeStretch)
{
  SimConfig config;
  config.actual = 4.0;
  const SimResult result = RunSimulation(config);
  EXPECT_EQ(result.underruns, 0u) << result;
  EXPECT_EQ(result.bad_frames, 0u) << result;
  EXPECT_LT(result.avg_latency_ms, 100.0) << result;
}

// Emulation can't keep up with the target speed.
TEST_F(CoreAudioStreamTest, CantKeepUpTimeStretch)
{
  SimConfig config;
  config.actual = 0.9;
  const SimResult result = RunSimulation(config);
  EXPECT_EQ(result.bad_frames, 0u) << result;
  EXPECT_LE(result.underruns, 2u) << result;
  EXPECT_LT(result.avg_latency_ms, 150.0) << result;
}

TEST_F(CoreAudioStreamTest, SlowMotionTimeStretch)
{
  SimConfig config;
  config.speed = 0.5f;
  const SimResult result = RunSimulation(config);
  EXPECT_EQ(result.underruns, 0u) << result;
  EXPECT_EQ(result.bad_frames, 0u) << result;
  EXPECT_LT(result.avg_latency_ms, 200.0) << result;
}

// Emulation stalls, e.g. loading. Underruns once, then recovers.
TEST_F(CoreAudioStreamTest, ProducerStallTimeStretch)
{
  SimConfig config;
  config.producer_stall_at = 5.0;
  config.producer_stall_ms = 300.0;
  const SimResult result = RunSimulation(config);
  EXPECT_EQ(result.underruns, 1u) << result;
  EXPECT_EQ(result.clicks, 0u) << result;
  EXPECT_EQ(result.bad_frames, 0u) << result;
  EXPECT_LT(result.avg_latency_ms, 70.0) << result;
}

TEST_F(CoreAudioStreamTest, ProducerStallOff)
{
  SimConfig config;
  config.mode = AudioStretchMode::Off;
  config.producer_stall_at = 5.0;
  config.producer_stall_ms = 300.0;
  const SimResult result = RunSimulation(config);
  EXPECT_EQ(result.underruns, 1u) << result;
  EXPECT_EQ(result.clicks, 0u) << result;
  EXPECT_EQ(result.bad_frames, 0u) << result;
}

// Audio device misses a callback or two.
TEST_F(CoreAudioStreamTest, ConsumerHiccupTimeStretch)
{
  SimConfig config;
  config.consumer_stall_at = 5.0;
  config.consumer_stall_ms = 40.0;
  const SimResult result = RunSimulation(config);
  EXPECT_EQ(result.underruns, 0u) << result;
  EXPECT_EQ(result.clicks, 0u) << result;
  EXPECT_EQ(result.bad_frames, 0u) << result;
  EXPECT_LT(result.avg_latency_ms, 70.0) << result;
}

// Audio device stalls for a long time, e.g. switching devices. The latency shouldn't stay built up afterwards.
TEST_F(CoreAudioStreamTest, ConsumerStallTimeStretch)
{
  SimConfig config;
  config.consumer_stall_at = 5.0;
  config.consumer_stall_ms = 300.0;
  const SimResult result = RunSimulation(config);

  // The backlog is below the input drop threshold, so the tempo overshoots while playing through it, and it underruns
  // a few times. The audio thread runs at a high priority, so a stall this long is unlikely.
  EXPECT_LE(result.underruns, 3u) << result;
  EXPECT_EQ(result.bad_frames, 0u) << result;
  EXPECT_LT(result.avg_latency_ms, 70.0) << result;
}

TEST_F(CoreAudioStreamTest, SpeedIncreaseTimeStretch)
{
  SimConfig config;
  config.speed_change_at = 5.0;
  config.speed_change_to = 4.0f;
  const SimResult result = RunSimulation(config);
  EXPECT_EQ(result.underruns, 0u) << result;
  EXPECT_EQ(result.bad_frames, 0u) << result;
  EXPECT_LT(result.avg_latency_ms, 70.0) << result;
}

// Dropping back from fast forward shouldn't leave the latency built up at the higher speed.
TEST_F(CoreAudioStreamTest, SpeedDecreaseTimeStretch)
{
  SimConfig config;
  config.speed = 4.0f;
  config.speed_change_at = 5.0;
  config.speed_change_to = 1.0f;
  const SimResult result = RunSimulation(config);
  EXPECT_LE(result.underruns, 1u) << result;
  EXPECT_EQ(result.bad_frames, 0u) << result;
  EXPECT_LT(result.avg_latency_ms, 70.0) << result;
}

TEST_F(CoreAudioStreamTest, SpeedDecreaseResample)
{
  SimConfig config;
  config.mode = AudioStretchMode::Resample;
  config.speed = 2.0f;
  config.speed_change_at = 5.0;
  config.speed_change_to = 1.0f;
  const SimResult result = RunSimulation(config);
  EXPECT_EQ(result.underruns, 0u) << result;
  EXPECT_EQ(result.bad_frames, 0u) << result;
  EXPECT_LT(result.avg_latency_ms, 70.0) << result;
}

TEST_F(CoreAudioStreamTest, ModeChange)
{
  for (const auto [from, to] : {std::pair{AudioStretchMode::TimeStretch, AudioStretchMode::Off},
                                std::pair{AudioStretchMode::Off, AudioStretchMode::TimeStretch},
                                std::pair{AudioStretchMode::Resample, AudioStretchMode::TimeStretch}})
  {
    SCOPED_TRACE(CoreAudioStream::GetStretchModeName(from));
    SCOPED_TRACE(CoreAudioStream::GetStretchModeName(to));
    SimConfig config;
    config.mode = from;
    config.mode_change_at = 5.0;
    config.mode_change_to = to;
    config.seconds = 10.0;
    const SimResult result = RunSimulation(config);

    // The buffer is emptied, so it'll underrun once, and cut off without a fade. The stream is stopped while
    // changing, so that's fine in practice.
    EXPECT_LE(result.underruns, 1u) << result;
    EXPECT_LE(result.clicks, 1u) << result;
    EXPECT_EQ(result.bad_frames, 0u) << result;
    EXPECT_LT(result.avg_latency_ms, 70.0) << result;
  }
}
