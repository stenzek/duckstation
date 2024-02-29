// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "assert.h"
#include "cd_image.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <optional>
#include <span>

Log_SetChannel(CDImageDevice);

// Common code
[[maybe_unused]] static constexpr u32 MAX_TRACK_NUMBER = 99;
[[maybe_unused]] static constexpr u32 SCSI_CMD_LENGTH = 12;

enum class SCSIReadMode : u8
{
  None,
  Raw,
  Full,
  SubQOnly,
};

[[maybe_unused]] static void FillSCSIReadCommand(u8 cmd[SCSI_CMD_LENGTH], u32 sector_number, SCSIReadMode mode)
{
  cmd[0] = 0xBE;                           // READ CD
  cmd[1] = 0x00;                           // sector type
  cmd[2] = Truncate8(sector_number >> 24); // Starting LBA
  cmd[3] = Truncate8(sector_number >> 16);
  cmd[4] = Truncate8(sector_number >> 8);
  cmd[5] = Truncate8(sector_number);
  cmd[6] = 0x00; // Transfer Count
  cmd[7] = 0x00;
  cmd[8] = 0x01;
  cmd[9] = (1 << 7) |    // include sync
           (0b11 << 5) | // include header codes
           (1 << 4) |    // include user data
           (1 << 3) |    // edc/ecc
           (0 << 2);     // don't include C2 data

  if (mode == SCSIReadMode::None || mode == SCSIReadMode::Raw)
    cmd[10] = 0b000;
  else if (mode == SCSIReadMode::Full)
    cmd[10] = 0b001;
  else // if (mode == SCSIReadMode::SubQOnly)
    cmd[10] = 0b010;
  cmd[11] = 0;
}

[[maybe_unused]] static void FillSCSISetSpeedCommand(u8 cmd[SCSI_CMD_LENGTH], u32 speed_multiplier)
{
  DebugAssert(speed_multiplier > 0);

  cmd[0] = 0xDA; // SET CD-ROM SPEED
  cmd[1] = 0x00;
  cmd[2] = Truncate8(speed_multiplier - 1);
  cmd[3] = 0x00;
  cmd[4] = 0x00;
  cmd[5] = 0x00;
  cmd[6] = 0x00;
  cmd[7] = 0x00;
  cmd[8] = 0x00;
  cmd[9] = 0x00;
  cmd[10] = 0x00;
  cmd[11] = 0x00;
}

[[maybe_unused]] static constexpr u32 SCSIReadCommandOutputSize(SCSIReadMode mode)
{
  switch (mode)
  {
    case SCSIReadMode::None:
    case SCSIReadMode::Raw:
      return CDImage::RAW_SECTOR_SIZE;
    case SCSIReadMode::Full:
      return CDImage::RAW_SECTOR_SIZE + CDImage::ALL_SUBCODE_SIZE;
    case SCSIReadMode::SubQOnly:
      return CDImage::RAW_SECTOR_SIZE + CDImage::SUBCHANNEL_BYTES_PER_FRAME;
    default:
      UnreachableCode();
  }
}

[[maybe_unused]] static bool VerifySCSIReadData(std::span<const u8> buffer, SCSIReadMode mode)
{
  const u32 expected_size = SCSIReadCommandOutputSize(mode);
  if (buffer.size() != expected_size)
  {
    Log_ErrorFmt("SCSI returned {} bytes, expected {}", buffer.size(), expected_size);
    return false;
  }

  if (mode == SCSIReadMode::Full)
  {
    // Validate subcode.
    u8 deinterleaved_subcode[CDImage::ALL_SUBCODE_SIZE];
    CDImage::SubChannelQ subq;
    CDImage::DeinterleaveSubcode(buffer.data() + CDImage::RAW_SECTOR_SIZE, deinterleaved_subcode);
    std::memcpy(&subq, &deinterleaved_subcode[CDImage::SUBCHANNEL_BYTES_PER_FRAME], sizeof(subq));
    if (!subq.IsCRCValid())
    {
      Log_WarningFmt("SCSI full subcode read returned invalid SubQ CRC (got {:02X} expected {:02X})", subq.crc,
                     CDImage::SubChannelQ::ComputeCRC(subq.data));
      return false;
    }

    return true;
  }
  else if (mode == SCSIReadMode::SubQOnly)
  {
    CDImage::SubChannelQ subq;
    std::memcpy(&subq, buffer.data() + CDImage::RAW_SECTOR_SIZE, sizeof(subq));
    if (!subq.IsCRCValid())
    {
      Log_WarningFmt("SCSI subq read returned invalid SubQ CRC (got {:02X} expected {:02X})", subq.crc,
                     CDImage::SubChannelQ::ComputeCRC(subq.data));
      return false;
    }

    return true;
  }
  else // if (mode == SCSIReadMode::None || mode == SCSIReadMode::Raw)
  {
    // I guess we could check the sector sync data too...
    return true;
  }
}

#if defined(_WIN32)

// The include order here is critical.
// clang-format off
#include "common/windows_headers.h"
#include <winioctl.h>
#include <ntddcdrm.h>
#include <ntddscsi.h>
// clang-format on

static u32 BEToU32(const u8* val)
{
  return (static_cast<u32>(val[0]) << 24) | (static_cast<u32>(val[1]) << 16) | (static_cast<u32>(val[2]) << 8) |
         static_cast<u32>(val[3]);
}

