// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "pio.h"
#include "bus.h"
#include "settings.h"
#include "system.h"
#include "types.h"

#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/bitfield.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/heap_array.h"
#include "common/log.h"
#include "common/path.h"

#include <limits>

LOG_CHANNEL(PIO);

namespace PIO {

// Atmel AT29C040A
class Flash
{
public:
  static constexpr u32 TOTAL_SIZE = 512 * 1024;

  Flash();
  ~Flash();

  ALWAYS_INLINE bool IsImageModified() const { return m_image_modified; }

  bool LoadImage(const char* path, Error* error);
  bool SaveImage(const char* path, Error* error);

  void Reset();
  bool DoState(StateWrapper& sw);

  u8 Read(u32 offset);
  void CodeRead(u32 offset, u32* words, u32 word_count);
  void Write(u32 offset, u8 value);

private:
  static constexpr u32 SECTOR_SIZE = 256;
  static constexpr u32 SECTOR_COUNT = TOTAL_SIZE / SECTOR_SIZE;
  static constexpr TickCount PROGRAM_TIMER_CYCLES = 5080; // ~150us

  enum : u8
  {
    // 3 byte commands
    FLASH_CMD_ENTER_ID_MODE = 0x90,
    FLASH_CMD_EXIT_ID_MODE = 0xF0,
    FLASH_CMD_WRITE_SECTOR_WITH_SDP = 0xA0,
    FLASH_CMD_BEGIN_5_BYTE_COMMAND = 0x80,

    // 5 byte commands
    FLASH_CMD_WRITE_SECTOR_WITHOUT_SDP = 0x20,
    FLASH_CMD_ALT_ENTER_ID_MODE = 0x60,
  };

  u8* SectorPtr(u32 sector);
  void PushCommandByte(u8 value);

  void ProgramWrite(u32 offset, u8 value);
  bool CheckForProgramTimeout();
  void EndProgramming();

  DynamicHeapArray<u8> m_data;
  GlobalTicks m_program_write_timeout = 0;
  std::array<u8, 6> m_command_buffer = {};
  bool m_flash_id_mode = false;
  bool m_write_enable = false;
  bool m_image_modified = false;
  u8 m_write_toggle_result = 0;
  u16 m_write_position = 0;
  u32 m_sector_address = 0;
  u32 m_max_data_address = 0;
};

} // namespace PIO

PIO::Flash::Flash() = default;

PIO::Flash::~Flash() = default;

u8 PIO::Flash::Read(u32 offset)
{
  if (m_flash_id_mode) [[unlikely]]
  {
    // Atmel AT29C040A
    static constexpr std::array<u8, 2> flash_id = {0x1F, 0xA4};
    return flash_id[offset & 1];
  }

  // WARNING_LOG("FLASH READ 0x{:X} 0x{:X} @ {}", offset, g_exp1_rom[offset], System::GetGlobalTickCounter());

  if (m_write_enable && !CheckForProgramTimeout()) [[unlikely]]
  {
    m_write_toggle_result ^= 0x40;
    WARNING_LOG("read while programming 0x{:02X}", m_write_toggle_result);
    EndProgramming();
    return m_write_toggle_result | 0x80;
  }

  return (offset >= TOTAL_SIZE) ? 0xFFu : m_data[offset];
}

void PIO::Flash::CodeRead(u32 offset, u32* words, u32 word_count)
{
  DebugAssert((offset + (word_count * sizeof(u32))) < TOTAL_SIZE);
  std::memcpy(words, m_data.data() + offset, word_count * sizeof(u32));
}

void PIO::Flash::PushCommandByte(u8 value)
{
  for (u32 i = static_cast<u32>(std::size(m_command_buffer) - 1); i > 0; i--)
    m_command_buffer[i] = m_command_buffer[i - 1];
  m_command_buffer[0] = value;
}

