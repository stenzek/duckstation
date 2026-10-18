// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/bitutils.h"
#include "common/gsvector.h"
#include "common/types.h"

#include "core/cpu_core.h"
#include "core/interrupt_controller.h"
#include "core/settings.h"
#include "core/spu.h"
#include "core/system.h"
#include "core/timing_event.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <random>

namespace {

static constexpr u32 NUM_VOICES = 24;

struct VoiceLoopResult
{
  std::array<u16, NUM_VOICES> steps;
  std::array<bool, NUM_VOICES> noise_enabled;
  s32 reverb_left;
  s32 reverb_right;

  bool operator==(const VoiceLoopResult&) const = default;
};

static VoiceLoopResult RunIndexedVoiceLoop(const std::array<s32, NUM_VOICES>& volumes,
                                           const std::array<s32, NUM_VOICES>& left,
                                           const std::array<s32, NUM_VOICES>& right,
                                           const std::array<u16, NUM_VOICES>& rates, u32 noise_modes,
                                           u32 pitch_modulation_enable, u32 reverb_on)
{
  VoiceLoopResult result = {};
  for (u32 voice = 0; voice < NUM_VOICES; voice++)
  {
    const bool noise_enabled = ((noise_modes >> voice) & 1u) != 0;
    const bool pitch_enabled = voice > 0 && ((pitch_modulation_enable >> voice) & 1u) != 0;
    u16 step = rates[voice];
    if (pitch_enabled)
    {
      const s32 factor = std::clamp(volumes[voice - 1], -0x8000, 0x7FFF) + 0x8000;
      step = static_cast<u16>(static_cast<u32>(static_cast<s32>(static_cast<s16>(step)) * factor) >> 15);
    }
    result.steps[voice] = std::min<u16>(step, 0x3FFF);

    if ((reverb_on >> voice) & 1u)
    {
      result.reverb_left += left[voice];
      result.reverb_right += right[voice];
    }

    result.noise_enabled[voice] = noise_enabled;
  }
  return result;
}

static VoiceLoopResult RunShiftedVoiceLoop(const std::array<s32, NUM_VOICES>& volumes,
                                           const std::array<s32, NUM_VOICES>& left,
                                           const std::array<s32, NUM_VOICES>& right,
                                           const std::array<u16, NUM_VOICES>& rates, u32 noise_modes,
                                           u32 pitch_modulation_enable, u32 reverb_on)
{
  VoiceLoopResult result = {};
  pitch_modulation_enable &= ~1u;
  s32 previous_voice_last_volume = 0;
  u32 voice = 0;
  for (const s32 volume : volumes)
  {
    const bool noise_enabled = (noise_modes & 1u) != 0;
    const bool pitch_enabled = (pitch_modulation_enable & 1u) != 0;
    u16 step = rates[voice];
    if (pitch_enabled)
    {
      const s32 factor = std::clamp(previous_voice_last_volume, -0x8000, 0x7FFF) + 0x8000;
      step = static_cast<u16>(static_cast<u32>(static_cast<s32>(static_cast<s16>(step)) * factor) >> 15);
    }
    result.steps[voice] = std::min<u16>(step, 0x3FFF);

    if (reverb_on & 1u)
    {
      result.reverb_left += left[voice];
      result.reverb_right += right[voice];
    }

    result.noise_enabled[voice] = noise_enabled;
    previous_voice_last_volume = volume;
    noise_modes >>= 1;
    pitch_modulation_enable >>= 1;
    reverb_on >>= 1;
    voice++;
  }
  return result;
}

TEST(SPU, ShiftedVoiceFlagsMatchIndexedFlags)
{
  std::mt19937 generator(0x4D595350u);
  std::uniform_int_distribution<u32> bits_distribution;
  std::uniform_int_distribution<s32> sample_distribution(-0x10000, 0xFFFF);
  std::uniform_int_distribution<u32> rate_distribution(0, 0xFFFF);

  for (u32 iteration = 0; iteration < 10000; iteration++)
  {
    std::array<s32, NUM_VOICES> volumes;
    std::array<s32, NUM_VOICES> left;
    std::array<s32, NUM_VOICES> right;
    std::array<u16, NUM_VOICES> rates;
    for (u32 voice = 0; voice < NUM_VOICES; voice++)
    {
      volumes[voice] = sample_distribution(generator);
      left[voice] = sample_distribution(generator);
      right[voice] = sample_distribution(generator);
      rates[voice] = static_cast<u16>(rate_distribution(generator));
    }

    const u32 noise_modes = bits_distribution(generator);
    const u32 pitch_modulation_enable = bits_distribution(generator);
    const u32 reverb_on = bits_distribution(generator);
    EXPECT_EQ(RunShiftedVoiceLoop(volumes, left, right, rates, noise_modes, pitch_modulation_enable, reverb_on),
              RunIndexedVoiceLoop(volumes, left, right, rates, noise_modes, pitch_modulation_enable, reverb_on));
  }
}

static s32 GaussianScalar(const std::array<s16, 4>& samples, const std::array<s16, 4>& coefficients)
{
  s32 result = 0;
  for (u32 i = 0; i < samples.size(); i++)
    result += (static_cast<s32>(samples[i]) * static_cast<s32>(coefficients[i])) >> 14;
  return result >> 1;
}

static s32 GaussianSIMD(const std::array<s16, 4>& samples, const std::array<s16, 4>& coefficients)
{
  const GSVector4i sample_vector = GSVector4i::loadl<false>(samples.data());
  const GSVector4i coefficient_vector = GSVector4i::loadl<false>(coefficients.data());
  // Matches the zero-extended coefficient table layout used by SPU::Voice::Interpolate().
  return sample_vector.upl16().madd_s16(coefficient_vector.upl16()).sra32<14>().addv_s32() >> 1;
}

TEST(SPU, GaussianPhaseMajorCoefficientsMatchIndexedTable)
{
  std::array<s16, 0x200> source = {};
  for (u32 i = 0; i < source.size(); i++)
    source[i] = static_cast<s16>(i - 0x100);

  std::array<std::array<s16, 4>, 0x100> phase_major = {};
  for (u32 phase = 0; phase < phase_major.size(); phase++)
  {
    phase_major[phase] = {source[0x0FF - phase], source[0x1FF - phase], source[0x100 + phase], source[phase]};
  }

  for (u32 phase = 0; phase < phase_major.size(); phase++)
  {
    EXPECT_EQ(phase_major[phase][0], source[0x0FF - phase]);
    EXPECT_EQ(phase_major[phase][1], source[0x1FF - phase]);
    EXPECT_EQ(phase_major[phase][2], source[0x100 + phase]);
    EXPECT_EQ(phase_major[phase][3], source[phase]);
  }
}

TEST(SPU, GaussianSIMDMatchesScalar)
{
  static constexpr std::array<std::array<s16, 4>, 6> edge_samples = {{
    {{-32768, -32768, -32768, -32768}},
    {{32767, 32767, 32767, 32767}},
    {{-32768, 32767, -32768, 32767}},
    {{32767, -32768, 32767, -32768}},
    {{-32768, 0, 0, 32767}},
    {{0, 0, 0, 0}},
  }};
  static constexpr std::array<std::array<s16, 4>, 5> edge_coefficients = {{
    {{-1, 22963, 0, 0}},
    {{22963, -1, 0, 0}},
    {{8192, 8192, 8192, 8192}},
    {{-8192, -8192, -8192, -8192}},
    {{0, 0, 0, 0}},
  }};

  for (const std::array<s16, 4>& samples : edge_samples)
  {
    for (const std::array<s16, 4>& coefficients : edge_coefficients)
      EXPECT_EQ(GaussianSIMD(samples, coefficients), GaussianScalar(samples, coefficients));
  }

  std::mt19937 generator(0x47415553u);
  std::uniform_int_distribution<s32> sample_distribution(-32768, 32767);
  std::uniform_int_distribution<s32> coefficient_distribution(-22963, 22963);
  for (u32 iteration = 0; iteration < 100000; iteration++)
  {
    std::array<s16, 4> samples;
    std::array<s16, 4> coefficients;
    s64 wide_sum = 0;
    for (u32 i = 0; i < samples.size(); i++)
    {
      samples[i] = static_cast<s16>(sample_distribution(generator));
      coefficients[i] = static_cast<s16>(coefficient_distribution(generator));
      wide_sum += static_cast<s64>(samples[i]) * coefficients[i];
    }

    if (wide_sum >= std::numeric_limits<s32>::min() && wide_sum <= std::numeric_limits<s32>::max())
    {
      EXPECT_EQ(GaussianSIMD(samples, coefficients), GaussianScalar(samples, coefficients));
    }
  }
}

struct CaptureState
{
  std::array<u8, 4 * 0x400> ram;
  u16 position;
  u16 irq_address;
  bool irq_enabled;
  bool irq_flag;
  bool second_half;
  u32 triggered_address;

