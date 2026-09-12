// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "util/cd_image.h"

#include "common/bcdutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/path.h"
#include "common/types.h"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

// TODO: Extract this out, use FileSystem temporary file
class TempFile
{
public:
  TempFile(const char* name, const char* extension)
    : m_path(::Path::Combine(FileSystem::GetWorkingDirectory(), fmt::format("{}_{}.{}", name, s_counter++, extension)))
  {
  }

  explicit TempFile(std::string path) : m_path(std::move(path)) {}

  ~TempFile()
  {
    if (!m_path.empty())
      FileSystem::DeleteFile(m_path.c_str());
  }

  const std::string& GetPath() const { return m_path; }

  bool Write(std::span<const u8> data)
  {
    std::FILE* fp = FileSystem::OpenCFile(m_path.c_str(), "wb");
    if (!fp)
      return false;

    const bool result = (data.empty() || std::fwrite(data.data(), data.size(), 1, fp) == 1);
    std::fclose(fp);
    return result;
  }

  bool WriteString(std::string_view data)
  {
    return Write(std::span<const u8>(reinterpret_cast<const u8*>(data.data()), data.size()));
  }

private:
  static inline u32 s_counter = 0;

  std::string m_path;
};

std::array<u8, CDImage::RAW_SECTOR_SIZE> MakePatternSector()
{
  std::array<u8, CDImage::RAW_SECTOR_SIZE> sector = {};
  for (u32 i = 0; i < sector.size(); i++)
    sector[i] = static_cast<u8>(i * 3 + 7);

  return sector;
}

void ExpectSyncAndHeader(const std::array<u8, CDImage::RAW_SECTOR_SIZE>& sector, u32 mode, u32 lba = 0)
{
  const CDImage::Position position = CDImage::Position::FromLBA(lba);

  EXPECT_EQ(std::memcmp(sector.data(), CDImage::SECTOR_SYNC_DATA.data(), CDImage::SECTOR_SYNC_DATA.size()), 0);
  EXPECT_EQ(sector[12], BinaryToBCD(position.minute));
  EXPECT_EQ(sector[13], BinaryToBCD(position.second));
  EXPECT_EQ(sector[14], BinaryToBCD(position.frame));
  EXPECT_EQ(sector[15], mode);
}

class SplitIndexCDImage final : public CDImage
{
public:
  struct ReadCall
  {
    LBA lba;
    u32 count;
    SectorReadMode mode;
  };

  SplitIndexCDImage()
  {
    Track track = {};
    track.track_number = 1;
    track.first_index = 0;
    track.length = 5;
    track.mode = TrackMode::Audio;
    track.control = SubChannelQ::Control(0);
    m_tracks.push_back(track);

    Index first = {};
    first.file_sector_size = RAW_SECTOR_SIZE;
    first.track_number = 1;
    first.index_number = 1;
    first.length = 2;
    first.mode = TrackMode::Audio;
    first.control = track.control;
    m_indices.push_back(first);

    Index second = first;
    second.start_lba_on_disc = 2;
    second.start_lba_in_track = 2;
    second.index_number = 2;
    second.length = 3;
    m_indices.push_back(second);

    m_lba_count = track.length;
    AddLeadOutIndex();
  }

  u32 ReadSectorsFromIndex(std::span<Sector> sectors, const Index& index, LBA lba_in_index,
                           SectorReadMode mode) override
  {
    EXPECT_NE(mode, SectorReadMode::SubQOnly);
    const LBA first_lba = index.start_lba_on_disc + lba_in_index;
    m_read_calls.push_back({first_lba, static_cast<u32>(sectors.size()), mode});
    for (u32 i = 0; i < sectors.size(); i++)
      sectors[i].data.fill(static_cast<u8>(first_lba + i));
    return static_cast<u32>(sectors.size());
  }

  std::vector<ReadCall> m_read_calls;
};

class PatchParentCDImage final : public CDImage
{
public:
  struct ReadCall
  {
    LBA lba;
    u32 count;
  };

  PatchParentCDImage()
  {
    Track track = {};
    track.track_number = 1;
    track.first_index = 0;
    track.length = 4;
    track.mode = TrackMode::Audio;
    track.control = SubChannelQ::Control(0);
    m_tracks.push_back(track);

    Index index = {};
    index.file_sector_size = RAW_SECTOR_SIZE;
    index.track_number = 1;
    index.index_number = 1;
    index.length = track.length;
    index.mode = track.mode;
    index.control = track.control;
    m_indices.push_back(index);

    m_lba_count = track.length;
    AddLeadOutIndex();
  }

