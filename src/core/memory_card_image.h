#pragma once
#include "common/bitfield.h"
#include "controller.h"
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace MemoryCardImage {
enum : u32
{
  DATA_SIZE = 128 * 1024, // 1mbit
  BLOCK_SIZE = 8192,
  FRAME_SIZE = 128,
  FRAMES_PER_BLOCK = BLOCK_SIZE / FRAME_SIZE,
  NUM_BLOCKS = DATA_SIZE / BLOCK_SIZE,
  NUM_FRAMES = DATA_SIZE / FRAME_SIZE,
  ICON_WIDTH = 16,
  ICON_HEIGHT = 16
};

using DataArray = std::array<u8, DATA_SIZE>;

bool LoadFromFile(DataArray* data, const char* filename);
bool SaveToFile(const DataArray& data, const char* filename);

void Format(DataArray* data);

struct IconFrame
{
  u32 pixels[ICON_WIDTH * ICON_HEIGHT];
};

struct FileInfo
{
  std::string filename;
  std::string title;
  u32 size;
  u32 first_block;
  u32 num_blocks;
  bool deleted;

  std::vector<IconFrame> icon_frames;
};

bool IsValid(const DataArray& data);
u32 GetFreeBlockCount(const DataArray& data);
std::vector<FileInfo> EnumerateFiles(const DataArray& data, bool include_deleted);
bool ReadFile(const DataArray& data, const FileInfo& fi, std::vector<u8>* buffer);
bool WriteFile(DataArray* data, const std::string_view& filename, const std::vector<u8>& buffer);
bool DeleteFile(DataArray* data, const FileInfo& fi, bool clear_sectors);
bool UndeleteFile(DataArray* data, const FileInfo& fi);
bool ImportCard(DataArray* data, const char* filename);
bool ImportCard(DataArray* data, const char* filename, std::vector<u8> file_data);
bool ExportSave(DataArray* data, const FileInfo& fi, const char* filename);
bool ImportSave(DataArray* data, const char* filename);
} // namespace MemoryCardImage