void PIO::Flash::Write(u32 offset, u8 value)
{
  if (m_write_enable && !CheckForProgramTimeout())
  {
    ProgramWrite(offset, value);
    return;
  }

  DEV_LOG("FLASH WRITE 0x{:X} 0x{:X}", offset, value);

  // Ignore banked addresses
  offset &= 0x3FFFF;
  if (offset == 0x2AAA || offset == 0x5555)
  {
    PushCommandByte(value);

    const auto& buf = m_command_buffer;
    if (buf[2] == 0xAA && buf[1] == 0x55)
    {
      if (value == FLASH_CMD_ENTER_ID_MODE)
      {
        DEV_LOG("Flash enter ID mode");
        m_flash_id_mode = true;
      }
      else if (value == FLASH_CMD_EXIT_ID_MODE)
      {
        DEV_LOG("Flash exit ID mode");
        m_flash_id_mode = false;
      }
      else if (value == FLASH_CMD_WRITE_SECTOR_WITH_SDP)
      {
        DEV_LOG("Flash write sector with SDP @ {}", System::GetGlobalTickCounter());
        m_write_enable = true;
        m_program_write_timeout = System::GetGlobalTickCounter() + PROGRAM_TIMER_CYCLES;
      }
      else if (buf[5] == 0xAA && buf[4] == 0x55 && buf[3] == 0x80)
      {
        if (value == FLASH_CMD_ALT_ENTER_ID_MODE)
        {
          DEV_LOG("Flash Alt Enter ID mode");
          m_flash_id_mode = true;
        }
        if (value == FLASH_CMD_WRITE_SECTOR_WITHOUT_SDP)
        {
          DEV_LOG("Flash Write sector WITHOUT SDP");
          m_write_enable = true;
          m_program_write_timeout = std::numeric_limits<GlobalTicks>::max();
        }
        else
        {
          ERROR_LOG("Unhandled 5-cycle flash command 0x{:02X}", value);
        }
      }
      else if (value != 0x80)
      {
        ERROR_LOG("Unhandled 3-cycle flash command 0x{:02X}", value);
      }
    }
  }
}

void PIO::Flash::ProgramWrite(u32 offset, u8 value)
{
  // reset the timeout.. event system suckage, we need it from _this_ cycle, not the first
  m_program_write_timeout = std::max(m_program_write_timeout, System::GetGlobalTickCounter() + PROGRAM_TIMER_CYCLES);

  static_assert((0x800 * 0x100) == TOTAL_SIZE);
  const u32 byte_address = (offset & 0xFFu);
  const u32 sector_address = (offset >> 8) & 0x7FFu;
  if (m_write_position == 0)
  {
    DEV_LOG("Writing to flash sector {} (offset 0x{:06X})", sector_address, sector_address * SECTOR_SIZE);
    m_sector_address = sector_address;

    const u32 sector_data_end = (sector_address * SECTOR_SIZE) + SECTOR_SIZE;
    if (sector_data_end > m_max_data_address)
    {
      m_max_data_address = sector_data_end;
      m_image_modified = true;
    }
  }

  if (sector_address == m_sector_address) [[likely]]
  {
    u8* byte_ptr = SectorPtr(sector_address) + byte_address;
    m_image_modified |= (*byte_ptr != value);
    *byte_ptr = value;
  }
  else
  {
    WARNING_LOG("Flash write: unexpected sector address of {}, expected {} (addr 0x{:05X}", sector_address,
                m_sector_address, offset);
  }

  m_write_position++;
  if (m_write_position == SECTOR_SIZE)
  {
    // end of flash write
    EndProgramming();
  }
}

bool PIO::Flash::CheckForProgramTimeout()
{
  DebugAssert(m_write_enable);
  if (System::GetGlobalTickCounter() < m_program_write_timeout)
    return false;

  WARNING_LOG("Flash program timeout at byte {}", m_write_position);

  // kinda cheating here, the sector would normally get buffered and then written
  // but the flash isn't supposed to be readable during programming anyway...
  if (m_write_position > 0)
  {
    const u32 bytes_to_erase = SECTOR_SIZE - m_write_position;
    if (bytes_to_erase > 0)
    {
      WARNING_LOG("Erasing {} unwritten bytes in sector {} (0x{:05X})", bytes_to_erase, m_write_position,
                  m_sector_address * SECTOR_SIZE);

      u8* sector = SectorPtr(m_sector_address) + m_write_position;
      bool image_modified = false;
      for (u32 i = 0; i < bytes_to_erase; i++)
      {
        image_modified |= (sector[i] != 0xFF);
        sector[i] = 0xFF;
      }
      m_image_modified |= image_modified;
    }
  }
  else
  {
    WARNING_LOG("No sector address set, skipping programming.");
  }

  EndProgramming();
  return true;
}