static void U16ToBE(u8* beval, u16 leval)
{
  beval[0] = static_cast<u8>(leval >> 8);
  beval[1] = static_cast<u8>(leval);
}

namespace {

class CDImageDeviceWin32 : public CDImage
{
public:
  CDImageDeviceWin32();
  ~CDImageDeviceWin32() override;

  bool Open(const char* filename, Error* error);

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasNonStandardSubchannel() const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  std::optional<u32> DoSCSICommand(u8 cmd[SCSI_CMD_LENGTH], std::span<u8> out_buffer);
  std::optional<u32> DoSCSIRead(LBA lba, SCSIReadMode read_mode);
  bool DoRawRead(LBA lba);
  bool DoSetSpeed(u32 speed_multiplier);

  bool ReadSectorToBuffer(LBA lba);
  bool DetermineReadMode(bool try_sptd);

  HANDLE m_hDevice = INVALID_HANDLE_VALUE;

  u32 m_current_lba = ~static_cast<LBA>(0);

  SCSIReadMode m_scsi_read_mode = SCSIReadMode::None;
  bool m_has_valid_subcode = false;

  std::array<u8, CD_RAW_SECTOR_WITH_SUBCODE_SIZE> m_buffer;
  std::array<u8, SUBCHANNEL_BYTES_PER_FRAME> m_subq;
};

} // namespace

CDImageDeviceWin32::CDImageDeviceWin32() = default;

CDImageDeviceWin32::~CDImageDeviceWin32()
{
  if (m_hDevice != INVALID_HANDLE_VALUE)
    CloseHandle(m_hDevice);
}

bool CDImageDeviceWin32::Open(const char* filename, Error* error)
{
  bool try_sptd = true;

  m_filename = filename;
  m_hDevice = CreateFile(filename, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, NULL);
  if (m_hDevice == INVALID_HANDLE_VALUE)
  {
    m_hDevice = CreateFile(filename, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, NULL);
    if (m_hDevice != INVALID_HANDLE_VALUE)
    {
      Log_WarningFmt("Could not open '{}' as read/write, can't use SPTD", filename);
      try_sptd = false;
    }
    else
    {
      Log_ErrorFmt("CreateFile('{}') failed: %08X", filename, GetLastError());
      if (error)
        error->SetWin32(GetLastError());

      return false;
    }
  }

  // Set it to 4x speed. A good balance between readahead and spinning up way too high.
  static constexpr u32 READ_SPEED_MULTIPLIER = 8;
  static constexpr u32 READ_SPEED_KBS = (DATA_SECTOR_SIZE * FRAMES_PER_SECOND * READ_SPEED_MULTIPLIER) / 1024;
  CDROM_SET_SPEED set_speed = {CdromSetSpeed, READ_SPEED_KBS, 0, CdromDefaultRotation};
  if (!DeviceIoControl(m_hDevice, IOCTL_CDROM_SET_SPEED, &set_speed, sizeof(set_speed), nullptr, 0, nullptr, nullptr))
    Log_WarningFmt("DeviceIoControl(IOCTL_CDROM_SET_SPEED) failed: {:08X}", GetLastError());

  CDROM_READ_TOC_EX read_toc_ex = {};
  read_toc_ex.Format = CDROM_READ_TOC_EX_FORMAT_TOC;
  read_toc_ex.Msf = 0;
  read_toc_ex.SessionTrack = 1;

  CDROM_TOC toc = {};
  U16ToBE(toc.Length, sizeof(toc) - sizeof(UCHAR) * 2);

  DWORD bytes_returned;
  if (!DeviceIoControl(m_hDevice, IOCTL_CDROM_READ_TOC_EX, &read_toc_ex, sizeof(read_toc_ex), &toc, sizeof(toc),
                       &bytes_returned, nullptr) ||
      toc.LastTrack < toc.FirstTrack)
  {
    Log_ErrorPrintf("DeviceIoCtl(IOCTL_CDROM_READ_TOC_EX) failed: %08X", GetLastError());
    if (error)
      error->SetWin32(GetLastError());

    return false;
  }

  DWORD last_track_address = 0;
  LBA disc_lba = 0;
  Log_DevPrintf("FirstTrack=%u, LastTrack=%u", toc.FirstTrack, toc.LastTrack);

  const u32 num_tracks_to_check = (toc.LastTrack - toc.FirstTrack) + 1 + 1;
  for (u32 track_index = 0; track_index < num_tracks_to_check; track_index++)
  {
    const TRACK_DATA& td = toc.TrackData[track_index];
    const u8 track_num = td.TrackNumber;
    const DWORD track_address = BEToU32(td.Address);
    Log_DevPrintf("  [%u]: Num=%02X, Address=%u", track_index, track_num, track_address);

    // fill in the previous track's length
    if (!m_tracks.empty())
    {
      if (track_num < m_tracks.back().track_number)
      {
        Log_ErrorPrintf("Invalid TOC, track %u less than %u", track_num, m_tracks.back().track_number);
        return false;
      }

      const LBA previous_track_length = static_cast<LBA>(track_address - last_track_address);
      m_tracks.back().length += previous_track_length;
      m_indices.back().length += previous_track_length;
      disc_lba += previous_track_length;
    }

    last_track_address = track_address;
    if (track_num == LEAD_OUT_TRACK_NUMBER)
    {
      AddLeadOutIndex();
      break;
    }

    // precompute subchannel q flags for the whole track
    SubChannelQ::Control control{};
    control.bits = td.Adr | (td.Control << 4);

    const LBA track_lba = static_cast<LBA>(track_address);
    const TrackMode track_mode = control.data ? CDImage::TrackMode::Mode2Raw : CDImage::TrackMode::Audio;

    // TODO: How the hell do we handle pregaps here?
    const u32 pregap_frames = (control.data && track_index == 0) ? 150 : 0;
    if (pregap_frames > 0)
    {
      Index pregap_index = {};
      pregap_index.start_lba_on_disc = disc_lba;
      pregap_index.start_lba_in_track = static_cast<LBA>(-static_cast<s32>(pregap_frames));
      pregap_index.length = pregap_frames;
      pregap_index.track_number = track_num;
      pregap_index.index_number = 0;
      pregap_index.mode = track_mode;
      pregap_index.submode = CDImage::SubchannelMode::None;
      pregap_index.control.bits = control.bits;
      pregap_index.is_pregap = true;
      m_indices.push_back(pregap_index);
      disc_lba += pregap_frames;
    }

    // index 1, will be filled in next iteration
    if (track_num <= MAX_TRACK_NUMBER)
    {
      // add the track itself
      m_tracks.push_back(
        Track{track_num, disc_lba, static_cast<u32>(m_indices.size()), 0, track_mode, SubchannelMode::None, control});

      Index index1;
      index1.start_lba_on_disc = disc_lba;
      index1.start_lba_in_track = 0;
      index1.length = 0;
      index1.track_number = track_num;
      index1.index_number = 1;
      index1.file_index = 0;
      index1.file_sector_size = RAW_SECTOR_SIZE;
      index1.file_offset = static_cast<u64>(track_lba);
      index1.mode = track_mode;
      index1.submode = CDImage::SubchannelMode::None;
      index1.control.bits = control.bits;
      index1.is_pregap = false;
      m_indices.push_back(index1);
    }
  }

  if (m_tracks.empty())
  {
    Log_ErrorPrintf("File '%s' contains no tracks", filename);
    Error::SetString(error, fmt::format("File '{}' contains no tracks", filename));
    return false;
  }

  m_lba_count = disc_lba;

  Log_DevPrintf("%u tracks, %u indices, %u lbas", static_cast<u32>(m_tracks.size()), static_cast<u32>(m_indices.size()),
                static_cast<u32>(m_lba_count));
  for (u32 i = 0; i < m_tracks.size(); i++)
  {
    Log_DevPrintf(" Track %u: Start %u, length %u, mode %u, control 0x%02X", static_cast<u32>(m_tracks[i].track_number),
                  static_cast<u32>(m_tracks[i].start_lba), static_cast<u32>(m_tracks[i].length),
                  static_cast<u32>(m_tracks[i].mode), static_cast<u32>(m_tracks[i].control.bits));
  }
  for (u32 i = 0; i < m_indices.size(); i++)
  {
    Log_DevPrintf(" Index %u: Track %u, Index %u, Start %u, length %u, file sector size %u, file offset %" PRIu64, i,
                  static_cast<u32>(m_indices[i].track_number), static_cast<u32>(m_indices[i].index_number),
                  static_cast<u32>(m_indices[i].start_lba_on_disc), static_cast<u32>(m_indices[i].length),
                  static_cast<u32>(m_indices[i].file_sector_size), m_indices[i].file_offset);
  }

  if (!DetermineReadMode(try_sptd))
  {
    Log_ErrorPrintf("Could not determine read mode");
    Error::SetString(error, "Could not determine read mode");
    return false;
  }

  return Seek(1, Position{0, 0, 0});
}

