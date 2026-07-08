// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "util/cd_image.h"

#include "common/bcdutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/path.h"
#include "common/types.h"

#include "fmt/core.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

namespace {

// TODO: Extract this out, use FileSystem temporary file
class TempFile
{
public:
  TempFile(const char* name, const char* extension)
    : m_path(::Path::Combine(FileSystem::GetWorkingDirectory(), fmt::format("{}_{}.{}", name, s_counter++, extension)))
  {
  }

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

  std::array<u8, CDImage::RAW_SECTOR_SIZE> sector;
  ASSERT_TRUE(image->ReadRawSector(sector.data(), nullptr));
  ExpectSyncAndHeader(sector, 0x01, 2 * CDImage::FRAMES_PER_SECOND);
  EXPECT_EQ(std::memcmp(&sector[16], payload.data(), payload.size()), 0);
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

  std::array<u8, CDImage::RAW_SECTOR_SIZE> sector;
  ASSERT_TRUE(image->ReadRawSector(sector.data(), nullptr));
  ExpectSyncAndHeader(sector, 0x01, 2 * CDImage::FRAMES_PER_SECOND);
  EXPECT_EQ(std::memcmp(&sector[16], payload.data(), payload.size()), 0);
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

  std::array<u8, CDImage::RAW_SECTOR_SIZE> sector;
  ASSERT_TRUE(image->ReadRawSector(sector.data(), nullptr));
  EXPECT_EQ(std::memcmp(sector.data(), raw.data(), raw.size()), 0);
}
