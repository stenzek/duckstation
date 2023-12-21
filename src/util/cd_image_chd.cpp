// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "cd_image.h"
#include "cd_subchannel_replacement.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/hash_combine.h"
#include "common/heap_array.h"
#include "common/intrin.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include "fmt/format.h"
#include "libchdr/cdrom.h"
#include "libchdr/chd.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>

Log_SetChannel(CDImageCHD);

namespace {

static std::optional<CDImage::TrackMode> ParseTrackModeString(const char* str)
{
  if (std::strncmp(str, "MODE2_FORM_MIX", 14) == 0)
    return CDImage::TrackMode::Mode2FormMix;
  else if (std::strncmp(str, "MODE2_FORM1", 10) == 0)
    return CDImage::TrackMode::Mode2Form1;
  else if (std::strncmp(str, "MODE2_FORM2", 10) == 0)
    return CDImage::TrackMode::Mode2Form2;
  else if (std::strncmp(str, "MODE2_RAW", 9) == 0)
    return CDImage::TrackMode::Mode2Raw;
  else if (std::strncmp(str, "MODE1_RAW", 9) == 0)
    return CDImage::TrackMode::Mode1Raw;
  else if (std::strncmp(str, "MODE1", 5) == 0)
    return CDImage::TrackMode::Mode1;
  else if (std::strncmp(str, "MODE2", 5) == 0)
    return CDImage::TrackMode::Mode2;
  else if (std::strncmp(str, "AUDIO", 5) == 0)
    return CDImage::TrackMode::Audio;
  else
    return std::nullopt;
}

static std::vector<std::pair<std::string, chd_header>> s_chd_hash_cache; // <filename, header>
static std::recursive_mutex s_chd_hash_cache_mutex;

class CDImageCHD : public CDImage
{
public:
  CDImageCHD();
  ~CDImageCHD() override;

  bool Open(const char* filename, Error* error);

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasNonStandardSubchannel() const override;
  PrecacheResult Precache(ProgressCallback* progress) override;
  bool IsPrecached() const override;
  s64 GetSizeOnDisk() const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  static constexpr u32 CHD_CD_SECTOR_DATA_SIZE = 2352 + 96;
  static constexpr u32 CHD_CD_TRACK_ALIGNMENT = 4;
  static constexpr u32 MAX_PARENTS = 32; // Surely someone wouldn't be insane enough to go beyond this...

  chd_file* OpenCHD(std::string_view filename, FileSystem::ManagedCFilePtr fp, Error* error, u32 recursion_level);
  bool UpdateHunkBuffer(const Index& index, LBA lba_in_index, u32& hunk_offset);

  static void CopyAndSwap(void* dst_ptr, const u8* src_ptr);

  chd_file* m_chd = nullptr;
  u32 m_hunk_size = 0;
  u32 m_sectors_per_hunk = 0;

  DynamicHeapArray<u8, 16> m_hunk_buffer;
  u32 m_current_hunk_index = static_cast<u32>(-1);
  bool m_precached = false;

  CDSubChannelReplacement m_sbi;
};
} // namespace

CDImageCHD::CDImageCHD() = default;

CDImageCHD::~CDImageCHD()
{
  if (m_chd)
    chd_close(m_chd);
}

