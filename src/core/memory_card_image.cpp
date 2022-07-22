#include "memory_card_image.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "system.h"
#include "util/shiftjis.h"
#include "util/state_wrapper.h"
#include <algorithm>
#include <cstdio>
#include <optional>
Log_SetChannel(MemoryCard);

namespace MemoryCardImage {

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

static constexpr u32 RGBA5551ToRGBA8888(u16 color)
{
  u8 r = Truncate8(color & 31);
  u8 g = Truncate8((color >> 5) & 31);
  u8 b = Truncate8((color >> 10) & 31);
  u8 a = Truncate8((color >> 15) & 1);

  // 00012345 -> 1234545
  b = (b << 3) | (b & 0b111);
  g = (g << 3) | (g & 0b111);
  r = (r << 3) | (r & 0b111);
  // a = a ? 255 : 0;
  a = (color == 0) ? 0 : 255;

  return ZeroExtend32(r) | (ZeroExtend32(g) << 8) | (ZeroExtend32(b) << 16) | (ZeroExtend32(a) << 24);
}

bool LoadFromFile(DataArray* data, const char* filename)
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
    Log_ErrorPrintf("Only read %zu of %u sectors from '%s'", num_read / FRAME_SIZE, NUM_FRAMES, filename);
    return false;
  }

  Log_InfoPrintf("Loaded memory card from %s", filename);
  return true;
}

bool SaveToFile(const DataArray& data, const char* filename)
{
  std::unique_ptr<ByteStream> stream =
    ByteStream::OpenFile(filename, BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_TRUNCATE | BYTESTREAM_OPEN_WRITE |
                                     BYTESTREAM_OPEN_ATOMIC_UPDATE | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
  {
    Log_ErrorPrintf("Failed to open '%s' for writing.", filename);
    return false;
  }

  if (!stream->Write2(data.data(), DATA_SIZE) || !stream->Commit())
  {
    Log_ErrorPrintf("Failed to write sectors to '%s'", filename);
    stream->Discard();
    return false;
  }

  Log_InfoPrintf("Saved memory card to '%s'", filename);
  return true;
}

bool IsValid(const DataArray& data)
{
  // TODO: Check checksum?
  const u8* fptr = GetFramePtr<u8>(data, 0, 0);
  return fptr[0] == 'M' && fptr[1] == 'C';
}

void Format(DataArray* data)
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

static std::optional<u32> GetNextFreeBlock(const DataArray& data)
{
  for (u32 dir_frame = 1; dir_frame < FRAMES_PER_BLOCK; dir_frame++)
  {
    const DirectoryFrame* df = GetFramePtr<DirectoryFrame>(data, 0, dir_frame);
    if ((df->block_allocation_state & 0xF0) == 0xA0)
      return dir_frame;
  }

  return std::nullopt;
}

u32 GetFreeBlockCount(const DataArray& data)
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

std::vector<FileInfo> EnumerateFiles(const DataArray& data, bool include_deleted)
{
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
      Log_WarningPrintf("Invalid block chain in block %u", dir_frame);
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
      Log_WarningPrintf("Unknown icon flag 0x%02X", tf->icon_flag);
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
        *(pixels_ptr++) = RGBA5551ToRGBA8888(tf->icon_palette[*indices_ptr & 0xF]);
        *(pixels_ptr++) = RGBA5551ToRGBA8888(tf->icon_palette[*indices_ptr >> 4]);
        indices_ptr++;
      }
    }

    files.push_back(std::move(fi));
  }

  return files;
}

bool ReadFile(const DataArray& data, const FileInfo& fi, std::vector<u8>* buffer)
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