  bool operator==(const CaptureState&) const = default;
};

static void TriggerCaptureIRQ(CaptureState* state, u32 address)
{
  state->irq_flag = true;
  state->triggered_address = address;
}

static void WriteCaptureBuffersOriginal(CaptureState* state, const std::array<s16, 4>& values)
{
  for (u32 index = 0; index < values.size(); index++)
  {
    const u32 ram_address = index * 0x400u | state->position;
    std::memcpy(&state->ram[ram_address], &values[index], sizeof(values[index]));
    if (state->irq_enabled && !state->irq_flag && static_cast<u32>(state->irq_address) * 8 == ram_address)
      TriggerCaptureIRQ(state, ram_address);
  }

  state->position += sizeof(s16);
  state->position %= 0x400;
  state->second_half = state->position >= 0x200;
}

static void WriteCaptureBuffersCombined(CaptureState* state, const std::array<s16, 4>& values)
{
  const u32 position = state->position;
  const u32 irq_address = static_cast<u32>(state->irq_address) * 8;
  bool irq_triggerable = state->irq_enabled && !state->irq_flag;
  for (u32 index = 0; index < values.size(); index++)
  {
    const u32 ram_address = index * 0x400u | position;
    std::memcpy(&state->ram[ram_address], &values[index], sizeof(values[index]));
    if (irq_triggerable && irq_address == ram_address)
    {
      TriggerCaptureIRQ(state, ram_address);
      irq_triggerable = false;
    }
  }

  state->position = (position + sizeof(s16)) & 0x3FF;
  state->second_half = state->position >= 0x200;
}

TEST(SPU, CombinedCaptureWritesMatchIndividualWrites)
{
  static constexpr std::array<s16, 4> values = {{-32768, -1, 0x1234, 32767}};
  for (u32 position = 0; position < 0x400; position += 2)
  {
    for (u32 irq_case = 0; irq_case < 7; irq_case++)
    {
      CaptureState original = {};
      original.ram.fill(0xA5);
      original.position = static_cast<u16>(position);
      original.irq_enabled = irq_case != 5;
      original.irq_flag = irq_case == 6;
      if (irq_case < 4 && (position % 8) == 0)
        original.irq_address = static_cast<u16>((irq_case * 0x400 + position) / 8);
      else
        original.irq_address = 0xFFFF;
      original.triggered_address = 0xFFFFFFFF;

      CaptureState combined = original;
      WriteCaptureBuffersOriginal(&original, values);
      WriteCaptureBuffersCombined(&combined, values);
      EXPECT_EQ(combined, original) << "position=" << position << " irq_case=" << irq_case;
    }
  }
}

struct ADPCMDecodeResult
{
  std::array<s16, 28> samples;
  std::array<s16, 2> last_samples;