chd_file* CDImageCHD::OpenCHD(std::string_view filename, FileSystem::ManagedCFilePtr fp, Error* error,
                              u32 recursion_level)
{
  chd_file* chd;
  chd_error err = chd_open_file(fp.get(), CHD_OPEN_READ | CHD_OPEN_TRANSFER_FILE, nullptr, &chd);
  if (err == CHDERR_NONE)
  {
    // fp is now managed by libchdr
    fp.release();
    return chd;
  }
  else if (err != CHDERR_REQUIRES_PARENT)
  {
    Log_ErrorFmt("Failed to open CHD '{}': {}", filename, chd_error_string(err));
    Error::SetString(error, chd_error_string(err));
    return nullptr;
  }

  if (recursion_level >= MAX_PARENTS)
  {
    Log_ErrorFmt("Failed to open CHD '{}': Too many parent files", filename);
    Error::SetString(error, "Too many parent files");
    return nullptr;
  }

  // Need to get the sha1 to look for.
  chd_header header;
  err = chd_read_header_file(fp.get(), &header);
  if (err != CHDERR_NONE)
  {
    Log_ErrorFmt("Failed to read CHD header '{}': {}", filename, chd_error_string(err));
    Error::SetString(error, chd_error_string(err));
    return nullptr;
  }

  // Find a chd with a matching sha1 in the same directory.
  // Have to do *.* and filter on the extension manually because Linux is case sensitive.
  chd_file* parent_chd = nullptr;
  const std::string parent_dir(Path::GetDirectory(filename));
  const std::unique_lock hash_cache_lock(s_chd_hash_cache_mutex);

  // Memoize which hashes came from what files, to avoid reading them repeatedly.
  for (auto it = s_chd_hash_cache.begin(); it != s_chd_hash_cache.end(); ++it)
  {
    if (!StringUtil::EqualNoCase(parent_dir, Path::GetDirectory(it->first)))
      continue;

    if (!chd_is_matching_parent(&header, &it->second))
      continue;

    // Re-check the header, it might have changed since we last opened.
    chd_header parent_header;
    auto parent_fp = FileSystem::OpenManagedSharedCFile(it->first.c_str(), "rb", FileSystem::FileShareMode::DenyWrite);
    if (parent_fp && chd_read_header_file(parent_fp.get(), &parent_header) == CHDERR_NONE &&
        chd_is_matching_parent(&header, &parent_header))
    {
      // Need to take a copy of the string, because the parent might add to the list and invalidate the iterator.
      const std::string filename_to_open = it->first;

      // Match! Open this one.
      parent_chd = OpenCHD(filename_to_open, std::move(parent_fp), error, recursion_level + 1);
      if (parent_chd)
      {
        Log_VerboseFmt("Using parent CHD '{}' from cache for '{}'.", Path::GetFileName(filename_to_open),
                       Path::GetFileName(filename));
      }
    }

    // No point checking any others. Since we recursively call OpenCHD(), the iterator is invalidated anyway.
    break;
  }
  if (!parent_chd)
  {
    // Look for files in the same directory as the chd.
    FileSystem::FindResultsArray parent_files;
    FileSystem::FindFiles(parent_dir.c_str(), "*.*",
                          FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_KEEP_ARRAY,
                          &parent_files);
    for (FILESYSTEM_FIND_DATA& fd : parent_files)
    {
      if (StringUtil::EndsWithNoCase(Path::GetExtension(fd.FileName), ".chd"))
        continue;

      // Re-check the header, it might have changed since we last opened.
      chd_header parent_header;
      auto parent_fp =
        FileSystem::OpenManagedSharedCFile(fd.FileName.c_str(), "rb", FileSystem::FileShareMode::DenyWrite);
      if (!parent_fp || chd_read_header_file(parent_fp.get(), &parent_header) != CHDERR_NONE)
        continue;

      // Don't duplicate in the cache. But update it, in case the file changed.
      auto cache_it = std::find_if(s_chd_hash_cache.begin(), s_chd_hash_cache.end(),
                                   [&fd](const auto& it) { return it.first == fd.FileName; });
      if (cache_it != s_chd_hash_cache.end())
        std::memcpy(&cache_it->second, &parent_header, sizeof(parent_header));
      else
        s_chd_hash_cache.emplace_back(fd.FileName, parent_header);

      if (!chd_is_matching_parent(&header, &parent_header))
        continue;

      // Match! Open this one.
      parent_chd = OpenCHD(fd.FileName, std::move(parent_fp), error, recursion_level + 1);
      if (parent_chd)
      {
        Log_VerboseFmt("Using parent CHD '{}' for '{}'.", Path::GetFileName(fd.FileName), Path::GetFileName(filename));
        break;
      }
    }
  }
  if (!parent_chd)
  {
    Log_ErrorFmt("Failed to open CHD '{}': Failed to find parent CHD, it must be in the same directory.", filename);
    Error::SetString(error, "Failed to find parent CHD, it must be in the same directory.");
    return nullptr;
  }

  // Now try re-opening with the parent.
  err = chd_open_file(fp.get(), CHD_OPEN_READ | CHD_OPEN_TRANSFER_FILE, parent_chd, &chd);
  if (err != CHDERR_NONE)
  {
    Log_ErrorFmt("Failed to open CHD '{}': {}", filename, chd_error_string(err));
    Error::SetString(error, chd_error_string(err));
    return nullptr;
  }

  // fp now owned by libchdr
  fp.release();
  return chd;
}

