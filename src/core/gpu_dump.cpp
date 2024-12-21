// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_dump.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "gpu.h"
#include "settings.h"

#include "scmversion/scmversion.h"

#include "util/compress_helpers.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/binary_reader_writer.h"
#include "common/error.h"
#include "common/fastjmp.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "fmt/format.h"

LOG_CHANNEL(GPUDump);

namespace GPUDump {
static constexpr GPUVersion GPU_VERSION = GPUVersion::V2_1MB_VRAM;

// Write the file header.
static constexpr u8 FILE_HEADER[] = {'P', 'S', 'X', 'G', 'P', 'U', 'D', 'U', 'M', 'P', 'v', '1', '\0', '\0'};

}; // namespace GPUDump

GPUDump::Recorder::Recorder(FileSystem::AtomicRenamedFile fp, u32 vsyncs_remaining, std::string path)
  : m_fp(std::move(fp)), m_vsyncs_remaining(vsyncs_remaining), m_path(path)
{
}

GPUDump::Recorder::~Recorder()
{
  if (m_fp)
    FileSystem::DiscardAtomicRenamedFile(m_fp);
}

bool GPUDump::Recorder::IsFinished()
{
  if (m_vsyncs_remaining == 0)
    return false;

  m_vsyncs_remaining--;
  return (m_vsyncs_remaining == 0);
}

bool GPUDump::Recorder::Close(Error* error)
{
  if (m_write_error)
  {
    Error::SetStringView(error, "Previous write error occurred.");
    return false;
  }

  return FileSystem::CommitAtomicRenamedFile(m_fp, error);
}

std::unique_ptr<GPUDump::Recorder> GPUDump::Recorder::Create(std::string path, std::string_view serial, u32 num_frames,
                                                             Error* error)
{
  std::unique_ptr<Recorder> ret;

  auto fp = FileSystem::CreateAtomicRenamedFile(path, error);
  if (!fp)
    return ret;

  ret = std::unique_ptr<Recorder>(new Recorder(std::move(fp), num_frames, std::move(path)));
  ret->WriteHeaders(serial);
  g_gpu.WriteCurrentVideoModeToDump(ret.get());
  ret->WriteCurrentVRAM();

  // Write start of stream.
  ret->BeginPacket(PacketType::TraceBegin);
  ret->EndPacket();

  if (ret->m_write_error)
  {
    Error::SetStringView(error, "Previous write error occurred.");
    ret.reset();
  }

  return ret;
}

bool GPUDump::Recorder::Compress(const std::string& source_path, GPUDumpCompressionMode mode, Error* error)
{
  if (mode == GPUDumpCompressionMode::Disabled)
    return true;

  std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(source_path.c_str(), error);
  if (!data)
    return false;

  if (mode >= GPUDumpCompressionMode::ZstLow && mode <= GPUDumpCompressionMode::ZstHigh)
  {
    const int clevel =
      ((mode == GPUDumpCompressionMode::ZstLow) ? 1 : ((mode == GPUDumpCompressionMode::ZstHigh) ? 19 : 0));
    if (!CompressHelpers::CompressToFile(fmt::format("{}.zst", source_path).c_str(), std::move(data.value()), clevel,
                                         true, error))
    {
      return false;
    }
  }
  else if (mode >= GPUDumpCompressionMode::XZLow && mode <= GPUDumpCompressionMode::XZHigh)
  {
    const int clevel =
      ((mode == GPUDumpCompressionMode::XZLow) ? 3 : ((mode == GPUDumpCompressionMode::ZstHigh) ? 9 : 5));
    if (!CompressHelpers::CompressToFile(fmt::format("{}.xz", source_path).c_str(), std::move(data.value()), clevel,
                                         true, error))
    {
      return false;
    }
  }
  else
  {
    Error::SetStringView(error, "Unknown compression mode.");
    return false;
  }

  // remove original file
  return FileSystem::DeleteFile(source_path.c_str(), error);
}

void GPUDump::Recorder::BeginGP0Packet(u32 size)
{
  BeginPacket(PacketType::GPUPort0Data, size);
}

void GPUDump::Recorder::WriteGP0Packet(u32 word)
{
  BeginGP0Packet(1);
  WriteWord(word);
  EndGP0Packet();
}