  bool operator==(const ADPCMDecodeResult&) const = default;
};

static s32 Clamp16ForTest(s32 value)
{
  return (value < -0x8000) ? -0x8000 : (value > 0x7FFF) ? 0x7FFF : value;
}

static ADPCMDecodeResult DecodeADPCMOriginal(const std::array<u8, 14>& data, u8 raw_shift, u8 filter,
                                             std::array<s16, 2> history)
{
  static constexpr std::array<s8, 16> filter_table_pos = {{0, 60, 115, 98, 122, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
  static constexpr std::array<s8, 16> filter_table_neg = {{0, 0, -52, -55, -60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};

  ADPCMDecodeResult result = {};
  const u8 shift = (raw_shift > 12) ? 9 : raw_shift;
  for (u32 i = 0; i < result.samples.size(); i++)
  {
    const u8 nibble = (data[i / 2] >> ((i % 2) * 4)) & 0x0F;
    s32 sample = static_cast<s16>(static_cast<u16>(nibble) << 12) >> shift;
    sample += (history[0] * filter_table_pos[filter]) >> 6;
    sample += (history[1] * filter_table_neg[filter]) >> 6;
    history[1] = history[0];
    result.samples[i] = history[0] = static_cast<s16>(Clamp16ForTest(sample));
  }
  result.last_samples = history;
  return result;
}

static ADPCMDecodeResult DecodeADPCMPaired(const std::array<u8, 14>& edata, u8 raw_shift, u8 filter,
                                           std::array<s16, 2> history)
{
  static constexpr std::array<s8, 16> filter_table_pos = {{0, 60, 115, 98, 122, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
  static constexpr std::array<s8, 16> filter_table_neg = {{0, 0, -52, -55, -60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};

  ADPCMDecodeResult result = {};
  const u8 shift = (raw_shift > 12) ? 9 : raw_shift;
  const s32 filter_pos = filter_table_pos[filter];
  const s32 filter_neg = filter_table_neg[filter];

  // decode pairs of nibbles on each iteration instead of alternating
  s32 last_sample_0 = history[0];
  s32 last_sample_1 = history[1];
  s16* output = result.samples.data();
  for (u32 i = 0; i < static_cast<u32>(edata.size()); i++)
  {
    const u8 data = edata[i];

    // extend 4-bit to 16-bit, apply shift from header and mix in previous samples
    // this is interleaved and whacky to try to maximize instruction-level parallelism, but basically, it's:
    // s32(static_cast<s16>(ZeroExtend16(block.GetNibble(i)) << 12) >> shift) +
    //   (last_samples[0] * filter_pos) >> 6
    //   (last_samples[1] * filter_neg) >> 6
    s32 s0 = static_cast<s32>(static_cast<s16>(ZeroExtend16(data & 0x0F) << 12) >> shift);
    s32 s1 = static_cast<s32>(static_cast<s16>(ZeroExtend16(data >> 4) << 12) >> shift);
    s0 += (last_sample_0 * filter_pos) >> 6;
    s1 += (last_sample_0 * filter_neg) >> 6;
    s0 += (last_sample_1 * filter_neg) >> 6;
    s0 = Clamp16ForTest(s0);
    s1 += (s0 * filter_pos) >> 6;
    s1 = Clamp16ForTest(s1);

    *(output++) = Truncate16(last_sample_1 = s0);
    *(output++) = Truncate16(last_sample_0 = s1);
  }

  result.last_samples[0] = Truncate16(last_sample_0);
  result.last_samples[1] = Truncate16(last_sample_1);
  return result;
}

TEST(SPU, PairedADPCMDecodeMatchesNibbleDecode)
{
  std::mt19937 generator(0x41445043u);
  std::uniform_int_distribution<u32> byte_distribution(0, 0xFF);
  std::uniform_int_distribution<s32> sample_distribution(-32768, 32767);

  for (u32 filter = 0; filter < 16; filter++)
  {
    for (u32 shift = 0; shift < 16; shift++)
    {
      for (u32 iteration = 0; iteration < 256; iteration++)
      {
        std::array<u8, 14> data;
        for (u8& value : data)
          value = static_cast<u8>(byte_distribution(generator));
        const std::array<s16, 2> history = {
          {static_cast<s16>(sample_distribution(generator)), static_cast<s16>(sample_distribution(generator))}};

        EXPECT_EQ(DecodeADPCMPaired(data, static_cast<u8>(shift), static_cast<u8>(filter), history),
                  DecodeADPCMOriginal(data, static_cast<u8>(shift), static_cast<u8>(filter), history))
          << "filter=" << filter << " shift=" << shift << " iteration=" << iteration;
      }
    }
  }
}

class SPUHardwareTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_old_settings = g_settings;
    g_settings.audio_backend = AudioBackend::Null;
    g_settings.cpu_overclock_enable = false;
    g_settings.SetCPUOverclockPercent(100);
    g_settings.UpdateOverclockActive();
    CPU::g_state.cop0_regs.sr.bits = 0;
    CPU::g_state.pending_ticks = 0;
    TimingEvents::Initialize();
    System::UpdateOverclock();
    DeactivateAllEvents();
    TimingEvents::Reset();
    InterruptController::Reset();
    SPU::Initialize();
    SPU::SetAudioOutputMuted(true);

    // Keep reverb away from the sample data and enable voice processing.
    SPU::WriteRegister(0x1A2, 0xFFFF);
    SPU::WriteRegister(0x1AA, 0xC000);
    auto& ram = SPU::GetWritableRAM();
    ram[0x1001] = 7; // Loop a silent block unless a test installs a waveform.
  }

  void TearDown() override
  {
    SPU::Shutdown();
    SPU::SetAudioOutputMuted(false);
    DeactivateAllEvents();
    g_settings = m_old_settings;
  }

  static void DeactivateAllEvents()
  {
    while (TimingEvent* event = *TimingEvents::GetHeadEventPtr())
      event->Deactivate();
  }

  static void RunSamples(u32 samples)
  {
    for (u32 i = 0; i < samples; i++)
    {
      CPU::AddPendingTicks(768);
      if (CPU::GetPendingTicks() >= CPU::g_state.downcount)
        TimingEvents::RunEvents();
    }
    SPU::GeneratePendingSamples();
  }

  static void KeyOn(u16 adsr_low, u16 adsr_high, u16 pitch = 0x1000)
  {
    SPU::WriteRegister(0x14, pitch);
    SPU::WriteRegister(0x16, 0x200);
    SPU::WriteRegister(0x1E, 0x200);
    SPU::WriteRegister(0x18, adsr_low);
    SPU::WriteRegister(0x1A, adsr_high);
    SPU::WriteRegister(0x188, 2);
  }

  static u16 Envelope() { return SPU::ReadRegister(0x1C); }

  template<size_t N>
  static void CheckCapture(const std::array<u8, 14>& adpcm, u16 pitch, const std::array<s16, N>& golden)
  {
    auto& ram = SPU::GetWritableRAM();
    std::copy(adpcm.begin(), adpcm.end(), ram.begin() + 0x1002);
    KeyOn(0x000F, 0x1FC0, pitch);
    RunSamples(1024); // Capture a complete, steady lap with no onset samples.

    std::array<s16, 512> capture;
    std::memcpy(capture.data(), &ram[0x800], sizeof(capture));
    u32 best_mismatches = static_cast<u32>(capture.size());
    for (u32 phase = 0; phase < N; phase++)
    {
      u32 mismatches = 0;
      for (u32 i = 0; i < capture.size(); i++)
        mismatches += (capture[i] != golden[(phase + i) % N]);
      best_mismatches = std::min(best_mismatches, mismatches);
    }
    EXPECT_EQ(best_mismatches, 0u);
  }

  Settings m_old_settings;
};

TEST_F(SPUHardwareTest, DecayCrossesSustainThreshold)
{
  // nugget-tests/spu: decay at the lowest sustain level settles at 07FFh,
  // and even the maximum sustain level must perform a decay step (7FF7h).
  KeyOn(0x04A0, 0x5FC0);
  RunSamples(8192);
  EXPECT_EQ(Envelope(), 0x07FFu);
  KeyOn(0x04FF, 0x1FCC);
  RunSamples(512);
  EXPECT_EQ(Envelope(), 0x7FF7u);
  SPU::WriteRegister(0x18C, 2);
  RunSamples(8192);
  EXPECT_EQ(Envelope(), 0u);
}

TEST_F(SPUHardwareTest, UnityPitchCaptureMatchesHardware)
{
  static constexpr std::array<u8, 14> adpcm = {0x10, 0x43, 0x76, 0x77, 0x77, 0x46, 0x13,
                                               0xE0, 0xBC, 0x89, 0x88, 0x88, 0xB9, 0xEC};
  // Periodic tail of nugget-tests/spu/sine.test.pcm, captured on hardware.
  static constexpr std::array<s16, 28> golden = {
    5822,  8467,   11942,  13978,  14278,  14278,  14278,  13974,  11930,  8455,   5811,  2335,  -309, -4088,
    -7864, -10508, -13984, -16020, -16320, -16320, -16320, -16016, -13972, -10497, -7852, -4073, -297, 2347,
  };
  CheckCapture(adpcm, 0x1000, golden);
}

TEST_F(SPUHardwareTest, FractionalPitchCaptureMatchesHardware)
{
  static constexpr std::array<u8, 14> adpcm = {0x60, 0x37, 0x9D, 0x0A, 0x76, 0xD3, 0xA9,
                                               0x60, 0x37, 0x9D, 0x0A, 0x76, 0xD3, 0xA9};
  // Periodic tail of nugget-tests/spu/sine7_pitch_1300.test.pcm, captured on hardware.
  static constexpr std::array<s16, 112> golden = {
    -1507,  11165,  11669,  695,    -10926, -11872, 22,     11890,  10897,  -742,   -11693, -11142, 1552,   12452,
    10000,  -2165,  -12313, -10246, 3070,   12856,  8998,   -3554,  -12775, -9191,  4564,   13108,  7908,   -4891,
    -13073, -7990,  6011,   13210,  6737,   -6165,  -13205, -6666,  7382,   13154,  5494,   -7370,  -13177, -5251,
    8644,   12933,  4186,   -8499,  -12994, -3776,  9769,   12548,  2821,   -9545,  -12662, -2268,  10740,  12000,
    1410,   -10493, -12176, -742,   11549,  11300,  -23,    -11326, -11528, 788,    12191,  10463,  -1458,  -12022,
    -10715, 2314,   12673,  9511,   -2866,  -12564, -9739,  3822,   13001,  8463,   -4230,  -12944, -8608,  5295,
    13178,  7332,   -5536,  -13159, -7341,  6707,   13202,  6124,   -6777,  -13210, -5967,  8028,   13064,  4848,
    -7945,  -13105, -4519,  9225,   12761,  3509,   -9033,  -12847, -3025,  10275,  12294,  2120,   -10032, -12438,
  };
  CheckCapture(adpcm, 0x1300, golden);
}

TEST_F(SPUHardwareTest, PrimedInterpolationAtDoublePitchMatchesHardware)
{
  static constexpr std::array<u8, 14> adpcm = {0x10, 0x43, 0x76, 0x77, 0x77, 0x46, 0x13,
                                               0xE0, 0xBC, 0x89, 0x88, 0x88, 0xB9, 0xEC};
  // Periodic tail of nugget tests/spu/sine_pitch_2000.test.pcm, captured on hardware.
  static constexpr std::array<s16, 14> golden = {
    8467, 13978, 14278, 13974, 8455, 2335, -4088, -10508, -16020, -16320, -16016, -10497, -4073, 2347,
  };
  CheckCapture(adpcm, 0x2000, golden);
}

TEST_F(SPUHardwareTest, PrimedInterpolationAtFractionalPitchMatchesHardware)
{
  static constexpr std::array<u8, 14> adpcm = {0x10, 0x43, 0x76, 0x77, 0x77, 0x46, 0x13,
                                               0xE0, 0xBC, 0x89, 0x88, 0x88, 0xB9, 0xEC};
  // Periodic tail of nugget tests/spu/sine_pitch_1500.test.pcm, captured on hardware.
  static constexpr std::array<s16, 64> golden = {
    -16320, -16303, -14672, -10310, -6566,  -1349,  2347,   6685,   10668,  13910,  14278,  14278,  14086,
    11320,  7134,   2944,   -708,   -5857,  -9804,  -14171, -16263, -16319, -16320, -15593, -11771, -8039,
    -3055,  1175,   5424,   9076,   13210,  14278,  14278,  14262,  12631,  8268,   4534,   556,    -4088,
    -8726,  -12709, -15952, -16320, -16319, -16128, -13362, -9175,  -4837,  69,     3855,   7763,   12129,
    14221,  14278,  14278,  13551,  9729,   5998,   1632,   -2320,  -7466,  -11118, -15251, -16319,
  };
  CheckCapture(adpcm, 0x1500, golden);
}

} // namespace
