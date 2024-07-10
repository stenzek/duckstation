// SPDX-FileCopyrightText: 2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "memory_card_icon_cache.h"
#include "system.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/timer.h"

Log_SetChannel(MemoryCardImage);

static constexpr const char EXPECTED_SIGNATURE[] = {'M', 'C', 'D', 'I', 'C', 'N', '0', '1'};

static FileSystem::ManagedCFilePtr OpenCache(const std::string& filename, bool for_write)
{
  const char* mode = for_write ? "r+b" : "rb";
  const FileSystem::FileShareMode share_mode =
    for_write ? FileSystem::FileShareMode::DenyReadWrite : FileSystem::FileShareMode::DenyWrite;
  FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedSharedCFile(filename.c_str(), mode, share_mode, nullptr);
  if (fp)
    return fp;

  // Doesn't exist? Create it.
  if (errno == ENOENT)
  {
    if (!for_write)
      return nullptr;

    mode = "w+b";
    fp = FileSystem::OpenManagedSharedCFile(filename.c_str(), mode, share_mode, nullptr);
    if (fp)
      return fp;
  }

  // If there's a sharing violation, try again for 100ms.
  if (errno != EACCES)
    return nullptr;

  Common::Timer timer;
  while (timer.GetTimeMilliseconds() <= 100.0f)
  {
    fp = FileSystem::OpenManagedSharedCFile(filename.c_str(), mode, share_mode, nullptr);
    if (fp)
      return fp;

    if (errno != EACCES)
      return nullptr;
  }

  ERROR_LOG("Timed out while trying to open memory card cache file.");
  return nullptr;
}

MemoryCardIconCache::MemoryCardIconCache(std::string filename) : m_filename(std::move(filename))
{
}

MemoryCardIconCache::~MemoryCardIconCache() = default;

bool MemoryCardIconCache::Reload()
{
  m_entries.clear();

  FileSystem::ManagedCFilePtr fp = OpenCache(m_filename, false);
  if (!fp)
    return false;

#ifndef _WIN32
  FileSystem::POSIXLock lock(fp.get());
#endif

  const s64 file_size = FileSystem::FSize64(fp.get());
  if (file_size < static_cast<s64>(sizeof(EXPECTED_SIGNATURE)))
    return false;

  const size_t count = (static_cast<size_t>(file_size) - sizeof(EXPECTED_SIGNATURE)) / sizeof(Entry);
  if (count <= 0)
    return false;

  char signature[sizeof(EXPECTED_SIGNATURE)];
  if (std::fread(signature, sizeof(signature), 1, fp.get()) != 1 ||
      std::memcmp(signature, EXPECTED_SIGNATURE, sizeof(signature)) != 0)
  {
    return false;
  }

  m_entries.resize(static_cast<size_t>(count));
  if (std::fread(m_entries.data(), sizeof(Entry), m_entries.size(), fp.get()) != m_entries.size())
  {
    m_entries = {};
    return false;
  }

  // Just in case.
  for (Entry& entry : m_entries)
    entry.serial[sizeof(entry.serial) - 1] = 0;

  return true;
}

const MemoryCardImage::IconFrame* MemoryCardIconCache::Lookup(std::string_view serial, std::string_view path)
{
  MemoryCardType type;
  std::string memcard_path = System::GetGameMemoryCardPath(serial, path, 0, &type);
  if (memcard_path.empty() || type == MemoryCardType::Shared)
    return nullptr;

  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(memcard_path.c_str(), &sd))
    return nullptr;

  const s64 timestamp = sd.ModificationTime;
  TinyString index_serial;
  index_serial.assign(serial.substr(0, std::min<size_t>(serial.length(), MAX_SERIAL_LENGTH - 1)));

  Entry* serial_entry = nullptr;
  for (Entry& entry : m_entries)
  {
    if (StringUtil::EqualNoCase(index_serial, entry.serial))
    {
      if (entry.memcard_timestamp == timestamp)
        return entry.is_valid ? &entry.icon : nullptr;

      serial_entry = &entry;
      break;
    }
  }

  if (!serial_entry)
  {
    serial_entry = &m_entries.emplace_back();
    std::memset(serial_entry, 0, sizeof(Entry));
  }

  serial_entry->is_valid = false;
  serial_entry->memcard_timestamp = timestamp;
  StringUtil::Strlcpy(serial_entry->serial, index_serial.view(), sizeof(serial_entry->serial));
  std::memset(serial_entry->icon.pixels, 0, sizeof(serial_entry->icon.pixels));

  MemoryCardImage::DataArray data;
  if (MemoryCardImage::LoadFromFile(&data, memcard_path.c_str()))
  {
    std::vector<MemoryCardImage::FileInfo> files = MemoryCardImage::EnumerateFiles(data, false);
    if (!files.empty())
    {
      const MemoryCardImage::FileInfo& fi = files.front();
      if (!fi.icon_frames.empty())
      {
        INFO_LOG("Extracted memory card icon from {} ({})", fi.filename, Path::GetFileTitle(memcard_path));
        std::memcpy(&serial_entry->icon, &fi.icon_frames.front(), sizeof(serial_entry->icon));
        serial_entry->is_valid = true;
      }
    }
  }

  UpdateInFile(*serial_entry);
  return serial_entry->is_valid ? &serial_entry->icon : nullptr;
}

bool MemoryCardIconCache::UpdateInFile(const Entry& entry)
{
  FileSystem::ManagedCFilePtr fp = OpenCache(m_filename, true);
  if (!fp)
    return false;

#ifndef _WIN32
  FileSystem::POSIXLock lock(fp.get());
#endif

  // check signature, write it if it's non-existent or invalid
  char signature[sizeof(EXPECTED_SIGNATURE)];
  if (std::fread(signature, sizeof(signature), 1, fp.get()) != 1 ||
      std::memcmp(signature, EXPECTED_SIGNATURE, sizeof(signature)) != 0)
  {
    if (!FileSystem::FTruncate64(fp.get(), 0) || FileSystem::FSeek64(fp.get(), 0, SEEK_SET) != 0 ||
        std::fwrite(EXPECTED_SIGNATURE, sizeof(EXPECTED_SIGNATURE), 1, fp.get()) != 1)
    {
      return false;
    }
  }

  // need to seek to switch from read->write?
  s64 current_pos = sizeof(EXPECTED_SIGNATURE);
  if (FileSystem::FSeek64(fp.get(), current_pos, SEEK_SET) != 0)
    return false;

  for (;;)
  {
    Entry existing_entry;
    if (std::fread(&existing_entry, sizeof(existing_entry), 1, fp.get()) != 1)
      break;

    existing_entry.serial[sizeof(existing_entry.serial) - 1] = 0;
    if (!StringUtil::EqualNoCase(existing_entry.serial, entry.serial))
    {
      current_pos += sizeof(existing_entry);
      continue;
    }

    // found it here, so overwrite
    return (FileSystem::FSeek64(fp.get(), current_pos, SEEK_SET) == 0 &&
            std::fwrite(&entry, sizeof(entry), 1, fp.get()) == 1);
  }

  if (FileSystem::FSeek64(fp.get(), current_pos, SEEK_SET) != 0)
    return false;

  // append it.
  return (std::fwrite(&entry, sizeof(entry), 1, fp.get()) == 1);
}