void GPUDump::Recorder::EndGP0Packet()
{
  DebugAssert(!m_packet_buffer.empty());
  EndPacket();
}

void GPUDump::Recorder::WriteGP1Packet(u32 value)
{
  const u32 command = (value >> 24) & 0x3F;

  // only in-range commands, no info
  if (command > static_cast<u32>(GP1Command::SetAllowTextureDisable))
    return;

  // filter DMA direction, we don't want to screw with that
  if (command == static_cast<u32>(GP1Command::SetDMADirection))
    return;

  WriteGP1Command(static_cast<GP1Command>(command), value & 0x00FFFFFFu);
}

void GPUDump::Recorder::WriteDiscardVRAMRead(u32 width, u32 height)
{
  const u32 num_words = Common::AlignUpPow2(width * height * static_cast<u32>(sizeof(u16)), sizeof(u32)) / sizeof(u32);
  if (num_words == 0)
    return;

  BeginPacket(GPUDump::PacketType::DiscardPort0Data, 1);
  WriteWord(num_words);
  EndPacket();
}

void GPUDump::Recorder::WriteVSync(u64 ticks)
{
  BeginPacket(GPUDump::PacketType::VSyncEvent, 2);
  WriteWord(static_cast<u32>(ticks));
  WriteWord(static_cast<u32>(ticks >> 32));
  EndPacket();
}

void GPUDump::Recorder::BeginPacket(PacketType packet, u32 minimum_size)
{
  DebugAssert(m_packet_buffer.empty());
  m_current_packet = packet;
  m_packet_buffer.reserve(minimum_size);
}

void GPUDump::Recorder::WriteWords(const u32* words, size_t word_count)
{
  Assert(((m_packet_buffer.size() + word_count) * sizeof(u32)) <= MAX_PACKET_LENGTH);

  // we don't need the zeroing here...
  const size_t current_offset = m_packet_buffer.size();
  m_packet_buffer.resize(current_offset + word_count);
  std::memcpy(&m_packet_buffer[current_offset], words, sizeof(u32) * word_count);
}

void GPUDump::Recorder::WriteWords(const std::span<const u32> words)
{
  WriteWords(words.data(), words.size());
}

void GPUDump::Recorder::WriteString(std::string_view str)
{
  const size_t aligned_length = Common::AlignDownPow2(str.length(), sizeof(u32));
  for (size_t i = 0; i < aligned_length; i += sizeof(u32))
  {
    u32 word;
    std::memcpy(&word, &str[i], sizeof(word));
    WriteWord(word);
  }

  // zero termination and/or padding for last bytes
  u8 pad_word[4] = {};
  for (size_t i = aligned_length, pad_i = 0; i < str.length(); i++, pad_i++)
    pad_word[pad_i] = str[i];

  WriteWord(std::bit_cast<u32>(pad_word));
}

void GPUDump::Recorder::WriteBytes(const void* data, size_t data_size_in_bytes)
{
  Assert(((m_packet_buffer.size() * sizeof(u32)) + data_size_in_bytes) <= MAX_PACKET_LENGTH);
  const u32 num_words = Common::AlignUpPow2(static_cast<u32>(data_size_in_bytes), sizeof(u32)) / sizeof(u32);
  const size_t current_offset = m_packet_buffer.size();

  // NOTE: assumes resize() zeros it out
  m_packet_buffer.resize(current_offset + num_words);
  std::memcpy(&m_packet_buffer[current_offset], data, data_size_in_bytes);
}

void GPUDump::Recorder::EndPacket()
{
  if (m_write_error)
    return;

  Assert(m_packet_buffer.size() <= MAX_PACKET_LENGTH);

  PacketHeader hdr = {};
  hdr.length = static_cast<u32>(m_packet_buffer.size());
  hdr.type = m_current_packet;
  if (std::fwrite(&hdr, sizeof(hdr), 1, m_fp.get()) != 1 ||
      (!m_packet_buffer.empty() &&
       std::fwrite(m_packet_buffer.data(), m_packet_buffer.size() * sizeof(u32), 1, m_fp.get()) != 1))
  {
    ERROR_LOG("Failed to write packet to file: {}", Error::CreateErrno(errno).GetDescription());
    m_write_error = true;
    m_packet_buffer.clear();
    return;
  }

  m_packet_buffer.clear();
}