void PIO::Flash::EndProgramming()
{
  m_write_enable = false;
  m_write_position = 0;
  m_sector_address = 0;
  m_write_toggle_result = 0;
}

u8* PIO::Flash::SectorPtr(u32 sector)
{
  DebugAssert(sector < SECTOR_COUNT);
  return (m_data.data() + sector * SECTOR_SIZE);
}

bool PIO::Flash::LoadImage(const char* path, Error* error)
{
  const FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(path, "rb", error);
  if (!fp)
  {
    Error::AddPrefixFmt(error, "Failed to open PIO flash image '{}': ", Path::GetFileName(path));
    return false;
  }

  const s64 file_size = std::max<s64>(FileSystem::FSize64(fp.get(), error), 0);
  if (file_size > TOTAL_SIZE)
  {
    WARNING_LOG("PIO flash image is too large ({} bytes), only {} bytes will be read.", file_size, TOTAL_SIZE);
  }
  else if (file_size < TOTAL_SIZE)
  {
    DEV_LOG("PIO flash image is too small ({} bytes), {} bytes of padding will be added.", file_size,
            TOTAL_SIZE - file_size);
  }

  const u32 read_size = static_cast<u32>(std::min<s64>(file_size, TOTAL_SIZE));
  m_data.resize(TOTAL_SIZE);
  if (read_size > 0 && std::fread(m_data.data(), read_size, 1, fp.get()) != 1)
  {
    Error::SetErrno(error, "Failed to read PIO flash image: ", errno);
    m_data.deallocate();
    return false;
  }

  const u32 padding_size = TOTAL_SIZE - read_size;
  if (padding_size > 0)
    std::memset(m_data.data() + read_size, 0, padding_size);

  m_max_data_address = read_size;
  return true;
}

bool PIO::Flash::SaveImage(const char* path, Error* error)
{
  WARNING_LOG("Writing PIO flash image '{}'", Path::GetFileName(path));

  if (!FileSystem::WriteBinaryFile(path, m_data.cspan(0, m_max_data_address), error))
  {
    Error::AddPrefixFmt(error, "Failed to write PIO flash image '{}': ", Path::GetFileName(path));
    return false;
  }

  return true;
}

void PIO::Flash::Reset()
{
  m_command_buffer.fill(0);
  m_flash_id_mode = false;
  m_write_enable = false;
  m_write_position = 0;
  m_sector_address = 0;
  m_program_write_timeout = 0;
  m_write_toggle_result = 0;
}

bool PIO::Flash::DoState(StateWrapper& sw)
{
  sw.DoBytes(m_data.data(), m_data.size());
  sw.DoBytes(m_command_buffer.data(), m_command_buffer.size());
  sw.Do(&m_flash_id_mode);
  sw.Do(&m_write_enable);
  sw.Do(&m_write_position);
  sw.Do(&m_sector_address);
  sw.Do(&m_program_write_timeout);
  sw.Do(&m_write_toggle_result);
  sw.Do(&m_image_modified);
  sw.Do(&m_max_data_address);
  return !sw.HasError();
}

namespace PIO {

class NullDevice : public Device
{
public:
  NullDevice();
  ~NullDevice() override;

  bool Initialize(Error* error) override;

  void Reset() override;
  void UpdateSettings(const Settings& old_settings) override;
  bool DoState(StateWrapper& sw) override;

  u8 ReadHandler(u32 offset) override;
  void CodeReadHandler(u32 offset, u32* words, u32 word_count) override;
  void WriteHandler(u32 offset, u8 value) override;
};

} // namespace PIO

PIO::NullDevice::NullDevice() = default;

PIO::NullDevice::~NullDevice() = default;

bool PIO::NullDevice::Initialize(Error* error)
{
  return true;
}

void PIO::NullDevice::Reset()
{
}

void PIO::NullDevice::UpdateSettings(const Settings& old_settings)
{
}

bool PIO::NullDevice::DoState(StateWrapper& sw)
{
  return true;
}