bool CDImageCHD::Open(const char* filename, Error* error)
{
  auto fp = FileSystem::OpenManagedSharedCFile(filename, "rb", FileSystem::FileShareMode::DenyWrite);
  if (!fp)
  {
    Log_ErrorFmt("Failed to open CHD '{}': errno {}", filename, errno);
    if (error)
      error->SetErrno(errno);

    return false;
  }

  m_chd = OpenCHD(filename, std::move(fp), error, 0);
  if (!m_chd)
    return false;

  const chd_header* header = chd_get_header(m_chd);
  m_hunk_size = header->hunkbytes;
  if ((m_hunk_size % CHD_CD_SECTOR_DATA_SIZE) != 0)
  {
    Log_ErrorFmt("Hunk size ({}) is not a multiple of {}", m_hunk_size, CHD_CD_SECTOR_DATA_SIZE);
    Error::SetString(error, fmt::format("Hunk size ({}) is not a multiple of {}", m_hunk_size,
                                        static_cast<u32>(CHD_CD_SECTOR_DATA_SIZE)));
    return false;
  }

  m_sectors_per_hunk = m_hunk_size / CHD_CD_SECTOR_DATA_SIZE;
  m_hunk_buffer.resize(m_hunk_size);
  m_filename = filename;

  u32 disc_lba = 0;
  u64 file_lba = 0;

  // for each track..
  int num_tracks = 0;
  for (;;)
  {
    char metadata_str[256];
    char type_str[256];
    char subtype_str[256];
    char pgtype_str[256];
    char pgsub_str[256];
    u32 metadata_length;

    int track_num = 0, frames = 0, pregap_frames = 0, postgap_frames = 0;
    chd_error err = chd_get_metadata(m_chd, CDROM_TRACK_METADATA2_TAG, num_tracks, metadata_str, sizeof(metadata_str),
                                     &metadata_length, nullptr, nullptr);
    if (err == CHDERR_NONE)
    {
      if (std::sscanf(metadata_str, CDROM_TRACK_METADATA2_FORMAT, &track_num, type_str, subtype_str, &frames,
                      &pregap_frames, pgtype_str, pgsub_str, &postgap_frames) != 8)
      {
        Log_ErrorFmt("Invalid track v2 metadata: '{}'", metadata_str);
        Error::SetString(error, fmt::format("Invalid track v2 metadata: '{}'", metadata_str));
        return false;
      }
    }
    else
    {
      // try old version
      err = chd_get_metadata(m_chd, CDROM_TRACK_METADATA_TAG, num_tracks, metadata_str, sizeof(metadata_str),
                             &metadata_length, nullptr, nullptr);
      if (err != CHDERR_NONE)
      {
        // not found, so no more tracks
        break;
      }

      if (std::sscanf(metadata_str, CDROM_TRACK_METADATA_FORMAT, &track_num, type_str, subtype_str, &frames) != 4)
      {
        Log_ErrorFmt("Invalid track metadata: '{}'", metadata_str);
        Error::SetString(error, fmt::format("Invalid track v2 metadata: '{}'", metadata_str));
        return false;
      }
    }

    u32 csubtype, csubsize;
    if (!cdrom_parse_subtype_string(subtype_str, &csubtype, &csubsize))
    {
      csubtype = CD_SUB_NONE;
      csubsize = 0;
    }

    if (track_num != (num_tracks + 1))
    {
      Log_ErrorFmt("Incorrect track number at index {}, expected {} got {}", num_tracks, (num_tracks + 1), track_num);
      Error::SetString(error, fmt::format("Incorrect track number at index {}, expected {} got {}", num_tracks,
                                          (num_tracks + 1), track_num));
      return false;
    }

    std::optional<TrackMode> mode = ParseTrackModeString(type_str);
    if (!mode.has_value())
    {
      Log_ErrorFmt("Invalid track mode: '{}'", type_str);
      Error::SetString(error, fmt::format("Invalid track mode: '{}'", type_str));
      return false;
    }

    // precompute subchannel q flags for the whole track
    SubChannelQ::Control control{};
    control.data = mode.value() != TrackMode::Audio;

    // two seconds pregap for track 1 is assumed if not specified
    const bool pregap_in_file = (pregap_frames > 0 && pgtype_str[0] == 'V');
    if (pregap_frames <= 0 && mode != TrackMode::Audio)
      pregap_frames = 2 * FRAMES_PER_SECOND;

    // create the index for the pregap
    if (pregap_frames > 0)
    {
      Index pregap_index = {};
      pregap_index.start_lba_on_disc = disc_lba;
      pregap_index.start_lba_in_track = static_cast<LBA>(static_cast<unsigned long>(-pregap_frames));
      pregap_index.length = pregap_frames;
      pregap_index.track_number = track_num;
      pregap_index.index_number = 0;
      pregap_index.mode = mode.value();
      pregap_index.submode = static_cast<SubchannelMode>(csubtype);
      pregap_index.control.bits = control.bits;
      pregap_index.is_pregap = true;

      if (pregap_in_file)
      {
        if (pregap_frames > frames)
        {
          Log_ErrorFmt("Pregap length {} exceeds track length {}", pregap_frames, frames);
          Error::SetString(error, fmt::format("Pregap length {} exceeds track length {}", pregap_frames, frames));
          return false;
        }

        pregap_index.file_index = 0;
        pregap_index.file_offset = file_lba;
        pregap_index.file_sector_size = CHD_CD_SECTOR_DATA_SIZE;
        file_lba += pregap_frames;
        frames -= pregap_frames;
      }

      m_indices.push_back(pregap_index);
      disc_lba += pregap_frames;
    }

    // add the track itself
    m_tracks.push_back(Track{static_cast<u32>(track_num), disc_lba, static_cast<u32>(m_indices.size()),
                             static_cast<u32>(frames + pregap_frames), mode.value(),
                             static_cast<SubchannelMode>(csubtype), control});

    // how many indices in this track?
    Index index = {};
    index.start_lba_on_disc = disc_lba;
    index.start_lba_in_track = 0;
    index.track_number = track_num;
    index.index_number = 1;
    index.file_index = 0;
    index.file_sector_size = CHD_CD_SECTOR_DATA_SIZE;
    index.file_offset = file_lba;
    index.mode = mode.value();
    index.submode = static_cast<SubchannelMode>(csubtype);
    index.control.bits = control.bits;
    index.is_pregap = false;
    index.length = static_cast<u32>(frames);
    m_indices.push_back(index);

    disc_lba += index.length;
    file_lba += index.length;
    num_tracks++;

    // each track is padded to a multiple of 4 frames, see chdman source.
    file_lba = Common::AlignUp(file_lba, CHD_CD_TRACK_ALIGNMENT);
  }

  if (m_tracks.empty())
  {
    Log_ErrorFmt("File '{}' contains no tracks", filename);
    Error::SetString(error, fmt::format("File '{}' contains no tracks", filename));
    return false;
  }

  m_lba_count = disc_lba;
  AddLeadOutIndex();

  m_sbi.LoadFromImagePath(filename);

  return Seek(1, Position{0, 0, 0});
}