void GPUDump::Recorder::WriteGP1Command(GP1Command command, u32 param)
{
  BeginPacket(PacketType::GPUPort1Data, 1);
  WriteWord(((static_cast<u32>(command) & 0x3F) << 24) | (param & 0x00FFFFFFu));
  EndPacket();
}

void GPUDump::Recorder::WriteHeaders(std::string_view serial)
{
  if (std::fwrite(FILE_HEADER, sizeof(FILE_HEADER), 1, m_fp.get()) != 1)
  {
    ERROR_LOG("Failed to write file header: {}", Error::CreateErrno(errno).GetDescription());
    m_write_error = true;
    return;
  }

  // Write GPU version.
  BeginPacket(PacketType::GPUVersion, 1);
  WriteWord(static_cast<u32>(GPU_VERSION));
  EndPacket();

  // Write Game ID.
  BeginPacket(PacketType::GameID);
  WriteString(serial.empty() ? std::string_view("UNKNOWN") : serial);
  EndPacket();

  // Write textual video mode.
  BeginPacket(PacketType::TextualVideoFormat);
  WriteString(g_gpu.IsInPALMode() ? "PAL" : "NTSC");
  EndPacket();

  // Write DuckStation version.
  BeginPacket(PacketType::Comment);
  WriteString(
    SmallString::from_format("Created by DuckStation {} for {}/{}.", g_scm_tag_str, TARGET_OS_STR, CPU_ARCH_STR));
  EndPacket();
}

void GPUDump::Recorder::WriteCurrentVRAM()
{
  BeginPacket(PacketType::GPUPort0Data, sizeof(u32) * 2 + (VRAM_SIZE / sizeof(u32)));

  // command, coords, size. size is written as zero, for 1024x512
  WriteWord(0xA0u << 24);
  WriteWord(0);
  WriteWord(0);

  // actual vram data
  WriteBytes(g_vram, VRAM_SIZE);

  EndPacket();
}

GPUDump::Player::Player(std::string path, DynamicHeapArray<u8> data) : m_data(std::move(data)), m_path(std::move(path))
{
}

GPUDump::Player::~Player() = default;

std::unique_ptr<GPUDump::Player> GPUDump::Player::Open(std::string path, Error* error)
{
  std::unique_ptr<Player> ret;

  Timer timer;

  std::optional<DynamicHeapArray<u8>> data;
  if (StringUtil::EndsWithNoCase(path, ".psxgpu.zst") || StringUtil::EndsWithNoCase(path, ".psxgpu.xz"))
    data = CompressHelpers::DecompressFile(path.c_str(), std::nullopt, error);
  else
    data = FileSystem::ReadBinaryFile(path.c_str(), error);
  if (!data.has_value())
    return ret;

  ret = std::unique_ptr<Player>(new Player(std::move(path), std::move(data.value())));
  if (!ret->Preprocess(error))
  {
    ret.reset();
    return ret;
  }

  INFO_LOG("Loading {} took {:.0f}ms.", Path::GetFileName(ret->GetPath()), timer.GetTimeMilliseconds());
  return ret;
}

std::optional<GPUDump::Player::PacketRef> GPUDump::Player::GetNextPacket()
{
  std::optional<PacketRef> ret;

  if (m_position >= m_data.size())
    return ret;

  size_t new_position = m_position;

  PacketHeader hdr;
  std::memcpy(&hdr, &m_data[new_position], sizeof(hdr));
  new_position += sizeof(hdr);

  if ((new_position + (hdr.length * sizeof(u32))) > m_data.size())
    return ret;

  ret = PacketRef{.type = hdr.type,
                  .data = (hdr.length > 0) ?
                            std::span<const u32>(reinterpret_cast<const u32*>(&m_data[new_position]), hdr.length) :
                            std::span<const u32>()};
  new_position += (hdr.length * sizeof(u32));
  m_position = new_position;
  return ret;
}

std::string_view GPUDump::Player::PacketRef::GetNullTerminatedString() const
{
  return data.empty() ?
           std::string_view() :
           std::string_view(reinterpret_cast<const char*>(data.data()),
                            StringUtil::Strnlen(reinterpret_cast<const char*>(data.data()), data.size_bytes()));
}