u8 PIO::NullDevice::ReadHandler(u32 offset)
{
  return 0xFFu;
}

void PIO::NullDevice::CodeReadHandler(u32 offset, u32* words, u32 word_count)
{
  std::memset(words, 0xFF, sizeof(u32) * word_count);
}

void PIO::NullDevice::WriteHandler(u32 offset, u8 value)
{
}

#if 0

namespace PIO {

namespace {

class DatelCartDevice : public Device
{
public:
  DatelCartDevice();
  ~DatelCartDevice() override;

  bool Initialize(Error* error) override;

  void Reset() override;
  void UpdateSettings(const Settings& old_settings) override;
  bool DoState(StateWrapper& sw) override;

  u8 ReadHandler(u32 offset) override;
  void CodeReadHandler(u32 offset, u32* words, u32 word_count) override;
  void WriteHandler(u32 offset, u8 value) override;

private:
  Flash m_flash;
};

}

} // namespace PIO

PIO::DatelCartDevice::DatelCartDevice() = default;

PIO::DatelCartDevice::~DatelCartDevice() = default;

bool PIO::DatelCartDevice::Initialize(Error* error)
{
  return false;
}

void PIO::DatelCartDevice::Reset()
{
}

void PIO::DatelCartDevice::UpdateSettings(const Settings& old_settings)
{
}

bool PIO::DatelCartDevice::DoState(StateWrapper& sw)
{
  return false;
}

u8 PIO::DatelCartDevice::ReadHandler(u32 offset)
{
  WARNING_LOG("Datel EXP1 read 0x{:08X}", offset);

  if (offset < 0x20000)
  {
    // first 128KB of flash
    return m_flash.Read(offset);
  }
  else if (offset >= 0x40000 && offset < 0x60000) // 1F040000->1F05FFFF
  {
    // second 128KB of flash
    return m_flash.Read((offset - 0x40000) + 0x20000);
  }
  else if (offset == 0x20018)
  {
    // switch setting
    return 1u;
  }
  else if (offset == 0x20010)
  {
    // comms link STB pin state (bit 0)
    return 0u;
  }
  else if (offset == 0x60000)
  {
    // comms link data in
    return 0u;
  }
  else
  {
    WARNING_LOG("Unhandled Datel EXP1 read: 0x{:08X}", offset);
    return 0xFFu;
  }
}

void PIO::DatelCartDevice::CodeReadHandler(u32 offset, u32* words, u32 word_count)
{
  if (offset < 0x20000)
    m_flash.CodeRead(offset, words, word_count);
  else if (offset >= 0x40000 && offset < 0x60000) // 1F040000->1F05FFFF
    m_flash.CodeRead((offset - 0x40000) + 0x20000, words, word_count);
  else
    std::memset(words, 0xFF, sizeof(u32) * word_count);
}

void PIO::DatelCartDevice::WriteHandler(u32 offset, u8 value)
{
  WARNING_LOG("DATEL WRITE 0x{:08X} 0x{:08X}", offset, value);
}

#endif

// Xplorer/Xploder
namespace PIO {

namespace {

class XplorerCart : public Device
{
public:
  static constexpr u32 SRAM_SIZE = 128 * 1024;

  XplorerCart();
  ~XplorerCart() override;

  bool Initialize(Error* error) override;
  void UpdateSettings(const Settings& old_settings) override;

  void Reset() override;
  bool DoState(StateWrapper& sw) override;

  u8 ReadHandler(u32 offset) override;
  void CodeReadHandler(u32 offset, u32* words, u32 word_count) override;
  void WriteHandler(u32 offset, u8 value) override;

private:
  ALWAYS_INLINE u32 GetFlashUpperBank() const { return m_memory_map.sram_bank ? (384 * 1024) : (256 * 1024); }

  union MemoryMappingRegister
  {
    u8 bits;

    BitField<u8, bool, 0, 1> pc_slct;
    BitField<u8, bool, 1, 1> pc_pe;
    BitField<u8, bool, 2, 1> pc_busy;
    BitField<u8, bool, 3, 1> pc_ack;
    BitField<u8, bool, 4, 1> sram_select;
    BitField<u8, bool, 5, 1> flash_bank;
    BitField<u8, bool, 6, 1> sram_bank;
    BitField<u8, bool, 7, 1> sram_bank_2;
  };