bool CDImageDeviceWin32::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  if (index.file_sector_size == 0 || !m_has_valid_subcode)
    return CDImage::ReadSubChannelQ(subq, index, lba_in_index);

  const LBA offset = static_cast<LBA>(index.file_offset) + lba_in_index;
  if (m_current_lba != offset && !ReadSectorToBuffer(offset))
    return false;

  // P, Q, ...
  std::memcpy(subq->data.data(), m_subq.data(), SUBCHANNEL_BYTES_PER_FRAME);
  return true;
}

bool CDImageDeviceWin32::HasNonStandardSubchannel() const
{
  return m_has_valid_subcode;
}

bool CDImageDeviceWin32::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  if (index.file_sector_size == 0)
    return false;

  const LBA offset = static_cast<LBA>(index.file_offset) + lba_in_index;
  if (m_current_lba != offset && !ReadSectorToBuffer(offset))
    return false;

  std::memcpy(buffer, m_buffer.data(), RAW_SECTOR_SIZE);
  return true;
}

std::optional<u32> CDImageDeviceWin32::DoSCSICommand(u8 cmd[SCSI_CMD_LENGTH], std::span<u8> out_buffer)
{
  struct SPTDBuffer
  {
    SCSI_PASS_THROUGH_DIRECT cmd;
    u8 sense[20];
  };
  SPTDBuffer sptd = {};
  sptd.cmd.Length = sizeof(sptd.cmd);
  sptd.cmd.CdbLength = SCSI_CMD_LENGTH;
  sptd.cmd.SenseInfoLength = sizeof(sptd.sense);
  sptd.cmd.DataIn = out_buffer.empty() ? SCSI_IOCTL_DATA_UNSPECIFIED : SCSI_IOCTL_DATA_IN;
  sptd.cmd.DataTransferLength = static_cast<u32>(out_buffer.size());
  sptd.cmd.TimeOutValue = 10;
  sptd.cmd.SenseInfoOffset = offsetof(SPTDBuffer, sense);
  sptd.cmd.DataBuffer = out_buffer.empty() ? nullptr : out_buffer.data();
  std::memcpy(sptd.cmd.Cdb, cmd, SCSI_CMD_LENGTH);

  DWORD bytes_returned;
  if (!DeviceIoControl(m_hDevice, IOCTL_SCSI_PASS_THROUGH_DIRECT, &sptd, sizeof(sptd), &sptd, sizeof(sptd),
                       &bytes_returned, nullptr))
  {
    Log_ErrorFmt("DeviceIoControl() for SCSI 0x{:02X} failed: {}", cmd[0], GetLastError());
    return std::nullopt;
  }

  if (sptd.cmd.ScsiStatus != 0)
  {
    Log_ErrorFmt("SCSI command 0x{:02X} failed: {}", cmd[0], sptd.cmd.ScsiStatus);
    return std::nullopt;
  }

  if (sptd.cmd.DataTransferLength != out_buffer.size())
    Log_WarningFmt("Only read {} of {} bytes", sptd.cmd.DataTransferLength, out_buffer.size());

  return sptd.cmd.DataTransferLength;
}

