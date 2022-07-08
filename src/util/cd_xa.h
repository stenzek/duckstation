#pragma once
#include "common/bitfield.h"
#include "common/types.h"

namespace CDXA {
enum
{
  XA_SUBHEADER_SIZE = 4,
  XA_ADPCM_SAMPLES_PER_SECTOR_4BIT = 4032, // 28 words * 8 nibbles per word * 18 chunks
  XA_ADPCM_SAMPLES_PER_SECTOR_8BIT = 2016  // 28 words * 4 bytes per word * 18 chunks
};

struct XASubHeader
{
  u8 file_number;
  u8 channel_number;
  union Submode
  {
    u8 bits;
    BitField<u8, bool, 0, 1> eor;
    BitField<u8, bool, 1, 1> video;
    BitField<u8, bool, 2, 1> audio;
    BitField<u8, bool, 3, 1> data;
    BitField<u8, bool, 4, 1> trigger;
    BitField<u8, bool, 5, 1> form2;
    BitField<u8, bool, 6, 1> realtime;
    BitField<u8, bool, 7, 1> eof;
  } submode;
  union Codinginfo
  {
    u8 bits;

    BitField<u8, u8, 0, 2> mono_stereo;
    BitField<u8, u8, 2, 2> sample_rate;
    BitField<u8, u8, 4, 2> bits_per_sample;
    BitField<u8, bool, 6, 1> emphasis;

    bool IsStereo() const { return mono_stereo == 1; }
    bool IsHalfSampleRate() const { return sample_rate == 1; }
    u32 GetSampleRate() const { return sample_rate == 1 ? 18900 : 37800; }
    u32 GetBitsPerSample() const { return bits_per_sample == 1 ? 8 : 4; }
    u32 GetSamplesPerSector() const
    {
      return bits_per_sample == 1 ? XA_ADPCM_SAMPLES_PER_SECTOR_8BIT : XA_ADPCM_SAMPLES_PER_SECTOR_4BIT;
    }
  } codinginfo;
};

union XA_ADPCMBlockHeader
{
  u8 bits;

  BitField<u8, u8, 0, 4> shift;
  BitField<u8, u8, 4, 2> filter;

  // For both 4bit and 8bit ADPCM, reserved shift values 13..15 will act same as shift=9).
  u8 GetShift() const
  {
    const u8 shift_value = shift;
    return (shift_value > 12) ? 9 : shift_value;
  }

  u8 GetFilter() const { return filter; }
};
static_assert(sizeof(XA_ADPCMBlockHeader) == 1, "XA-ADPCM block header is one byte");

// Decodes XA-ADPCM samples in an audio sector. Stereo samples are interleaved with left first.
void DecodeADPCMSector(const void* data, s16* samples, s32* last_samples);

} // namespace CDXA