  DynamicHeapArray<u8> m_sram;
  Flash m_flash;
  MemoryMappingRegister m_memory_map = {};
  bool m_switch_state = false;
};

} // namespace

} // namespace PIO

PIO::XplorerCart::XplorerCart()
{
  m_sram.resize(SRAM_SIZE);
}

PIO::XplorerCart::~XplorerCart()
{
  if (g_settings.pio_flash_write_enable && m_flash.IsImageModified())
  {
    Error error;
    if (!m_flash.SaveImage(g_settings.pio_flash_image_path.c_str(), &error))
      ERROR_LOG("Failed to update Xplorer flash image: {}", error.GetDescription());
  }
}

bool PIO::XplorerCart::Initialize(Error* error)
{
  if (!m_flash.LoadImage(g_settings.pio_flash_image_path.c_str(), error))
    return false;

  m_switch_state = g_settings.pio_switch_active;
  return true;
}

void PIO::XplorerCart::UpdateSettings(const Settings& old_settings)
{
  m_switch_state = g_settings.pio_switch_active;
}

void PIO::XplorerCart::Reset()
{
  m_flash.Reset();
  std::memset(m_sram.data(), 0, m_sram.size());
  m_memory_map.bits = 0;
}

bool PIO::XplorerCart::DoState(StateWrapper& sw)
{
  m_flash.DoState(sw);
  sw.DoBytes(m_sram.data(), m_sram.size());
  sw.Do(&m_memory_map.bits);
  return !sw.HasError();
}

u8 PIO::XplorerCart::ReadHandler(u32 offset)
{
  // WARNING_LOG("Xplorer EXP1 read size {}: 0x{:08X}", 1u << (u32)size, address);

  if (offset < 0x40000) // 1F000000->1F03FFFF
  {
    // first 256KB of flash
    return m_flash.Read(offset);
  }
  else if (offset < 0x60000) // 1F040000->1F05FFFF
  {
    // second 256KB of flash or SRAM
    offset &= 0x3FFFF;
    if (m_memory_map.sram_select)
    {
      DebugAssert(offset < SRAM_SIZE);
      return m_sram[offset];
    }
    else
    {
      return m_flash.Read(offset | GetFlashUpperBank());
    }
  }
  else if (offset >= 0x60000 && offset < 0x70000)
  {
    // I/O, mirrored
    switch (offset & 0x07)
    {
      case 0:
      {
        // switch setting
        return 0xFEu | BoolToUInt8(m_switch_state);
      }

      case 1:
      {
        // data from PC
        return 0u;
      }

      case 2:
      {
        // handshake from PC
        return 0xFEu;
      }

      case 3:
      case 4:
      case 5:
      case 6:
      case 7:
      {
        // unknown
        WARNING_LOG("Unhandled Xplorer I/O register read: 0x{:08X}", offset);
        return 0xFFu;
      }

        DefaultCaseIsUnreachable()
    }
  }
  else
  {
    WARNING_LOG("Unhandled Xplorer EXP1 read: 0x{:08X}", offset);
    return 0xFFu;
  }
}

void PIO::XplorerCart::CodeReadHandler(u32 offset, u32* words, u32 word_count)
{
  if ((offset + word_count) <= 0x40000)
  {
    m_flash.CodeRead(offset, words, word_count);
  }
  else if (offset >= 0x40000 && (offset + word_count) <= 0x60000)
  {
    // second 256KB of flash or SRAM
    const u32 bank_offset = offset - 0x40000;
    if (m_memory_map.sram_select)
    {
      DebugAssert(bank_offset < SRAM_SIZE);
      std::memcpy(words, &m_sram[bank_offset], word_count * sizeof(u32));
    }
    else
    {
      m_flash.CodeRead(bank_offset, words, word_count * sizeof(u32));
    }
  }
  else if (offset < 0x60000) [[unlikely]]
  {
    // partial read of both banks
    if (offset < 0x40000)
    {
      const u32 words_from_first = (0x40000 - offset) / sizeof(u32);
      m_flash.CodeRead(offset, words, words_from_first);
      words += words_from_first;
      word_count -= words_from_first;
      offset += words_from_first * sizeof(u32);
    }

    const u32 words_from_second = std::min(0x60000 - offset, word_count);
    const u32 second_bank_offset = offset - 0x40000;
    if (m_memory_map.sram_bank)
    {
      std::memcpy(words, &m_sram[second_bank_offset], words_from_second * sizeof(u32));
    }
    else
    {
      m_flash.CodeRead(second_bank_offset + GetFlashUpperBank(), words, words_from_second);
    }

    words += words_from_second;
    word_count -= words_from_second;
    if (word_count > 0)
      std::memset(words, 0xFF, sizeof(u32) * word_count);
  }
  else
  {
    std::memset(words, 0xFF, sizeof(u32) * word_count);
  }
}