  u32 ReadSectorsFromIndex(std::span<Sector> sectors, const Index& index, LBA lba_in_index,
                           SectorReadMode mode) override
  {
    const LBA first_lba = index.start_lba_on_disc + lba_in_index;
    m_read_calls.push_back({first_lba, static_cast<u32>(sectors.size())});
    if (m_fail_reads)
      return 0;

    EXPECT_NE(mode, SectorReadMode::SubQOnly);
    for (u32 i = 0; i < sectors.size(); i++)
      sectors[i].data.fill(static_cast<u8>(first_lba + i));
    return static_cast<u32>(sectors.size());
  }

  std::vector<ReadCall> m_read_calls;
  bool m_fail_reads = false;
};

} // namespace

TEST(CDImage, ConvertMode1ToRaw)
{
  std::array<u8, CDImage::RAW_SECTOR_SIZE> sector = MakePatternSector();
  const std::array<u8, CDImage::DATA_SECTOR_SIZE> payload = [&sector] {
    std::array<u8, CDImage::DATA_SECTOR_SIZE> ret;
    std::memcpy(ret.data(), sector.data(), ret.size());
    return ret;
  }();

  CDImage::ConvertSectorToRaw(sector.data(), 0, CDImage::TrackMode::Mode1);

  ExpectSyncAndHeader(sector, 0x01);
  EXPECT_EQ(std::memcmp(&sector[16], payload.data(), payload.size()), 0);
  EXPECT_EQ(std::memcmp(&sector[2068], std::array<u8, 8>{}.data(), 8), 0);
}

TEST(CDImage, ConvertMode2ToRaw)
{
  std::array<u8, CDImage::RAW_SECTOR_SIZE> sector = MakePatternSector();
  const std::array<u8, CDImage::MODE2_DATA_SECTOR_SIZE> payload = [&sector] {
    std::array<u8, CDImage::MODE2_DATA_SECTOR_SIZE> ret;
    std::memcpy(ret.data(), sector.data(), ret.size());
    return ret;
  }();

  CDImage::ConvertSectorToRaw(sector.data(), 0, CDImage::TrackMode::Mode2);

  ExpectSyncAndHeader(sector, 0x02);
  EXPECT_EQ(std::memcmp(&sector[16], payload.data(), payload.size()), 0);
}

TEST(CDImage, ConvertMode2FormsToRaw)
{
  std::array<u8, CDImage::RAW_SECTOR_SIZE> form1 = MakePatternSector();
  std::array<u8, CDImage::DATA_SECTOR_SIZE> form1_payload;
  std::memcpy(form1_payload.data(), form1.data(), form1_payload.size());

  CDImage::ConvertSectorToRaw(form1.data(), 0, CDImage::TrackMode::Mode2Form1);

  ExpectSyncAndHeader(form1, 0x02);
  EXPECT_EQ(form1[18], 0x08);
  EXPECT_EQ(form1[22], 0x08);
  EXPECT_EQ(std::memcmp(&form1[24], form1_payload.data(), form1_payload.size()), 0);

  std::array<u8, CDImage::RAW_SECTOR_SIZE> form2 = MakePatternSector();
  std::array<u8, 2324> form2_payload;
  std::memcpy(form2_payload.data(), form2.data(), form2_payload.size());

  CDImage::ConvertSectorToRaw(form2.data(), 0, CDImage::TrackMode::Mode2Form2);

  ExpectSyncAndHeader(form2, 0x02);
  EXPECT_EQ(form2[18], 0x28);
  EXPECT_EQ(form2[22], 0x28);
  EXPECT_EQ(std::memcmp(&form2[24], form2_payload.data(), form2_payload.size()), 0);
}

TEST(CDImage, ConvertMode2FormMixToRaw)
{
  std::array<u8, CDImage::RAW_SECTOR_SIZE> sector = MakePatternSector();
  sector[2] = 0x20;
  sector[6] = 0x20;

  std::array<u8, 2332> payload;
  std::memcpy(payload.data(), sector.data(), payload.size());

  CDImage::ConvertSectorToRaw(sector.data(), 0, CDImage::TrackMode::Mode2FormMix);

  ExpectSyncAndHeader(sector, 0x02);
  EXPECT_EQ(std::memcmp(&sector[16], payload.data(), payload.size()), 0);
}