bool CDImageCHD::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  if (m_sbi.GetReplacementSubChannelQ(index.start_lba_on_disc + lba_in_index, subq))
    return true;

  if (index.submode == CDImage::SubchannelMode::None)
    return CDImage::ReadSubChannelQ(subq, index, lba_in_index);

  u32 hunk_offset;
  if (!UpdateHunkBuffer(index, lba_in_index, hunk_offset))
    return false;

  u8 deinterleaved_subchannel_data[96];
  const u8* raw_subchannel_data = &m_hunk_buffer[hunk_offset + RAW_SECTOR_SIZE];
  const u8* real_subchannel_data = raw_subchannel_data;
  if (index.submode == CDImage::SubchannelMode::RawInterleaved)
  {
    DeinterleaveSubcode(raw_subchannel_data, deinterleaved_subchannel_data);
    real_subchannel_data = deinterleaved_subchannel_data;
  }

  // P, Q, R, S, T, U, V, W
  std::memcpy(subq->data.data(), real_subchannel_data + (1 * SUBCHANNEL_BYTES_PER_FRAME), SUBCHANNEL_BYTES_PER_FRAME);
  return true;
}

bool CDImageCHD::HasNonStandardSubchannel() const
{
  // Just look at the first track for in-CHD subq.
  return (m_sbi.GetReplacementSectorCount() > 0 || m_tracks.front().submode != CDImage::SubchannelMode::None);
}

CDImage::PrecacheResult CDImageCHD::Precache(ProgressCallback* progress)
{
  if (m_precached)
    return CDImage::PrecacheResult::Success;

  progress->SetStatusText(fmt::format("Precaching {}...", FileSystem::GetDisplayNameFromPath(m_filename)).c_str());
  progress->SetProgressRange(100);

  auto callback = [](size_t pos, size_t total, void* param) {
    const u32 percent = static_cast<u32>((pos * 100) / total);
    static_cast<ProgressCallback*>(param)->SetProgressValue(std::min<u32>(percent, 100));
  };

  if (chd_precache_progress(m_chd, callback, progress) != CHDERR_NONE)
    return CDImage::PrecacheResult::ReadError;

  m_precached = true;
  return CDImage::PrecacheResult::Success;
}

bool CDImageCHD::IsPrecached() const
{
  return m_precached;
}