void PIO::XplorerCart::WriteHandler(u32 offset, u8 value)
{
  if (offset < 0x40000)
  {
    m_flash.Write(offset, value);
  }
  else if (offset < 0x60000)
  {
    const u32 bank_offset = offset - 0x40000;
    if (m_memory_map.sram_bank)
    {
      m_sram[bank_offset] = value;
    }
    else
    {
      const u32 flash_offset = m_memory_map.sram_bank ? (384 * 1024) : (256 * 1024);
      m_flash.Write(flash_offset + bank_offset, value);
    }
  }
  else if (offset == 0x60001)
  {
    DEV_LOG("Memory map <- 0x{:02X}", value);
    m_memory_map.bits = value;
  }
  else
  {
    WARNING_LOG("Unhandled Xplorer WRITE 0x{:08X} 0x{:08X}", offset, value);
  }
}

namespace PIO {

static std::unique_ptr<PIO::Device> CreateDevice(PIODeviceType type);

} // namespace PIO

std::unique_ptr<PIO::Device> g_pio_device;

PIO::Device::~Device() = default;

bool PIO::Initialize(Error* error)
{
  g_pio_device = CreateDevice(g_settings.pio_device_type);
  Assert(g_pio_device);

  if (!g_pio_device->Initialize(error))
  {
    g_pio_device.reset();
    return false;
  }

  return true;
}

void PIO::UpdateSettings(const Settings& old_settings)
{
  if (g_settings.pio_device_type != old_settings.pio_device_type)
  {
    Error error;
    g_pio_device.reset();
    g_pio_device = CreateDevice(g_settings.pio_device_type);
    if (!g_pio_device->Initialize(&error))
    {
      ERROR_LOG("Failed to create new PIO device: {}", error.GetDescription());
      g_pio_device = CreateDevice(PIODeviceType::None);
    }
  }
  else
  {
    g_pio_device->UpdateSettings(old_settings);
  }
}

void PIO::Shutdown()
{
  g_pio_device.reset();
}

std::unique_ptr<PIO::Device> PIO::CreateDevice(PIODeviceType type)
{
  switch (type)
  {
    case PIODeviceType::None:
      return std::make_unique<NullDevice>();

    case PIODeviceType::XplorerCart:
      return std::make_unique<XplorerCart>();

    default:
      return nullptr;
  }
}

void PIO::Reset()
{
  g_pio_device->Reset();
}

bool PIO::DoState(StateWrapper& sw)
{
  PIODeviceType device_type = g_settings.pio_device_type;
  sw.Do(&device_type);

  const size_t pio_state_pos = sw.GetPosition();
  u32 pio_state_size = 0;
  sw.Do(&pio_state_size);

  if (device_type == g_settings.pio_device_type) [[likely]]
  {
    if (!g_pio_device->DoState(sw))
      return false;

    // rewrite size field
    if (sw.IsWriting())
    {
      const size_t new_pos = sw.GetPosition();
      sw.SetPosition(pio_state_pos);
      pio_state_size = static_cast<u32>(new_pos - pio_state_pos);
      sw.Do(&pio_state_size);
      sw.SetPosition(new_pos);
    }
  }
  else
  {
    WARNING_LOG("State contains PIO device {}, expected {}", Settings::GetPIODeviceTypeModeName(device_type),
                Settings::GetPIODeviceTypeModeName(g_settings.pio_device_type));
    g_pio_device->Reset();
    sw.SkipBytes(pio_state_size - sizeof(pio_state_size));
  }

  return !sw.HasError();
}
