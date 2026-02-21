// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cd_image.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/heterogeneous_containers.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>

LOG_CHANNEL(CDImage);

namespace {

class CDImageCCD : public CDImage
{
public:
  CDImageCCD();
  ~CDImageCCD() override;

  bool OpenAndParse(const char* filename, Error* error);

  s64 GetSizeOnDisk() const override;

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasSubchannelData() const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  static constexpr SubchannelMode SUBCHANNEL_MODE = SubchannelMode::Raw;
  static constexpr u32 IMG_SECTOR_SIZE = RAW_SECTOR_SIZE;

  std::FILE* m_img_file = nullptr;
  std::FILE* m_sub_file = nullptr;
  s64 m_img_file_position = 0;
};

} // namespace

CDImageCCD::CDImageCCD() = default;

CDImageCCD::~CDImageCCD()
{
  if (m_sub_file)
    std::fclose(m_sub_file);
  if (m_img_file)
    std::fclose(m_img_file);
}

bool CDImageCCD::OpenAndParse(const char* filename, Error* error)
{
  // Read the CCD file as text.
  std::optional<std::string> ccd_data = FileSystem::ReadFileToString(filename, error);
  if (!ccd_data.has_value())
  {
    Error::AddPrefixFmt(error, "Failed to open ccd '{}': ", Path::GetFileName(filename));
    return false;
  }

  // Open the IMG file (raw 2352-byte sector data).
  std::string img_filename(Path::ReplaceExtension(filename, "img"));
  m_img_file = FileSystem::OpenSharedCFile(img_filename.c_str(), "rb", FileSystem::FileShareMode::DenyWrite, error);
  if (!m_img_file)
  {
    Error::AddPrefixFmt(error, "Failed to open img file '{}': ", Path::GetFileName(img_filename));
    return false;
  }

  // Open the SUB file (96-byte raw interleaved subchannel data per sector).
  std::string sub_filename(Path::ReplaceExtension(filename, "sub"));
  m_sub_file = FileSystem::OpenSharedCFile(sub_filename.c_str(), "rb", FileSystem::FileShareMode::DenyWrite, error);
  if (!m_sub_file)
  {
    Error::AddPrefixFmt(error, "Failed to open sub file '{}': ", Path::GetFileName(sub_filename));
    return false;
  }

  // Parse CCD INI-style file into sections.
  StringMap<StringMap<std::string>> sections;
  std::string current_section;

  const std::vector<std::string_view> lines = StringUtil::SplitString(ccd_data.value(), '\n', true);
  for (const std::string_view& line : lines)
  {
    const std::string_view stripped = StringUtil::StripWhitespace(line);
    if (stripped.empty() || stripped[0] == ';')
      continue;

    if (stripped.front() == '[' && stripped.back() == ']')
    {
      current_section = std::string(stripped.substr(1, stripped.size() - 2));
      continue;
    }

    std::string_view key, value;
    if (StringUtil::ParseAssignmentString(stripped, &key, &value))
      sections[current_section][std::string(key)] = std::string(value);
  }

  // Verify CloneCD header.
  if (sections.find("CloneCD") == sections.end())
  {
    ERROR_LOG("Missing [CloneCD] header in '{}'", Path::GetFileName(filename));
    Error::SetStringFmt(error, "Missing [CloneCD] header in '{}'", Path::GetFileName(filename));
    return false;
  }

  // Get disc info.
  if (sections.find("Disc") == sections.end())
  {
    ERROR_LOG("Missing [Disc] section in '{}'", Path::GetFileName(filename));
    Error::SetStringFmt(error, "Missing [Disc] section in '{}'", Path::GetFileName(filename));
    return false;
  }

  // Helper to read integer values from sections, supporting both decimal and hex (0x) prefixes.
  const auto get_int_value = [&sections](std::string_view section, std::string_view key) -> std::optional<s32> {
    auto section_it = sections.find(section);
    if (section_it == sections.end())
      return std::nullopt;
    auto key_it = section_it->second.find(key);
    if (key_it == section_it->second.end())
      return std::nullopt;
    return StringUtil::FromCharsWithOptionalBase<s32>(key_it->second);
  };

  const std::optional<s32> toc_entries = get_int_value("Disc", "TocEntries");
  if (!toc_entries.has_value() || toc_entries.value() < 3)
  {
    ERROR_LOG("Invalid or missing TocEntries in '{}'", Path::GetFileName(filename));
    Error::SetStringFmt(error, "Invalid or missing TocEntries in '{}'", Path::GetFileName(filename));
    return false;
  }

  // Parse TOC entries to get track control flags and lead-out LBA.
  LBA leadout_lba = 0;

  struct TocEntry
  {
    u8 point;
    u8 adr;
    u8 control;
    s32 plba;
  };
  std::map<u32, TocEntry> track_toc_entries; // keyed by track number

  for (s32 i = 0; i < toc_entries.value(); i++)
  {
    const std::string section_name = "Entry " + std::to_string(i);
    const std::optional<s32> point = get_int_value(section_name, "Point");
    if (!point.has_value())
      continue;

    const std::optional<s32> adr = get_int_value(section_name, "ADR");
    const std::optional<s32> control = get_int_value(section_name, "Control");
    const std::optional<s32> plba = get_int_value(section_name, "PLBA");

    const u8 point_val = static_cast<u8>(point.value());
    if (point_val == 0xA2 && plba.has_value())
    {
      // Lead-out position.
      leadout_lba = static_cast<LBA>(plba.value());
    }
    else if (point_val >= 1 && point_val <= 99)
    {
      // Track entry.
      TocEntry entry;
      entry.point = point_val;
      entry.adr = adr.has_value() ? static_cast<u8>(adr.value()) : 0x01;
      entry.control = control.has_value() ? static_cast<u8>(control.value()) : 0x00;
      entry.plba = plba.has_value() ? plba.value() : 0;
      track_toc_entries[point_val] = entry;
    }
  }

  if (track_toc_entries.empty())
  {
    ERROR_LOG("File '{}' contains no track entries", Path::GetFileName(filename));
    Error::SetStringFmt(error, "File '{}' contains no track entries", Path::GetFileName(filename));
    return false;
  }

  // If lead-out was not found, derive from IMG file size.
  if (leadout_lba == 0)
  {
    const s64 img_size = FileSystem::FSize64(m_img_file);
    if (img_size > 0)
    {
      leadout_lba = static_cast<LBA>(img_size / RAW_SECTOR_SIZE);
    }
    else
    {
      ERROR_LOG("Could not determine lead-out position in '{}'", Path::GetFileName(filename));
      Error::SetStringFmt(error, "Could not determine lead-out position in '{}'", Path::GetFileName(filename));
      return false;
    }
  }

  // Build per-track info using [TRACK N] sections if available, falling back to [Entry N] data.
  struct TrackInfo
  {
    u32 track_number;
    TrackMode mode;
    s32 index0; // -1 if not present
    s32 index1;
    u8 control;
  };
  std::vector<TrackInfo> parsed_tracks;

  for (const auto& [track_num, toc_entry] : track_toc_entries)
  {
    const std::string track_section = "TRACK " + std::to_string(track_num);

    TrackInfo info;
    info.track_number = track_num;
    info.control = toc_entry.control;

    // Determine track mode from [TRACK N] section or from the Control field.
    const std::optional<s32> mode_val = get_int_value(track_section, "MODE");
    if (mode_val.has_value())
    {
      switch (mode_val.value())
      {
        case 0:
          info.mode = TrackMode::Audio;
          break;
        case 1:
          info.mode = TrackMode::Mode1Raw;
          break;
        case 2:
        default:
          info.mode = TrackMode::Mode2Raw;
          break;
      }
    }
    else
    {
      info.mode = (toc_entry.control & 0x04) ? TrackMode::Mode2Raw : TrackMode::Audio;
    }

    // Read index positions from [TRACK N] section, or fall back to PLBA.
    const std::optional<s32> idx0 = get_int_value(track_section, "INDEX 0");
    const std::optional<s32> idx1 = get_int_value(track_section, "INDEX 1");

    info.index0 = idx0.has_value() ? idx0.value() : -1;
    info.index1 = idx1.has_value() ? idx1.value() : toc_entry.plba;

    parsed_tracks.push_back(info);
  }

  // Sort by track number.
  std::sort(parsed_tracks.begin(), parsed_tracks.end(),
            [](const TrackInfo& a, const TrackInfo& b) { return a.track_number < b.track_number; });

  // Should have track 1, and no missing tracks.
  if (parsed_tracks.empty() || parsed_tracks[0].track_number != 1)
  {
    ERROR_LOG("File '{}' must contain a track 1", Path::GetFileName(filename));
    Error::SetStringFmt(error, "File '{}' must contain a track 1", Path::GetFileName(filename));
    return false;
  }
  for (size_t i = 1; i < parsed_tracks.size(); i++)
  {
    if (parsed_tracks[i].track_number != parsed_tracks[i - 1].track_number + 1)
    {
      ERROR_LOG("File '{}' has missing track number {}", Path::GetFileName(filename),
                parsed_tracks[i - 1].track_number + 1);
      Error::SetStringFmt(error, "File '{}' has missing track number {}", Path::GetFileName(filename),
                          parsed_tracks[i - 1].track_number + 1);
      return false;
    }
  }

  // Track 1 pregap offset.
  const LBA plba_offset = (parsed_tracks[0].index0 >= 0) ? 0 : 150;

  // Build CDImage tracks and indices.
  for (size_t i = 0; i < parsed_tracks.size(); i++)
  {
    const TrackInfo& ti = parsed_tracks[i];
    const LBA track_index0 = static_cast<LBA>((ti.index0 >= 0) ? ti.index0 : ti.index1);
    const LBA track_index1 = static_cast<LBA>(ti.index1);

    // Determine where the next track (or lead-out) begins.
    LBA next_track_index0, next_track_index1;
    if (i + 1 < parsed_tracks.size())
    {
      const TrackInfo& next = parsed_tracks[i + 1];
      next_track_index0 = static_cast<LBA>((next.index0 >= 0) ? next.index0 : next.index1);
      next_track_index1 = static_cast<LBA>(next.index1);
    }
    else
    {
      next_track_index0 = leadout_lba;
      next_track_index1 = leadout_lba;
    }

    if (track_index1 < track_index0 || next_track_index0 <= track_index1 || next_track_index0 > next_track_index1)
    {
      ERROR_LOG("Track {} has invalid length (start {}/{}, next {}/{})", ti.track_number, track_index0, track_index1,
                next_track_index0, next_track_index1);
      Error::SetStringFmt(error, "Track {} has invalid length", ti.track_number);
      return false;
    }

    SubChannelQ::Control control{};
    control.data = ti.mode != TrackMode::Audio;

    // Track length is the distance from this track's start to the next track's start (or lead-out).
    // I'm not sure if this is correct, it's needed for Rayman (Japan) where track 2 is the only track with a pregap.
    u32 toc_track_length = next_track_index0 - track_index0;

    // Handle pregap (index 0).
    if (track_index1 > track_index0)
    {
      // Pregap is present in the IMG file.
      const LBA pregap_length = track_index1 - track_index0;

      Index pregap_index = {};
      pregap_index.start_lba_on_disc = static_cast<LBA>(ti.index0) + plba_offset;
      pregap_index.start_lba_in_track = static_cast<LBA>(-static_cast<s32>(pregap_length));
      pregap_index.length = pregap_length;
      pregap_index.track_number = ti.track_number;
      pregap_index.index_number = 0;
      pregap_index.file_index = 0;
      pregap_index.file_sector_size = IMG_SECTOR_SIZE;
      pregap_index.file_offset = static_cast<u64>(ti.index0) * IMG_SECTOR_SIZE;
      pregap_index.mode = ti.mode;
      pregap_index.submode = SUBCHANNEL_MODE;
      pregap_index.control.bits = control.bits;
      pregap_index.is_pregap = true;
      m_indices.push_back(pregap_index);
    }
    else if (ti.track_number == 1 && plba_offset > 0)
    {
      Index pregap_index = {};
      pregap_index.start_lba_on_disc = 0;
      pregap_index.start_lba_in_track = static_cast<LBA>(-static_cast<s32>(plba_offset));
      pregap_index.length = plba_offset;
      pregap_index.track_number = ti.track_number;
      pregap_index.index_number = 0;
      pregap_index.mode = ti.mode;
      pregap_index.submode = CDImage::SubchannelMode::None;
      pregap_index.control.bits = control.bits;
      pregap_index.is_pregap = true;
      m_indices.push_back(pregap_index);
      toc_track_length += plba_offset;
    }

    // Add the track.
    m_tracks.push_back(Track{ti.track_number, track_index1 + plba_offset, static_cast<u32>(m_indices.size()),
                             toc_track_length, ti.mode, SubchannelMode::None, control});

    // Add data index (index 1).
    Index data_index = {};
    data_index.start_lba_on_disc = track_index1 + plba_offset;
    data_index.start_lba_in_track = 0;
    data_index.track_number = ti.track_number;
    data_index.index_number = 1;
    data_index.file_index = 0;
    data_index.file_sector_size = IMG_SECTOR_SIZE;
    data_index.file_offset = static_cast<u64>(track_index1) * IMG_SECTOR_SIZE;
    data_index.mode = ti.mode;
    data_index.submode = SUBCHANNEL_MODE;
    data_index.control.bits = control.bits;
    data_index.is_pregap = false;
    data_index.length = next_track_index0 - track_index1;
    m_indices.push_back(data_index);
  }

  if (m_tracks.empty())
  {
    ERROR_LOG("File '{}' contains no tracks", Path::GetFileName(filename));
    Error::SetStringFmt(error, "File '{}' contains no tracks", Path::GetFileName(filename));
    return false;
  }

  m_lba_count = m_tracks.back().start_lba + m_tracks.back().length;
  AddLeadOutIndex();

  m_filename = filename;

  return Seek(1, Position{0, 0, 0});
}

