// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "ini_settings_interface.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include <algorithm>
#include <iterator>
#include <mutex>

Log_SetChannel(INISettingsInterface);

#ifdef _WIN32
#include <io.h> // _mktemp_s
#else
#include <stdlib.h> // mktemp
#include <unistd.h>
#endif

// To prevent races between saving and loading settings, particularly with game settings,
// we only allow one ini to be parsed at any point in time.
static std::mutex s_ini_load_save_mutex;

static std::FILE* GetTemporaryFile(std::string* temporary_filename, const std::string& original_filename,
                                   const char* mode, Error* error)
{
  temporary_filename->clear();
  temporary_filename->reserve(original_filename.length() + 8);
  temporary_filename->append(original_filename);

#ifdef _WIN32
  temporary_filename->append(".XXXXXXX");
  const errno_t err = _mktemp_s(temporary_filename->data(), temporary_filename->length() + 1);
  if (err != 0)
  {
    Error::SetErrno(error, "_mktemp_s() failed: ", err);
    return nullptr;
  }

  return FileSystem::OpenCFile(temporary_filename->c_str(), mode, error);
#else
  temporary_filename->append(".XXXXXX");
  const int fd = mkstemp(temporary_filename->data());
  if (fd < 0)
  {
    Error::SetErrno(error, "mkstemp() failed: ", errno);
    return nullptr;
  }

  std::FILE* fp = fdopen(fd, mode);
  if (!fp)
  {
    Error::SetErrno(error, "mkstemp() failed: ", errno);
    close(fd);
    return nullptr;
  }

  return fp;
#endif
}

INISettingsInterface::INISettingsInterface(std::string filename) : m_filename(std::move(filename)), m_ini(true, true)
{
}

INISettingsInterface::~INISettingsInterface()
{
  if (m_dirty)
    Save();
}

bool INISettingsInterface::Load(Error* error /* = nullptr */)
{
  if (m_filename.empty())
  {
    Error::SetStringView(error, "Filename is not set.");
    return false;
  }

  std::unique_lock lock(s_ini_load_save_mutex);
  SI_Error err = SI_FAIL;
  auto fp = FileSystem::OpenManagedCFile(m_filename.c_str(), "rb", error);
  if (fp)
  {
    err = m_ini.LoadFile(fp.get());
    if (err != SI_OK)
      Error::SetStringFmt(error, "INI LoadFile() failed: {}", static_cast<int>(err));
  }

  return (err == SI_OK);
}

bool INISettingsInterface::Save(Error* error /* = nullptr */)
{
  if (m_filename.empty())
  {
    Error::SetStringView(error, "Filename is not set.");
    return false;
  }

  std::unique_lock lock(s_ini_load_save_mutex);
  std::string temp_filename;
  std::FILE* fp = GetTemporaryFile(&temp_filename, m_filename, "wb", error);
  SI_Error err = SI_FAIL;
  if (fp)
  {
    err = m_ini.SaveFile(fp, false);
    std::fclose(fp);

    if (err != SI_OK)
    {
      Error::SetStringFmt(error, "INI SaveFile() failed: {}", static_cast<int>(err));

      // remove temporary file
      FileSystem::DeleteFile(temp_filename.c_str());
    }
    else if (!FileSystem::RenamePath(temp_filename.c_str(), m_filename.c_str(), error))
    {
      Error::AddPrefixFmt(error, "Failed to rename '{}' to '{}': ", temp_filename, m_filename);
      FileSystem::DeleteFile(temp_filename.c_str());
      return false;
    }
  }

  if (err != SI_OK)
  {
    WARNING_LOG("Failed to save settings to '{}'.", m_filename);
    return false;
  }

  m_dirty = false;
  return true;
}

void INISettingsInterface::Clear()
{
  m_ini.Reset();
}

bool INISettingsInterface::IsEmpty()
{
  return (m_ini.GetKeyCount() == 0);
}

bool INISettingsInterface::GetIntValue(const char* section, const char* key, s32* value) const
{
  const char* str_value = m_ini.GetValue(section, key);
  if (!str_value)
    return false;

  std::optional<s32> parsed_value = StringUtil::FromChars<s32>(str_value, 10);
  if (!parsed_value.has_value())
    return false;

  *value = parsed_value.value();
  return true;
}

bool INISettingsInterface::GetUIntValue(const char* section, const char* key, u32* value) const
{
  const char* str_value = m_ini.GetValue(section, key);
  if (!str_value)
    return false;

  std::optional<u32> parsed_value = StringUtil::FromChars<u32>(str_value, 10);
  if (!parsed_value.has_value())
    return false;

  *value = parsed_value.value();
  return true;
}

bool INISettingsInterface::GetFloatValue(const char* section, const char* key, float* value) const
{
  const char* str_value = m_ini.GetValue(section, key);
  if (!str_value)
    return false;

  std::optional<float> parsed_value = StringUtil::FromChars<float>(str_value);
  if (!parsed_value.has_value())
    return false;

  *value = parsed_value.value();
  return true;
}

bool INISettingsInterface::GetDoubleValue(const char* section, const char* key, double* value) const
{
  const char* str_value = m_ini.GetValue(section, key);
  if (!str_value)
    return false;

  std::optional<double> parsed_value = StringUtil::FromChars<double>(str_value);
  if (!parsed_value.has_value())
    return false;

  *value = parsed_value.value();
  return true;
}

bool INISettingsInterface::GetBoolValue(const char* section, const char* key, bool* value) const
{
  const char* str_value = m_ini.GetValue(section, key);
  if (!str_value)
    return false;

  std::optional<bool> parsed_value = StringUtil::FromChars<bool>(str_value);
  if (!parsed_value.has_value())
    return false;

  *value = parsed_value.value();
  return true;
}