bool WriteFile(DataArray* data, const std::string_view& filename, const std::vector<u8>& buffer)
{
  if (buffer.empty())
  {
    Log_ErrorPrintf("Failed to write file to memory card: buffer is empty");
    return false;
  }

  const u32 num_blocks = (static_cast<u32>(buffer.size()) + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
  if (GetFreeBlockCount(*data) < num_blocks)
  {
    Log_ErrorPrintf("Failed to write file to memory card: insufficient free blocks");
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

  Log_InfoPrintf("Wrote %zu byte (%u block) file to memory card", buffer.size(), num_blocks);
  return true;
}

bool DeleteFile(DataArray* data, const FileInfo& fi, bool clear_sectors)
{
  Log_InfoPrintf("Deleting '%s' from memory card (%u blocks)", fi.filename.c_str(), fi.num_blocks);

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

bool UndeleteFile(DataArray* data, const FileInfo& fi)
{
  if (!fi.deleted)
  {
    Log_ErrorPrintf("File '%s' is not deleted", fi.filename.c_str());
    return false;
  }

  Log_InfoPrintf("Undeleting '%s' from memory card (%u blocks)", fi.filename.c_str(), fi.num_blocks);

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
        Log_ErrorPrintf("Incorrect block state for %u, expected 0xA1 got 0x%02X", this_block_number,
                        df->block_allocation_state);
        return false;
      }
    }
    else if (i == (fi.num_blocks - 1))
    {
      if (df->block_allocation_state != 0xA3)
      {
        Log_ErrorPrintf("Incorrect block state for %u, expected 0xA3 got 0x%02X", this_block_number,
                        df->block_allocation_state);
        return false;
      }
    }
    else
    {
      if (df->block_allocation_state != 0xA2)
      {
        Log_WarningPrintf("Incorrect block state for %u, expected 0xA2 got 0x%02X", this_block_number,
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

static bool ImportCardMCD(DataArray* data, const char* filename, std::vector<u8> file_data)
{
  if (file_data.size() != DATA_SIZE)
  {
    Log_ErrorPrintf("Failed to import memory card from '%s': file is incorrect size.", filename);
    return false;
  }

  std::memcpy(data->data(), file_data.data(), DATA_SIZE);
  return true;
}

static bool ImportCardGME(DataArray* data, const char* filename, std::vector<u8> file_data)
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

  if (file_data.size() < (sizeof(GMEHeader) + BLOCK_SIZE))
  {
    Log_ErrorPrintf("Failed to import GME memory card from '%s': file is incorrect size.", filename);
    return false;
  }

  // if it's too small, pad it
  const u32 expected_size = sizeof(GMEHeader) + DATA_SIZE;
  if (file_data.size() < expected_size)
  {
    Log_WarningPrintf("GME memory card '%s' is too small (got %zu expected %u), padding with zeroes", filename,
                      file_data.size(), expected_size);
    file_data.resize(expected_size);
  }

  // we don't actually care about the header, just skip over it
  std::memcpy(data->data(), file_data.data() + sizeof(GMEHeader), DATA_SIZE);
  return true;
}

bool ImportCard(DataArray* data, const char* filename, std::vector<u8> file_data)
{
  const char* extension = std::strrchr(filename, '.');
  if (!extension)
  {
    Log_ErrorPrintf("Failed to import memory card from '%s': missing extension?", filename);
    return false;
  }

  if (StringUtil::Strcasecmp(extension, ".mcd") == 0 || StringUtil::Strcasecmp(extension, ".mcr") == 0 ||
      StringUtil::Strcasecmp(extension, ".mc") == 0 || StringUtil::Strcasecmp(extension, ".srm") == 0)
  {
    return ImportCardMCD(data, filename, std::move(file_data));
  }
  else if (StringUtil::Strcasecmp(extension, ".gme") == 0)
  {
    return ImportCardGME(data, filename, std::move(file_data));
  }
  else
  {
    Log_ErrorPrintf("Failed to import memory card from '%s': unknown extension?", filename);
    return false;
  }
}

bool ImportCard(DataArray* data, const char* filename)
{
  std::optional<std::vector<u8>> file_data = FileSystem::ReadBinaryFile(filename);
  if (!file_data.has_value())
    return false;

  return ImportCard(data, filename, std::move(file_data.value()));
}

bool ExportSave(DataArray* data, const FileInfo& fi, const char* filename)
{
  std::unique_ptr<ByteStream> stream =
    ByteStream::OpenFile(filename, BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_TRUNCATE | BYTESTREAM_OPEN_WRITE |
                                     BYTESTREAM_OPEN_ATOMIC_UPDATE | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
  {
    Log_ErrorPrintf("Failed to open '%s' for writing.", filename);
    return false;
  }

  DirectoryFrame* df_ptr = GetFramePtr<DirectoryFrame>(data, 0, fi.first_block);
  std::vector<u8> header = std::vector<u8>(static_cast<size_t>(FRAME_SIZE));
  std::memcpy(header.data(), df_ptr, sizeof(*df_ptr));

  std::vector<u8> blocks;
  if (!ReadFile(*data, fi, &blocks))
  {
    Log_ErrorPrintf("Failed to read save blocks from memory card data");
    return false;
  }

  if (!stream->Write(header.data(), static_cast<u32>(header.size())) ||
      !stream->Write(blocks.data(), static_cast<u32>(blocks.size())) || !stream->Commit())
  {
    Log_ErrorPrintf("Failed to write exported save to '%s'", filename);
    stream->Discard();
    return false;
  }

  return true;
}

static bool ImportSaveWithDirectoryFrame(DataArray* data, const char* filename, const FILESYSTEM_STAT_DATA& sd)
{
  // Make sure the size of the actual file is valid
  if (sd.Size <= FRAME_SIZE || (sd.Size - FRAME_SIZE) % BLOCK_SIZE != 0u || (sd.Size - FRAME_SIZE) / BLOCK_SIZE > 15u)
  {
    Log_ErrorPrintf("Invalid size for save file '%s'", filename);
    return false;
  }

  std::unique_ptr<ByteStream> stream = ByteStream::OpenFile(filename, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
  {
    Log_ErrorPrintf("Failed to open '%s' for reading", filename);
    return false;
  }

  DirectoryFrame df;
  if (stream->Read(&df, FRAME_SIZE) != FRAME_SIZE)
  {
    Log_ErrorPrintf("Failed to read directory frame from '%s'", filename);
    return false;
  }

  // Make sure the size reported by the directory frame is valid
  if (df.file_size < BLOCK_SIZE || df.file_size % BLOCK_SIZE != 0 || df.file_size / BLOCK_SIZE > 15u)
  {
    Log_ErrorPrintf("Invalid size (%u bytes) reported by directory frame", df.file_size);
    return false;
  }

  std::vector<u8> blocks = std::vector<u8>(static_cast<size_t>(df.file_size));
  if (stream->Read(blocks.data(), df.file_size) != df.file_size)
  {
    Log_ErrorPrintf("Failed to read block bytes from '%s'", filename);
    return false;
  }

  const u32 num_blocks = (static_cast<u32>(blocks.size()) + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
  if (GetFreeBlockCount(*data) < num_blocks)
  {
    Log_ErrorPrintf("Failed to write file to memory card: insufficient free blocks");
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
        Log_ErrorPrintf("Save file with the same name '%s' already exists in memory card", fi.filename.c_str());
        return false;
      }

      DeleteFile(data, fi, true);
    }
  }

  return WriteFile(data, df.filename, blocks);
}

static bool ImportRawSave(DataArray* data, const char* filename, const FILESYSTEM_STAT_DATA& sd)
{
  const std::string display_name(FileSystem::GetDisplayNameFromPath(filename));
  std::string save_name(Path::GetFileTitle(filename));
  if (save_name.length() == 0)
  {
    Log_ErrorPrintf("Invalid filename: '%s'", filename);
    return false;
  }

  if (save_name.length() > DirectoryFrame::FILE_NAME_LENGTH)
    save_name.erase(DirectoryFrame::FILE_NAME_LENGTH);

  std::optional<std::vector<u8>> blocks = FileSystem::ReadBinaryFile(filename);
  if (!blocks.has_value())
  {
    Log_ErrorPrintf("Failed to read '%s'", filename);
    return false;
  }

  const u32 num_blocks = (static_cast<u32>(blocks->size()) + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
  if (GetFreeBlockCount(*data) < num_blocks)
  {
    Log_ErrorPrintf("Failed to write file to memory card: insufficient free blocks");
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
        Log_ErrorPrintf("Save file with the same name '%s' already exists in memory card", fi.filename.c_str());
        return false;
      }

      DeleteFile(data, fi, true);
    }
  }

  return WriteFile(data, save_name, blocks.value());
}

bool ImportSave(DataArray* data, const char* filename)
{
  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(filename, &sd))
  {
    Log_ErrorPrintf("Failed to stat file '%s'", filename);
    return false;
  }

  // Make sure the size of the actual file is valid
  if (sd.Size == 0)
  {
    Log_ErrorPrintf("Invalid size for save file '%s'", filename);
    return false;
  }

  if (StringUtil::EndsWith(filename, ".mcs"))
  {
    return ImportSaveWithDirectoryFrame(data, filename, sd);
  }
  else if (sd.Size > 0 && sd.Size < DATA_SIZE && (sd.Size % BLOCK_SIZE) == 0)
  {
    return ImportRawSave(data, filename, sd);
  }
  else
  {
    Log_ErrorPrintf("Unknown save format for '%s'", filename);
    return false;
  }
}

} // namespace MemoryCardImage
