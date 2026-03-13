// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "ini_settings_interface.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include <algorithm>
#include <cstring>
#include <mutex>

LOG_CHANNEL(Settings);

// To prevent races between saving and loading settings, particularly with game settings,
// we only allow one ini to be parsed at any point in time.
static std::mutex s_ini_load_save_mutex;

INISettingsInterface::INISettingsInterface() = default;

INISettingsInterface::INISettingsInterface(std::string path) : m_path(std::move(path))
{
}

INISettingsInterface::~INISettingsInterface()
{
  if (m_dirty)
    Save();
}

void INISettingsInterface::SetPath(std::string path)
{
  m_dirty |= (path != m_path);
  m_path = std::move(path);
}

std::string_view INISettingsInterface::GetPoolStringView(const PoolString& ps) const
{
  return m_string_pool.GetString(static_cast<BumpUniqueStringPool::Offset>(ps.offset), static_cast<size_t>(ps.length));
}

INISettingsInterface::PoolString INISettingsInterface::AddPoolString(std::string_view str)
{
  if (str.empty())
    return PoolString{static_cast<u32>(BumpUniqueStringPool::InvalidOffset), 0};
  const BumpUniqueStringPool::Offset off = m_string_pool.AddString(str);
  return PoolString{static_cast<u32>(off), static_cast<u32>(str.size())};
}

INISettingsInterface::SectionList::const_iterator INISettingsInterface::FindSection(std::string_view name) const
{
  auto it =
    std::lower_bound(m_sections.begin(), m_sections.end(), name,
                     [this](const Section& s, const std::string_view& n) { return (GetPoolStringView(s.name) < n); });
  if (it != m_sections.end() && GetPoolStringView(it->name) == name)
    return it;
  return m_sections.end();
}

INISettingsInterface::SectionList::iterator INISettingsInterface::FindSection(std::string_view name)
{
  auto it =
    std::lower_bound(m_sections.begin(), m_sections.end(), name,
                     [this](const Section& s, const std::string_view& n) { return (GetPoolStringView(s.name) < n); });
  if (it != m_sections.end() && GetPoolStringView(it->name) == name)
    return it;
  return m_sections.end();
}

INISettingsInterface::Section& INISettingsInterface::GetOrCreateSection(std::string_view name)
{
  auto it =
    std::lower_bound(m_sections.begin(), m_sections.end(), name,
                     [this](const Section& s, const std::string_view& n) { return (GetPoolStringView(s.name) < n); });
  if (it != m_sections.end() && GetPoolStringView(it->name) == name)
    return *it;
  Section sec;
  sec.name = AddPoolString(name);
  return *m_sections.insert(it, std::move(sec));
}

INISettingsInterface::KeyValueList::const_iterator INISettingsInterface::FindKey(const Section& section,
                                                                                 std::string_view key) const
{
  auto it = std::lower_bound(
    section.entries.begin(), section.entries.end(), key,
    [this](const KeyValuePair& kv, const std::string_view& k) { return (GetPoolStringView(kv.key) < k); });
  if (it != section.entries.end() && GetPoolStringView(it->key) == key)
    return it;
  return section.entries.end();
}

INISettingsInterface::KeyValueList::iterator INISettingsInterface::FindKey(Section& section, std::string_view key)
{
  auto it = std::lower_bound(
    section.entries.begin(), section.entries.end(), key,
    [this](const KeyValuePair& kv, const std::string_view& k) { return (GetPoolStringView(kv.key) < k); });
  if (it != section.entries.end() && GetPoolStringView(it->key) == key)
    return it;
  return section.entries.end();
}

INISettingsInterface::KeyValueList::const_iterator INISettingsInterface::FindKeyEnd(const Section& section,
                                                                                    std::string_view key) const
{
  return std::upper_bound(
    section.entries.begin(), section.entries.end(), key,
    [this](const std::string_view& k, const KeyValuePair& kv) { return (k < GetPoolStringView(kv.key)); });
}