std::optional<u32> CDImageDeviceWin32::DoSCSIRead(LBA lba, SCSIReadMode read_mode)
{
  u8 cmd[SCSI_CMD_LENGTH];
  FillSCSIReadCommand(cmd, lba, read_mode);

  const u32 size = SCSIReadCommandOutputSize(read_mode);
  return DoSCSICommand(cmd, std::span<u8>(m_buffer.data(), size));
}

bool CDImageDeviceWin32::DoSetSpeed(u32 speed_multiplier)
{
  u8 cmd[SCSI_CMD_LENGTH];
  FillSCSISetSpeedCommand(cmd, speed_multiplier);
  return DoSCSICommand(cmd, {}).has_value();
}

bool CDImageDeviceWin32::DoRawRead(LBA lba)
{
  const DWORD expected_size = RAW_SECTOR_SIZE + ALL_SUBCODE_SIZE;

  RAW_READ_INFO rri;
  rri.DiskOffset.QuadPart = static_cast<u64>(lba) * 2048;
  rri.SectorCount = 1;
  rri.TrackMode = RawWithSubCode;

  DWORD bytes_returned;
  if (!DeviceIoControl(m_hDevice, IOCTL_CDROM_RAW_READ, &rri, sizeof(rri), m_buffer.data(),
                       static_cast<DWORD>(m_buffer.size()), &bytes_returned, nullptr))
  {
    Log_ErrorFmt("DeviceIoControl(IOCTL_CDROM_RAW_READ) for LBA {} failed: {:08X}", lba, GetLastError());
    return false;
  }

  if (bytes_returned != expected_size)
    Log_WarningFmt("Only read {} of {} bytes", bytes_returned, expected_size);

  return true;
}

bool CDImageDeviceWin32::ReadSectorToBuffer(LBA lba)
{
  if (m_scsi_read_mode != SCSIReadMode::None)
  {
    const std::optional<u32> size = DoSCSIRead(lba, m_scsi_read_mode);
    const u32 expected_size = SCSIReadCommandOutputSize(m_scsi_read_mode);
    if (size.value_or(0) != expected_size)
    {
      Log_ErrorFmt("Read of LBA {} failed: only got {} of {} bytes", lba, size.value(), expected_size);
      return false;
    }

    if (m_scsi_read_mode == SCSIReadMode::Full)
    {
      // need to deinterleave the subcode
      u8 deinterleaved_subcode[ALL_SUBCODE_SIZE];
      DeinterleaveSubcode(m_buffer.data() + RAW_SECTOR_SIZE, deinterleaved_subcode);
      std::memcpy(&m_subq, deinterleaved_subcode + SUBCHANNEL_BYTES_PER_FRAME, sizeof(m_subq)); // P,Q
    }
    else if (m_scsi_read_mode == SCSIReadMode::SubQOnly)
    {
      // copy out subq
      std::memcpy(&m_subq, m_buffer.data() + RAW_SECTOR_SIZE, sizeof(m_subq));
    }
  }
  else
  {
    if (!DoRawRead(lba))
      return false;
  }

  m_current_lba = lba;
  return true;
}

