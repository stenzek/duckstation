// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "memory_card_image.h"
#include "gpu_types.h"
#include "system.h"

#include "util/shiftjis.h"
#include "util/state_wrapper.h"

#include "common/bitutils.h"
#include "common/byte_stream.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include <algorithm>
#include <cstdio>
#include <optional>

Log_SetChannel(MemoryCard);

namespace MemoryCardImage {
namespace {

#pragma pack(push, 1)

struct DirectoryFrame
{
  enum : u32
  {
    FILE_NAME_LENGTH = 20
  };

  u32 block_allocation_state;
  u32 file_size;
  u16 next_block_number;
  char filename[FILE_NAME_LENGTH + 1];
  u8 zero_pad_1;
  u8 pad_2[95];
  u8 checksum;
};

static_assert(sizeof(DirectoryFrame) == FRAME_SIZE);

struct TitleFrame
{
  char id[2];
  u8 icon_flag;
  u8 unk_block_number;
  u8 title[64];
  u8 pad_1[12];
  u8 pad_2[16];
  u16 icon_palette[16];
};

static_assert(sizeof(TitleFrame) == FRAME_SIZE);

#pragma pack(pop)

} // namespace

static u8 GetChecksum(const u8* frame)
{
  u8 checksum = frame[0];
  for (u32 i = 1; i < FRAME_SIZE - 1; i++)
    checksum ^= frame[i];
  return checksum;
}

static void UpdateChecksum(DirectoryFrame* df)
{
  df->checksum = GetChecksum(reinterpret_cast<u8*>(df));
}

template<typename T>
T* GetFramePtr(DataArray* data, u32 block, u32 frame)
{
  return reinterpret_cast<T*>(data->data() + (block * BLOCK_SIZE) + (frame * FRAME_SIZE));
}

template<typename T>
const T* GetFramePtr(const DataArray& data, u32 block, u32 frame)
{
  return reinterpret_cast<const T*>(&data[(block * BLOCK_SIZE) + (frame * FRAME_SIZE)]);
}

static std::optional<u32> GetNextFreeBlock(const DataArray& data);
static bool ImportCardMCD(DataArray* data, const char* filename, std::vector<u8> file_data, Error* error);
static bool ImportCardGME(DataArray* data, const char* filename, std::vector<u8> file_data, Error* error);
static bool ImportCardVGS(DataArray* data, const char* filename, std::vector<u8> file_data, Error* error);
static bool ImportCardPSX(DataArray* data, const char* filename, std::vector<u8> file_data, Error* error);
static bool ImportSaveWithDirectoryFrame(DataArray* data, const char* filename, const FILESYSTEM_STAT_DATA& sd,
                                         Error* error);
static bool ImportRawSave(DataArray* data, const char* filename, const FILESYSTEM_STAT_DATA& sd, Error* error);
} // namespace MemoryCardImage

bool MemoryCardImage::LoadFromFile(DataArray* data, const char* filename)
{
  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(filename, &sd) || sd.Size != DATA_SIZE)
    return false;

  std::unique_ptr<ByteStream> stream = ByteStream::OpenFile(filename, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
  if (!stream || stream->GetSize() != DATA_SIZE)
    return false;

  const size_t num_read = stream->Read(data->data(), DATA_SIZE);
  if (num_read != DATA_SIZE)
  {
    Log_ErrorFmt("Only read {} of {} sectors from '{}'", num_read / FRAME_SIZE, static_cast<u32>(NUM_FRAMES), filename);
    return false;
  }

  Log_VerboseFmt("Loaded memory card from {}", filename);
  return true;
}