TEST(CDImage, CueMode1_2048ReadsAsRaw)
{
  TempFile bin("duckstation_cd_image_mode1", "bin");
  std::array<u8, CDImage::DATA_SECTOR_SIZE> payload = {};
  for (u32 i = 0; i < payload.size(); i++)
    payload[i] = static_cast<u8>(i);
  ASSERT_TRUE(bin.Write(payload));

  TempFile cue("duckstation_cd_image_mode1", "cue");
  const std::string cue_data =
    fmt::format("FILE \"{}\" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n", Path::GetFileName(bin.GetPath()));
  ASSERT_TRUE(cue.WriteString(cue_data));

  Error error;
  std::unique_ptr<CDImage> image = CDImage::Open(cue.GetPath().c_str(), false, &error);
  ASSERT_TRUE(image) << error.GetDescription();

  CDImage::Sector sector;
  ASSERT_EQ(image->ReadSectors(2 * CDImage::FRAMES_PER_SECOND, std::span<CDImage::Sector>(&sector, 1),
                               CDImage::SectorReadMode::DataAndSubQ),
            1u);
  ExpectSyncAndHeader(sector.data, 0x01, 2 * CDImage::FRAMES_PER_SECOND);
  EXPECT_EQ(std::memcmp(&sector.data[16], payload.data(), payload.size()), 0);
}

TEST(CDImage, BatchReadUsesExplicitLBA)
{
  TempFile bin("duckstation_cd_image_batch", "bin");
  std::array<u8, CDImage::DATA_SECTOR_SIZE * 3> payload = {};
  for (u32 sector = 0; sector < 3; sector++)
  {
    std::fill_n(payload.data() + (sector * CDImage::DATA_SECTOR_SIZE), CDImage::DATA_SECTOR_SIZE,
                static_cast<u8>(sector + 1));
  }
  ASSERT_TRUE(bin.Write(payload));

  TempFile cue("duckstation_cd_image_batch", "cue");
  const std::string cue_data =
    fmt::format("FILE \"{}\" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n", Path::GetFileName(bin.GetPath()));
  ASSERT_TRUE(cue.WriteString(cue_data));

  Error error;
  std::unique_ptr<CDImage> image = CDImage::Open(cue.GetPath().c_str(), false, &error);
  ASSERT_TRUE(image) << error.GetDescription();

  std::array<CDImage::Sector, 2> sectors;
  ASSERT_EQ(image->ReadSectors(2 * CDImage::FRAMES_PER_SECOND + 1, sectors, CDImage::SectorReadMode::DataAndSubQ),
            sectors.size());
  for (u32 i = 0; i < sectors.size(); i++)
  {
    ExpectSyncAndHeader(sectors[i].data, 0x01, 2 * CDImage::FRAMES_PER_SECOND + i + 1);
    EXPECT_EQ(sectors[i].data[16], i + 2);
    EXPECT_TRUE(sectors[i].subq.IsCRCValid());
  }

  std::array<CDImage::Sector, 4> partial;
  EXPECT_EQ(image->ReadSectors(image->GetLBACount() + CDImage::LEAD_OUT_SECTOR_COUNT - 1, partial,
                               CDImage::SectorReadMode::DataAndSubQ),
            1u);
  EXPECT_EQ(image->ReadSectors(image->GetLBACount() + CDImage::LEAD_OUT_SECTOR_COUNT, partial,
                               CDImage::SectorReadMode::DataAndSubQ),
            0u);
}

TEST(CDImage, BatchReadSplitsAtIndexBoundaries)
{
  SplitIndexCDImage image;
  std::array<CDImage::Sector, 4> sectors;
  ASSERT_EQ(image.ReadSectors(1, sectors, CDImage::SectorReadMode::DataAndSubQ), sectors.size());

  ASSERT_EQ(image.m_read_calls.size(), 2u);
  EXPECT_EQ(image.m_read_calls[0].lba, 1u);
  EXPECT_EQ(image.m_read_calls[0].count, 1u);
  EXPECT_EQ(image.m_read_calls[1].lba, 2u);
  EXPECT_EQ(image.m_read_calls[1].count, 3u);

  for (u32 i = 0; i < sectors.size(); i++)
  {
    EXPECT_EQ(sectors[i].data.front(), i + 1);
    EXPECT_TRUE(sectors[i].subq.IsCRCValid());
    EXPECT_EQ(sectors[i].subq.index_number_bcd, BinaryToBCD(static_cast<u8>(i == 0 ? 1 : 2)));
  }
}