bool CDImageDeviceWin32::DetermineReadMode(bool try_sptd)
{
  // Prefer raw reads if we can use them
  const LBA track_1_lba = static_cast<LBA>(m_indices[m_tracks[0].first_index].file_offset);

  Log_DevPrint("Trying raw reads...");
  if (DoRawRead(track_1_lba))
  {
    // verify subcode
    if (VerifySCSIReadData(std::span<u8>(m_buffer.data(), SCSIReadCommandOutputSize(SCSIReadMode::Full)),
                           SCSIReadMode::Full))
    {
      Log_VerbosePrint("Using raw reads with full subcode");
      m_scsi_read_mode = SCSIReadMode::None;
      m_has_valid_subcode = true;
      return true;
    }
  }

  std::optional<u32> transfer_size;

  Log_DevPrint("Trying SCSI read with full subcode...");
  if ((transfer_size = DoSCSIRead(track_1_lba, SCSIReadMode::Full)).has_value())
  {
    if (VerifySCSIReadData(std::span<u8>(m_buffer.data(), transfer_size.value()), SCSIReadMode::Full))
    {
      Log_VerbosePrint("Using SCSI reads with subcode");
      m_scsi_read_mode = SCSIReadMode::Full;
      m_has_valid_subcode = true;
      return true;
    }
  }

  Log_WarningPrint("Full subcode failed, trying SCSI read with only subq...");
  if ((transfer_size = DoSCSIRead(track_1_lba, SCSIReadMode::SubQOnly)).has_value())
  {
    if (VerifySCSIReadData(std::span<u8>(m_buffer.data(), transfer_size.value()), SCSIReadMode::SubQOnly))
    {
      Log_VerbosePrint("Using SCSI reads with subq only");
      m_scsi_read_mode = SCSIReadMode::SubQOnly;
      m_has_valid_subcode = true;
      return true;
    }
  }

  // As a last ditch effort, try SCSI without subcode.
  Log_WarningPrint("Subq only failed failed, trying SCSI without subcode...");
  if ((transfer_size = DoSCSIRead(track_1_lba, SCSIReadMode::Raw)).has_value())
  {
    if (VerifySCSIReadData(std::span<u8>(m_buffer.data(), transfer_size.value()), SCSIReadMode::Raw))
    {
      Log_WarningPrint("Using SCSI raw reads, libcrypt games will not run correctly");
      m_scsi_read_mode = SCSIReadMode::Raw;
      m_has_valid_subcode = false;
      return true;
    }
  }

  Log_WarningPrint("Raw SCSI read failed, trying raw but ignoring subcode...");
  if (DoRawRead(track_1_lba))
  {
    Log_WarningPrint("Using raw reads without subcode, libcrypt games will not run correctly");
    m_scsi_read_mode = SCSIReadMode::None;
    m_has_valid_subcode = false;
    return true;
  }

  Log_ErrorPrint("No read modes were successful, cannot use device.");
  return false;
}

std::unique_ptr<CDImage> CDImage::OpenDeviceImage(const char* filename, Error* error)
{
  std::unique_ptr<CDImageDeviceWin32> image = std::make_unique<CDImageDeviceWin32>();
  if (!image->Open(filename, error))
    return {};

  return image;
}

std::vector<std::pair<std::string, std::string>> CDImage::GetDeviceList()
{
  std::vector<std::pair<std::string, std::string>> ret;

  char buf[256];
  if (GetLogicalDriveStringsA(sizeof(buf), buf) != 0)
  {
    const char* ptr = buf;
    while (*ptr != '\0')
    {
      std::size_t len = std::strlen(ptr);
      const DWORD type = GetDriveTypeA(ptr);
      if (type != DRIVE_CDROM)
      {
        ptr += len + 1u;
        continue;
      }

      // Drop the trailing slash.
      const std::size_t append_len = (ptr[len - 1] == '\\') ? (len - 1) : len;

      std::string path;
      path.append("\\\\.\\");
      path.append(ptr, append_len);

      std::string name(ptr, append_len);

      ret.emplace_back(std::move(path), std::move(name));

      ptr += len + 1u;
    }
  }

  return ret;
}

bool CDImage::IsDeviceName(const char* filename)
{
  return std::string_view(filename).starts_with("\\\\.\\");
}

#elif defined(__linux__) && !defined(__ANDROID__)

#include <fcntl.h>
#include <libudev.h>
#include <linux/cdrom.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

class CDImageDeviceLinux : public CDImage
{
public:
  CDImageDeviceLinux();
  ~CDImageDeviceLinux() override;

  bool Open(const char* filename, Error* error);

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasNonStandardSubchannel() const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  // Raw reads use an offset of 00:02:00
  static constexpr LBA RAW_READ_OFFSET = 2 * FRAMES_PER_SECOND;

  bool ReadSectorToBuffer(LBA lba);
  bool DetermineReadMode(Error* error);

  std::optional<u32> DoSCSICommand(u8 cmd[SCSI_CMD_LENGTH], std::span<u8> out_buffer);
  std::optional<u32> DoSCSIRead(LBA lba, SCSIReadMode read_mode);
  bool DoRawRead(LBA lba);
  bool DoSetSpeed(u32 speed_multiplier);

  int m_fd = -1;
  LBA m_current_lba = ~static_cast<LBA>(0);

  SCSIReadMode m_scsi_read_mode = SCSIReadMode::None;

  std::array<u8, RAW_SECTOR_SIZE + ALL_SUBCODE_SIZE> m_buffer;
  std::array<u8, SUBCHANNEL_BYTES_PER_FRAME> m_subq;
};

} // namespace

CDImageDeviceLinux::CDImageDeviceLinux() = default;

CDImageDeviceLinux::~CDImageDeviceLinux()
{
  if (m_fd >= 0)
    close(m_fd);
}