INISettingsInterface::KeyValueList::iterator INISettingsInterface::FindKeyEnd(Section& section, std::string_view key)
{
  return std::upper_bound(
    section.entries.begin(), section.entries.end(), key,
    [this](const std::string_view& k, const KeyValuePair& kv) { return (k < GetPoolStringView(kv.key)); });
}

void INISettingsInterface::InsertKeyValue(Section& section, std::string_view key, std::string_view value)
{
  auto it = std::lower_bound(
    section.entries.begin(), section.entries.end(), key,
    [this](const KeyValuePair& kv, const std::string_view& k) { return (GetPoolStringView(kv.key) < k); });
  // Skip past existing entries with the same key to append at end of group.
  while (it != section.entries.end() && GetPoolStringView(it->key) == key)
    ++it;
  KeyValuePair kvp;
  kvp.key = AddPoolString(key);
  kvp.value = AddPoolString(value);
  section.entries.insert(it, kvp);
}

const INISettingsInterface::KeyValuePair* INISettingsInterface::FindFirstKeyValue(std::string_view section,
                                                                                  std::string_view key) const
{
  auto sit = FindSection(section);
  if (sit == m_sections.end())
    return nullptr;
  auto kit = FindKey(*sit, key);
  if (kit == sit->entries.end())
    return nullptr;
  return &(*kit);
}

// Strips inline comment from a raw value. Double-quoted values are returned verbatim (quotes removed).
static std::string_view StripValueComment(std::string_view value)
{
  value = StringUtil::StripWhitespace(value);
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    return value.substr(1, value.size() - 2);
  const size_t comment_pos = value.find_first_of("#;");
  if (comment_pos != std::string_view::npos)
    value = value.substr(0, comment_pos);
  return StringUtil::StripWhitespace(value);
}

bool INISettingsInterface::Load(Error* error)
{
  if (m_path.empty())
  {
    Error::SetStringView(error, "Filename is not set.");
    return false;
  }

  std::unique_lock lock(s_ini_load_save_mutex);

  std::optional<std::string> file_data = FileSystem::ReadFileToString(m_path.c_str(), error);
  if (!file_data.has_value())
    return false;

  LoadFromString(file_data.value());
  m_dirty = false;
  return true;
}

bool INISettingsInterface::Load(std::string new_path, Error* error)
{
  if (m_path != new_path)
    m_path = std::move(new_path);

  return Load(error);
}

bool INISettingsInterface::LoadFromString(std::string_view data)
{
  m_string_pool.Clear();
  m_sections.clear();

  const size_t num_lines = std::ranges::count(data, '\n') + 1;
  m_string_pool.Reserve(num_lines * 2, data.size());

  Section* current_section = nullptr;

  while (!data.empty())
  {
    std::string_view line;
    const size_t nl = data.find('\n');
    if (nl != std::string_view::npos)
    {
      line = data.substr(0, nl);
      data = data.substr(nl + 1);
    }
    else
    {
      line = data;
      data = {};
    }

    line = StringUtil::StripWhitespace(line);
    if (line.empty() || line.front() == '#' || line.front() == ';')
      continue;

    // Section header.
    if (line.front() == '[')
    {
      const size_t close = line.find(']', 1);
      if (close != std::string_view::npos)
      {
        std::string_view section_name = StringUtil::StripWhitespace(line.substr(1, close - 1));
        current_section = &GetOrCreateSection(section_name);
      }
      continue;
    }

    // Key = value pair.
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos)
      continue;

    const std::string_view key = StringUtil::StripWhitespace(line.substr(0, eq));
    const std::string_view value = StripValueComment(line.substr(eq + 1));

    if (key.empty())
      continue;

    if (!current_section)
      current_section = &GetOrCreateSection({});

    InsertKeyValue(*current_section, key, value);
  }

  m_dirty = false;
  return true;
}