bool CDImageCCD::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  const s64 file_position = static_cast<s64>(index.file_offset + (static_cast<u64>(lba_in_index) * IMG_SECTOR_SIZE));
  if (m_img_file_position != file_position)
  {
    if (FileSystem::FSeek64(m_img_file, file_position, SEEK_SET) != 0)
      return false;

    m_img_file_position = file_position;
  }

  if (std::fread(buffer, IMG_SECTOR_SIZE, 1, m_img_file) != 1)
  {
    FileSystem::FSeek64(m_img_file, m_img_file_position, SEEK_SET);
    return false;
  }

  m_img_file_position += IMG_SECTOR_SIZE;
  return true;
}

bool CDImageCCD::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  // For virtual pregaps (not in file), fall back to generated subchannel Q.
  if (index.is_pregap && index.file_sector_size == 0)
    return CDImage::ReadSubChannelQ(subq, index, lba_in_index);

  // Q subchannel is the second 12-byte block (P, Q, R, S, T, U, V, W).
  static constexpr u64 q_offset = SUBCHANNEL_BYTES_PER_FRAME;

  // Have to wrangle this because of the two second implicit pregap.
  const s64 sub_offset = static_cast<s64>(((index.file_offset / IMG_SECTOR_SIZE) * ALL_SUBCODE_SIZE) +
                                          (static_cast<u64>(lba_in_index) * ALL_SUBCODE_SIZE) + q_offset);

  // Since we're only reading partially, the position's never going to match for sequential. Always seek.
  if (FileSystem::FSeek64(m_sub_file, static_cast<s64>(sub_offset), SEEK_SET) != 0 ||
      std::fread(subq->data.data(), SUBCHANNEL_BYTES_PER_FRAME, 1, m_sub_file) != 1)
  {
    WARNING_LOG("Failed to read subq for sector {}", index.start_lba_on_disc + lba_in_index);
    return CDImage::ReadSubChannelQ(subq, index, lba_in_index);
  }

  return true;
}

bool CDImageCCD::HasSubchannelData() const
{
  return true;
}

s64 CDImageCCD::GetSizeOnDisk() const
{
  s64 size = std::max<s64>(FileSystem::FSize64(m_img_file), 0);
  if (m_sub_file)
    size += std::max<s64>(FileSystem::FSize64(m_sub_file), 0);

  return size;
}

std::unique_ptr<CDImage> CDImage::OpenCCDImage(const char* path, Error* error)
{
  std::unique_ptr<CDImageCCD> image = std::make_unique<CDImageCCD>();
  if (!image->OpenAndParse(path, error))
    return {};

  return image;
}