bool GPUDump::Player::Preprocess(Error* error)
{
  if (!ProcessHeader(error))
  {
    Error::AddPrefix(error, "Failed to process header: ");
    return false;
  }

  m_position = m_start_offset;

  if (!FindFrameStarts(error))
  {
    Error::AddPrefix(error, "Failed to process header: ");
    return false;
  }

  m_position = m_start_offset;
  return true;
}

bool GPUDump::Player::ProcessHeader(Error* error)
{
  if (m_data.size() < sizeof(FILE_HEADER) || std::memcmp(m_data.data(), FILE_HEADER, sizeof(FILE_HEADER)) != 0)
  {
    Error::SetStringView(error, "File does not have the correct header.");
    return false;
  }

  m_start_offset = sizeof(FILE_HEADER);
  m_position = m_start_offset;

  for (;;)
  {
    const std::optional<PacketRef> packet = GetNextPacket();
    if (!packet.has_value())
    {
      Error::SetStringView(error, "EOF reached before reaching trace begin.");
      return false;
    }

    switch (packet->type)
    {
      case PacketType::TextualVideoFormat:
      {
        const std::string_view region_str = packet->GetNullTerminatedString();
        DEV_LOG("Dump video format: {}", region_str);
        if (StringUtil::EqualNoCase(region_str, "NTSC"))
          m_region = ConsoleRegion::NTSC_U;
        else if (StringUtil::EqualNoCase(region_str, "PAL"))
          m_region = ConsoleRegion::PAL;
        else
          WARNING_LOG("Unknown console region: {}", region_str);
      }
      break;

      case PacketType::GameID:
      {
        const std::string_view serial = packet->GetNullTerminatedString();
        DEV_LOG("Dump serial: {}", serial);
        m_serial = serial;
      }
      break;

      case PacketType::Comment:
      {
        const std::string_view comment = packet->GetNullTerminatedString();
        DEV_LOG("Dump comment: {}", comment);
      }
      break;

      case PacketType::TraceBegin:
      {
        DEV_LOG("Trace start found at offset {}", m_position);
        return true;
      }

      default:
      {
        // ignore packet
      }
      break;
    }
  }
}

bool GPUDump::Player::FindFrameStarts(Error* error)
{
  for (;;)
  {
    const std::optional<PacketRef> packet = GetNextPacket();
    if (!packet.has_value())
      break;

    switch (packet->type)
    {
      case PacketType::TraceBegin:
      {
        if (!m_frame_offsets.empty())
        {
          Error::SetStringView(error, "VSync or trace begin event found before final trace begin.");
          return false;
        }

        m_frame_offsets.push_back(m_position);
      }
      break;

      case PacketType::VSyncEvent:
      {
        if (m_frame_offsets.empty())
        {
          Error::SetStringView(error, "Trace begin event missing before first VSync.");
          return false;
        }

        m_frame_offsets.push_back(m_position);
      }
      break;

      default:
      {
        // ignore packet
      }
      break;
    }
  }

  if (m_frame_offsets.size() < 2)
  {
    Error::SetStringView(error, "Dump does not contain at least one frame.");
    return false;
  }

#if defined(_DEBUG) || defined(_DEVEL)
  for (size_t i = 0; i < m_frame_offsets.size(); i++)
    DEBUG_LOG("Frame {} starts at offset {}", i, m_frame_offsets[i]);
#endif

  return true;
}

void GPUDump::Player::ProcessPacket(const PacketRef& pkt)
{
  if (pkt.type <= PacketType::VSyncEvent)
  {
    // gp0/gp1/vsync => direct to gpu
    g_gpu.ProcessGPUDumpPacket(pkt.type, pkt.data);
    return;
  }
}

void GPUDump::Player::Execute()
{
  if (fastjmp_set(CPU::GetExecutionJmpBuf()) != 0)
    return;

  for (;;)
  {
    const std::optional<PacketRef> packet = GetNextPacket();
    if (!packet.has_value())
    {
      m_position = g_settings.gpu_dump_fast_replay_mode ? m_frame_offsets.front() : m_start_offset;
      continue;
    }

    ProcessPacket(packet.value());
  }
}