static bool ValueNeedsQuoting(std::string_view value)
{
  return value.find_first_of("#;") != std::string_view::npos;
}

std::string INISettingsInterface::SaveToString(SectionSaveOrder save_order /* = {} */) const
{
  // Calculate the exact output size to preallocate.
  size_t total_size = 0;
  bool first_section = true;
  for (const Section& section : m_sections)
  {
    if (!first_section)
      total_size += 1; // blank line between sections
    first_section = false;

    const std::string_view section_name = GetPoolStringView(section.name);
    if (!section_name.empty())
      total_size += 1 + section_name.size() + 2; // "[name]\n"

    for (const KeyValuePair& kv : section.entries)
    {
      const std::string_view key = GetPoolStringView(kv.key);
      const std::string_view value = GetPoolStringView(kv.value);
      // "key = value\n" or "key = \"value\"\n"
      total_size += key.size() + 3 + value.size() + 1;
      if (ValueNeedsQuoting(value))
        total_size += 2; // quotes
    }
  }

  std::string output;
  output.reserve(total_size);

  first_section = true;

  // No save order is easier.
  if (save_order.empty())
  {
    for (const Section& section : m_sections)
    {
      if (!first_section)
        output += '\n';
      first_section = false;

      SaveSection(output, section);
    }
  }
  else
  {
    // Save ordered sections first.
    std::vector<bool> saved_sections(m_sections.size(), false);
    for (const char* ordered_section : save_order)
    {
      const std::string_view ordered_section_v = ordered_section;
      for (size_t i = 0; i < m_sections.size(); i++)
      {
        if (saved_sections[i])
          continue;

        const Section& section = m_sections[i];
        const std::string_view section_name = GetPoolStringView(section.name);

        // Save this section if it matches the ordered section name, or if it starts with it (for prefix matching).
        if (section_name == ordered_section_v ||
            (section_name.length() > ordered_section_v.size() && section_name[ordered_section_v.size()] == '/' &&
             section_name.starts_with(ordered_section_v)))
        {
          if (!first_section)
            output += '\n';
          first_section = false;
          saved_sections[i] = true;
          SaveSection(output, section);
        }
      }
    }

    // And then anything remaining.
    for (size_t i = 0; i < m_sections.size(); i++)
    {
      if (saved_sections[i])
        continue;

      if (!first_section)
        output += '\n';
      first_section = false;

      SaveSection(output, m_sections[i]);
    }
  }

  return output;
}

void INISettingsInterface::SaveSection(std::string& output, const Section& section) const
{
  const std::string_view section_name = GetPoolStringView(section.name);
  if (!section_name.empty())
  {
    output += '[';
    output += section_name;
    output += "]\n";
  }

  for (const KeyValuePair& kv : section.entries)
  {
    const std::string_view key = GetPoolStringView(kv.key);
    const std::string_view value = GetPoolStringView(kv.value);

    output += key;
    output += " = ";
    if (ValueNeedsQuoting(value))
    {
      output += '"';
      output += value;
      output += '"';
    }
    else
    {
      output += value;
    }
    output += '\n';
  }
}

bool INISettingsInterface::Save(Error* error /* = nullptr */, SectionSaveOrder save_order /* = {} */)
{
  if (m_path.empty())
  {
    Error::SetStringView(error, "Filename is not set.");
    return false;
  }

  std::unique_lock lock(s_ini_load_save_mutex);
  FileSystem::AtomicRenamedFile fp = FileSystem::CreateAtomicRenamedFile(m_path, error);
  if (!fp)
  {
    WARNING_LOG("Failed to save settings to '{}'.", m_path);
    return false;
  }

  const std::string data = SaveToString(save_order);
  const bool write_ok = (std::fwrite(data.data(), 1, data.size(), fp.get()) == data.size());

  if (!write_ok)
  {
    Error::SetStringView(error, "Failed to write INI data.");
    FileSystem::DiscardAtomicRenamedFile(fp);
    WARNING_LOG("Failed to save settings to '{}'.", m_path);
    return false;
  }

  if (!FileSystem::CommitAtomicRenamedFile(fp, error))
  {
    WARNING_LOG("Failed to save settings to '{}'.", m_path);
    return false;
  }

  m_dirty = false;
  return true;
}