bool MemoryCardImage::SaveToFile(const DataArray& data, const char* filename)
{
  std::unique_ptr<ByteStream> stream =
    ByteStream::OpenFile(filename, BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_TRUNCATE | BYTESTREAM_OPEN_WRITE |
                                     BYTESTREAM_OPEN_ATOMIC_UPDATE | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
  {
    Log_ErrorFmt("Failed to open '{}' for writing.", filename);
    return false;
  }

  if (!stream->Write2(data.data(), DATA_SIZE) || !stream->Commit())
  {
    Log_ErrorFmt("Failed to write sectors to '{}'", filename);
    stream->Discard();
    return false;
  }

  Log_VerboseFmt("Saved memory card to '{}'", filename);
  return true;
}

bool MemoryCardImage::IsValid(const DataArray& data)
{
  // TODO: Check checksum?
  const u8* fptr = GetFramePtr<u8>(data, 0, 0);
  return fptr[0] == 'M' && fptr[1] == 'C';
}

void MemoryCardImage::Format(DataArray* data)
{
  // fill everything with FF
  data->fill(u8(0xFF));

  // header
  {
    u8* fptr = GetFramePtr<u8>(data, 0, 0);
    std::fill_n(fptr, FRAME_SIZE, u8(0));
    fptr[0] = 'M';
    fptr[1] = 'C';
    fptr[0x7F] = GetChecksum(fptr);
  }

  // directory
  for (u32 frame = 1; frame < 16; frame++)
  {
    u8* fptr = GetFramePtr<u8>(data, 0, frame);
    std::fill_n(fptr, FRAME_SIZE, u8(0));
    fptr[0] = 0xA0;                 // free
    fptr[8] = 0xFF;                 // pointer to next file
    fptr[9] = 0xFF;                 // pointer to next file
    fptr[0x7F] = GetChecksum(fptr); // checksum
  }

  // broken sector list
  for (u32 frame = 16; frame < 36; frame++)
  {
    u8* fptr = GetFramePtr<u8>(data, 0, frame);
    std::fill_n(fptr, FRAME_SIZE, u8(0));
    fptr[0] = 0xFF;
    fptr[1] = 0xFF;
    fptr[2] = 0xFF;
    fptr[3] = 0xFF;
    fptr[8] = 0xFF;                 // pointer to next file
    fptr[9] = 0xFF;                 // pointer to next file
    fptr[0x7F] = GetChecksum(fptr); // checksum
  }

  // broken sector replacement data
  for (u32 frame = 36; frame < 56; frame++)
  {
    u8* fptr = GetFramePtr<u8>(data, 0, frame);
    std::fill_n(fptr, FRAME_SIZE, u8(0x00));
  }

  // unused frames
  for (u32 frame = 56; frame < 63; frame++)
  {
    u8* fptr = GetFramePtr<u8>(data, 0, frame);
    std::fill_n(fptr, FRAME_SIZE, u8(0x00));
  }

  // write test frame
  std::memcpy(GetFramePtr<u8>(data, 0, 63), GetFramePtr<u8>(data, 0, 0), FRAME_SIZE);
}

std::optional<u32> MemoryCardImage::GetNextFreeBlock(const DataArray& data)
{
  for (u32 dir_frame = 1; dir_frame < FRAMES_PER_BLOCK; dir_frame++)
  {
    const DirectoryFrame* df = GetFramePtr<DirectoryFrame>(data, 0, dir_frame);
    if ((df->block_allocation_state & 0xF0) == 0xA0)
      return dir_frame;
  }

  return std::nullopt;
}

u32 MemoryCardImage::GetFreeBlockCount(const DataArray& data)
{
  u32 count = 0;
  for (u32 dir_frame = 1; dir_frame < FRAMES_PER_BLOCK; dir_frame++)
  {
    const DirectoryFrame* df = GetFramePtr<DirectoryFrame>(data, 0, dir_frame);
    if ((df->block_allocation_state & 0xF0) == 0xA0)
      count++;
  }

  return count;
}

std::vector<MemoryCardImage::FileInfo> MemoryCardImage::EnumerateFiles(const DataArray& data, bool include_deleted)
{
  // For getting the icon, we only consider binary transparency. Some games set the alpha to 0.
  static constexpr auto icon_to_rgba8 = [](u16 col) { return (col == 0) ? 0u : VRAMRGBA5551ToRGBA8888(col | 0x8000); };

  std::vector<FileInfo> files;

  for (u32 dir_frame = 1; dir_frame < FRAMES_PER_BLOCK; dir_frame++)
  {
    const DirectoryFrame* df = GetFramePtr<DirectoryFrame>(data, 0, dir_frame);
    if (df->block_allocation_state != 0x51 &&
        (!include_deleted || (df->block_allocation_state != 0xA1 && df->block_allocation_state != 0xA2 &&
                              df->block_allocation_state != 0xA3)))
    {
      continue;
    }

    u32 filename_length = 0;
    while (filename_length < sizeof(df->filename) && df->filename[filename_length] != '\0')
      filename_length++;

    FileInfo fi;
    fi.filename.append(df->filename, filename_length);
    fi.first_block = dir_frame;
    fi.size = df->file_size;
    fi.num_blocks = 1;
    fi.deleted = (df->block_allocation_state != 0x51);

    const DirectoryFrame* next_df = df;
    while (next_df->next_block_number < (NUM_BLOCKS - 1) && fi.num_blocks < FRAMES_PER_BLOCK)
    {
      fi.num_blocks++;
      next_df = GetFramePtr<DirectoryFrame>(data, 0, next_df->next_block_number + 1);
    }

    if (fi.num_blocks == FRAMES_PER_BLOCK)
    {
      // invalid
      Log_WarningFmt("Invalid block chain in block {}", dir_frame);
      continue;
    }

    const TitleFrame* tf = GetFramePtr<TitleFrame>(data, dir_frame, 0);
    u32 num_icon_frames = 0;
    if (tf->icon_flag == 0x11)
      num_icon_frames = 1;
    else if (tf->icon_flag == 0x12)
      num_icon_frames = 2;
    else if (tf->icon_flag == 0x13)
      num_icon_frames = 3;
    else
    {
      Log_WarningFmt("Unknown icon flag 0x{:02X}", tf->icon_flag);
      continue;
    }

    char title_sjis[sizeof(tf->title) + 2];
    std::memcpy(title_sjis, tf->title, sizeof(tf->title));
    title_sjis[sizeof(tf->title)] = 0;
    title_sjis[sizeof(tf->title) + 1] = 0;
    char* title_utf8 = sjis2utf8(title_sjis);
    fi.title = title_utf8;
    std::free(title_utf8);

    fi.icon_frames.resize(num_icon_frames);
    for (u32 icon_frame = 0; icon_frame < num_icon_frames; icon_frame++)
    {
      const u8* indices_ptr = GetFramePtr<u8>(data, dir_frame, 1 + icon_frame);
      u32* pixels_ptr = fi.icon_frames[icon_frame].pixels;
      for (u32 i = 0; i < ICON_WIDTH * ICON_HEIGHT; i += 2)
      {
        *(pixels_ptr++) = icon_to_rgba8(tf->icon_palette[*indices_ptr & 0xF]);
        *(pixels_ptr++) = icon_to_rgba8(tf->icon_palette[*indices_ptr >> 4]);
        indices_ptr++;
      }
    }

    files.push_back(std::move(fi));
  }

  return files;
}

bool MemoryCardImage::ReadFile(const DataArray& data, const FileInfo& fi, std::vector<u8>* buffer, Error* error)
{
  buffer->resize(fi.num_blocks * BLOCK_SIZE);

  u32 block_number = fi.first_block;
  for (u32 i = 0; i < fi.num_blocks; i++)
  {
    Assert(block_number < FRAMES_PER_BLOCK);
    std::memcpy(buffer->data() + (i * BLOCK_SIZE), GetFramePtr<u8>(data, block_number, 0), BLOCK_SIZE);

    const DirectoryFrame* df = GetFramePtr<DirectoryFrame>(data, 0, block_number);
    block_number = df->next_block_number + 1;
  }

  return true;
}

bool MemoryCardImage::WriteFile(DataArray* data, const std::string_view& filename, const std::vector<u8>& buffer,
                                Error* error)
{
  if (buffer.empty())
  {
    Error::SetStringView(error, "Buffer is empty.");
    return false;
  }

  const u32 free_block_count = GetFreeBlockCount(*data);
  const u32 num_blocks = (static_cast<u32>(buffer.size()) + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
  if (free_block_count < num_blocks)
  {
    Error::SetStringFmt(error, "Insufficient free blocks, {} blocks are needed, but only have {}.", num_blocks,
                        free_block_count);
    return false;
  }

  DirectoryFrame* last_df = nullptr;
  for (u32 i = 0; i < num_blocks; i++)
  {
    std::optional<u32> block_number = GetNextFreeBlock(*data);
    Assert(block_number.has_value());

    DirectoryFrame* df = GetFramePtr<DirectoryFrame>(data, 0, block_number.value());
    std::memset(df, 0, sizeof(DirectoryFrame));

    if (last_df)
    {
      // not first sector
      last_df->next_block_number = Truncate16(block_number.value() - 1);
      UpdateChecksum(last_df);

      // 53 for last otherwise 52
      df->block_allocation_state = (i == (num_blocks - 1)) ? 0x53 : 0x52;
    }
    else
    {
      // first sector
      df->block_allocation_state = 0x51;
      df->file_size = static_cast<u32>(buffer.size());
      StringUtil::Strlcpy(df->filename, filename, sizeof(df->filename));
    }

    df->next_block_number = 0xFFFF;
    UpdateChecksum(df);
    last_df = df;

    u8* data_block = GetFramePtr<u8>(data, block_number.value(), 0);
    const u32 src_offset = i * BLOCK_SIZE;
    const u32 size_to_copy = std::min<u32>(BLOCK_SIZE, static_cast<u32>(buffer.size()) - src_offset);
    const u32 size_to_zero = BLOCK_SIZE - size_to_copy;
    std::memcpy(data_block, buffer.data() + src_offset, size_to_copy);
    if (size_to_zero)
      std::memset(data_block + size_to_copy, 0, size_to_zero);
  }

  Log_InfoFmt("Wrote {} byte ({} block) file to memory card", buffer.size(), num_blocks);
  return true;
}

bool MemoryCardImage::DeleteFile(DataArray* data, const FileInfo& fi, bool clear_sectors)
{
  Log_InfoFmt("Deleting '{}' from memory card ({} blocks)", fi.filename, fi.num_blocks);

  u32 block_number = fi.first_block;
  for (u32 i = 0; i < fi.num_blocks && (block_number > 0 && block_number < NUM_BLOCKS); i++)
  {
    DirectoryFrame* df = GetFramePtr<DirectoryFrame>(data, 0, block_number);
    block_number = ZeroExtend32(df->next_block_number) + 1;
    if (clear_sectors)
    {
      std::memset(df, 0, sizeof(DirectoryFrame));
      df->block_allocation_state = 0xA0;
    }
    else
    {
      if (i == 0)
        df->block_allocation_state = 0xA1;
      else if (i == (fi.num_blocks - 1))
        df->block_allocation_state = 0xA3;
      else
        df->block_allocation_state = 0xA2;
    }

    df->next_block_number = 0xFFFF;
    UpdateChecksum(df);
  }

  return true;
}

bool MemoryCardImage::UndeleteFile(DataArray* data, const FileInfo& fi)
{
  if (!fi.deleted)
  {
    Log_ErrorFmt("File '{}' is not deleted", fi.filename);
    return false;
  }

  Log_InfoFmt("Undeleting '{}' from memory card ({} blocks)", fi.filename, fi.num_blocks);

  // check that all blocks are present first
  u32 block_number = fi.first_block;
  for (u32 i = 0; i < fi.num_blocks && (block_number > 0 && block_number < NUM_BLOCKS); i++)
  {
    const u32 this_block_number = block_number;
    DirectoryFrame* df = GetFramePtr<DirectoryFrame>(data, 0, block_number);
    block_number = ZeroExtend32(df->next_block_number) + 1;

    if (i == 0)
    {
      if (df->block_allocation_state != 0xA1)
      {
        Log_ErrorFmt("Incorrect block state for {}, expected 0xA1 got 0x{:02X}", this_block_number,
                     df->block_allocation_state);
        return false;
      }
    }
    else if (i == (fi.num_blocks - 1))
    {
      if (df->block_allocation_state != 0xA3)
      {
        Log_ErrorFmt("Incorrect block state for %u, expected 0xA3 got 0x{:02X}", this_block_number,
                     df->block_allocation_state);
        return false;
      }
    }
    else
    {
      if (df->block_allocation_state != 0xA2)
      {
        Log_ErrorFmt("Incorrect block state for {}, expected 0xA2 got 0x{:02X}", this_block_number,
                     df->block_allocation_state);
        return false;
      }
    }
  }

  block_number = fi.first_block;
  for (u32 i = 0; i < fi.num_blocks && (block_number > 0 && block_number < NUM_BLOCKS); i++)
  {
    DirectoryFrame* df = GetFramePtr<DirectoryFrame>(data, 0, block_number);
    block_number = ZeroExtend32(df->next_block_number) + 1;

    if (i == 0)
      df->block_allocation_state = 0x51;
    else if (i == (fi.num_blocks - 1))
      df->block_allocation_state = 0x53;
    else
      df->block_allocation_state = 0x52;

    UpdateChecksum(df);
  }

  return true;
}

bool MemoryCardImage::ImportCardMCD(DataArray* data, const char* filename, std::vector<u8> file_data, Error* error)
{
  if (file_data.size() != DATA_SIZE)
  {
    Error::SetStringFmt(error, "File is incorrect size, expected {} bytes, got {} bytes.", static_cast<u32>(DATA_SIZE),
                        file_data.size());
    return false;
  }

  std::memcpy(data->data(), file_data.data(), DATA_SIZE);
  return true;
}

bool MemoryCardImage::ImportCardGME(DataArray* data, const char* filename, std::vector<u8> file_data, Error* error)
{
#pragma pack(push, 1)
  struct GMEHeader
  {
    char id[12];
    u8 unk1[4];
    u8 unk2[5];
    u8 sector0[16];
    u8 sector1[16];
    u8 unk3[11];
    char descriptions[256][15];
  };
  static_assert(sizeof(GMEHeader) == 0xF40);
#pragma pack(pop)

  // some gme files are raw files in disguise...
  if (file_data.size() == DATA_SIZE)
    return ImportCardMCD(data, filename, std::move(file_data), error);

  constexpr u32 MIN_SIZE = sizeof(GMEHeader) + BLOCK_SIZE;

  if (file_data.size() < MIN_SIZE)
  {
    Error::SetStringFmt(error, "File is incorrect size, expected at least {} bytes, got {} bytes.", MIN_SIZE,
                        file_data.size());
    return false;
  }

  // if it's too small, pad it
  const u32 expected_size = sizeof(GMEHeader) + DATA_SIZE;
  if (file_data.size() < expected_size)
  {
    Log_WarningFmt("GME memory card '{}' is too small (got {} expected {}), padding with zeroes", filename,
                   file_data.size(), expected_size);
    file_data.resize(expected_size);
  }

  // we don't actually care about the header, just skip over it
  std::memcpy(data->data(), file_data.data() + sizeof(GMEHeader), DATA_SIZE);
  return true;
}

bool MemoryCardImage::ImportCardVGS(DataArray* data, const char* filename, std::vector<u8> file_data, Error* error)
{
  constexpr u32 HEADER_SIZE = 64;
  constexpr u32 EXPECTED_SIZE = HEADER_SIZE + DATA_SIZE;

  if (file_data.size() != EXPECTED_SIZE)
  {
    Error::SetStringFmt(error, "File is incorrect size, expected {} bytes, got {} bytes.", EXPECTED_SIZE,
                        file_data.size());
    return false;
  }

  // Connectix Virtual Game Station format (.MEM): "VgsM", 64 bytes
  if (file_data[0] != 'V' || file_data[1] != 'g' || file_data[2] != 's' || file_data[3] != 'M')
  {
    Error::SetStringView(error, "Incorrect header.");
    return false;
  }

  std::memcpy(data->data(), &file_data[HEADER_SIZE], DATA_SIZE);
  return true;
}

bool MemoryCardImage::ImportCardPSX(DataArray* data, const char* filename, std::vector<u8> file_data, Error* error)
{
  constexpr u32 HEADER_SIZE = 256;
  constexpr u32 EXPECTED_SIZE = HEADER_SIZE + DATA_SIZE;

  if (file_data.size() != EXPECTED_SIZE)
  {
    Error::SetStringFmt(error, "File is incorrect size, expected {} bytes, got {} bytes.", EXPECTED_SIZE,
                        file_data.size());
    return false;
  }

  // Connectix Virtual Game Station format (.MEM): "VgsM", 64 bytes
  if (file_data[0] != 'P' || file_data[1] != 'S' || file_data[2] != 'V')
  {
    Error::SetStringView(error, "Incorrect header.");
    return false;
  }

  std::memcpy(data->data(), &file_data[HEADER_SIZE], DATA_SIZE);
  return true;
}

bool MemoryCardImage::ImportCard(DataArray* data, const char* filename, std::vector<u8> file_data, Error* error)
{
  const std::string_view extension = Path::GetExtension(filename);
  if (extension.empty())
  {
    Error::SetStringFmt(error, "File must have an extension.");
    return false;
  }

  if (StringUtil::EqualNoCase(extension, "mcd") || StringUtil::EqualNoCase(extension, "mcr") ||
      StringUtil::EqualNoCase(extension, "mc") || StringUtil::EqualNoCase(extension, "srm") ||
      StringUtil::EqualNoCase(extension, "psm") || StringUtil::EqualNoCase(extension, "ps") ||
      StringUtil::EqualNoCase(extension, "ddf"))
  {
    return ImportCardMCD(data, filename, std::move(file_data), error);
  }
  else if (StringUtil::EqualNoCase(extension, "gme"))
  {
    return ImportCardGME(data, filename, std::move(file_data), error);
  }
  else if (StringUtil::EqualNoCase(extension, "mem") || StringUtil::EqualNoCase(extension, "vgs"))
  {
    return ImportCardVGS(data, filename, std::move(file_data), error);
  }
  else if (StringUtil::EqualNoCase(extension, "psx"))
  {
    return ImportCardPSX(data, filename, std::move(file_data), error);
  }
  else
  {
    Error::SetStringFmt(error, "Unknown extension '{}'.", extension);
    return false;
  }
}

bool MemoryCardImage::ImportCard(DataArray* data, const char* filename, Error* error)
{
  std::optional<std::vector<u8>> file_data = FileSystem::ReadBinaryFile(filename, error);
  if (!file_data.has_value())
    return false;

  return ImportCard(data, filename, std::move(file_data.value()), error);
}

bool MemoryCardImage::ExportSave(DataArray* data, const FileInfo& fi, const char* filename, Error* error)
{
  std::unique_ptr<ByteStream> stream =
    ByteStream::OpenFile(filename,
                         BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_TRUNCATE | BYTESTREAM_OPEN_WRITE |
                           BYTESTREAM_OPEN_ATOMIC_UPDATE | BYTESTREAM_OPEN_STREAMED,
                         error);
  if (!stream)
    return false;

  DirectoryFrame* df_ptr = GetFramePtr<DirectoryFrame>(data, 0, fi.first_block);
  std::vector<u8> header = std::vector<u8>(static_cast<size_t>(FRAME_SIZE));
  std::memcpy(header.data(), df_ptr, sizeof(*df_ptr));

  std::vector<u8> blocks;
  if (!ReadFile(*data, fi, &blocks, error))
    return false;

  if (!stream->Write(header.data(), static_cast<u32>(header.size())) ||
      !stream->Write(blocks.data(), static_cast<u32>(blocks.size())) || !stream->Commit())
  {
    Error::SetStringView(error, "Failed to write exported save.");
    stream->Discard();
    return false;
  }

  return true;
}

bool MemoryCardImage::ImportSaveWithDirectoryFrame(DataArray* data, const char* filename,
                                                   const FILESYSTEM_STAT_DATA& sd, Error* error)
{
  // Make sure the size of the actual file is valid
  if (sd.Size <= FRAME_SIZE || (sd.Size - FRAME_SIZE) % BLOCK_SIZE != 0u || (sd.Size - FRAME_SIZE) / BLOCK_SIZE > 15u)
  {
    Error::SetStringView(error, "Invalid size for save file.");
    return false;
  }

  std::unique_ptr<ByteStream> stream =
    ByteStream::OpenFile(filename, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED, error);
  if (!stream)
    return false;

  DirectoryFrame df;
  if (stream->Read(&df, FRAME_SIZE) != FRAME_SIZE)
  {
    Error::SetStringView(error, "Failed to read directory frame.");
    return false;
  }

  // Make sure the size reported by the directory frame is valid
  if (df.file_size < BLOCK_SIZE || df.file_size % BLOCK_SIZE != 0 || df.file_size / BLOCK_SIZE > 15u)
  {
    Error::SetStringFmt(error, "Invalid size ({} bytes) reported by directory frame.", df.file_size);
    return false;
  }

  std::vector<u8> blocks = std::vector<u8>(static_cast<size_t>(df.file_size));
  if (stream->Read(blocks.data(), df.file_size) != df.file_size)
  {
    Error::SetStringView(error, "Failed to read block bytes.");
    return false;
  }

  const u32 num_blocks = (static_cast<u32>(blocks.size()) + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
  if (GetFreeBlockCount(*data) < num_blocks)
  {
    Error::SetStringView(error, "Insufficient free blocks.");
    return false;
  }

  // Make sure there isn't already a save with the same name
  std::vector<FileInfo> fileinfos = EnumerateFiles(*data, true);
  for (const FileInfo& fi : fileinfos)
  {
    if (fi.filename.compare(0, sizeof(df.filename), df.filename) == 0)
    {
      if (!fi.deleted)
      {
        Error::SetStringFmt(error, "Save file with the same name '{}' already exists in memory card", fi.filename);
        return false;
      }

      DeleteFile(data, fi, true);
    }
  }

  return WriteFile(data, df.filename, blocks, error);
}

bool MemoryCardImage::ImportRawSave(DataArray* data, const char* filename, const FILESYSTEM_STAT_DATA& sd, Error* error)
{
  const std::string display_name = FileSystem::GetDisplayNameFromPath(filename);
  std::string save_name(Path::GetFileTitle(filename));
  if (save_name.length() == 0)
  {
    Error::SetStringView(error, "Invalid filename.");
    return false;
  }

  if (save_name.length() > DirectoryFrame::FILE_NAME_LENGTH)
    save_name.erase(DirectoryFrame::FILE_NAME_LENGTH);

  std::optional<std::vector<u8>> blocks = FileSystem::ReadBinaryFile(filename, error);
  if (!blocks.has_value())
    return false;

  const u32 free_block_count = GetFreeBlockCount(*data);
  const u32 num_blocks = (static_cast<u32>(blocks->size()) + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
  if (free_block_count < num_blocks)
  {
    Error::SetStringFmt(error, "Insufficient free blocks, needs {} blocks, but only have {}.", num_blocks,
                        free_block_count);
    return false;
  }

  // Make sure there isn't already a save with the same name
  std::vector<FileInfo> fileinfos = EnumerateFiles(*data, true);
  for (const FileInfo& fi : fileinfos)
  {
    if (fi.filename.compare(save_name) == 0)
    {
      if (!fi.deleted)
      {
        Error::SetStringFmt(error, "Save file with the same name '{}' already exists in memory card.", fi.filename);
        return false;
      }

      DeleteFile(data, fi, true);
    }
  }

  return WriteFile(data, save_name, blocks.value(), error);
}

bool MemoryCardImage::ImportSave(DataArray* data, const char* filename, Error* error)
{
  // Make sure the size of the actual file is valid
  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(filename, &sd) || sd.Size == 0)
  {
    Error::SetStringView(error, "File does not exist, or is empty.");
    return false;
  }

  if (StringUtil::EqualNoCase(Path::GetExtension(filename), "mcs"))
  {
    return ImportSaveWithDirectoryFrame(data, filename, sd, error);
  }
  else if (sd.Size > 0 && sd.Size < DATA_SIZE && (sd.Size % BLOCK_SIZE) == 0)
  {
    return ImportRawSave(data, filename, sd, error);
  }
  else
  {
    Error::SetStringView(error, "Unknown save format.");
    return false;
  }
}