bool INISettingsInterface::GetStringValue(const char* section, const char* key, std::string* value) const
{
  const char* str_value = m_ini.GetValue(section, key);
  if (!str_value)
    return false;

  value->assign(str_value);
  return true;
}

bool INISettingsInterface::GetStringValue(const char* section, const char* key, SmallStringBase* value) const
{
  const char* str_value = m_ini.GetValue(section, key);
  if (!str_value)
    return false;

  value->assign(str_value);
  return true;
}

void INISettingsInterface::SetIntValue(const char* section, const char* key, s32 value)
{
  m_dirty = true;
  m_ini.SetValue(section, key, StringUtil::ToChars(value).c_str(), nullptr, true);
}

void INISettingsInterface::SetUIntValue(const char* section, const char* key, u32 value)
{
  m_dirty = true;
  m_ini.SetValue(section, key, StringUtil::ToChars(value).c_str(), nullptr, true);
}

void INISettingsInterface::SetFloatValue(const char* section, const char* key, float value)
{
  m_dirty = true;
  m_ini.SetValue(section, key, StringUtil::ToChars(value).c_str(), nullptr, true);
}

void INISettingsInterface::SetDoubleValue(const char* section, const char* key, double value)
{
  m_dirty = true;
  m_ini.SetValue(section, key, StringUtil::ToChars(value).c_str(), nullptr, true);
}

void INISettingsInterface::SetBoolValue(const char* section, const char* key, bool value)
{
  m_dirty = true;
  m_ini.SetBoolValue(section, key, value, nullptr, true);
}

void INISettingsInterface::SetStringValue(const char* section, const char* key, const char* value)
{
  m_dirty = true;
  m_ini.SetValue(section, key, value, nullptr, true);
}

bool INISettingsInterface::ContainsValue(const char* section, const char* key) const
{
  return (m_ini.GetValue(section, key, nullptr) != nullptr);
}

void INISettingsInterface::DeleteValue(const char* section, const char* key)
{
  m_dirty = true;
  m_ini.Delete(section, key);
}

void INISettingsInterface::ClearSection(const char* section)
{
  m_dirty = true;
  m_ini.Delete(section, nullptr);
  m_ini.SetValue(section, nullptr, nullptr);
}

void INISettingsInterface::RemoveSection(const char* section)
{
  if (!m_ini.GetSection(section))
    return;

  m_dirty = true;
  m_ini.Delete(section, nullptr);
}

void INISettingsInterface::RemoveEmptySections()
{
  std::list<CSimpleIniA::Entry> entries;
  m_ini.GetAllSections(entries);
  for (const CSimpleIniA::Entry& entry : entries)
  {
    if (m_ini.GetSectionSize(entry.pItem) > 0)
      continue;

    m_dirty = true;
    m_ini.Delete(entry.pItem, nullptr);
  }
}

std::vector<std::string> INISettingsInterface::GetStringList(const char* section, const char* key) const
{
  std::list<CSimpleIniA::Entry> entries;
  if (!m_ini.GetAllValues(section, key, entries))
    return {};

  std::vector<std::string> ret;
  ret.reserve(entries.size());
  for (const CSimpleIniA::Entry& entry : entries)
    ret.emplace_back(entry.pItem);
  return ret;
}

void INISettingsInterface::SetStringList(const char* section, const char* key, const std::vector<std::string>& items)
{
  m_dirty = true;
  m_ini.Delete(section, key);

  for (const std::string& sv : items)
    m_ini.SetValue(section, key, sv.c_str(), nullptr, false);
}

bool INISettingsInterface::RemoveFromStringList(const char* section, const char* key, const char* item)
{
  m_dirty = true;
  return m_ini.DeleteValue(section, key, item, true);
}

bool INISettingsInterface::AddToStringList(const char* section, const char* key, const char* item)
{
  std::list<CSimpleIniA::Entry> entries;
  if (m_ini.GetAllValues(section, key, entries) &&
      std::find_if(entries.begin(), entries.end(),
                   [item](const CSimpleIniA::Entry& e) { return (std::strcmp(e.pItem, item) == 0); }) != entries.end())
  {
    return false;
  }

  m_dirty = true;
  m_ini.SetValue(section, key, item, nullptr, false);
  return true;
}

std::vector<std::pair<std::string, std::string>> INISettingsInterface::GetKeyValueList(const char* section) const
{
  using Entry = CSimpleIniA::Entry;
  using KVEntry = std::pair<const char*, Entry>;
  std::vector<KVEntry> entries;
  std::vector<std::pair<std::string, std::string>> output;
  std::list<Entry> keys, values;
  if (m_ini.GetAllKeys(section, keys))
  {
    for (Entry& key : keys)
    {
      if (!m_ini.GetAllValues(section, key.pItem, values)) // [[unlikely]]
      {
        ERROR_LOG("Got no values for a key returned from GetAllKeys!");
        continue;
      }
      for (const Entry& value : values)
        entries.emplace_back(key.pItem, value);
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const KVEntry& a, const KVEntry& b) { return a.second.nOrder < b.second.nOrder; });
  for (const KVEntry& entry : entries)
    output.emplace_back(entry.first, entry.second.pItem);

  return output;
}

void INISettingsInterface::SetKeyValueList(const char* section,
                                           const std::vector<std::pair<std::string, std::string>>& items)
{
  m_ini.Delete(section, nullptr);
  for (const std::pair<std::string, std::string>& item : items)
    m_ini.SetValue(section, item.first.c_str(), item.second.c_str(), nullptr, false);
}
