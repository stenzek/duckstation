// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_types.h"
#include "types.h"

#include "common/bitfield.h"
#include "common/file_system.h"

#include <memory>

// Implements the specification from https://github.com/ps1dev/standards/blob/main/GPUDUMP.md

class Error;

namespace GPUDump {

enum class GPUVersion : u8
{
  V1_1MB_VRAM,
  V2_1MB_VRAM,
  V2_2MB_VRAM,
};

enum class PacketType : u8
{
  GPUPort0Data = 0x00,
  GPUPort1Data = 0x01,
  VSyncEvent = 0x02,
  DiscardPort0Data = 0x03,
  ReadbackPort0Data = 0x04,
  TraceBegin = 0x05,
  GPUVersion = 0x06,
  GameID = 0x10,
  TextualVideoFormat = 0x11,
  Comment = 0x12,
};

inline constexpr u32 MAX_PACKET_LENGTH = ((1u << 24) - 1); // 3 bytes for packet size

union PacketHeader
{
  // Length0,Length1,Length2,Type
  BitField<u32, u32, 0, 24> length;
  BitField<u32, PacketType, 24, 8> type;
  u32 bits;
};
static_assert(sizeof(PacketHeader) == 4);

class Recorder
{
public:
  ~Recorder();

  static std::unique_ptr<Recorder> Create(std::string path, std::string_view serial, u32 num_frames, Error* error);

  /// Compresses an already-created dump.
  static bool Compress(const std::string& source_path, GPUDumpCompressionMode mode, Error* error);

  ALWAYS_INLINE const std::string& GetPath() const { return m_path; }

  /// Returns true if the caller should stop recording data.
  bool IsFinished();

  bool Close(Error* error);

  void BeginPacket(PacketType packet, u32 minimum_size = 0);
  ALWAYS_INLINE void WriteWord(u32 word) { m_packet_buffer.push_back(word); }
  void WriteWords(const u32* words, size_t word_count);
  void WriteWords(const std::span<const u32> words);
  void WriteString(std::string_view str);
  void WriteBytes(const void* data, size_t data_size_in_bytes);
  void EndPacket();

  void WriteGP1Command(GP1Command command, u32 param);

  void BeginGP0Packet(u32 size);
  void WriteGP0Packet(u32 word);
  void EndGP0Packet();
  void WriteGP1Packet(u32 value);

  void WriteDiscardVRAMRead(u32 width, u32 height);
  void WriteVSync(u64 ticks);

private:
  Recorder(FileSystem::AtomicRenamedFile fp, u32 vsyncs_remaining, std::string path);

  void WriteHeaders(std::string_view serial);
  void WriteCurrentVRAM();

  FileSystem::AtomicRenamedFile m_fp;
  std::vector<u32> m_packet_buffer;
  u32 m_vsyncs_remaining = 0;
  PacketType m_current_packet = PacketType::Comment;
  bool m_write_error = false;

  std::string m_path;
};

class Player
{
public:
  ~Player();

  ALWAYS_INLINE const std::string& GetPath() const { return m_path; }
  ALWAYS_INLINE const std::string& GetSerial() const { return m_serial; }
  ALWAYS_INLINE ConsoleRegion GetRegion() const { return m_region; }
  ALWAYS_INLINE size_t GetFrameCount() const { return m_frame_offsets.size(); }

  static std::unique_ptr<Player> Open(std::string path, Error* error);

  void Execute();

private:
  Player(std::string path, DynamicHeapArray<u8> data);

  struct PacketRef
  {
    PacketType type;
    std::span<const u32> data;

    std::string_view GetNullTerminatedString() const;
  };

  std::optional<PacketRef> GetNextPacket();

  bool Preprocess(Error* error);
  bool ProcessHeader(Error* error);
  bool FindFrameStarts(Error* error);

  void ProcessPacket(const PacketRef& pkt);

  DynamicHeapArray<u8> m_data;
  size_t m_start_offset = 0;
  size_t m_position = 0;

  std::string m_path;
  std::string m_serial;
  ConsoleRegion m_region = ConsoleRegion::NTSC_U;
  std::vector<size_t> m_frame_offsets;
};

} // namespace GPUDump