bool CDImageDeviceLinux::Open(const char* filename, Error* error)
{
  m_filename = filename;

  m_fd = open(filename, O_RDONLY);
  if (m_fd < 0)
  {
    Error::SetErrno(error, "Failed to open device: ", errno);
    return false;
  }

  // Set it to 4x speed. A good balance between readahead and spinning up way too high.
  const int read_speed = 4;
  if (!DoSetSpeed(read_speed) && ioctl(m_fd, CDROM_SELECT_SPEED, &read_speed) != 0)
    Log_WarningFmt("ioctl(CDROM_SELECT_SPEED) failed: {}", errno);

  // Read ToC
  cdrom_tochdr toc_hdr = {};
  if (ioctl(m_fd, CDROMREADTOCHDR, &toc_hdr) != 0)
  {
    Error::SetErrno(error, "ioctl(CDROMREADTOCHDR) failed: ", errno);
    return false;
  }

  Log_DevFmt("FirstTrack={}, LastTrack={}", toc_hdr.cdth_trk0, toc_hdr.cdth_trk1);
  if (toc_hdr.cdth_trk1 < toc_hdr.cdth_trk0)
  {
    Error::SetStringFmt(error, "Last track {} is before first track {}", toc_hdr.cdth_trk1, toc_hdr.cdth_trk0);
    return false;
  }

  cdrom_tocentry toc_ent = {};
  toc_ent.cdte_format = CDROM_LBA;

  LBA disc_lba = 0;
  int last_track_lba = 0;
  const u32 num_tracks_to_check = (toc_hdr.cdth_trk1 - toc_hdr.cdth_trk0) + 1;
  for (u32 track_index = 0; track_index < num_tracks_to_check; track_index++)
  {
    const u32 track_num = toc_hdr.cdth_trk0 + track_index;

    toc_ent.cdte_track = static_cast<u8>(track_num);
    if (ioctl(m_fd, CDROMREADTOCENTRY, &toc_ent) < 0)
    {
      Error::SetErrno(error, "ioctl(CDROMREADTOCENTRY) failed: ", errno);
      return false;
    }

    Log_DevFmt("  [{}]: Num={}, LBA={}", track_index, track_num, toc_ent.cdte_addr.lba);

    // fill in the previous track's length
    if (!m_tracks.empty())
    {
      if (track_num < m_tracks.back().track_number)
      {
        Log_ErrorPrintf("Invalid TOC, track %u less than %u", track_num, m_tracks.back().track_number);
        return false;
      }

      const LBA previous_track_length = static_cast<LBA>(toc_ent.cdte_addr.lba - last_track_lba);
      m_tracks.back().length += previous_track_length;
      m_indices.back().length += previous_track_length;
      disc_lba += previous_track_length;
    }

    last_track_lba = toc_ent.cdte_addr.lba;

    // precompute subchannel q flags for the whole track
    SubChannelQ::Control control{};
    control.bits = toc_ent.cdte_adr | (toc_ent.cdte_ctrl << 4);

    const LBA track_lba = static_cast<LBA>(toc_ent.cdte_addr.lba);
    const TrackMode track_mode = control.data ? CDImage::TrackMode::Mode2Raw : CDImage::TrackMode::Audio;

    // TODO: How the hell do we handle pregaps here?
    const u32 pregap_frames = (control.data && track_index == 0) ? 150 : 0;
    if (pregap_frames > 0)
    {
      Index pregap_index = {};
      pregap_index.start_lba_on_disc = disc_lba;
      pregap_index.start_lba_in_track = static_cast<LBA>(-static_cast<s32>(pregap_frames));
      pregap_index.length = pregap_frames;
      pregap_index.track_number = track_num;
      pregap_index.index_number = 0;
      pregap_index.mode = track_mode;
      pregap_index.submode = CDImage::SubchannelMode::None;
      pregap_index.control.bits = control.bits;
      pregap_index.is_pregap = true;
      m_indices.push_back(pregap_index);
      disc_lba += pregap_frames;
    }

    // index 1, will be filled in next iteration
    if (track_num <= MAX_TRACK_NUMBER)
    {
      // add the track itself
      m_tracks.push_back(
        Track{track_num, disc_lba, static_cast<u32>(m_indices.size()), 0, track_mode, SubchannelMode::None, control});

      Index index1;
      index1.start_lba_on_disc = disc_lba;
      index1.start_lba_in_track = 0;
      index1.length = 0;
      index1.track_number = track_num;
      index1.index_number = 1;
      index1.file_index = 0;
      index1.file_sector_size = RAW_SECTOR_SIZE;
      index1.file_offset = static_cast<u64>(track_lba);
      index1.mode = track_mode;
      index1.submode = CDImage::SubchannelMode::None;
      index1.control.bits = control.bits;
      index1.is_pregap = false;
      m_indices.push_back(index1);
    }
  }

  if (m_tracks.empty())
  {
    Log_ErrorPrintf("File '%s' contains no tracks", filename);
    Error::SetString(error, fmt::format("File '{}' contains no tracks", filename));
    return false;
  }

  // Read lead-out.
  toc_ent.cdte_track = 0xAA;
  if (ioctl(m_fd, CDROMREADTOCENTRY, &toc_ent) < 0)
  {
    Error::SetErrno(error, "ioctl(CDROMREADTOCENTRY) for lead-out failed: ", errno);
    return false;
  }
  if (toc_ent.cdte_addr.lba < last_track_lba)
  {
    Error::SetStringFmt(error, "Lead-out LBA {} is less than last track {}", toc_ent.cdte_addr.lba, last_track_lba);
    return false;
  }

  // Fill last track length from lead-out.
  const LBA previous_track_length = static_cast<LBA>(toc_ent.cdte_addr.lba - last_track_lba);
  m_tracks.back().length += previous_track_length;
  m_indices.back().length += previous_track_length;
  disc_lba += previous_track_length;

  // And add the lead-out itself.
  AddLeadOutIndex();

  m_lba_count = disc_lba;

  Log_DevPrintf("%u tracks, %u indices, %u lbas", static_cast<u32>(m_tracks.size()), static_cast<u32>(m_indices.size()),
                static_cast<u32>(m_lba_count));
  for (u32 i = 0; i < m_tracks.size(); i++)
  {
    Log_DevPrintf(" Track %u: Start %u, length %u, mode %u, control 0x%02X", static_cast<u32>(m_tracks[i].track_number),
                  static_cast<u32>(m_tracks[i].start_lba), static_cast<u32>(m_tracks[i].length),
                  static_cast<u32>(m_tracks[i].mode), static_cast<u32>(m_tracks[i].control.bits));
  }
  for (u32 i = 0; i < m_indices.size(); i++)
  {
    Log_DevPrintf(" Index %u: Track %u, Index %u, Start %u, length %u, file sector size %u, file offset %" PRIu64, i,
                  static_cast<u32>(m_indices[i].track_number), static_cast<u32>(m_indices[i].index_number),
                  static_cast<u32>(m_indices[i].start_lba_on_disc), static_cast<u32>(m_indices[i].length),
                  static_cast<u32>(m_indices[i].file_sector_size), m_indices[i].file_offset);
  }

  if (!DetermineReadMode(error))
    return false;

  return Seek(1, Position{0, 0, 0});
}