TEST(CDImage, GeneratedSubQOnlySkipsBackendRead)
{
  SplitIndexCDImage image;
  CDImage::Sector sector;
  sector.data.fill(0x5A);

  ASSERT_EQ(image.ReadSectors(1, std::span<CDImage::Sector>(&sector, 1), CDImage::SectorReadMode::SubQOnly), 1u);
  EXPECT_TRUE(image.m_read_calls.empty());
  EXPECT_TRUE(sector.subq.IsCRCValid());
  EXPECT_TRUE(std::all_of(sector.data.begin(), sector.data.end(), [](u8 value) { return value == 0x5A; }));
}

TEST(CDImage, DataOnlyDoesNotGenerateSubQ)
{
  SplitIndexCDImage image;
  CDImage::Sector sector;
  sector.subq.data.fill(0x5A);

  ASSERT_EQ(image.ReadSectors(1, std::span<CDImage::Sector>(&sector, 1), CDImage::SectorReadMode::DataOnly), 1u);
  ASSERT_EQ(image.m_read_calls.size(), 1u);
  EXPECT_EQ(image.m_read_calls.front().mode, CDImage::SectorReadMode::DataOnly);
  EXPECT_EQ(sector.data.front(), 1u);
  EXPECT_TRUE(std::all_of(sector.subq.data.begin(), sector.subq.data.end(), [](u8 value) { return value == 0x5A; }));
}

TEST(CDImage, CCDSubQOnlyDoesNotDependOnImageData)
{
  TempFile ccd("duckstation_cd_image_subq_only", "ccd");
  TempFile img(Path::ReplaceExtension(ccd.GetPath(), "img"));
  TempFile sub(Path::ReplaceExtension(ccd.GetPath(), "sub"));

  // The image deliberately contains no readable sector. A valid SUB record must still be independently readable.
  ASSERT_TRUE(img.Write(std::span<const u8>()));
  SplitIndexCDImage subq_generator;
  CDImage::SubChannelQ expected_subq;
  ASSERT_TRUE(subq_generator.GenerateSubChannelQ(&expected_subq, 1));
  std::array<u8, CDImage::ALL_SUBCODE_SIZE> subcode = {};
  std::memcpy(subcode.data() + CDImage::SUBCHANNEL_BYTES_PER_FRAME, expected_subq.data.data(),
              expected_subq.data.size());
  ASSERT_TRUE(sub.Write(subcode));

  static constexpr std::string_view ccd_data = "[CloneCD]\n"
                                               "Version=3\n"
                                               "[Disc]\n"
                                               "TocEntries=3\n"
                                               "[Entry 0]\n"
                                               "Point=0xA0\n"
                                               "[Entry 1]\n"
                                               "Point=1\n"
                                               "ADR=1\n"
                                               "Control=4\n"
                                               "PLBA=0\n"
                                               "[Entry 2]\n"
                                               "Point=0xA2\n"
                                               "PLBA=1\n"
                                               "[TRACK 1]\n"
                                               "MODE=1\n"
                                               "INDEX 1=0\n";
  ASSERT_TRUE(ccd.WriteString(ccd_data));

  Error error;
  std::unique_ptr<CDImage> image = CDImage::OpenCCDImage(ccd.GetPath().c_str(), &error);
  ASSERT_TRUE(image) << error.GetDescription();

  CDImage::Sector sector;
  EXPECT_EQ(image->ReadSectors(2 * CDImage::FRAMES_PER_SECOND, std::span<CDImage::Sector>(&sector, 1),
                               CDImage::SectorReadMode::DataAndSubQ),
            0u);

  sector.data.fill(0x5A);
  ASSERT_EQ(image->ReadSectors(2 * CDImage::FRAMES_PER_SECOND, std::span<CDImage::Sector>(&sector, 1),
                               CDImage::SectorReadMode::SubQOnly),
            1u);
  EXPECT_EQ(sector.subq.data, expected_subq.data);
  EXPECT_TRUE(std::all_of(sector.data.begin(), sector.data.end(), [](u8 value) { return value == 0x5A; }));
}