ALWAYS_INLINE_RELEASE void CDImageCHD::CopyAndSwap(void* dst_ptr, const u8* src_ptr)
{
  constexpr u32 data_size = RAW_SECTOR_SIZE;

  u8* dst_ptr_byte = static_cast<u8*>(dst_ptr);
#if defined(CPU_ARCH_SSE) || defined(CPU_ARCH_NEON)
  static_assert((data_size % 16) == 0);
  constexpr u32 num_values = data_size / 16;

#if defined(CPU_ARCH_SSE)
  // Requires SSSE3.
  // const __m128i mask = _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
  for (u32 i = 0; i < num_values; i++)
  {
    __m128i value = _mm_load_si128(reinterpret_cast<const __m128i*>(src_ptr));
    // value = _mm_shuffle_epi8(value, mask);
    value = _mm_or_si128(_mm_slli_epi16(value, 8), _mm_srli_epi16(value, 8));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_ptr_byte), value);
    src_ptr += sizeof(value);
    dst_ptr_byte += sizeof(value);
  }
#elif defined(CPU_ARCH_NEON)
  for (u32 i = 0; i < num_values; i++)
  {
    uint16x8_t value = vld1q_u16(reinterpret_cast<const u16*>(src_ptr));
    value = vorrq_u16(vshlq_n_u16(value, 8), vshrq_n_u16(value, 8));
    vst1q_u16(reinterpret_cast<u16*>(dst_ptr_byte), value);
    src_ptr += sizeof(value);
    dst_ptr_byte += sizeof(value);
  }
#endif
#elif defined(CPU_ARCH_RISCV64)
  constexpr u32 num_values = data_size / 8;
  for (u32 i = 0; i < num_values; i++)
  {
    u64 value;
    std::memcpy(&value, src_ptr, sizeof(value));
    value = ((value >> 8) & UINT64_C(0x00FF00FF00FF00FF)) | ((value << 8) & UINT64_C(0xFF00FF00FF00FF00));
    std::memcpy(dst_ptr_byte, &value, sizeof(value));
    src_ptr += sizeof(value);
    dst_ptr_byte += sizeof(value);
  }
#else
  constexpr u32 num_values = data_size / 4;
  for (u32 i = 0; i < num_values; i++)
  {
    u32 value;
    std::memcpy(&value, src_ptr, sizeof(value));
    value = ((value >> 8) & UINT32_C(0x00FF00FF)) | ((value << 8) & UINT32_C(0xFF00FF00));
    std::memcpy(dst_ptr_byte, &value, sizeof(value));
    src_ptr += sizeof(value);
    dst_ptr_byte += sizeof(value);
  }
#endif
}

bool CDImageCHD::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  u32 hunk_offset;
  if (!UpdateHunkBuffer(index, lba_in_index, hunk_offset))
    return false;

  // Audio data is in big-endian, so we have to swap it for little endian hosts...
  if (index.mode == TrackMode::Audio)
    CopyAndSwap(buffer, &m_hunk_buffer[hunk_offset]);
  else
    std::memcpy(buffer, &m_hunk_buffer[hunk_offset], RAW_SECTOR_SIZE);

  return true;
}

ALWAYS_INLINE_RELEASE bool CDImageCHD::UpdateHunkBuffer(const Index& index, LBA lba_in_index, u32& hunk_offset)
{
  const u32 disc_frame = static_cast<LBA>(index.file_offset) + lba_in_index;
  const u32 hunk_index = static_cast<u32>(disc_frame / m_sectors_per_hunk);
  hunk_offset = static_cast<u32>((disc_frame % m_sectors_per_hunk) * CHD_CD_SECTOR_DATA_SIZE);
  DebugAssert((m_hunk_size - hunk_offset) >= CHD_CD_SECTOR_DATA_SIZE);

  if (m_current_hunk_index == hunk_index)
    return true;

  const chd_error err = chd_read(m_chd, hunk_index, m_hunk_buffer.data());
  if (err != CHDERR_NONE)
  {
    Log_ErrorFmt("chd_read({}) failed: %s", hunk_index, chd_error_string(err));

    // data might have been partially written
    m_current_hunk_index = static_cast<u32>(-1);
    return false;
  }

  m_current_hunk_index = hunk_index;
  return true;
}

s64 CDImageCHD::GetSizeOnDisk() const
{
  return static_cast<s64>(chd_get_compressed_size(m_chd));
}

std::unique_ptr<CDImage> CDImage::OpenCHDImage(const char* filename, Error* error)
{
  std::unique_ptr<CDImageCHD> image = std::make_unique<CDImageCHD>();
  if (!image->Open(filename, error))
    return {};

  return image;
}