void INISettingsInterface::Clear()
{
  m_string_pool.Clear();
  m_sections.clear();
}

void INISettingsInterface::ClearPathAndContents()
{
  Clear();
  m_path = {};
  m_dirty = false;
}

bool INISettingsInterface::IsEmpty()
{
  for (const Section& section : m_sections)
  {
    if (!section.entries.empty())
      return false;
  }
  return true;
}

void INISettingsInterface::CompactStrings()
{
  size_t num_strings = 0;
  for (const Section& section : m_sections)
    num_strings += 1 + section.entries.size() * 2;

  BumpUniqueStringPool new_pool;
  new_pool.Reserve(num_strings, m_string_pool.GetSize());

  for (Section& section : m_sections)
  {
    const std::string_view name = GetPoolStringView(section.name);
    section.name = {static_cast<u32>(new_pool.AddString(name)), static_cast<u32>(name.size())};

    for (KeyValuePair& kv : section.entries)
    {
      const std::string_view key = GetPoolStringView(kv.key);
      kv.key = {static_cast<u32>(new_pool.AddString(key)), static_cast<u32>(key.size())};

      const std::string_view value = GetPoolStringView(kv.value);
      kv.value = {static_cast<u32>(new_pool.AddString(value)), static_cast<u32>(value.size())};
    }
  }

  m_string_pool = std::move(new_pool);
}

bool INISettingsInterface::LookupValue(const char* section, const char* key, std::string_view* value) const
{
  const KeyValuePair* kv = FindFirstKeyValue(section, key);
  if (!kv)
    return false;

  *value = GetPoolStringView(kv->value);
  return true;
}

void INISettingsInterface::StoreValue(const char* section, const char* key, std::string_view value)
{
  const std::string_view key_sv(key);

  Section& sec = GetOrCreateSection(section);
  auto it = FindKey(sec, key_sv);
  if (it != sec.entries.end())
  {
    if (GetPoolStringView(it->value) == value)
      return;

    // Update existing entry (old value string becomes waste in pool).
    it->value = AddPoolString(value);

    // Remove any duplicate keys beyond the first.
    auto end_it = FindKeyEnd(sec, key_sv);
    if ((end_it - it) > 1)
      sec.entries.erase(it + 1, end_it);
  }
  else
  {
    InsertKeyValue(sec, key_sv, value);
  }

  m_dirty = true;
}

bool INISettingsInterface::ContainsValue(const char* section, const char* key) const
{
  return FindFirstKeyValue(section, key) != nullptr;
}

void INISettingsInterface::DeleteValue(const char* section, const char* key)
{
  auto sit = FindSection(section);
  if (sit == m_sections.end())
    return;

  const std::string_view key_sv(key);
  auto begin_it = FindKey(*sit, key_sv);
  if (begin_it == sit->entries.end())
    return;

  auto end_it = FindKeyEnd(*sit, key_sv);
  sit->entries.erase(begin_it, end_it);
  m_dirty = true;
}

void INISettingsInterface::ClearSection(const char* section)
{
  auto sit = FindSection(section);
  if (sit != m_sections.end())
  {
    sit->entries.clear();
    m_dirty = true;
  }
  else
  {
    GetOrCreateSection(section);
    m_dirty = true;
  }
}

void INISettingsInterface::RemoveSection(const char* section)
{
  auto sit = FindSection(section);
  if (sit == m_sections.end())
    return;
  m_sections.erase(sit);
  m_dirty = true;
}