TEST(CDImage, PPFReplacementSectorsAreAuthoritative)
{
  auto parent = std::make_unique<PatchParentCDImage>();
  PatchParentCDImage* parent_ptr = parent.get();

  // A minimal PPF1 patch replacing byte 7 of sector 1. Loading materializes the entire source sector once.
  std::vector<u8> patch(56, 0);
  std::memcpy(patch.data(), "PPF1", 4);
  const u32 patch_offset = CDImage::RAW_SECTOR_SIZE + 7;
  const u8 patch_size = 1;
  const u8 patch_value = 0xCC;
  patch.insert(patch.end(), reinterpret_cast<const u8*>(&patch_offset),
               reinterpret_cast<const u8*>(&patch_offset) + sizeof(patch_offset));
  patch.push_back(patch_size);
  patch.push_back(patch_value);

  TempFile ppf("duckstation_cd_image_authoritative", "ppf");
  ASSERT_TRUE(ppf.Write(patch));

  Error error;
  std::unique_ptr<CDImage> image = CDImage::OverlayPPFPatch(ppf.GetPath().c_str(), std::move(parent), &error);
  ASSERT_TRUE(image) << error.GetDescription();

  // Once materialized, a replacement no longer depends on the parent sector remaining readable.
  parent_ptr->m_fail_reads = true;
  CDImage::Sector replaced_sector;
  ASSERT_EQ(
    image->ReadSectors(1, std::span<CDImage::Sector>(&replaced_sector, 1), CDImage::SectorReadMode::DataAndSubQ), 1u);
  EXPECT_EQ(replaced_sector.data[0], 1u);
  EXPECT_EQ(replaced_sector.data[7], patch_value);

  // A batch is split around replacement slots, so the parent sees only the unpatched gaps.
  parent_ptr->m_fail_reads = false;
  parent_ptr->m_read_calls.clear();
  std::array<CDImage::Sector, 4> sectors;
  ASSERT_EQ(image->ReadSectors(0, sectors, CDImage::SectorReadMode::DataAndSubQ), sectors.size());
  ASSERT_EQ(parent_ptr->m_read_calls.size(), 2u);
  EXPECT_EQ(parent_ptr->m_read_calls[0].lba, 0u);
  EXPECT_EQ(parent_ptr->m_read_calls[0].count, 1u);
  EXPECT_EQ(parent_ptr->m_read_calls[1].lba, 2u);
  EXPECT_EQ(parent_ptr->m_read_calls[1].count, 2u);
  EXPECT_EQ(sectors[1].data[7], patch_value);
}

TEST(CDImage, Iso2048DetectedAsMode1)
{
  TempFile iso("duckstation_cd_image_mode1", "iso");
  std::array<u8, CDImage::DATA_SECTOR_SIZE> payload = {};
  for (u32 i = 0; i < payload.size(); i++)
    payload[i] = static_cast<u8>(i + 1);
  ASSERT_TRUE(iso.Write(payload));

  Error error;
  std::unique_ptr<CDImage> image = CDImage::Open(iso.GetPath().c_str(), false, &error);
  ASSERT_TRUE(image) << error.GetDescription();
  EXPECT_EQ(image->GetTrackMode(1), CDImage::TrackMode::Mode1);

  CDImage::Sector sector;
  ASSERT_EQ(image->ReadSectors(2 * CDImage::FRAMES_PER_SECOND, std::span<CDImage::Sector>(&sector, 1),
                               CDImage::SectorReadMode::DataAndSubQ),
            1u);
  ExpectSyncAndHeader(sector.data, 0x01, 2 * CDImage::FRAMES_PER_SECOND);
  EXPECT_EQ(std::memcmp(&sector.data[16], payload.data(), payload.size()), 0);
}

TEST(CDImage, IsoRawDetectedAsRaw)
{
  TempFile iso("duckstation_cd_image_raw", "iso");
  std::array<u8, CDImage::RAW_SECTOR_SIZE> raw = MakePatternSector();
  std::memcpy(raw.data(), CDImage::SECTOR_SYNC_DATA.data(), CDImage::SECTOR_SYNC_DATA.size());
  raw[12] = 0x00;
  raw[13] = 0x00;
  raw[14] = 0x00;
  raw[15] = 0x02;
  ASSERT_TRUE(iso.Write(raw));

  Error error;
  std::unique_ptr<CDImage> image = CDImage::Open(iso.GetPath().c_str(), false, &error);
  ASSERT_TRUE(image) << error.GetDescription();
  EXPECT_EQ(image->GetTrackMode(1), CDImage::TrackMode::Mode2Raw);

  CDImage::Sector sector;
  ASSERT_EQ(image->ReadSectors(2 * CDImage::FRAMES_PER_SECOND, std::span<CDImage::Sector>(&sector, 1),
                               CDImage::SectorReadMode::DataAndSubQ),
            1u);
  EXPECT_EQ(std::memcmp(sector.data.data(), raw.data(), raw.size()), 0);
}