bool CDImageDeviceLinux::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  if (index.file_sector_size == 0 || m_scsi_read_mode < SCSIReadMode::Full)
    return CDImage::ReadSubChannelQ(subq, index, lba_in_index);

  const LBA disc_lba = static_cast<LBA>(index.file_offset) + lba_in_index;
  if (m_current_lba != disc_lba && !ReadSectorToBuffer(disc_lba))
    return false;

  std::memcpy(subq->data.data(), m_subq.data(), SUBCHANNEL_BYTES_PER_FRAME);
  return true;
}

bool CDImageDeviceLinux::HasNonStandardSubchannel() const
{
  // Can only read subchannel through SPTD.
  return m_scsi_read_mode >= SCSIReadMode::Full;
}

bool CDImageDeviceLinux::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  if (index.file_sector_size == 0)
    return false;

  const LBA disc_lba = static_cast<LBA>(index.file_offset) + lba_in_index;
  if (m_current_lba != disc_lba && !ReadSectorToBuffer(disc_lba))
    return false;

  std::memcpy(buffer, m_buffer.data(), RAW_SECTOR_SIZE);
  return true;
}

std::optional<u32> CDImageDeviceLinux::DoSCSICommand(u8 cmd[SCSI_CMD_LENGTH], std::span<u8> out_buffer)
{
  sg_io_hdr_t hdr;
  std::memset(&hdr, 0, sizeof(hdr));
  hdr.cmd_len = SCSI_CMD_LENGTH;
  hdr.interface_id = 'S';
  hdr.dxfer_direction = out_buffer.empty() ? SG_DXFER_NONE : SG_DXFER_FROM_DEV;
  hdr.mx_sb_len = 0;
  hdr.dxfer_len = static_cast<u32>(out_buffer.size());
  hdr.dxferp = out_buffer.empty() ? nullptr : out_buffer.data();
  hdr.cmdp = cmd;
  hdr.timeout = 10000; // milliseconds

  if (ioctl(m_fd, SG_IO, &hdr) != 0)
  {
    Log_ErrorFmt("ioctl(SG_IO) for command {:02X} failed: {}", cmd[0], errno);
    return std::nullopt;
  }
  else if (hdr.status != 0)
  {
    Log_ErrorFmt("SCSI command {:02X} failed with status {}", cmd[0], hdr.status);
    return std::nullopt;
  }

  return hdr.dxfer_len;
}

std::optional<u32> CDImageDeviceLinux::DoSCSIRead(LBA lba, SCSIReadMode read_mode)
{
  u8 cmd[SCSI_CMD_LENGTH];
  FillSCSIReadCommand(cmd, lba, read_mode);

  const u32 size = SCSIReadCommandOutputSize(read_mode);
  return DoSCSICommand(cmd, std::span<u8>(m_buffer.data(), size));
}

bool CDImageDeviceLinux::DoSetSpeed(u32 speed_multiplier)
{
  u8 cmd[SCSI_CMD_LENGTH];
  FillSCSISetSpeedCommand(cmd, speed_multiplier);
  return DoSCSICommand(cmd, {}).has_value();
}

bool CDImageDeviceLinux::DoRawRead(LBA lba)
{
  const Position msf = Position::FromLBA(lba + RAW_READ_OFFSET);
  std::memcpy(m_buffer.data(), &msf, sizeof(msf));
  if (ioctl(m_fd, CDROMREADRAW, m_buffer.data()) != 0)
  {
    Log_ErrorFmt("CDROMREADRAW for LBA {} (MSF {}:{}:{}) failed: {}", lba, msf.minute, msf.second, msf.frame, errno);
    return false;
  }

  return true;
}