void INISettingsInterface::RemoveEmptySections()
{
  for (auto it = m_sections.begin(); it != m_sections.end();)
  {
    if (it->entries.empty())
    {
      it = m_sections.erase(it);
      m_dirty = true;
    }
    else
    {
      ++it;
    }
  }
}

std::vector<std::string> INISettingsInterface::GetStringList(const char* section, const char* key) const
{
  auto sit = FindSection(std::string_view(section));
  if (sit == m_sections.end())
    return {};

  const std::string_view key_sv(key);
  auto begin_it = FindKey(*sit, key_sv);
  if (begin_it == sit->entries.end())
    return {};

  auto end_it = FindKeyEnd(*sit, key_sv);
  std::vector<std::string> result;
  result.reserve(static_cast<size_t>(end_it - begin_it));
  for (auto it = begin_it; it != end_it; ++it)
    result.emplace_back(GetPoolStringView(it->value));
  return result;
}

void INISettingsInterface::SetStringList(const char* section, const char* key, const std::vector<std::string>& items)
{
  const std::string_view section_sv(section);
  const std::string_view key_sv(key);

  Section& sec = GetOrCreateSection(section_sv);

  // Remove existing entries for this key.
  auto begin_it = FindKey(sec, key_sv);
  if (begin_it != sec.entries.end())
  {
    auto end_it = FindKeyEnd(sec, key_sv);
    sec.entries.erase(begin_it, end_it);
  }

  for (const std::string& item : items)
    InsertKeyValue(sec, key_sv, std::string_view(item));

  m_dirty = true;
}

bool INISettingsInterface::RemoveFromStringList(const char* section, const char* key, const char* item)
{
  auto sit = FindSection(std::string_view(section));
  if (sit == m_sections.end())
    return false;

  const std::string_view key_sv(key);
  const std::string_view item_sv(item);

  auto begin_it = FindKey(*sit, key_sv);
  if (begin_it == sit->entries.end())
    return false;

  auto end_it = FindKeyEnd(*sit, key_sv);
  for (auto it = begin_it; it != end_it; ++it)
  {
    if (GetPoolStringView(it->value) == item_sv)
    {
      sit->entries.erase(it);
      m_dirty = true;
      return true;
    }
  }
  return false;
}

bool INISettingsInterface::AddToStringList(const char* section, const char* key, const char* item)
{
  const std::string_view section_sv(section);
  const std::string_view key_sv(key);
  const std::string_view item_sv(item);

  auto sit = FindSection(section_sv);
  if (sit != m_sections.end())
  {
    auto begin_it = FindKey(*sit, key_sv);
    if (begin_it != sit->entries.end())
    {
      auto end_it = FindKeyEnd(*sit, key_sv);
      for (auto it = begin_it; it != end_it; ++it)
      {
        if (GetPoolStringView(it->value) == item_sv)
          return false;
      }
    }
  }

  Section& sec = GetOrCreateSection(section_sv);
  InsertKeyValue(sec, key_sv, item_sv);
  m_dirty = true;
  return true;
}

std::vector<std::pair<std::string, std::string>> INISettingsInterface::GetKeyValueList(const char* section) const
{
  auto sit = FindSection(section);
  if (sit == m_sections.end())
    return {};

  std::vector<std::pair<std::string, std::string>> result;
  result.reserve(sit->entries.size());
  for (const KeyValuePair& kv : sit->entries)
    result.emplace_back(std::string(GetPoolStringView(kv.key)), std::string(GetPoolStringView(kv.value)));
  return result;
}

void INISettingsInterface::SetKeyValueList(const char* section,
                                           const std::vector<std::pair<std::string, std::string>>& items)
{
  Section& sec = GetOrCreateSection(section);
  sec.entries.clear();

  for (const auto& [k, v] : items)
    InsertKeyValue(sec, std::string_view(k), std::string_view(v));

  m_dirty = true;
}
