// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/bitutils.h"
#include "common/gsvector.h"
#include "common/types.h"

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
  s32 result = static_cast<s32>(samples[0]) * static_cast<s32>(coefficients[0]);
  result += static_cast<s32>(samples[1]) * static_cast<s32>(coefficients[1]);
  result += static_cast<s32>(samples[2]) * static_cast<s32>(coefficients[2]);
  result += static_cast<s32>(samples[3]) * static_cast<s32>(coefficients[3]);
  return result >> 15;
}

static s32 GaussianSIMD(const std::array<s16, 4>& samples, const std::array<s16, 4>& coefficients)
{
  const GSVector4i sample_vector = GSVector4i::loadl<false>(samples.data());
  const GSVector4i coefficient_vector = GSVector4i::loadl<false>(coefficients.data());
  return sample_vector.madd_s16(coefficient_vector).xy().addv_s32() >> 15;
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
    const u8 data = edata.data[i];

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

} // namespace
