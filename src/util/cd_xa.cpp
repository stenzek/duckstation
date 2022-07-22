#include "cd_xa.h"
#include "cd_image.h"
#include <algorithm>
#include <array>

namespace CDXA {
static constexpr std::array<s32, 4> s_xa_adpcm_filter_table_pos = {{0, 60, 115, 98}};
static constexpr std::array<s32, 4> s_xa_adpcm_filter_table_neg = {{0, 0, -52, -55}};

template<bool IS_STEREO, bool IS_8BIT>
static void DecodeXA_ADPCMChunk(const u8* chunk_ptr, s16* samples, s32* last_samples)
{
  // The data layout is annoying here. Each word of data is interleaved with the other blocks, requiring multiple
  // passes to decode the whole chunk.
  constexpr u32 NUM_BLOCKS = IS_8BIT ? 4 : 8;
  constexpr u32 WORDS_PER_BLOCK = 28;

  const u8* headers_ptr = chunk_ptr + 4;
  const u8* words_ptr = chunk_ptr + 16;

  for (u32 block = 0; block < NUM_BLOCKS; block++)
  {
    const XA_ADPCMBlockHeader block_header{headers_ptr[block]};
    const u8 shift = block_header.GetShift();
    const u8 filter = block_header.GetFilter();
    const s32 filter_pos = s_xa_adpcm_filter_table_pos[filter];
    const s32 filter_neg = s_xa_adpcm_filter_table_neg[filter];

    s16* out_samples_ptr =
      IS_STEREO ? &samples[(block / 2) * (WORDS_PER_BLOCK * 2) + (block % 2)] : &samples[block * WORDS_PER_BLOCK];
    constexpr u32 out_samples_increment = IS_STEREO ? 2 : 1;

    for (u32 word = 0; word < 28; word++)
    {
      // NOTE: assumes LE
      u32 word_data;
      std::memcpy(&word_data, &words_ptr[word * sizeof(u32)], sizeof(word_data));

      // extract nibble from block
      const u32 nibble = IS_8BIT ? ((word_data >> (block * 8)) & 0xFF) : ((word_data >> (block * 4)) & 0x0F);
      const s16 sample = static_cast<s16>(Truncate16(nibble << 12)) >> shift;

      // mix in previous values
      s32* prev = IS_STEREO ? &last_samples[(block & 1) * 2] : last_samples;
      const s32 interp_sample = s32(sample) + ((prev[0] * filter_pos) + (prev[1] * filter_neg) + 32) / 64;

      // update previous values
      prev[1] = prev[0];
      prev[0] = interp_sample;

      *out_samples_ptr = static_cast<s16>(std::clamp<s32>(interp_sample, -0x8000, 0x7FFF));
      out_samples_ptr += out_samples_increment;
    }
  }
}

template<bool IS_STEREO, bool IS_8BIT>
static void DecodeXA_ADPCMChunks(const u8* chunk_ptr, s16* samples, s32* last_samples)
{
  constexpr u32 NUM_CHUNKS = 18;
  constexpr u32 CHUNK_SIZE_IN_BYTES = 128;
  constexpr u32 WORDS_PER_CHUNK = 28;
  constexpr u32 SAMPLES_PER_CHUNK = WORDS_PER_CHUNK * (IS_8BIT ? 4 : 8);

  for (u32 i = 0; i < NUM_CHUNKS; i++)
  {
    DecodeXA_ADPCMChunk<IS_STEREO, IS_8BIT>(chunk_ptr, samples, last_samples);
    samples += SAMPLES_PER_CHUNK;
    chunk_ptr += CHUNK_SIZE_IN_BYTES;
  }
}

void DecodeADPCMSector(const void* data, s16* samples, s32* last_samples)
{
  const XASubHeader* subheader = reinterpret_cast<const XASubHeader*>(
    reinterpret_cast<const u8*>(data) + CDImage::SECTOR_SYNC_SIZE + sizeof(CDImage::SectorHeader));

  // The XA subheader is repeated?
  const u8* chunk_ptr = reinterpret_cast<const u8*>(data) + CDImage::SECTOR_SYNC_SIZE + sizeof(CDImage::SectorHeader) +
                        sizeof(XASubHeader) + 4;

  if (subheader->codinginfo.bits_per_sample != 1)
  {
    if (subheader->codinginfo.mono_stereo != 1)
      DecodeXA_ADPCMChunks<false, false>(chunk_ptr, samples, last_samples);
    else
      DecodeXA_ADPCMChunks<true, false>(chunk_ptr, samples, last_samples);
  }
  else
  {
    if (subheader->codinginfo.mono_stereo != 1)
      DecodeXA_ADPCMChunks<false, true>(chunk_ptr, samples, last_samples);
    else
      DecodeXA_ADPCMChunks<true, true>(chunk_ptr, samples, last_samples);
  }
}

} // namespace CDXA