bool CDImageDeviceLinux::ReadSectorToBuffer(LBA lba)
{
  if (m_scsi_read_mode != SCSIReadMode::None)
  {
    const std::optional<u32> size = DoSCSIRead(lba, m_scsi_read_mode);
    const u32 expected_size = SCSIReadCommandOutputSize(m_scsi_read_mode);
    if (size.value_or(0) != expected_size)
    {
      Log_ErrorFmt("Read of LBA {} failed: only got {} of {} bytes", lba, size.value(), expected_size);
      return false;
    }

    if (m_scsi_read_mode == SCSIReadMode::Full)
    {
      // need to deinterleave the subcode
      u8 deinterleaved_subcode[ALL_SUBCODE_SIZE];
      DeinterleaveSubcode(m_buffer.data() + RAW_SECTOR_SIZE, deinterleaved_subcode);
      std::memcpy(&m_subq, deinterleaved_subcode + SUBCHANNEL_BYTES_PER_FRAME, sizeof(m_subq)); // P,Q
    }
    else if (m_scsi_read_mode == SCSIReadMode::SubQOnly)
    {
      // copy out subq
      std::memcpy(&m_subq, m_buffer.data() + RAW_SECTOR_SIZE, sizeof(m_subq));
    }
  }
  else
  {
    if (!DoRawRead(lba))
      return false;
  }

  m_current_lba = lba;
  return true;
}

bool CDImageDeviceLinux::DetermineReadMode(Error* error)
{
  const LBA track_1_lba = static_cast<LBA>(m_indices[m_tracks[0].first_index].file_offset);
  std::optional<u32> transfer_size;

  Log_DevPrint("Trying SCSI read with full subcode...");
  if ((transfer_size = DoSCSIRead(track_1_lba, SCSIReadMode::Full)).has_value())
  {
    if (VerifySCSIReadData(std::span<u8>(m_buffer.data(), transfer_size.value()), SCSIReadMode::Full))
    {
      Log_VerbosePrint("Using SCSI reads with subcode");
      m_scsi_read_mode = SCSIReadMode::Full;
      return true;
    }
  }

  Log_WarningPrint("Full subcode failed, trying SCSI read with only subq...");
  if ((transfer_size = DoSCSIRead(track_1_lba, SCSIReadMode::SubQOnly)).has_value())
  {
    if (VerifySCSIReadData(std::span<u8>(m_buffer.data(), transfer_size.value()), SCSIReadMode::SubQOnly))
    {
      Log_VerbosePrint("Using SCSI reads with subq only");
      m_scsi_read_mode = SCSIReadMode::SubQOnly;
      return true;
    }
  }

  Log_WarningPrint("SCSI subcode reads failed, trying CDROMREADRAW...");
  if (DoRawRead(track_1_lba))
  {
    Log_WarningPrint("Using CDROMREADRAW, libcrypt games will not run correctly");
    m_scsi_read_mode = SCSIReadMode::None;
    return true;
  }

  // As a last ditch effort, try SCSI without subcode.
  Log_WarningPrint("CDROMREADRAW failed, trying SCSI without subcode...");
  if ((transfer_size = DoSCSIRead(track_1_lba, SCSIReadMode::Raw)).has_value())
  {
    if (VerifySCSIReadData(std::span<u8>(m_buffer.data(), transfer_size.value()), SCSIReadMode::Raw))
    {
      Log_WarningPrint("Using SCSI raw reads, libcrypt games will not run correctly");
      m_scsi_read_mode = SCSIReadMode::Raw;
      return true;
    }
  }

  Log_ErrorPrint("No read modes were successful, cannot use device.");
  return false;
}

std::unique_ptr<CDImage> CDImage::OpenDeviceImage(const char* filename, Error* error)
{
  std::unique_ptr<CDImageDeviceLinux> image = std::make_unique<CDImageDeviceLinux>();
  if (!image->Open(filename, error))
    return {};

  return image;
}

std::vector<std::pair<std::string, std::string>> CDImage::GetDeviceList()
{
  std::vector<std::pair<std::string, std::string>> ret;

  // borrowed from PCSX2
  udev* udev_context = udev_new();
  if (udev_context)
  {
    udev_enumerate* enumerate = udev_enumerate_new(udev_context);
    if (enumerate)
    {
      udev_enumerate_add_match_subsystem(enumerate, "block");
      udev_enumerate_add_match_property(enumerate, "ID_CDROM", "1");
      udev_enumerate_scan_devices(enumerate);
      udev_list_entry* devices = udev_enumerate_get_list_entry(enumerate);

      udev_list_entry* dev_list_entry;
      udev_list_entry_foreach(dev_list_entry, devices)
      {
        const char* path = udev_list_entry_get_name(dev_list_entry);
        udev_device* device = udev_device_new_from_syspath(udev_context, path);
        const char* devnode = udev_device_get_devnode(device);
        if (devnode)
          ret.emplace_back(devnode, devnode);
        udev_device_unref(device);
      }
      udev_enumerate_unref(enumerate);
    }
    udev_unref(udev_context);
  }

  return ret;
}

bool CDImage::IsDeviceName(const char* filename)
{
  if (!std::string_view(filename).starts_with("/dev"))
    return false;

  const int fd = open(filename, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    return false;

  const bool is_cdrom = (ioctl(fd, CDROM_GET_CAPABILITY, 0) >= 0);
  close(fd);
  return is_cdrom;
}

#else

std::unique_ptr<CDImage> CDImage::OpenDeviceImage(const char* filename, Error* error)
{
  return {};
}

std::vector<std::pair<std::string, std::string>> CDImage::GetDeviceList()
{
  return {};
}

bool CDImage::IsDeviceName(const char* filename)
{
  return false;
}

#endif
