// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cheats.h"
#include "achievements.h"
#include "bus.h"
#include "controller.h"
#include "cpu_core.h"
#include "game_database.h"
#include "host.h"
#include "system.h"

#include "util/imgui_manager.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/settings_interface.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/zip_helpers.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome5.h"
#include "fmt/format.h"

LOG_CHANNEL(Cheats);

namespace {
class CheatFileReader
{
public:
  CheatFileReader(const std::string_view contents) : m_contents(contents) {}

  ALWAYS_INLINE size_t GetCurrentOffset() const { return m_current_offset; }
  ALWAYS_INLINE size_t GetCurrentLineOffset() const { return m_current_line_offset; }
  ALWAYS_INLINE u32 GetCurrentLineNumber() const { return m_current_line_number; }

  bool GetLine(std::string_view* line)
  {
    const size_t length = m_contents.length();
    if (m_current_offset == length)
    {
      m_current_line_offset = m_current_offset;
      return false;
    }

    size_t end_position = m_current_offset;
    for (; end_position < length; end_position++)
    {
      // ignore carriage returns
      if (m_contents[end_position] == '\r')
        continue;

      if (m_contents[end_position] == '\n')
        break;
    }

    m_current_line_number++;
    m_current_line_offset = m_current_offset;
    *line = m_contents.substr(m_current_offset, end_position - m_current_offset);
    m_current_offset = std::min(end_position + 1, length);
    return true;
  }

  std::optional<std::string_view> GetLine()
  {
    std::optional<std::string_view> ret = std::string_view();
    if (!GetLine(&ret.value()))
      ret.reset();
    return ret;
  }

  template<typename... T>
  bool LogError(Error* error, bool stop_on_error, fmt::format_string<T...> fmt, T&&... args)
  {
    if (!stop_on_error)
    {
      Log::WriteFmtArgs(Log::PackCategory(Log::Channel::Cheats, Log::Level::Warning, Log::Color::StrongOrange), fmt,
                        fmt::make_format_args(args...));
      return true;
    }

    if (error)
      error->SetString(fmt::vformat(fmt, fmt::make_format_args(args...)));

    return false;
  }

private:
  const std::string_view m_contents;
  size_t m_current_offset = 0;
  size_t m_current_line_offset = 0;
  u32 m_current_line_number = 0;
};

class CheatArchive
{
public:
  ~CheatArchive()
  {
    // zip has to be destroyed before data
    m_zip.reset();
    m_data.deallocate();
  }

  ALWAYS_INLINE bool IsOpen() const { return static_cast<bool>(m_zip); }

  bool Open(bool cheats)
  {
    if (m_zip)
      return true;

#ifndef __ANDROID__
    const char* name = cheats ? "cheats.zip" : "patches.zip";
#else
    const char* name = cheats ? "patchcodes.zip" : "patches.zip";
#endif

    Error error;
    std::optional<DynamicHeapArray<u8>> data = Host::ReadResourceFile(name, false, &error);
    if (!data.has_value())
    {
      ERROR_LOG("Failed to read cheat archive {}: {}", name, error.GetDescription());
      return false;
    }

    m_data = std::move(data.value());
    m_zip = ZipHelpers::OpenManagedZipBuffer(m_data.data(), m_data.size(), 0, false, &error);
    if (!m_zip) [[unlikely]]
    {
      ERROR_LOG("Failed to open cheat archive {}: {}", name, error.GetDescription());
      return false;
    }

    return true;
  }

  std::optional<std::string> ReadFile(const char* name) const
  {
    Error error;
    std::optional<std::string> ret = ZipHelpers::ReadFileInZipToString(m_zip.get(), name, true, &error);
    if (!ret.has_value())
      DEV_LOG("Failed to read {} from zip: {}", name, error.GetDescription());
    return ret;
  }

private:
  // Maybe counter-intuitive, but it ends up faster for reading a single game's cheats if we keep a
  // copy of the archive in memory, as opposed to reading from disk.
  DynamicHeapArray<u8> m_data;
  ZipHelpers::ManagedZipT m_zip;
};

} // namespace

namespace Cheats {

namespace {
/// Represents a cheat code, after being parsed.
class CheatCode
{
public:
  /// Additional metadata to a cheat code, present for all types.
  struct Metadata
  {
    std::string name;
    CodeType type = CodeType::Gameshark;
    CodeActivation activation = CodeActivation::EndFrame;
    std::optional<u32> override_cpu_overclock;
    std::optional<DisplayAspectRatio> override_aspect_ratio;
    bool has_options : 1;
    bool disable_widescreen_rendering : 1;
    bool enable_8mb_ram : 1;
    bool disallow_for_achievements : 1;
  };

public:
  CheatCode(Metadata metadata);
  virtual ~CheatCode();

  ALWAYS_INLINE const Metadata& GetMetadata() const { return m_metadata; }
  ALWAYS_INLINE const std::string& GetName() const { return m_metadata.name; }
  ALWAYS_INLINE CodeActivation GetActivation() const { return m_metadata.activation; }
  ALWAYS_INLINE bool IsManuallyActivated() const { return (m_metadata.activation == CodeActivation::Manual); }
  ALWAYS_INLINE bool HasOptions() const { return m_metadata.has_options; }

  bool HasAnySettingOverrides() const;
  void ApplySettingOverrides();

  virtual void SetOptionValue(u32 value) = 0;

  virtual void Apply() const = 0;
  virtual void ApplyOnDisable() const = 0;

protected:
  Metadata m_metadata;
};
} // namespace

using CheatCodeList = std::vector<std::unique_ptr<CheatCode>>;
using ActiveCodeList = std::vector<const CheatCode*>;
using EnableCodeList = std::vector<std::string>;

static std::string GetChtTemplate(const std::string_view serial, std::optional<GameHash> hash, bool add_wildcard);
static std::vector<std::string> FindChtFilesOnDisk(const std::string_view serial, std::optional<GameHash> hash,
                                                   bool cheats);
static bool ExtractCodeInfo(CodeInfoList* dst, const std::string_view file_data, bool from_database, bool stop_on_error,
                            Error* error);
static void AppendCheatToList(CodeInfoList* dst, CodeInfo code);

static bool ShouldLoadDatabaseCheats();
static bool AreAnyPatchesEnabled();
static void ReloadEnabledLists();
static u32 EnableCheats(const CheatCodeList& patches, const EnableCodeList& enable_list, const char* section,
                        bool hc_mode_active);
static void UpdateActiveCodes(bool reload_enabled_list, bool verbose, bool verbose_if_changed,
                              bool show_disabled_codes);

template<typename F>
bool SearchCheatArchive(CheatArchive& archive, std::string_view serial, std::optional<GameHash> hash, const F& f);

template<typename F>
static void EnumerateChtFiles(const std::string_view serial, std::optional<GameHash> hash, bool cheats, bool for_ui,
                              bool load_from_disk, bool load_from_database, const F& f);

static std::optional<CodeOption> ParseOption(const std::string_view value);
static bool ParseOptionRange(const std::string_view value, u16* out_range_start, u16* out_range_end);
extern void ParseFile(CheatCodeList* dst_list, const std::string_view file_contents);

static Cheats::FileFormat DetectFileFormat(const std::string_view file_contents);
static bool ImportPCSXFile(CodeInfoList* dst, const std::string_view file_contents, bool stop_on_error, Error* error);
static bool ImportLibretroFile(CodeInfoList* dst, const std::string_view file_contents, bool stop_on_error,
                               Error* error);
static bool ImportEPSXeFile(CodeInfoList* dst, const std::string_view file_contents, bool stop_on_error, Error* error);
static bool ImportOldChtFile(const std::string_view serial);

static std::unique_ptr<CheatCode> ParseGamesharkCode(CheatCode::Metadata metadata, const std::string_view data,
                                                     Error* error);

const char* PATCHES_CONFIG_SECTION = "Patches";
const char* CHEATS_CONFIG_SECTION = "Cheats";
const char* PATCH_ENABLE_CONFIG_KEY = "Enable";

static std::mutex s_zip_mutex;
static CheatArchive s_patches_zip;
static CheatArchive s_cheats_zip;
static CheatCodeList s_patch_codes;
static CheatCodeList s_cheat_codes;
static EnableCodeList s_enabled_cheats;
static EnableCodeList s_enabled_patches;

static ActiveCodeList s_frame_end_codes;

static u32 s_active_patch_count = 0;
static u32 s_active_cheat_count = 0;
static bool s_patches_enabled = false;
static bool s_cheats_enabled = false;
static bool s_database_cheat_codes_enabled = false;

} // namespace Cheats

Cheats::CheatCode::CheatCode(Metadata metadata) : m_metadata(std::move(metadata))
{
}

Cheats::CheatCode::~CheatCode() = default;

bool Cheats::CheatCode::HasAnySettingOverrides() const
{
  return (m_metadata.disable_widescreen_rendering || m_metadata.enable_8mb_ram ||
          m_metadata.override_aspect_ratio.has_value() || m_metadata.override_cpu_overclock.has_value());
}

void Cheats::CheatCode::ApplySettingOverrides()
{
  if (m_metadata.disable_widescreen_rendering && g_settings.gpu_widescreen_hack)
  {
    DEV_LOG("Disabling widescreen rendering from {} patch.", GetName());
    g_settings.gpu_widescreen_hack = false;
  }
  if (m_metadata.enable_8mb_ram && !g_settings.enable_8mb_ram)
  {
    DEV_LOG("Enabling 8MB ram from {} patch.", GetName());
    g_settings.enable_8mb_ram = true;
  }
  if (m_metadata.override_aspect_ratio.has_value() && g_settings.display_aspect_ratio == DisplayAspectRatio::Auto)
  {
    DEV_LOG("Setting aspect ratio to {} from {} patch.",
            Settings::GetDisplayAspectRatioName(m_metadata.override_aspect_ratio.value()), GetName());
    g_settings.display_aspect_ratio = m_metadata.override_aspect_ratio.value();
  }
  if (m_metadata.override_cpu_overclock.has_value() && !g_settings.cpu_overclock_active)
  {
    DEV_LOG("Setting CPU overclock to {} from {} patch.", m_metadata.override_cpu_overclock.value(), GetName());
    g_settings.SetCPUOverclockPercent(m_metadata.override_cpu_overclock.value());
    g_settings.cpu_overclock_enable = true;
    g_settings.UpdateOverclockActive();
  }
}

static std::array<const char*, 1> s_cheat_code_type_names = {{"Gameshark"}};
static std::array<const char*, 1> s_cheat_code_type_display_names{{TRANSLATE_NOOP("Cheats", "Gameshark")}};

const char* Cheats::GetTypeName(CodeType type)
{
  return s_cheat_code_type_names[static_cast<u32>(type)];
}

const char* Cheats::GetTypeDisplayName(CodeType type)
{
  return TRANSLATE("Cheats", s_cheat_code_type_display_names[static_cast<u32>(type)]);
}

std::optional<Cheats::CodeType> Cheats::ParseTypeName(const std::string_view str)
{
  for (size_t i = 0; i < s_cheat_code_type_names.size(); i++)
  {
    if (str == s_cheat_code_type_names[i])
      return static_cast<CodeType>(i);
  }

  return std::nullopt;
}

static std::array<const char*, 2> s_cheat_code_activation_names = {{"Manual", "EndFrame"}};
static std::array<const char*, 2> s_cheat_code_activation_display_names{
  {TRANSLATE_NOOP("Cheats", "Manual"), TRANSLATE_NOOP("Cheats", "Automatic (Frame End)")}};

const char* Cheats::GetActivationName(CodeActivation activation)
{
  return s_cheat_code_activation_names[static_cast<u32>(activation)];
}

const char* Cheats::GetActivationDisplayName(CodeActivation activation)
{
  return TRANSLATE("Cheats", s_cheat_code_activation_display_names[static_cast<u32>(activation)]);
}

std::optional<Cheats::CodeActivation> Cheats::ParseActivationName(const std::string_view str)
{
  for (u32 i = 0; i < static_cast<u32>(s_cheat_code_activation_names.size()); i++)
  {
    if (str == s_cheat_code_activation_names[i])
      return static_cast<CodeActivation>(i);
  }

  return std::nullopt;
}

std::string Cheats::GetChtTemplate(const std::string_view serial, std::optional<GameHash> hash, bool add_wildcard)
{
  if (!hash.has_value())
    return fmt::format("{}{}.cht", serial, add_wildcard ? "*" : "");
  else
    return fmt::format("{}_{:016X}{}.cht", serial, hash.value(), add_wildcard ? "*" : "");
}

std::vector<std::string> Cheats::FindChtFilesOnDisk(const std::string_view serial, std::optional<GameHash> hash,
                                                    bool cheats)
{
  std::vector<std::string> ret;
  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(cheats ? EmuFolders::Cheats.c_str() : EmuFolders::Patches.c_str(),
                        GetChtTemplate(serial, std::nullopt, true).c_str(),
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files);
  ret.reserve(files.size());

  for (FILESYSTEM_FIND_DATA& fd : files)
  {
    // Skip mismatched hashes.
    if (hash.has_value())
    {
      if (const std::string_view filename = Path::GetFileTitle(fd.FileName); filename.length() >= serial.length() + 17)
      {
        const std::string_view filename_hash = filename.substr(serial.length() + 1, 16);
        const std::optional filename_parsed_hash = StringUtil::FromChars<GameHash>(filename_hash, 16);
        if (filename_parsed_hash.has_value() && filename_parsed_hash.value() != hash.value())
          continue;
      }
    }
    ret.push_back(std::move(fd.FileName));
  }

  return ret;
}

template<typename F>
bool Cheats::SearchCheatArchive(CheatArchive& archive, std::string_view serial, std::optional<GameHash> hash,
                                const F& f)
{
  // Prefer filename with hash.
  std::string zip_filename = GetChtTemplate(serial, hash, false);
  std::optional<std::string> data = archive.ReadFile(zip_filename.c_str());
  if (!data.has_value() && hash.has_value())
  {
    // Try without the hash.
    zip_filename = GetChtTemplate(serial, std::nullopt, false);
    data = archive.ReadFile(zip_filename.c_str());
  }
  if (data.has_value())
  {
    f(std::move(zip_filename), std::move(data.value()), true);
    return true;
  }

  return false;
}

template<typename F>
void Cheats::EnumerateChtFiles(const std::string_view serial, std::optional<GameHash> hash, bool cheats, bool for_ui,
                               bool load_from_files, bool load_from_database, const F& f)
{
  // Prefer files on disk over the zip, so we have to load the zip first.
  if (load_from_database)
  {
    const std::unique_lock lock(s_zip_mutex);
    CheatArchive& archive = cheats ? s_cheats_zip : s_patches_zip;
    if (!archive.IsOpen())
      archive.Open(cheats);

    if (archive.IsOpen())
    {
      if (!SearchCheatArchive(archive, serial, hash, f))
      {
        // Is this game part of a disc set? Try codes for the other discs.
        const GameDatabase::Entry* gentry = GameDatabase::GetEntryForSerial(serial);
        if (gentry && gentry->disc_set_serials.size() > 1)
        {
          for (const std::string& set_serial : gentry->disc_set_serials)
          {
            if (set_serial == serial)
              continue;
            else if (SearchCheatArchive(archive, set_serial, std::nullopt, f))
              break;
          }
        }
      }
    }
  }

  if (load_from_files)
  {
    std::vector<std::string> disk_patch_files;
    if (for_ui || !Achievements::IsHardcoreModeActive())
    {
      disk_patch_files = FindChtFilesOnDisk(serial, hash, cheats);
      if (cheats && disk_patch_files.empty())
      {
        // Check if there's an old-format titled file.
        if (ImportOldChtFile(serial))
          disk_patch_files = FindChtFilesOnDisk(serial, hash, cheats);
      }
    }

    Error error;
    if (!disk_patch_files.empty())
    {
      for (const std::string& file : disk_patch_files)
      {
        const std::optional<std::string> contents = FileSystem::ReadFileToString(file.c_str(), &error);
        if (contents.has_value())
          f(std::move(file), std::move(contents.value()), false);
        else
          WARNING_LOG("Failed to read cht file '{}': {}", Path::GetFileName(file), error.GetDescription());
      }
    }
  }
}

std::string_view Cheats::CodeInfo::GetNamePart() const
{
  const std::string::size_type pos = name.rfind('\\');
  std::string_view ret = name;
  if (pos != std::string::npos)
    ret = ret.substr(pos + 1);
  return ret;
}

std::string_view Cheats::CodeInfo::GetNameParentPart() const
{
  const std::string::size_type pos = name.rfind('\\');
  std::string_view ret;
  if (pos != std::string::npos)
    ret = std::string_view(name).substr(0, pos);
  return ret;
}

std::string_view Cheats::CodeInfo::MapOptionValueToName(u32 value) const
{
  std::string_view ret;
  if (!options.empty())
    ret = options.front().first;

  for (const Cheats::CodeOption& opt : options)
  {
    if (opt.second == value)
    {
      ret = opt.first;
      break;
    }
  }

  return ret;
}

std::string_view Cheats::CodeInfo::MapOptionValueToName(const std::string_view value) const
{
  const std::optional<u32> value_uint = StringUtil::FromChars<u32>(value);
  return MapOptionValueToName(value_uint.value_or(options.empty() ? 0 : options.front().second));
}

u32 Cheats::CodeInfo::MapOptionNameToValue(const std::string_view opt_name) const
{
  for (const Cheats::CodeOption& opt : options)
  {
    if (opt.first == opt_name)
      return opt.second;
  }

  return options.empty() ? 0 : options.front().second;
}

Cheats::CodeInfoList Cheats::GetCodeInfoList(const std::string_view serial, std::optional<GameHash> hash, bool cheats,
                                             bool load_from_database, bool sort_by_name)
{
  CodeInfoList ret;

  EnumerateChtFiles(serial, hash, cheats, true, true, load_from_database,
                    [&ret](const std::string& filename, const std::string& data, bool from_database) {
                      ExtractCodeInfo(&ret, data, from_database, false, nullptr);
                    });

  if (sort_by_name)
  {
    std::sort(ret.begin(), ret.end(), [](const CodeInfo& lhs, const CodeInfo& rhs) {
      // ungrouped cheats go together first
      if (const int lhs_group = static_cast<int>(lhs.name.find('\\') != std::string::npos),
          rhs_group = static_cast<int>(rhs.name.find('\\') != std::string::npos);
          lhs_group != rhs_group)
      {
        return (lhs_group < rhs_group);
      }

      // sort special characters first
      static constexpr auto is_special = [](char ch) {
        return !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                 (ch >= 0x0A && ch <= 0x0D));
      };
      if (const int lhs_is_special = static_cast<int>(!lhs.name.empty() && is_special(lhs.name.front())),
          rhs_is_special = static_cast<int>(!rhs.name.empty() && is_special(rhs.name.front()));
          lhs_is_special != rhs_is_special)
      {
        return (lhs_is_special > rhs_is_special);
      }

      return lhs.name < rhs.name;
    });
  }

  return ret;
}

std::vector<std::string_view> Cheats::GetCodeListUniquePrefixes(const CodeInfoList& list, bool include_empty)
{
  std::vector<std::string_view> ret;
  for (const Cheats::CodeInfo& code : list)
  {
    const std::string_view prefix = code.GetNameParentPart();
    if (prefix.empty())
    {
      if (include_empty && (ret.empty() || !ret.front().empty()))
        ret.insert(ret.begin(), std::string_view());

      continue;
    }

    if (std::find(ret.begin(), ret.end(), prefix) == ret.end())
      ret.push_back(prefix);
  }
  return ret;
}

const Cheats::CodeInfo* Cheats::FindCodeInInfoList(const CodeInfoList& list, const std::string_view name)
{
  const auto it = std::find_if(list.cbegin(), list.cend(), [&name](const CodeInfo& rhs) { return name == rhs.name; });
  return (it != list.end()) ? &(*it) : nullptr;
}

Cheats::CodeInfo* Cheats::FindCodeInInfoList(CodeInfoList& list, const std::string_view name)
{
  const auto it = std::find_if(list.begin(), list.end(), [&name](const CodeInfo& rhs) { return name == rhs.name; });
  return (it != list.end()) ? &(*it) : nullptr;
}

std::string Cheats::FormatCodeForFile(const CodeInfo& code)
{
  fmt::memory_buffer buf;
  auto appender = std::back_inserter(buf);
  fmt::format_to(appender, "[{}]\n", code.name);
  if (!code.author.empty())
    fmt::format_to(appender, "Author = {}\n", code.author);
  if (!code.description.empty())
    fmt::format_to(appender, "Description = {}\n", code.description);
  fmt::format_to(appender, "Type = {}\n", GetTypeName(code.type));
  fmt::format_to(appender, "Activation = {}\n", GetActivationName(code.activation));
  if (code.HasOptionChoices())
  {
    for (const CodeOption& opt : code.options)
      fmt::format_to(appender, "Option = {}:{}\n", opt.first, opt.second);
  }
  else if (code.HasOptionRange())
  {
    fmt::format_to(appender, "OptionRange = {}:{}\n", code.option_range_start, code.option_range_end);
  }

  // remove trailing whitespace
  std::string_view code_body = code.body;
  while (!code_body.empty() && StringUtil::IsWhitespace(code_body.back()))
    code_body = code_body.substr(0, code_body.length() - 1);
  if (!code_body.empty())
    buf.append(code_body);

  buf.push_back('\n');
  return std::string(buf.begin(), buf.end());
}

bool Cheats::UpdateCodeInFile(const char* path, const std::string_view name, const CodeInfo* code, Error* error)
{
  std::string file_contents;
  if (FileSystem::FileExists(path))
  {
    std::optional<std::string> ofile_contents = FileSystem::ReadFileToString(path, error);
    if (!ofile_contents.has_value())
    {
      Error::AddPrefix(error, "Failed to read existing file: ");
      return false;
    }
    file_contents = std::move(ofile_contents.value());
  }

  // This is a bit crap, we're allocating everything and then tossing it away.
  // Hopefully it won't fragment too much at least, because it's freed in reverse order...
  std::optional<size_t> replace_start, replace_end;
  if (!file_contents.empty() && !name.empty())
  {
    CodeInfoList existing_codes_in_file;
    ExtractCodeInfo(&existing_codes_in_file, file_contents, false, false, nullptr);

    const CodeInfo* existing_code = FindCodeInInfoList(existing_codes_in_file, name);
    if (existing_code)
    {
      replace_start = existing_code->file_offset_start;
      replace_end = existing_code->file_offset_end;
    }
  }

  if (replace_start.has_value())
  {
    const auto start = file_contents.begin() + replace_start.value();
    const auto end = file_contents.begin() + replace_end.value();
    if (code)
      file_contents.replace(start, end, FormatCodeForFile(*code));
    else
      file_contents.erase(start, end);
  }
  else if (code)
  {
    const std::string code_body = FormatCodeForFile(*code);
    file_contents.reserve(file_contents.length() + 1 + code_body.length());
    while (!file_contents.empty() && StringUtil::IsWhitespace(file_contents.back()))
      file_contents.pop_back();
    if (!file_contents.empty())
      file_contents.append("\n\n");
    file_contents.append(code_body);
  }

  INFO_LOG("Updating {}...", path);
  if (!FileSystem::WriteStringToFile(path, file_contents, error))
  {
    Error::AddPrefix(error, "Failed to rewrite file: ");
    return false;
  }

  return true;
}

bool Cheats::SaveCodesToFile(const char* path, const CodeInfoList& codes, Error* error)
{
  std::string file_contents;
  if (FileSystem::FileExists(path))
  {
    std::optional<std::string> ofile_contents = FileSystem::ReadFileToString(path, error);
    if (!ofile_contents.has_value())
    {
      Error::AddPrefix(error, "Failed to read existing file: ");
      return false;
    }
    file_contents = std::move(ofile_contents.value());
  }

  for (const CodeInfo& code : codes)
  {
    // This is _really_ crap.. but it's only on importing.
    std::optional<size_t> replace_start, replace_end;
    if (!file_contents.empty())
    {
      CodeInfoList existing_codes_in_file;
      ExtractCodeInfo(&existing_codes_in_file, file_contents, false, false, nullptr);

      const CodeInfo* existing_code = FindCodeInInfoList(existing_codes_in_file, code.name);
      if (existing_code)
      {
        replace_start = existing_code->file_offset_start;
        replace_end = existing_code->file_offset_end;
      }
    }

    if (replace_start.has_value())
    {
      const auto start = file_contents.begin() + replace_start.value();
      const auto end = file_contents.begin() + replace_end.value();
      file_contents.replace(start, end, FormatCodeForFile(code));
    }
    else
    {
      const std::string code_body = FormatCodeForFile(code);
      file_contents.reserve(file_contents.length() + 1 + code_body.length());
      while (!file_contents.empty() && StringUtil::IsWhitespace(file_contents.back()))
        file_contents.pop_back();
      if (!file_contents.empty())
        file_contents.append("\n\n");
      file_contents.append(code_body);
    }
  }

  INFO_LOG("Updating {}...", path);
  if (!FileSystem::WriteStringToFile(path, file_contents, error))
  {
    Error::AddPrefix(error, "Failed to rewrite file: ");
    return false;
  }

  return true;
}

void Cheats::RemoveAllCodes(const std::string_view serial, const std::string_view title, std::optional<GameHash> hash)
{
  Error error;
  std::string path = GetChtFilename(serial, hash, true);
  if (FileSystem::FileExists(path.c_str()))
  {
    if (!FileSystem::DeleteFile(path.c_str(), &error))
      ERROR_LOG("Failed to remove cht file '{}': {}", Path::GetFileName(path), error.GetDescription());
  }

  // check for a non-hashed path and remove that too
  path = GetChtFilename(serial, std::nullopt, true);
  if (FileSystem::FileExists(path.c_str()))
  {
    if (!FileSystem::DeleteFile(path.c_str(), &error))
      ERROR_LOG("Failed to remove cht file '{}': {}", Path::GetFileName(path), error.GetDescription());
  }

  // and a legacy cht file with the game title
  if (!title.empty())
  {
    path = fmt::format("{}" FS_OSPATH_SEPARATOR_STR "{}.cht", EmuFolders::Cheats, Path::SanitizeFileName(title));
    if (FileSystem::FileExists(path.c_str()))
    {
      if (!FileSystem::DeleteFile(path.c_str(), &error))
        ERROR_LOG("Failed to remove cht file '{}': {}", Path::GetFileName(path), error.GetDescription());
    }
  }
}

std::string Cheats::GetChtFilename(const std::string_view serial, std::optional<GameHash> hash, bool cheats)
{
  return Path::Combine(cheats ? EmuFolders::Cheats : EmuFolders::Patches, GetChtTemplate(serial, hash, false));
}

bool Cheats::AreCheatsEnabled()
{
  if (Achievements::IsHardcoreModeActive() || g_settings.disable_all_enhancements)
    return false;

  // Only in the gameini.
  const SettingsInterface* sif = Host::Internal::GetGameSettingsLayer();
  return (sif && sif->GetBoolValue("Cheats", "EnableCheats", false));
}

bool Cheats::ShouldLoadDatabaseCheats()
{
  // Only in the gameini.
  const SettingsInterface* sif = Host::Internal::GetGameSettingsLayer();
  return (sif && sif->GetBoolValue("Cheats", "LoadCheatsFromDatabase", true));
}

bool Cheats::AreAnyPatchesEnabled()
{
  if (g_settings.disable_all_enhancements)
    return false;

  // Only in the gameini.
  const SettingsInterface* sif = Host::Internal::GetGameSettingsLayer();
  return (sif && sif->ContainsValue("Patches", "Enable"));
}

void Cheats::ReloadEnabledLists()
{
  const SettingsInterface* sif = Host::Internal::GetGameSettingsLayer();
  if (!sif)
  {
    // no gameini => nothing is going to be enabled.
    s_enabled_cheats = {};
    s_enabled_patches = {};
    return;
  }

  if (AreCheatsEnabled())
    s_enabled_cheats = sif->GetStringList(CHEATS_CONFIG_SECTION, PATCH_ENABLE_CONFIG_KEY);
  else
    s_enabled_cheats = {};

  s_enabled_patches = sif->GetStringList(PATCHES_CONFIG_SECTION, PATCH_ENABLE_CONFIG_KEY);
}

u32 Cheats::EnableCheats(const CheatCodeList& patches, const EnableCodeList& enable_list, const char* section,
                         bool hc_mode_active)
{
  u32 count = 0;
  for (const std::unique_ptr<CheatCode>& p : patches)
  {
    // ignore manually-activated codes
    if (p->IsManuallyActivated())
      continue;

    // don't load banned patches
    if (p->GetMetadata().disallow_for_achievements && hc_mode_active)
      continue;

    if (std::find(enable_list.begin(), enable_list.end(), p->GetName()) == enable_list.end())
      continue;

    INFO_LOG("Enabled code from {}: {}", section, p->GetName());

    switch (p->GetActivation())
    {
      case CodeActivation::EndFrame:
        s_frame_end_codes.push_back(p.get());
        break;

      default:
        break;
    }

    if (p->HasOptions())
    {
      // need to extract the option from the ini
      SettingsInterface* sif = Host::Internal::GetGameSettingsLayer();
      if (sif) [[likely]]
      {
        if (const std::optional<u32> value = sif->GetOptionalUIntValue(section, p->GetName().c_str(), std::nullopt))
        {
          DEV_LOG("Setting {} option value to 0x{:X}", p->GetName(), value.value());
          p->SetOptionValue(value.value());
        }
      }
    }

    count++;
  }

  return count;
}

void Cheats::ReloadCheats(bool reload_files, bool reload_enabled_list, bool verbose, bool verbose_if_changed,
                          bool show_disabled_codes)
{
  for (const CheatCode* code : s_frame_end_codes)
    code->ApplyOnDisable();

  // Reload files if cheats or patches are enabled, and they were not previously.
  const bool patches_are_enabled = AreAnyPatchesEnabled();
  const bool cheats_are_enabled = AreCheatsEnabled();
  const bool cheatdb_is_enabled = cheats_are_enabled && ShouldLoadDatabaseCheats();
  reload_files = reload_files || (s_patches_enabled != patches_are_enabled);
  reload_files = reload_files || (s_cheats_enabled != cheats_are_enabled);
  reload_files = reload_files || (s_database_cheat_codes_enabled != cheatdb_is_enabled);

  if (reload_files)
  {
    s_patch_codes.clear();
    s_cheat_codes.clear();

    if (const std::string& serial = System::GetGameSerial(); !serial.empty())
    {
      const GameHash hash = System::GetGameHash();

      s_patches_enabled = patches_are_enabled;
      if (patches_are_enabled)
      {
        EnumerateChtFiles(serial, hash, false, false, !Achievements::IsHardcoreModeActive(), true,
                          [](const std::string& filename, const std::string& file_contents, bool from_database) {
                            ParseFile(&s_patch_codes, file_contents);
                            if (s_patch_codes.size() > 0)
                              INFO_LOG("Found {} game patches in {}.", s_patch_codes.size(), filename);
                          });
      }

      s_cheats_enabled = cheats_are_enabled;
      s_database_cheat_codes_enabled = cheatdb_is_enabled;
      if (cheats_are_enabled)
      {
        EnumerateChtFiles(serial, hash, true, false, true, cheatdb_is_enabled,
                          [](const std::string& filename, const std::string& file_contents, bool from_database) {
                            ParseFile(&s_cheat_codes, file_contents);
                            if (s_cheat_codes.size() > 0)
                              INFO_LOG("Found {} cheats in {}.", s_cheat_codes.size(), filename);
                          });
      }
    }
  }

  UpdateActiveCodes(reload_enabled_list, verbose, verbose_if_changed, show_disabled_codes);

  // Reapply frame end codes immediately. Otherwise you end up with a single frame where the old code is used.
  ApplyFrameEndCodes();
}

void Cheats::UnloadAll()
{
  s_active_cheat_count = 0;
  s_active_patch_count = 0;
  s_frame_end_codes = ActiveCodeList();
  s_enabled_patches = EnableCodeList();
  s_enabled_cheats = EnableCodeList();
  s_cheat_codes = CheatCodeList();
  s_patch_codes = CheatCodeList();
  s_patches_enabled = false;
  s_cheats_enabled = false;
  s_database_cheat_codes_enabled = false;
}

bool Cheats::HasAnySettingOverrides()
{
  for (const std::string& name : s_enabled_patches)
  {
    for (std::unique_ptr<CheatCode>& code : s_patch_codes)
    {
      if (name == code->GetName())
      {
        if (code->HasAnySettingOverrides())
          return true;

        break;
      }
    }
  }

  return false;
}

void Cheats::ApplySettingOverrides()
{
  // only need to check patches for this
  for (const std::string& name : s_enabled_patches)
  {
    for (std::unique_ptr<CheatCode>& code : s_patch_codes)
    {
      if (name == code->GetName())
      {
        code->ApplySettingOverrides();
        break;
      }
    }
  }
}

void Cheats::UpdateActiveCodes(bool reload_enabled_list, bool verbose, bool verbose_if_changed,
                               bool show_disabled_codes)
{
  if (reload_enabled_list)
    ReloadEnabledLists();

  const size_t prev_count = s_frame_end_codes.size();
  s_frame_end_codes.clear();

  s_active_patch_count = 0;
  s_active_cheat_count = 0;

  const bool hc_mode_active = Achievements::IsHardcoreModeActive();

  if (!g_settings.disable_all_enhancements)
  {
    s_active_patch_count = EnableCheats(s_patch_codes, s_enabled_patches, "Patches", hc_mode_active);
    s_active_cheat_count =
      AreCheatsEnabled() ? EnableCheats(s_cheat_codes, s_enabled_cheats, "Cheats", hc_mode_active) : 0;
  }

  // Display message on first boot when we load patches.
  // Except when it's just GameDB.
  const size_t new_count = s_frame_end_codes.size();
  if (verbose || (verbose_if_changed && prev_count != new_count))
  {
    if (s_active_patch_count > 0)
    {
      System::SetTaint(System::Taint::Patches);
      Host::AddIconOSDMessage(
        "LoadPatches", ICON_FA_BAND_AID,
        TRANSLATE_PLURAL_STR("Cheats", "%n game patches are active.", "OSD Message", s_active_patch_count),
        Host::OSD_INFO_DURATION);
    }
    if (s_active_cheat_count > 0)
    {
      System::SetTaint(System::Taint::Cheats);
      Host::AddIconOSDMessage("LoadCheats", ICON_EMOJI_WARNING,
                              TRANSLATE_PLURAL_STR("Cheats", "%n cheats are enabled. This may crash games.",
                                                   "OSD Message", s_active_cheat_count),
                              Host::OSD_WARNING_DURATION);
    }
    else if (s_active_patch_count == 0)
    {
      Host::RemoveKeyedOSDMessage("LoadPatches");
      Host::AddIconOSDMessage("LoadCheats", ICON_FA_BAND_AID,
                              TRANSLATE_STR("Cheats", "No cheats/patches are found or enabled."),
                              Host::OSD_INFO_DURATION);
    }
  }

  if (show_disabled_codes && (hc_mode_active || g_settings.disable_all_enhancements))
  {
    const SettingsInterface* sif = Host::Internal::GetGameSettingsLayer();
    const u32 requested_cheat_count = (sif && sif->GetBoolValue("Cheats", "EnableCheats", false)) ?
                                        static_cast<u32>(sif->GetStringList("Cheats", "Enable").size()) :
                                        0;
    const u32 requested_patches_count = sif ? static_cast<u32>(sif->GetStringList("Patches", "Enable").size()) : 0;
    const u32 blocked_cheats =
      (s_active_cheat_count < requested_cheat_count) ? requested_cheat_count - s_active_cheat_count : 0;
    const u32 blocked_patches =
      (s_active_patch_count < requested_patches_count) ? requested_patches_count - s_active_patch_count : 0;
    if (blocked_cheats > 0 || blocked_patches > 0)
    {
      const SmallString blocked_cheats_msg =
        TRANSLATE_PLURAL_SSTR("Cheats", "%n cheats", "Cheats blocked by hardcore mode", blocked_cheats);
      const SmallString blocked_patches_msg =
        TRANSLATE_PLURAL_SSTR("Cheats", "%n patches", "Patches blocked by hardcore mode", blocked_patches);
      std::string message =
        (blocked_cheats > 0 && blocked_patches > 0) ?
          fmt::format(TRANSLATE_FS("Cheats", "{0} and {1} disabled by achievements hardcore mode/safe mode."),
                      blocked_cheats_msg.view(), blocked_patches_msg.view()) :
          fmt::format(TRANSLATE_FS("Cheats", "{} disabled by achievements hardcore mode/safe mode."),
                      (blocked_cheats > 0) ? blocked_cheats_msg.view() : blocked_patches_msg.view());
      Host::AddIconOSDMessage("CheatsBlocked", ICON_EMOJI_WARNING, std::move(message), Host::OSD_INFO_DURATION);
    }
  }
}

void Cheats::ApplyFrameEndCodes()
{
  for (const CheatCode* code : s_frame_end_codes)
    code->Apply();
}

bool Cheats::EnumerateManualCodes(std::function<bool(const std::string& name)> callback)
{
  for (const std::unique_ptr<CheatCode>& code : s_cheat_codes)
  {
    if (code->IsManuallyActivated())
    {
      if (!callback(code->GetName()))
        return false;
    }
  }
  return true;
}

bool Cheats::ApplyManualCode(const std::string_view name)
{
  for (const std::unique_ptr<CheatCode>& code : s_cheat_codes)
  {
    if (code->IsManuallyActivated() && code->GetName() == name)
    {
      Host::AddIconOSDMessage(code->GetName(), ICON_FA_BAND_AID,
                              fmt::format(TRANSLATE_FS("Cheats", "Cheat '{}' applied."), code->GetName()),
                              Host::OSD_INFO_DURATION);
      code->Apply();
      return true;
    }
  }

  return false;
}

u32 Cheats::GetActivePatchCount()
{
  return s_active_patch_count;
}

u32 Cheats::GetActiveCheatCount()
{
  return s_active_cheat_count;
}

//////////////////////////////////////////////////////////////////////////
// File Parsing
//////////////////////////////////////////////////////////////////////////

bool Cheats::ExtractCodeInfo(CodeInfoList* dst, std::string_view file_data, bool from_database, bool stop_on_error,
                             Error* error)
{
  CodeInfo current_code;

  std::optional<std::string> legacy_group;
  std::optional<CodeType> legacy_type;
  std::optional<CodeActivation> legacy_activation;
  bool ignore_this_code = false;

  CheatFileReader reader(file_data);

  const auto finish_code = [&dst, &file_data, &stop_on_error, &error, &current_code, &ignore_this_code, &reader]() {
    if (current_code.file_offset_end > current_code.file_offset_body_start)
    {
      current_code.body = file_data.substr(current_code.file_offset_body_start,
                                           current_code.file_offset_end - current_code.file_offset_body_start);
    }
    else
    {
      if (!reader.LogError(error, stop_on_error, "Empty body for cheat '{}'", current_code.name))
        return false;
    }

    if (!ignore_this_code)
      AppendCheatToList(dst, std::move(current_code));

    return true;
  };

  std::string_view line;
  while (reader.GetLine(&line))
  {
    std::string_view linev = StringUtil::StripWhitespace(line);
    if (linev.empty())
      continue;

    // legacy metadata parsing
    if (linev.starts_with("#group="))
    {
      legacy_group = StringUtil::StripWhitespace(linev.substr(7));
      continue;
    }
    else if (linev.starts_with("#type="))
    {
      legacy_type = ParseTypeName(StringUtil::StripWhitespace(linev.substr(6)));
      if (!legacy_type.has_value()) [[unlikely]]
      {
        if (!reader.LogError(error, stop_on_error, "Unknown type at line {}: {}", reader.GetCurrentLineNumber(), line))
          return false;

        continue;
      }
    }
    else if (linev.starts_with("#activation="))
    {
      legacy_activation = ParseActivationName(StringUtil::StripWhitespace(linev.substr(12)));
      if (!legacy_activation.has_value()) [[unlikely]]
      {
        if (!reader.LogError(error, stop_on_error, "Unknown type at line {}: {}", reader.GetCurrentLineNumber(), line))
          return false;

        continue;
      }
    }

    // skip comments
    if (linev[0] == '#' || linev[0] == ';')
      continue;

    if (linev.front() == '[')
    {
      if (linev.size() < 3 || linev.back() != ']')
      {
        if (!reader.LogError(error, stop_on_error, "Malformed code at line {}: {}", reader.GetCurrentLineNumber(),
                             line))
        {
          return false;
        }

        continue;
      }

      const std::string_view name = StringUtil::StripWhitespace(linev.substr(1, linev.length() - 2));
      if (name.empty())
      {
        if (!reader.LogError(error, stop_on_error, "Empty code name at line {}: {}", reader.GetCurrentLineNumber(),
                             line))
        {
          return false;
        }

        continue;
      }

      // new code.
      if (!current_code.name.empty())
      {
        // overwrite existing codes with the same name.
        finish_code();
        current_code = CodeInfo();
        ignore_this_code = false;
      }

      current_code.name =
        legacy_group.has_value() ? fmt::format("{}\\{}", legacy_group.value(), name) : std::string(name);
      current_code.type = legacy_type.value_or(CodeType::Gameshark);
      current_code.activation = legacy_activation.value_or(CodeActivation::EndFrame);
      current_code.file_offset_start = static_cast<u32>(reader.GetCurrentLineOffset());
      current_code.file_offset_end = current_code.file_offset_start;
      current_code.file_offset_body_start = current_code.file_offset_start;
      current_code.from_database = from_database;
      continue;
    }

    // strip comments off end of lines
    const std::string_view::size_type comment_pos = linev.find_last_of("#;");
    if (comment_pos != std::string_view::npos)
    {
      linev = StringUtil::StripWhitespace(linev.substr(0, comment_pos));
      if (linev.empty())
        continue;
    }

    // metadata?
    if (linev.find('=') != std::string_view::npos)
    {
      std::string_view key, value;
      if (!StringUtil::ParseAssignmentString(linev, &key, &value))
      {
        if (!reader.LogError(error, stop_on_error, "Malformed code at line {}: {}", reader.GetCurrentLineNumber(),
                             line))
        {
          return false;
        }

        continue;
      }

      if (key == "Description")
      {
        current_code.description = value;
      }
      else if (key == "Author")
      {
        current_code.author = value;
      }
      else if (key == "Type")
      {
        const std::optional<CodeType> type = ParseTypeName(value);
        if (type.has_value()) [[unlikely]]
        {
          current_code.type = type.value();
        }
        else
        {
          if (!reader.LogError(error, stop_on_error, "Unknown code type at line {}: {}", reader.GetCurrentLineNumber(),
                               line))
          {
            return false;
          }
        }
      }
      else if (key == "Activation")
      {
        const std::optional<CodeActivation> activation = ParseActivationName(value);
        if (activation.has_value()) [[unlikely]]
        {
          current_code.activation = activation.value();
        }
        else
        {
          if (!reader.LogError(error, stop_on_error, "Unknown code activation at line {}: {}",
                               reader.GetCurrentLineNumber(), line))
          {
            return false;
          }
        }
      }
      else if (key == "Option")
      {
        if (std::optional<Cheats::CodeOption> opt = ParseOption(value))
        {
          current_code.options.push_back(std::move(opt.value()));
        }
        else
        {
          if (!reader.LogError(error, stop_on_error, "Invalid option declaration at line {}: {}",
                               reader.GetCurrentLineNumber(), line))
          {
            return false;
          }
        }
      }
      else if (key == "OptionRange")
      {
        if (!ParseOptionRange(value, &current_code.option_range_start, &current_code.option_range_end))
        {
          if (!reader.LogError(error, stop_on_error, "Invalid option range declaration at line {}: {}",
                               reader.GetCurrentLineNumber(), line))
          {
            return false;
          }
        }
      }
      else if (key == "Ignore")
      {
        ignore_this_code = StringUtil::FromChars<bool>(value).value_or(false);
      }

      // ignore other keys when we're only grabbing info
      continue;
    }

    if (current_code.name.empty())
    {
      if (!reader.LogError(error, stop_on_error, "Code data specified without name at line {}: {}",
                           reader.GetCurrentLineNumber(), line))
      {
        return false;
      }

      continue;
    }

    if (current_code.file_offset_body_start == current_code.file_offset_start)
      current_code.file_offset_body_start = static_cast<u32>(reader.GetCurrentLineOffset());

    // if it's a code line, update the ending point
    current_code.file_offset_end = static_cast<u32>(reader.GetCurrentOffset());
  }

  // last code.
  if (!current_code.name.empty())
    return finish_code();
  else
    return true;
}

void Cheats::AppendCheatToList(CodeInfoList* dst, CodeInfo code)
{
  const auto iter =
    std::find_if(dst->begin(), dst->end(), [&code](const CodeInfo& rhs) { return code.name == rhs.name; });
  if (iter != dst->end())
    *iter = std::move(code);
  else
    dst->push_back(std::move(code));
}

void Cheats::ParseFile(CheatCodeList* dst_list, const std::string_view file_contents)
{
  CheatFileReader reader(file_contents);

  std::string_view next_code_group;
  CheatCode::Metadata next_code_metadata;
  bool next_code_ignored = false;
  std::optional<size_t> code_body_start;

  const auto finish_code = [&dst_list, &file_contents, &reader, &next_code_group, &next_code_metadata,
                            &next_code_ignored, &code_body_start]() {
    if (!code_body_start.has_value())
    {
      WARNING_LOG("Empty cheat body at line {}", reader.GetCurrentLineNumber());
      next_code_metadata = CheatCode::Metadata();
      return;
    }

    const std::string_view code_body =
      file_contents.substr(code_body_start.value(), reader.GetCurrentLineOffset() - code_body_start.value());

    std::unique_ptr<CheatCode> code;
    if (next_code_metadata.type == CodeType::Gameshark)
    {
      Error error;
      code = ParseGamesharkCode(std::move(next_code_metadata), code_body, &error);
      if (!code)
      {
        WARNING_LOG("Failed to parse gameshark code ending on line {}: {}", reader.GetCurrentLineNumber(),
                    error.GetDescription());
        return;
      }
    }
    else
    {
      WARNING_LOG("Unknown code type ending at line {}", reader.GetCurrentLineNumber());
      return;
    }

    next_code_group = {};
    next_code_metadata = CheatCode::Metadata();
    code_body_start.reset();
    if (std::exchange(next_code_ignored, false))
      return;

    // overwrite existing codes with the same name.
    const auto iter = std::find_if(dst_list->begin(), dst_list->end(), [&code](const std::unique_ptr<CheatCode>& rhs) {
      return code->GetName() == rhs->GetName();
    });
    if (iter != dst_list->end())
      *iter = std::move(code);
    else
      dst_list->push_back(std::move(code));
  };

  std::string_view line;
  while (reader.GetLine(&line))
  {
    std::string_view linev = StringUtil::StripWhitespace(line);
    if (linev.empty())
      continue;

    // legacy metadata parsing
    if (linev.starts_with("#group="))
    {
      next_code_group = StringUtil::StripWhitespace(linev.substr(7));
      continue;
    }
    else if (linev.starts_with("#type="))
    {
      const std::optional<CodeType> type = ParseTypeName(StringUtil::StripWhitespace(linev.substr(6)));
      if (!type.has_value())
        WARNING_LOG("Unknown type at line {}: {}", reader.GetCurrentLineNumber(), line);
      else
        next_code_metadata.type = type.value();

      continue;
    }
    else if (linev.starts_with("#activation="))
    {
      const std::optional<CodeActivation> activation =
        ParseActivationName(StringUtil::StripWhitespace(linev.substr(12)));
      if (!activation.has_value())
        WARNING_LOG("Unknown type at line {}: {}", reader.GetCurrentLineNumber(), line);
      else
        next_code_metadata.activation = activation.value();

      continue;
    }

    // skip comments
    if (linev[0] == '#' || linev[0] == ';')
      continue;

    if (linev.front() == '[')
    {
      if (linev.size() < 3 || linev.back() != ']')
      {
        WARNING_LOG("Malformed code at line {}: {}", reader.GetCurrentLineNumber(), line);
        continue;
      }

      const std::string_view name = StringUtil::StripWhitespace(linev.substr(1, linev.length() - 2));
      if (name.empty())
      {
        WARNING_LOG("Empty cheat code name at line {}: {}", reader.GetCurrentLineNumber(), line);
        continue;
      }

      if (!next_code_metadata.name.empty())
        finish_code();

      // new code.
      next_code_metadata.name =
        next_code_group.empty() ? std::string(name) : fmt::format("{}\\{}", next_code_group, name);
      continue;
    }

    // strip comments off end of lines
    const std::string_view::size_type comment_pos = linev.find_last_of("#;");
    if (comment_pos != std::string_view::npos)
    {
      linev = StringUtil::StripWhitespace(linev.substr(0, comment_pos));
      if (linev.empty())
        continue;
    }

    // metadata?
    if (linev.find('=') != std::string_view::npos)
    {
      std::string_view key, value;
      if (!StringUtil::ParseAssignmentString(linev, &key, &value))
      {
        WARNING_LOG("Malformed code at line {}: {}", reader.GetCurrentLineNumber(), line);
        continue;
      }

      if (key == "Type")
      {
        const std::optional<CodeType> type = ParseTypeName(value);
        if (!type.has_value())
          WARNING_LOG("Unknown code type at line {}: {}", reader.GetCurrentLineNumber(), line);
        else
          next_code_metadata.type = type.value();
      }
      else if (key == "Activation")
      {
        const std::optional<CodeActivation> activation = ParseActivationName(value);
        if (!activation.has_value())
          WARNING_LOG("Unknown code activation at line {}: {}", reader.GetCurrentLineNumber(), line);
        else
          next_code_metadata.activation = activation.value();
      }
      else if (key == "OverrideAspectRatio")
      {
        const std::optional<DisplayAspectRatio> aspect_ratio =
          Settings::ParseDisplayAspectRatio(TinyString(value).c_str());
        if (!aspect_ratio.has_value())
          WARNING_LOG("Unknown aspect ratio at line {}: {}", reader.GetCurrentLineNumber(), line);
        else
          next_code_metadata.override_aspect_ratio = aspect_ratio;
      }
      else if (key == "OverrideCPUOverclock")
      {
        const std::optional<u32> ocvalue = StringUtil::FromChars<u32>(value);
        if (!ocvalue.has_value() || ocvalue.value() == 0)
          WARNING_LOG("Invalid CPU overclock at line {}: {}", reader.GetCurrentLineNumber(), line);
        else
          next_code_metadata.override_cpu_overclock = ocvalue.value();
      }
      else if (key == "DisableWidescreenRendering")
      {
        next_code_metadata.disable_widescreen_rendering = StringUtil::FromChars<bool>(value).value_or(false);
      }
      else if (key == "Enable8MBRAM")
      {
        next_code_metadata.enable_8mb_ram = StringUtil::FromChars<bool>(value).value_or(false);
      }
      else if (key == "DisallowForAchievements")
      {
        next_code_metadata.disallow_for_achievements = StringUtil::FromChars<bool>(value).value_or(false);
      }
      else if (key == "Option" || key == "OptionRange")
      {
        // we don't care about the actual values, we load them from the config
        next_code_metadata.has_options = true;
      }
      else if (key == "Author" || key == "Description")
      {
        // ignored when loading
      }
      else if (key == "Ignore")
      {
        next_code_ignored = StringUtil::FromChars<bool>(value).value_or(false);
      }
      else
      {
        WARNING_LOG("Unknown parameter {} at line {}", key, reader.GetCurrentLineNumber());
      }

      continue;
    }

    if (!code_body_start.has_value())
      code_body_start = reader.GetCurrentLineOffset();
  }

  finish_code();
}

std::optional<Cheats::CodeOption> Cheats::ParseOption(const std::string_view value)
{
  // Option = Value1:0x1
  std::optional<CodeOption> ret;
  if (const std::string_view::size_type pos = value.rfind(':'); pos != std::string_view::npos)
  {
    const std::string_view opt_name = StringUtil::StripWhitespace(value.substr(0, pos));
    const std::optional<u32> opt_value =
      StringUtil::FromCharsWithOptionalBase<u32>(StringUtil::StripWhitespace(value.substr(pos + 1)));
    if (opt_value.has_value())
      ret = CodeOption(opt_name, opt_value.value());
  }
  return ret;
}

bool Cheats::ParseOptionRange(const std::string_view value, u16* out_range_start, u16* out_range_end)
{
  // OptionRange = 0:255
  if (const std::string_view::size_type pos = value.rfind(':'); pos != std::string_view::npos)
  {
    const std::optional<u32> start =
      StringUtil::FromCharsWithOptionalBase<u32>(StringUtil::StripWhitespace(value.substr(0, pos)));
    const std::optional<u32> end =
      StringUtil::FromCharsWithOptionalBase<u32>(StringUtil::StripWhitespace(value.substr(pos + 1)));
    if (start.has_value() && end.has_value() && start.value() <= std::numeric_limits<u16>::max() &&
        end.value() <= std::numeric_limits<u16>::max() && end.value() > start.value())
    {
      *out_range_start = static_cast<u16>(start.value());
      *out_range_end = static_cast<u16>(end.value());
      return true;
    }
  }

  return false;
}

//////////////////////////////////////////////////////////////////////////
// File Importing
//////////////////////////////////////////////////////////////////////////

bool Cheats::ExportCodesToFile(std::string path, const CodeInfoList& codes, Error* error)
{
  if (codes.empty())
  {
    Error::SetStringView(error, "Code list is empty.");
    return false;
  }

  auto fp = FileSystem::CreateAtomicRenamedFile(std::move(path), error);
  if (!fp)
    return false;

  for (const CodeInfo& code : codes)
  {
    const std::string code_body = FormatCodeForFile(code);
    if (std::fwrite(code_body.data(), code_body.length(), 1, fp.get()) != 1 || std::fputc('\n', fp.get()) == EOF)
    {
      Error::SetErrno(error, "fwrite() failed: ", errno);
      FileSystem::DiscardAtomicRenamedFile(fp);
      return false;
    }
  }

  return FileSystem::CommitAtomicRenamedFile(fp, error);
}

bool Cheats::ImportCodesFromString(CodeInfoList* dst, const std::string_view file_contents, FileFormat file_format,
                                   bool stop_on_error, Error* error)
{
  if (file_format == FileFormat::Unknown)
    file_format = DetectFileFormat(file_contents);

  if (file_format == FileFormat::DuckStation)
  {
    if (!ExtractCodeInfo(dst, file_contents, false, stop_on_error, error))
      return false;
  }
  else if (file_format == FileFormat::PCSX)
  {
    if (!ImportPCSXFile(dst, file_contents, stop_on_error, error))
      return false;
  }
  else if (file_format == FileFormat::Libretro)
  {
    if (!ImportLibretroFile(dst, file_contents, stop_on_error, error))
      return false;
  }
  else if (file_format == FileFormat::EPSXe)
  {
    if (!ImportEPSXeFile(dst, file_contents, stop_on_error, error))
      return false;
  }
  else
  {
    Error::SetStringView(error, "Unknown file format.");
    return false;
  }

  if (dst->empty())
  {
    Error::SetStringView(error, "No codes found in file.");
    return false;
  }

  return true;
}

Cheats::FileFormat Cheats::DetectFileFormat(const std::string_view file_contents)
{
  CheatFileReader reader(file_contents);
  std::string_view line;
  while (reader.GetLine(&line))
  {
    // skip comments/empty lines
    std::string_view linev = StringUtil::StripWhitespace(line);
    if (linev.empty() || linev[0] == ';' || linev[0] == '#')
      continue;

    if (linev.starts_with("cheats"))
      return FileFormat::Libretro;

    // native if we see brackets and a type string
    if (linev[0] == '[' && file_contents.find("\nType ="))
      return FileFormat::DuckStation;

    // pcsxr if we see brackets
    if (linev[0] == '[')
      return FileFormat::PCSX;

    // otherwise if it's a code, it's probably epsxe
    if (StringUtil::IsHexDigit(linev[0]))
      return FileFormat::EPSXe;
  }

  return FileFormat::Unknown;
}

bool Cheats::ImportPCSXFile(CodeInfoList* dst, const std::string_view file_contents, bool stop_on_error, Error* error)
{
  CheatFileReader reader(file_contents);
  CodeInfo current_code;

  const auto finish_code = [&dst, &file_contents, &stop_on_error, &error, &current_code, &reader]() {
    if (current_code.file_offset_end <= current_code.file_offset_body_start)
    {
      if (!reader.LogError(error, stop_on_error, "Empty body for cheat '{}'", current_code.name))
        return false;
    }

    current_code.body = std::string_view(file_contents)
                          .substr(current_code.file_offset_body_start,
                                  current_code.file_offset_end - current_code.file_offset_body_start);

    AppendCheatToList(dst, std::move(current_code));
    return true;
  };

  std::string_view line;
  while (reader.GetLine(&line))
  {
    std::string_view linev = StringUtil::StripWhitespace(line);
    if (linev.empty() || linev[0] == '#' || linev[0] == ';')
      continue;

    if (linev.front() == '[')
    {
      if (linev.size() < 3 || linev.back() != ']' || (linev[1] == '*' && linev.size() < 4))
      {
        if (!reader.LogError(error, stop_on_error, "Malformed code at line {}: {}", reader.GetCurrentLineNumber(),
                             line))
        {
          return false;
        }

        continue;
      }

      std::string_view name_part = StringUtil::StripWhitespace(linev.substr(1, linev.length() - 2));
      if (!name_part.empty() && name_part.front() == '*')
        name_part = name_part.substr(1);
      if (name_part.empty())
      {
        if (!reader.LogError(error, stop_on_error, "Empty code name at line {}: {}", reader.GetCurrentLineNumber(),
                             line))
        {
          return false;
        }

        continue;
      }

      // new code.
      if (!current_code.name.empty() && !finish_code())
        return false;

      current_code = CodeInfo();
      current_code.name = name_part;
      current_code.file_offset_start = static_cast<u32>(reader.GetCurrentLineOffset());
      current_code.file_offset_end = current_code.file_offset_start;
      current_code.file_offset_body_start = current_code.file_offset_start;
      current_code.type = CodeType::Gameshark;
      current_code.activation = CodeActivation::EndFrame;
      current_code.from_database = false;
      continue;
    }

    // strip comments off end of lines
    const std::string_view::size_type comment_pos = linev.find_last_of("#;");
    if (comment_pos != std::string_view::npos)
    {
      linev = StringUtil::StripWhitespace(linev.substr(0, comment_pos));
      if (linev.empty())
        continue;
    }

    if (current_code.name.empty())
    {
      if (!reader.LogError(error, stop_on_error, "Code data specified without name at line {}: {}",
                           reader.GetCurrentLineNumber(), line))
      {
        return false;
      }

      continue;
    }

    if (current_code.file_offset_body_start == current_code.file_offset_start)
      current_code.file_offset_body_start = static_cast<u32>(reader.GetCurrentLineOffset());

    // if it's a code line, update the ending point
    current_code.file_offset_end = static_cast<u32>(reader.GetCurrentOffset());
  }

  // last code.
  if (!current_code.name.empty() && !finish_code())
    return false;

  return true;
}

bool Cheats::ImportLibretroFile(CodeInfoList* dst, const std::string_view file_contents, bool stop_on_error,
                                Error* error)
{
  std::vector<std::pair<std::string_view, std::string_view>> kvp;

  static constexpr auto FindKey = [](const std::vector<std::pair<std::string_view, std::string_view>>& kvp,
                                     const std::string_view search) -> const std::string_view* {
    for (const auto& it : kvp)
    {
      if (StringUtil::EqualNoCase(search, it.first))
        return &it.second;
    }

    return nullptr;
  };

  CheatFileReader reader(file_contents);
  std::string_view line;
  while (reader.GetLine(&line))
  {
    const std::string_view linev = StringUtil::StripWhitespace(line);
    if (linev.empty())
      continue;

    // skip comments
    if (linev[0] == '#' || linev[0] == ';')
      continue;

    std::string_view key, value;
    if (!StringUtil::ParseAssignmentString(linev, &key, &value))
    {
      if (!reader.LogError(error, stop_on_error, "Malformed code at line {}: {}", reader.GetCurrentLineNumber(), line))
        return false;

      continue;
    }

    kvp.emplace_back(key, value);
  }

  if (kvp.empty())
  {
    reader.LogError(error, stop_on_error, "No key/values found.");
    return false;
  }

  const std::string_view* num_cheats_value = FindKey(kvp, "cheats");
  const u32 num_cheats = num_cheats_value ? StringUtil::FromChars<u32>(*num_cheats_value).value_or(0) : 0;
  if (num_cheats == 0)
    return false;

  for (u32 i = 0; i < num_cheats; i++)
  {
    const std::string_view* desc = FindKey(kvp, TinyString::from_format("cheat{}_desc", i));
    const std::string_view* code = FindKey(kvp, TinyString::from_format("cheat{}_code", i));
    if (!desc || desc->empty() || !code || code->empty())
    {
      if (!reader.LogError(error, stop_on_error, "Missing desc/code for cheat {}", i))
        return false;

      continue;
    }

    // Need to convert + to newlines.
    CodeInfo info;
    info.name = *desc;
    info.body = StringUtil::ReplaceAll(*code, '+', '\n');
    info.file_offset_start = 0;
    info.file_offset_end = 0;
    info.file_offset_body_start = 0;
    info.type = CodeType::Gameshark;
    info.activation = CodeActivation::EndFrame;
    info.from_database = false;
    AppendCheatToList(dst, std::move(info));
  }

  return true;
}

bool Cheats::ImportEPSXeFile(CodeInfoList* dst, const std::string_view file_contents, bool stop_on_error, Error* error)
{
  CheatFileReader reader(file_contents);
  CodeInfo current_code;

  const auto finish_code = [&dst, &file_contents, &stop_on_error, &error, &current_code, &reader]() {
    if (current_code.file_offset_end <= current_code.file_offset_body_start)
    {
      if (!reader.LogError(error, stop_on_error, "Empty body for cheat '{}'", current_code.name))
        return false;
    }

    current_code.body = std::string_view(file_contents)
                          .substr(current_code.file_offset_body_start,
                                  current_code.file_offset_end - current_code.file_offset_body_start);
    StringUtil::StripWhitespace(&current_code.body);

    AppendCheatToList(dst, std::move(current_code));
    return true;
  };

  std::string_view line;
  while (reader.GetLine(&line))
  {
    std::string_view linev = StringUtil::StripWhitespace(line);
    if (linev.empty() || linev[0] == ';')
      continue;

    if (linev.front() == '#')
    {
      if (linev.size() < 2)
      {
        if (!reader.LogError(error, stop_on_error, "Malformed code at line {}: {}", reader.GetCurrentLineNumber(),
                             line))
        {
          return false;
        }

        continue;
      }

      const std::string_view name_part = StringUtil::StripWhitespace(linev.substr(1));
      if (name_part.empty())
      {
        if (!reader.LogError(error, stop_on_error, "Empty code name at line {}: {}", reader.GetCurrentLineNumber(),
                             line))
        {
          return false;
        }

        continue;
      }

      if (!current_code.name.empty() && !finish_code())
        return false;

      // new code.
      current_code = CodeInfo();
      current_code.name = name_part;
      current_code.file_offset_start = static_cast<u32>(reader.GetCurrentOffset());
      current_code.file_offset_end = current_code.file_offset_start;
      current_code.file_offset_body_start = current_code.file_offset_start;
      current_code.type = CodeType::Gameshark;
      current_code.activation = CodeActivation::EndFrame;
      current_code.from_database = false;
      continue;
    }

    if (current_code.name.empty())
    {
      if (!reader.LogError(error, stop_on_error, "Code data specified without name at line {}: {}",
                           reader.GetCurrentLineNumber(), line))
      {
        return false;
      }

      continue;
    }

    // if it's a code line, update the ending point
    current_code.file_offset_end = static_cast<u32>(reader.GetCurrentOffset());
  }

  // last code.
  if (!current_code.name.empty() && !finish_code())
    return false;

  return true;
}

bool Cheats::ImportOldChtFile(const std::string_view serial)
{
  const GameDatabase::Entry* dbentry = GameDatabase::GetEntryForSerial(serial);
  if (!dbentry || dbentry->title.empty())
    return false;

  const std::string old_path = fmt::format("{}" FS_OSPATH_SEPARATOR_STR "{}.cht", EmuFolders::Cheats, dbentry->title);
  if (!FileSystem::FileExists(old_path.c_str()))
    return false;

  Error error;
  std::optional<std::string> old_data = FileSystem::ReadFileToString(old_path.c_str(), &error);
  if (!old_data.has_value())
  {
    ERROR_LOG("Failed to open old cht file '{}' for importing: {}", Path::GetFileName(old_path),
              error.GetDescription());
    return false;
  }

  CodeInfoList new_codes;
  if (!ImportCodesFromString(&new_codes, old_data.value(), FileFormat::Unknown, false, &error) || new_codes.empty())
  {
    ERROR_LOG("Failed to import old cht file '{}': {}", Path::GetFileName(old_path), error.GetDescription());
    return false;
  }

  const std::string new_path = GetChtFilename(serial, std::nullopt, true);
  if (!SaveCodesToFile(new_path.c_str(), new_codes, &error))
  {
    ERROR_LOG("Failed to write new cht file '{}': {}", Path::GetFileName(new_path), error.GetDescription());
    return false;
  }

  INFO_LOG("Imported {} codes from {}.", new_codes.size(), Path::GetFileName(old_path));
  return true;
}

//////////////////////////////////////////////////////////////////////////
// Gameshark codes
//////////////////////////////////////////////////////////////////////////

namespace Cheats {
namespace {

class GamesharkCheatCode final : public CheatCode
{
public:
  GamesharkCheatCode(Metadata metadata);
  ~GamesharkCheatCode() override;

  static std::unique_ptr<GamesharkCheatCode> Parse(Metadata metadata, const std::string_view data, Error* error);

  void SetOptionValue(u32 value) override;

  void Apply() const override;
  void ApplyOnDisable() const override;

private:
  enum class InstructionCode : u8
  {
    Nop = 0x00,
    ConstantWrite8 = 0x30,
    ConstantWrite16 = 0x80,
    ScratchpadWrite16 = 0x1F,
    Increment16 = 0x10,
    Decrement16 = 0x11,
    Increment8 = 0x20,
    Decrement8 = 0x21,
    DelayActivation = 0xC1,
    SkipIfNotEqual16 = 0xC0,
    SkipIfButtonsNotEqual = 0xD5,
    SkipIfButtonsEqual = 0xD6,
    CompareButtons = 0xD4,
    CompareEqual16 = 0xD0,
    CompareNotEqual16 = 0xD1,
    CompareLess16 = 0xD2,
    CompareGreater16 = 0xD3,
    CompareEqual8 = 0xE0,
    CompareNotEqual8 = 0xE1,
    CompareLess8 = 0xE2,
    CompareGreater8 = 0xE3,
    Slide = 0x50,
    MemoryCopy = 0xC2,
    ExtImprovedSlide = 0x53,

    // Extension opcodes, not present on original GameShark.
    ExtConstantWrite32 = 0x90,
    ExtScratchpadWrite32 = 0xA5,
    ExtCompareEqual32 = 0xA0,
    ExtCompareNotEqual32 = 0xA1,
    ExtCompareLess32 = 0xA2,
    ExtCompareGreater32 = 0xA3,
    ExtSkipIfNotEqual32 = 0xA4,
    ExtIncrement32 = 0x60,
    ExtDecrement32 = 0x61,
    ExtConstantWriteIfMatch16 = 0xA6,
    ExtConstantWriteIfMatchWithRestore16 = 0xA7,
    ExtConstantWriteIfMatchWithRestore8 = 0xA8,
    ExtConstantForceRange8 = 0xF0,
    ExtConstantForceRangeLimits16 = 0xF1,
    ExtConstantForceRangeRollRound16 = 0xF2,
    ExtConstantForceRange16 = 0xF3,
    ExtFindAndReplace = 0xF4,
    ExtConstantSwap16 = 0xF5,

    ExtConstantBitSet8 = 0x31,
    ExtConstantBitClear8 = 0x32,
    ExtConstantBitSet16 = 0x81,
    ExtConstantBitClear16 = 0x82,
    ExtConstantBitSet32 = 0x91,
    ExtConstantBitClear32 = 0x92,

    ExtBitCompareButtons = 0xD7,
    ExtSkipIfNotLess8 = 0xC3,
    ExtSkipIfNotGreater8 = 0xC4,
    ExtSkipIfNotLess16 = 0xC5,
    ExtSkipIfNotGreater16 = 0xC6,
    ExtMultiConditionals = 0xF6,

    ExtCheatRegisters = 0x51,
    ExtCheatRegistersCompare = 0x52,

    ExtCompareBitsSet8 = 0xE4,   // Only used inside ExtMultiConditionals
    ExtCompareBitsClear8 = 0xE5, // Only used inside ExtMultiConditionals
  };

  union Instruction
  {
    u64 bits;

    struct
    {
      u32 second;
      u32 first;
    };

    BitField<u64, InstructionCode, 32 + 24, 8> code;
    BitField<u64, u32, 32, 24> address;
    BitField<u64, u32, 0, 32> value32;
    BitField<u64, u16, 0, 16> value16;
    BitField<u64, u8, 0, 8> value8;
  };

  std::vector<Instruction> instructions;
  std::vector<std::tuple<u32, u8, u8>> option_instruction_values;

  u32 GetNextNonConditionalInstruction(u32 index) const;

  static bool IsConditionalInstruction(InstructionCode code);
};

} // namespace

} // namespace Cheats

Cheats::GamesharkCheatCode::GamesharkCheatCode(Metadata metadata) : CheatCode(std::move(metadata))
{
}

Cheats::GamesharkCheatCode::~GamesharkCheatCode() = default;

static std::optional<u32> ParseHexOptionMask(const std::string_view str, u8* out_option_start, u8* out_option_count)
{
  if (str.length() > 8)
    return std::nullopt;

  const u32 num_nibbles = static_cast<u32>(str.size());
  std::array<char, 8> nibble_values;
  u32 option_nibble_start = 0;
  u32 option_nibble_count = 0;
  bool last_was_option = false;
  for (u32 i = 0; i < num_nibbles; i++)
  {
    if (str[i] == '?')
    {
      if (option_nibble_count == 0)
      {
        option_nibble_start = i;
      }
      else if (!last_was_option)
      {
        // ? must be consecutive
        return false;
      }

      option_nibble_count++;
      last_was_option = true;
      nibble_values[i] = '0';
    }
    else if (StringUtil::IsHexDigit(str[i]))
    {
      last_was_option = false;
      nibble_values[i] = str[i];
    }
    else
    {
      // not a valid hex digit
      return false;
    }
  }

  // use stringutil to decode it, it has zeros in the place
  const std::optional<u32> parsed = StringUtil::FromChars<u32>(std::string_view(nibble_values.data(), num_nibbles), 16);
  if (!parsed.has_value()) [[unlikely]]
    return std::nullopt;

  // LSB comes first, so reverse
  *out_option_start = static_cast<u8>((num_nibbles - option_nibble_start - option_nibble_count) * 4);
  *out_option_count = static_cast<u8>(option_nibble_count * 4);
  return parsed;
}

std::unique_ptr<Cheats::GamesharkCheatCode> Cheats::GamesharkCheatCode::Parse(Metadata metadata,
                                                                              const std::string_view data, Error* error)
{
  std::unique_ptr<GamesharkCheatCode> code = std::make_unique<GamesharkCheatCode>(std::move(metadata));
  CheatFileReader reader(data);
  std::string_view line;
  while (reader.GetLine(&line))
  {
    // skip comments/empty lines
    std::string_view linev = StringUtil::StripWhitespace(line);
    if (linev.empty() || !StringUtil::IsHexDigit(linev[0]))
      continue;

    std::string_view next;
    const std::optional<u32> first = StringUtil::FromChars<u32>(linev, 16, &next);
    if (!first.has_value())
    {
      Error::SetStringFmt(error, "Malformed instruction at line {}: {}", reader.GetCurrentLineNumber(), linev);
      code.reset();
      break;
    }

    size_t next_offset = 0;
    while (next_offset < next.size() && next[next_offset] != '?' && !StringUtil::IsHexDigit(next[next_offset]))
      next_offset++;
    next = (next_offset < next.size()) ? next.substr(next_offset) : std::string_view();

    std::optional<u32> second;
    if (next.find('?') != std::string_view::npos)
    {
      u8 option_bitpos = 0, option_bitcount = 0;
      second = ParseHexOptionMask(next, &option_bitpos, &option_bitcount);
      if (second.has_value())
      {
        code->option_instruction_values.emplace_back(static_cast<u32>(code->instructions.size()), option_bitpos,
                                                     option_bitcount);
      }
    }
    else
    {
      second = StringUtil::FromChars<u32>(next, 16);
    }

    if (!second.has_value())
    {
      Error::SetStringFmt(error, "Malformed instruction at line {}: {}", reader.GetCurrentLineNumber(), linev);
      code.reset();
      break;
    }

    Instruction inst;
    inst.first = first.value();
    inst.second = second.value();
    code->instructions.push_back(inst);
  }

  if (code && code->instructions.empty())
  {
    Error::SetStringFmt(error, "No instructions in code.");
    code.reset();
  }

  return code;
}

static std::array<u32, 256> cht_register; // Used for D7 ,51 & 52 cheat types

template<typename T>
NEVER_INLINE static T DoMemoryRead(VirtualMemoryAddress address)
{
  using UnsignedType = typename std::make_unsigned_t<T>;
  static_assert(std::is_same_v<UnsignedType, u8> || std::is_same_v<UnsignedType, u16> ||
                std::is_same_v<UnsignedType, u32>);

  T result;
  if constexpr (std::is_same_v<UnsignedType, u8>)
    return CPU::SafeReadMemoryByte(address, &result) ? result : static_cast<T>(0);
  else if constexpr (std::is_same_v<UnsignedType, u16>)
    return CPU::SafeReadMemoryHalfWord(address, &result) ? result : static_cast<T>(0);
  else // if constexpr (std::is_same_v<UnsignedType, u32>)
    return CPU::SafeReadMemoryWord(address, &result) ? result : static_cast<T>(0);
}

template<typename T>
NEVER_INLINE static void DoMemoryWrite(PhysicalMemoryAddress address, T value)
{
  using UnsignedType = typename std::make_unsigned_t<T>;
  static_assert(std::is_same_v<UnsignedType, u8> || std::is_same_v<UnsignedType, u16> ||
                std::is_same_v<UnsignedType, u32>);

  if constexpr (std::is_same_v<UnsignedType, u8>)
    CPU::SafeWriteMemoryByte(address, value);
  else if constexpr (std::is_same_v<UnsignedType, u16>)
    CPU::SafeWriteMemoryHalfWord(address, value);
  else // if constexpr (std::is_same_v<UnsignedType, u32>)
    CPU::SafeWriteMemoryWord(address, value);
}

NEVER_INLINE static u32 GetControllerButtonBits()
{
  static constexpr std::array<u16, 16> button_mapping = {{
    0x0100, // Select
    0x0200, // L3
    0x0400, // R3
    0x0800, // Start
    0x1000, // Up
    0x2000, // Right
    0x4000, // Down
    0x8000, // Left
    0x0001, // L2
    0x0002, // R2
    0x0004, // L1
    0x0008, // R1
    0x0010, // Triangle
    0x0020, // Circle
    0x0040, // Cross
    0x0080, // Square
  }};

  u32 bits = 0;
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = System::GetController(i);
    if (!controller)
      continue;

    bits |= controller->GetButtonStateBits();
  }

  u32 translated_bits = 0;
  for (u32 i = 0, bit = 1; i < static_cast<u32>(button_mapping.size()); i++, bit <<= 1)
  {
    if (bits & bit)
      translated_bits |= button_mapping[i];
  }

  return translated_bits;
}

NEVER_INLINE static u32 GetControllerAnalogBits()
{
  // 0x010000 - Right Thumb Up
  // 0x020000 - Right Thumb Right
  // 0x040000 - Right Thumb Down
  // 0x080000 - Right Thumb Left
  // 0x100000 - Left Thumb Up
  // 0x200000 - Left Thumb Right
  // 0x400000 - Left Thumb Down
  // 0x800000 - Left Thumb Left

  u32 bits = 0;
  u8 l_ypos = 0;
  u8 l_xpos = 0;
  u8 r_ypos = 0;
  u8 r_xpos = 0;

  std::optional<u32> analog = 0;
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = System::GetController(i);
    if (!controller)
      continue;

    analog = controller->GetAnalogInputBytes();
    if (analog.has_value())
    {
      l_ypos = Truncate8((analog.value() & 0xFF000000u) >> 24);
      l_xpos = Truncate8((analog.value() & 0x00FF0000u) >> 16);
      r_ypos = Truncate8((analog.value() & 0x0000FF00u) >> 8);
      r_xpos = Truncate8(analog.value() & 0x000000FFu);
      if (l_ypos < 0x50)
        bits |= 0x100000;
      else if (l_ypos > 0xA0)
        bits |= 0x400000;
      if (l_xpos < 0x50)
        bits |= 0x800000;
      else if (l_xpos > 0xA0)
        bits |= 0x200000;
      if (r_ypos < 0x50)
        bits |= 0x10000;
      else if (r_ypos > 0xA0)
        bits |= 0x40000;
      if (r_xpos < 0x50)
        bits |= 0x80000;
      else if (r_xpos > 0xA0)
        bits |= 0x20000;
    }
  }
  return bits;
}

bool Cheats::GamesharkCheatCode::IsConditionalInstruction(InstructionCode code)
{
  switch (code)
  {
    case InstructionCode::CompareEqual16:       // D0
    case InstructionCode::CompareNotEqual16:    // D1
    case InstructionCode::CompareLess16:        // D2
    case InstructionCode::CompareGreater16:     // D3
    case InstructionCode::CompareEqual8:        // E0
    case InstructionCode::CompareNotEqual8:     // E1
    case InstructionCode::CompareLess8:         // E2
    case InstructionCode::CompareGreater8:      // E3
    case InstructionCode::CompareButtons:       // D4
    case InstructionCode::ExtCompareEqual32:    // A0
    case InstructionCode::ExtCompareNotEqual32: // A1
    case InstructionCode::ExtCompareLess32:     // A2
    case InstructionCode::ExtCompareGreater32:  // A3
      return true;

    default:
      return false;
  }
}

u32 Cheats::GamesharkCheatCode::GetNextNonConditionalInstruction(u32 index) const
{
  const u32 count = static_cast<u32>(instructions.size());
  for (; index < count; index++)
  {
    if (!IsConditionalInstruction(instructions[index].code))
    {
      // we've found the first non conditional instruction in the chain, so skip over the instruction following it
      return index + 1;
    }
  }

  return index;
}

void Cheats::GamesharkCheatCode::Apply() const
{
  const u32 count = static_cast<u32>(instructions.size());
  u32 index = 0;
  for (; index < count;)
  {
    const Instruction& inst = instructions[index];
    switch (inst.code)
    {
      case InstructionCode::Nop:
      {
        index++;
      }
      break;

      case InstructionCode::ConstantWrite8:
      {
        DoMemoryWrite<u8>(inst.address, inst.value8);
        index++;
      }
      break;

      case InstructionCode::ConstantWrite16:
      {
        DoMemoryWrite<u16>(inst.address, inst.value16);
        index++;
      }
      break;

      case InstructionCode::ExtConstantWrite32:
      {
        DoMemoryWrite<u32>(inst.address, inst.value32);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitSet8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address) | inst.value8;
        DoMemoryWrite<u8>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitSet16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address) | inst.value16;
        DoMemoryWrite<u16>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitSet32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address) | inst.value32;
        DoMemoryWrite<u32>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitClear8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address) & ~inst.value8;
        DoMemoryWrite<u8>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitClear16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address) & ~inst.value16;
        DoMemoryWrite<u16>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitClear32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address) & ~inst.value32;
        DoMemoryWrite<u32>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ScratchpadWrite16:
      {
        DoMemoryWrite<u16>(CPU::SCRATCHPAD_ADDR | (inst.address & CPU::SCRATCHPAD_OFFSET_MASK), inst.value16);
        index++;
      }
      break;

      case InstructionCode::ExtScratchpadWrite32:
      {
        DoMemoryWrite<u32>(CPU::SCRATCHPAD_ADDR | (inst.address & CPU::SCRATCHPAD_OFFSET_MASK), inst.value32);
        index++;
      }
      break;

      case InstructionCode::ExtIncrement32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address);
        DoMemoryWrite<u32>(inst.address, value + inst.value32);
        index++;
      }
      break;

      case InstructionCode::ExtDecrement32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address);
        DoMemoryWrite<u32>(inst.address, value - inst.value32);
        index++;
      }
      break;

      case InstructionCode::Increment16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        DoMemoryWrite<u16>(inst.address, value + inst.value16);
        index++;
      }
      break;

      case InstructionCode::Decrement16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        DoMemoryWrite<u16>(inst.address, value - inst.value16);
        index++;
      }
      break;

      case InstructionCode::Increment8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        DoMemoryWrite<u8>(inst.address, value + inst.value8);
        index++;
      }
      break;

      case InstructionCode::Decrement8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        DoMemoryWrite<u8>(inst.address, value - inst.value8);
        index++;
      }
      break;

      case InstructionCode::ExtCompareEqual32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address);
        if (value == inst.value32)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtCompareNotEqual32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address);
        if (value != inst.value32)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtCompareLess32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address);
        if (value < inst.value32)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtCompareGreater32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address);
        if (value > inst.value32)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtConstantWriteIfMatch16:
      case InstructionCode::ExtConstantWriteIfMatchWithRestore16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        const u16 comparevalue = Truncate16(inst.value32 >> 16);
        const u16 newvalue = Truncate16(inst.value32 & 0xFFFFu);
        if (value == comparevalue)
          DoMemoryWrite<u16>(inst.address, newvalue);

        index++;
      }
      break;

      case InstructionCode::ExtConstantWriteIfMatchWithRestore8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        const u8 comparevalue = Truncate8(inst.value16 >> 8);
        const u8 newvalue = Truncate8(inst.value16 & 0xFFu);
        if (value == comparevalue)
          DoMemoryWrite<u8>(inst.address, newvalue);

        index++;
      }
      break;

      case InstructionCode::ExtConstantForceRange8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        const u8 min = Truncate8(inst.value32 & 0x000000FFu);
        const u8 max = Truncate8((inst.value32 & 0x0000FF00u) >> 8);
        const u8 overmin = Truncate8((inst.value32 & 0x00FF0000u) >> 16);
        const u8 overmax = Truncate8((inst.value32 & 0xFF000000u) >> 24);
        if ((value < min) || (value < min && min == 0x00u && max < 0xFEu))
          DoMemoryWrite<u8>(inst.address, overmin); // also handles a min value of 0x00
        else if (value > max)
          DoMemoryWrite<u8>(inst.address, overmax);
        index++;
      }
      break;

      case InstructionCode::ExtConstantForceRangeLimits16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        const u16 min = Truncate16(inst.value32 & 0x0000FFFFu);
        const u16 max = Truncate16((inst.value32 & 0xFFFF0000u) >> 16);
        if ((value < min) || (value < min && min == 0x0000u && max < 0xFFFEu))
          DoMemoryWrite<u16>(inst.address, min); // also handles a min value of 0x0000
        else if (value > max)
          DoMemoryWrite<u16>(inst.address, max);
        index++;
      }
      break;

      case InstructionCode::ExtConstantForceRangeRollRound16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        const u16 min = Truncate16(inst.value32 & 0x0000FFFFu);
        const u16 max = Truncate16((inst.value32 & 0xFFFF0000u) >> 16);
        if ((value < min) || (value < min && min == 0x0000u && max < 0xFFFEu))
          DoMemoryWrite<u16>(inst.address, max); // also handles a min value of 0x0000
        else if (value > max)
          DoMemoryWrite<u16>(inst.address, min);
        index++;
      }
      break;

      case InstructionCode::ExtConstantForceRange16:
      {
        const u16 min = Truncate16(inst.value32 & 0x0000FFFFu);
        const u16 max = Truncate16((inst.value32 & 0xFFFF0000u) >> 16);
        const u16 value = DoMemoryRead<u16>(inst.address);
        const Instruction& inst2 = instructions[index + 1];
        const u16 overmin = Truncate16(inst2.value32 & 0x0000FFFFu);
        const u16 overmax = Truncate16((inst2.value32 & 0xFFFF0000u) >> 16);

        if ((value < min) || (value < min && min == 0x0000u && max < 0xFFFEu))
          DoMemoryWrite<u16>(inst.address, overmin); // also handles a min value of 0x0000
        else if (value > max)
          DoMemoryWrite<u16>(inst.address, overmax);
        index += 2;
      }
      break;

      case InstructionCode::ExtConstantSwap16:
      {
        const u16 value1 = Truncate16(inst.value32 & 0x0000FFFFu);
        const u16 value2 = Truncate16((inst.value32 & 0xFFFF0000u) >> 16);
        const u16 value = DoMemoryRead<u16>(inst.address);

        if (value == value1)
          DoMemoryWrite<u16>(inst.address, value2);
        else if (value == value2)
          DoMemoryWrite<u16>(inst.address, value1);
        index++;
      }
      break;

      case InstructionCode::ExtFindAndReplace:
      {

        if ((index + 4) >= instructions.size())
        {
          ERROR_LOG("Incomplete find/replace instruction");
          return;
        }
        const Instruction& inst2 = instructions[index + 1];
        const Instruction& inst3 = instructions[index + 2];
        const Instruction& inst4 = instructions[index + 3];
        const Instruction& inst5 = instructions[index + 4];

        const u32 offset = Truncate16(inst.value32 & 0x0000FFFFu) << 1;
        const u8 wildcard = Truncate8((inst.value32 & 0x00FF0000u) >> 16);
        const u32 minaddress = inst.address - offset;
        const u32 maxaddress = inst.address + offset;
        const u8 f1 = Truncate8((inst2.first & 0xFF000000u) >> 24);
        const u8 f2 = Truncate8((inst2.first & 0x00FF0000u) >> 16);
        const u8 f3 = Truncate8((inst2.first & 0x0000FF00u) >> 8);
        const u8 f4 = Truncate8(inst2.first & 0x000000FFu);
        const u8 f5 = Truncate8((inst2.value32 & 0xFF000000u) >> 24);
        const u8 f6 = Truncate8((inst2.value32 & 0x00FF0000u) >> 16);
        const u8 f7 = Truncate8((inst2.value32 & 0x0000FF00u) >> 8);
        const u8 f8 = Truncate8(inst2.value32 & 0x000000FFu);
        const u8 f9 = Truncate8((inst3.first & 0xFF000000u) >> 24);
        const u8 f10 = Truncate8((inst3.first & 0x00FF0000u) >> 16);
        const u8 f11 = Truncate8((inst3.first & 0x0000FF00u) >> 8);
        const u8 f12 = Truncate8(inst3.first & 0x000000FFu);
        const u8 f13 = Truncate8((inst3.value32 & 0xFF000000u) >> 24);
        const u8 f14 = Truncate8((inst3.value32 & 0x00FF0000u) >> 16);
        const u8 f15 = Truncate8((inst3.value32 & 0x0000FF00u) >> 8);
        const u8 f16 = Truncate8(inst3.value32 & 0x000000FFu);
        const u8 r1 = Truncate8((inst4.first & 0xFF000000u) >> 24);
        const u8 r2 = Truncate8((inst4.first & 0x00FF0000u) >> 16);
        const u8 r3 = Truncate8((inst4.first & 0x0000FF00u) >> 8);
        const u8 r4 = Truncate8(inst4.first & 0x000000FFu);
        const u8 r5 = Truncate8((inst4.value32 & 0xFF000000u) >> 24);
        const u8 r6 = Truncate8((inst4.value32 & 0x00FF0000u) >> 16);
        const u8 r7 = Truncate8((inst4.value32 & 0x0000FF00u) >> 8);
        const u8 r8 = Truncate8(inst4.value32 & 0x000000FFu);
        const u8 r9 = Truncate8((inst5.first & 0xFF000000u) >> 24);
        const u8 r10 = Truncate8((inst5.first & 0x00FF0000u) >> 16);
        const u8 r11 = Truncate8((inst5.first & 0x0000FF00u) >> 8);
        const u8 r12 = Truncate8(inst5.first & 0x000000FFu);
        const u8 r13 = Truncate8((inst5.value32 & 0xFF000000u) >> 24);
        const u8 r14 = Truncate8((inst5.value32 & 0x00FF0000u) >> 16);
        const u8 r15 = Truncate8((inst5.value32 & 0x0000FF00u) >> 8);
        const u8 r16 = Truncate8(inst5.value32 & 0x000000FFu);

        for (u32 address = minaddress; address <= maxaddress; address += 2)
        {
          if ((DoMemoryRead<u8>(address) == f1 || f1 == wildcard) &&
              (DoMemoryRead<u8>(address + 1) == f2 || f2 == wildcard) &&
              (DoMemoryRead<u8>(address + 2) == f3 || f3 == wildcard) &&
              (DoMemoryRead<u8>(address + 3) == f4 || f4 == wildcard) &&
              (DoMemoryRead<u8>(address + 4) == f5 || f5 == wildcard) &&
              (DoMemoryRead<u8>(address + 5) == f6 || f6 == wildcard) &&
              (DoMemoryRead<u8>(address + 6) == f7 || f7 == wildcard) &&
              (DoMemoryRead<u8>(address + 7) == f8 || f8 == wildcard) &&
              (DoMemoryRead<u8>(address + 8) == f9 || f9 == wildcard) &&
              (DoMemoryRead<u8>(address + 9) == f10 || f10 == wildcard) &&
              (DoMemoryRead<u8>(address + 10) == f11 || f11 == wildcard) &&
              (DoMemoryRead<u8>(address + 11) == f12 || f12 == wildcard) &&
              (DoMemoryRead<u8>(address + 12) == f13 || f13 == wildcard) &&
              (DoMemoryRead<u8>(address + 13) == f14 || f14 == wildcard) &&
              (DoMemoryRead<u8>(address + 14) == f15 || f15 == wildcard) &&
              (DoMemoryRead<u8>(address + 15) == f16 || f16 == wildcard))
          {
            if (r1 != wildcard)
              DoMemoryWrite<u8>(address, r1);
            if (r2 != wildcard)
              DoMemoryWrite<u8>(address + 1, r2);
            if (r3 != wildcard)
              DoMemoryWrite<u8>(address + 2, r3);
            if (r4 != wildcard)
              DoMemoryWrite<u8>(address + 3, r4);
            if (r5 != wildcard)
              DoMemoryWrite<u8>(address + 4, r5);
            if (r6 != wildcard)
              DoMemoryWrite<u8>(address + 5, r6);
            if (r7 != wildcard)
              DoMemoryWrite<u8>(address + 6, r7);
            if (r8 != wildcard)
              DoMemoryWrite<u8>(address + 7, r8);
            if (r9 != wildcard)
              DoMemoryWrite<u8>(address + 8, r9);
            if (r10 != wildcard)
              DoMemoryWrite<u8>(address + 9, r10);
            if (r11 != wildcard)
              DoMemoryWrite<u8>(address + 10, r11);
            if (r12 != wildcard)
              DoMemoryWrite<u8>(address + 11, r12);
            if (r13 != wildcard)
              DoMemoryWrite<u8>(address + 12, r13);
            if (r14 != wildcard)
              DoMemoryWrite<u8>(address + 13, r14);
            if (r15 != wildcard)
              DoMemoryWrite<u8>(address + 14, r15);
            if (r16 != wildcard)
              DoMemoryWrite<u8>(address + 15, r16);
            address = address + 16;
          }
        }
        index += 5;
      }
      break;

      case InstructionCode::CompareEqual16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        if (value == inst.value16)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareNotEqual16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        if (value != inst.value16)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareLess16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        if (value < inst.value16)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareGreater16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        if (value > inst.value16)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareEqual8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        if (value == inst.value8)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareNotEqual8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        if (value != inst.value8)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareLess8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        if (value < inst.value8)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareGreater8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        if (value > inst.value8)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareButtons: // D4
      {
        if (inst.value16 == GetControllerButtonBits())
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtCheatRegisters: // 51
      {
        const u32 poke_value = inst.value32;
        const u8 cht_reg_no1 = Truncate8(inst.address & 0xFFu);
        const u8 cht_reg_no2 = Truncate8((inst.address & 0xFF00u) >> 8);
        const u8 cht_reg_no3 = Truncate8(inst.value32 & 0xFFu);
        const u8 sub_type = Truncate8((inst.address & 0xFF0000u) >> 16);
        const u16 cht_offset = Truncate16((inst.value32 & 0xFFFF0000u) >> 16);

        switch (sub_type)
        {
          case 0x00: // Write the u8 from cht_register[cht_reg_no1] to address
            DoMemoryWrite<u8>(inst.value32, Truncate8(cht_register[cht_reg_no1]) & 0xFFu);
            break;
          case 0x01: // Read the u8 from address to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<u8>(inst.value32);
            break;
          case 0x02: // Write the u8 from address field to the address stored in cht_register[cht_reg_no1]
            DoMemoryWrite<u8>(cht_register[cht_reg_no1], Truncate8(poke_value & 0xFFu));
            break;
          case 0x03: // Write the u8 from cht_register[cht_reg_no2] to cht_register[cht_reg_no1]
            // and add the u8 from the address field to it
            cht_register[cht_reg_no1] = Truncate8(cht_register[cht_reg_no2] & 0xFFu) + Truncate8(poke_value & 0xFFu);
            break;
          case 0x04: // Write the u8 from the value stored in cht_register[cht_reg_no2] + poke_value to the address
            // stored in cht_register[cht_reg_no1]
            DoMemoryWrite<u8>(cht_register[cht_reg_no1],
                              Truncate8(cht_register[cht_reg_no2] & 0xFFu) + Truncate8(poke_value & 0xFFu));
            break;
          case 0x05: // Write the u8 poke value to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = Truncate8(poke_value & 0xFFu);
            break;
          case 0x06: // Read the u8 value from the address (cht_register[cht_reg_no2] + poke_value) to
            // cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<u8>(cht_register[cht_reg_no2] + poke_value);
            break;
          case 0x07: // Write the u8 poke_value to a specific index of a single array in a series of consecutive arrays
            // This cheat type requires a separate cheat to set up 4 consecutive cht_arrays before this will work
            // cht_register[cht_reg_no1] = the base address of the first element of the first array
            // cht_register[cht_reg_no1+1] = the array size (basically the address diff between the start of each array)
            // cht_register[cht_reg_no1+2] = the index of which array in the series to poke (this must be greater than
            // 0) cht_register[cht_reg_no1+3] must == 0xD0D0 to ensure it only pokes when the above cht_regs have been
            // set
            //                                     (safety valve)
            // cht_offset = the index of the individual array to change (so must be 0 to cht_register[cht_reg_no1+1])
            if ((cht_reg_no1 <= (std::size(cht_register) - 4)) && cht_register[cht_reg_no1 + 3] == 0xD0D0 &&
                cht_register[cht_reg_no1 + 2] > 0 && cht_register[cht_reg_no1 + 1] >= cht_offset)
            {
              DoMemoryWrite<u8>((cht_register[cht_reg_no1] - cht_register[cht_reg_no1 + 1]) +
                                  (cht_register[cht_reg_no1 + 1] * cht_register[cht_reg_no1 + 2]) + cht_offset,
                                Truncate8(poke_value & 0xFFu));
            }
            break;

          case 0x40: // Write the u16 from cht_register[cht_reg_no1] to address
            DoMemoryWrite<u16>(inst.value32, Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x41: // Read the u16 from address to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<u16>(inst.value32);
            break;
          case 0x42: // Write the u16 from address field to the address stored in cht_register[cht_reg_no1]
            DoMemoryWrite<u16>(cht_register[cht_reg_no1], Truncate16(poke_value & 0xFFFFu));
            break;
          case 0x43: // Write the u16 from cht_register[cht_reg_no2] to cht_register[cht_reg_no1]
            // and add the u16 from the address field to it
            cht_register[cht_reg_no1] =
              Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) + Truncate16(poke_value & 0xFFFFu);
            break;
          case 0x44: // Write the u16 from the value stored in cht_register[cht_reg_no2] + poke_value to the address
            // stored in cht_register[cht_reg_no1]
            DoMemoryWrite<u16>(cht_register[cht_reg_no1],
                               Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) + Truncate16(poke_value & 0xFFFFu));
            break;
          case 0x45: // Write the u16 poke value to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = Truncate16(poke_value & 0xFFFFu);
            break;
          case 0x46: // Read the u16 value from the address (cht_register[cht_reg_no2] + poke_value) to
            // cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<u16>(cht_register[cht_reg_no2] + poke_value);
            break;
          case 0x47: // Write the u16 poke_value to a specific index of a single array in a series of consecutive arrays
            // This cheat type requires a separate cheat to set up 4 consecutive cht_arrays before this will work
            // cht_register[cht_reg_no1] = the base address of the first element of the first array
            // cht_register[cht_reg_no1+1] = the array size (basically the address diff between the start of each array)
            // cht_register[cht_reg_no1+2] = the index of which array in the series to poke (this must be greater than
            // 0) cht_register[cht_reg_no1+3] must == 0xD0D0 to ensure it only pokes when the above cht_regs have been
            // set
            //                                     (safety valve)
            // cht_offset = the index of the individual array to change (so must be 0 to cht_register[cht_reg_no1+1])
            if ((cht_reg_no1 <= (std::size(cht_register) - 4)) && cht_register[cht_reg_no1 + 3] == 0xD0D0 &&
                cht_register[cht_reg_no1 + 2] > 0 && cht_register[cht_reg_no1 + 1] >= cht_offset)
            {
              DoMemoryWrite<u16>((cht_register[cht_reg_no1] - cht_register[cht_reg_no1 + 1]) +
                                   (cht_register[cht_reg_no1 + 1] * cht_register[cht_reg_no1 + 2]) + cht_offset,
                                 Truncate16(poke_value & 0xFFFFu));
            }
            break;

          case 0x80: // Write the u32 from cht_register[cht_reg_no1] to address
            DoMemoryWrite<u32>(inst.value32, cht_register[cht_reg_no1]);
            break;
          case 0x81: // Read the u32 from address to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<u32>(inst.value32);
            break;
          case 0x82: // Write the u32 from address field to the address stored in cht_register[cht_reg_no]
            DoMemoryWrite<u32>(cht_register[cht_reg_no1], poke_value);
            break;
          case 0x83: // Write the u32 from cht_register[cht_reg_no2] to cht_register[cht_reg_no1]
            // and add the u32 from the address field to it
            cht_register[cht_reg_no1] = cht_register[cht_reg_no2] + poke_value;
            break;
          case 0x84: // Write the u32 from the value stored in cht_register[cht_reg_no2] + poke_value to the address
            // stored in cht_register[cht_reg_no1]
            DoMemoryWrite<u32>(cht_register[cht_reg_no1], cht_register[cht_reg_no2] + poke_value);
            break;
          case 0x85: // Write the u32 poke value to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = poke_value;
            break;
          case 0x86: // Read the u32 value from the address (cht_register[cht_reg_no2] + poke_value) to
            // cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<u32>(cht_register[cht_reg_no2] + poke_value);
            break;
            // Do not use 0x87 as it's not possible to duplicate 0x07, 0x47 for a 32 bit write as not enough characters

          case 0xC0: // Reg3 = Reg2 + Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] + cht_register[cht_reg_no1];
            break;
          case 0xC1: // Reg3 = Reg2 - Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] - cht_register[cht_reg_no1];
            break;
          case 0xC2: // Reg3 = Reg2 * Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] * cht_register[cht_reg_no1];
            break;
          case 0xC3: // Reg3 = Reg2 / Reg1 with DIV0 handling
            if (cht_register[cht_reg_no1] == 0)
              cht_register[cht_reg_no3] = 0;
            else
              cht_register[cht_reg_no3] = cht_register[cht_reg_no2] / cht_register[cht_reg_no1];
            break;
          case 0xC4: // Reg3 = Reg2 % Reg1 (with DIV0 handling)
            if (cht_register[cht_reg_no1] == 0)
              cht_register[cht_reg_no3] = cht_register[cht_reg_no2];
            else
              cht_register[cht_reg_no3] = cht_register[cht_reg_no2] % cht_register[cht_reg_no1];
            break;
          case 0xC5: // Reg3 = Reg2 & Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] & cht_register[cht_reg_no1];
            break;
          case 0xC6: // Reg3 = Reg2 | Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] | cht_register[cht_reg_no1];
            break;
          case 0xC7: // Reg3 = Reg2 ^ Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] ^ cht_register[cht_reg_no1];
            break;
          case 0xC8: // Reg3 = ~Reg1
            cht_register[cht_reg_no3] = ~cht_register[cht_reg_no1];
            break;
          case 0xC9: // Reg3 = Reg1 << X
            cht_register[cht_reg_no3] = cht_register[cht_reg_no1] << cht_reg_no2;
            break;
          case 0xCA: // Reg3 = Reg1 >> X
            cht_register[cht_reg_no3] = cht_register[cht_reg_no1] >> cht_reg_no2;
            break;
            // Lots of options exist for expanding into this space
          default:
            break;
        }
        index++;
      }
      break;

      case InstructionCode::SkipIfNotEqual16:      // C0
      case InstructionCode::ExtSkipIfNotEqual32:   // A4
      case InstructionCode::SkipIfButtonsNotEqual: // D5
      case InstructionCode::SkipIfButtonsEqual:    // D6
      case InstructionCode::ExtSkipIfNotLess8:     // C3
      case InstructionCode::ExtSkipIfNotGreater8:  // C4
      case InstructionCode::ExtSkipIfNotLess16:    // C5
      case InstructionCode::ExtSkipIfNotGreater16: // C6
      case InstructionCode::ExtMultiConditionals:  // F6
      {
        index++;

        bool activate_codes;
        switch (inst.code)
        {
          case InstructionCode::SkipIfNotEqual16: // C0
            activate_codes = (DoMemoryRead<u16>(inst.address) == inst.value16);
            break;
          case InstructionCode::ExtSkipIfNotEqual32: // A4
            activate_codes = (DoMemoryRead<u32>(inst.address) == inst.value32);
            break;
          case InstructionCode::SkipIfButtonsNotEqual: // D5
            activate_codes = (GetControllerButtonBits() == inst.value16);
            break;
          case InstructionCode::SkipIfButtonsEqual: // D6
            activate_codes = (GetControllerButtonBits() != inst.value16);
            break;
          case InstructionCode::ExtSkipIfNotLess8: // C3
            activate_codes = (DoMemoryRead<u8>(inst.address) < inst.value8);
            break;
          case InstructionCode::ExtSkipIfNotGreater8: // C4
            activate_codes = (DoMemoryRead<u8>(inst.address) > inst.value8);
            break;
          case InstructionCode::ExtSkipIfNotLess16: // C5
            activate_codes = (DoMemoryRead<u16>(inst.address) < inst.value16);
            break;
          case InstructionCode::ExtSkipIfNotGreater16: // C6
            activate_codes = (DoMemoryRead<u16>(inst.address) > inst.value16);
            break;
          case InstructionCode::ExtMultiConditionals: // F6
          {
            // Ensure any else if or else that are hit outside the if context are skipped
            if ((inst.value32 & 0xFFFFFF00u) != 0x1F000000)
            {
              activate_codes = false;
              break;
            }
            for (;;)
            {
              const u8 totalConds = Truncate8(instructions[index - 1].value32 & 0x000000FFu);
              const u8 conditionType = Truncate8(instructions[index - 1].address & 0x000000FFu);

              bool conditions_check;

              if (conditionType == 0x00 && totalConds > 0) // AND
              {
                conditions_check = true;

                for (int i = 1; totalConds >= i; index++, i++)
                {
                  switch (instructions[index].code)
                  {
                    case InstructionCode::CompareEqual16: // D0
                      conditions_check &=
                        (DoMemoryRead<u16>(instructions[index].address) == instructions[index].value16);
                      break;
                    case InstructionCode::CompareNotEqual16: // D1
                      conditions_check &=
                        (DoMemoryRead<u16>(instructions[index].address) != instructions[index].value16);
                      break;
                    case InstructionCode::CompareLess16: // D2
                      conditions_check &=
                        (DoMemoryRead<u16>(instructions[index].address) < instructions[index].value16);
                      break;
                    case InstructionCode::CompareGreater16: // D3
                      conditions_check &=
                        (DoMemoryRead<u16>(instructions[index].address) > instructions[index].value16);
                      break;
                    case InstructionCode::CompareEqual8: // E0
                      conditions_check &= (DoMemoryRead<u8>(instructions[index].address) == instructions[index].value8);
                      break;
                    case InstructionCode::CompareNotEqual8: // E1
                      conditions_check &= (DoMemoryRead<u8>(instructions[index].address) != instructions[index].value8);
                      break;
                    case InstructionCode::CompareLess8: // E2
                      conditions_check &= (DoMemoryRead<u8>(instructions[index].address) < instructions[index].value8);
                      break;
                    case InstructionCode::CompareGreater8: // E3
                      conditions_check &= (DoMemoryRead<u8>(instructions[index].address) > instructions[index].value8);
                      break;
                    case InstructionCode::ExtCompareEqual32: // A0
                      conditions_check &=
                        (DoMemoryRead<u32>(instructions[index].address) == instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareNotEqual32: // A1
                      conditions_check &=
                        (DoMemoryRead<u32>(instructions[index].address) != instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareLess32: // A2
                      conditions_check &=
                        (DoMemoryRead<u32>(instructions[index].address) < instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareGreater32: // A3
                      conditions_check &=
                        (DoMemoryRead<u32>(instructions[index].address) > instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareBitsSet8: // E4 Internal to F6
                      conditions_check &=
                        (instructions[index].value8 ==
                         (DoMemoryRead<u8>(instructions[index].address) & instructions[index].value8));
                      break;
                    case InstructionCode::ExtCompareBitsClear8: // E5 Internal to F6
                      conditions_check &=
                        ((DoMemoryRead<u8>(instructions[index].address) & instructions[index].value8) == 0);
                      break;
                    case InstructionCode::ExtBitCompareButtons: // D7
                    {
                      const u32 frame_compare_value = instructions[index].address & 0xFFFFu;
                      const u8 cht_reg_no = Truncate8((instructions[index].value32 & 0xFF000000u) >> 24);
                      const bool bit_comparison_type = ((instructions[index].address & 0x100000u) >> 20);
                      const u8 frame_comparison = Truncate8((instructions[index].address & 0xF0000u) >> 16);
                      const u32 check_value = (instructions[index].value32 & 0xFFFFFFu);
                      const u32 value1 = GetControllerButtonBits();
                      const u32 value2 = GetControllerAnalogBits();
                      u32 value = value1 | value2;

                      if ((bit_comparison_type == false && check_value == (value & check_value)) // Check Bits are set
                          ||
                          (bit_comparison_type == true && check_value != (value & check_value))) // Check Bits are clear
                      {
                        cht_register[cht_reg_no] += 1;
                        switch (frame_comparison)
                        {
                          case 0x0: // No comparison on frame count, just do it
                            conditions_check &= true;
                            break;
                          case 0x1: // Check if frame_compare_value == current count
                            conditions_check &= (cht_register[cht_reg_no] == frame_compare_value);
                            break;
                          case 0x2: // Check if frame_compare_value < current count
                            conditions_check &= (cht_register[cht_reg_no] < frame_compare_value);
                            break;
                          case 0x3: // Check if frame_compare_value > current count
                            conditions_check &= (cht_register[cht_reg_no] > frame_compare_value);
                            break;
                          case 0x4: // Check if frame_compare_value != current count
                            conditions_check &= (cht_register[cht_reg_no] != frame_compare_value);
                            break;
                          default:
                            conditions_check &= false;
                            break;
                        }
                      }
                      else
                      {
                        cht_register[cht_reg_no] = 0;
                        conditions_check &= false;
                      }
                      break;
                    }
                    default:
                      ERROR_LOG("Incorrect conditional instruction (see chtdb.txt for supported instructions)");
                      return;
                  }
                }
              }
              else if (conditionType == 0x01 && totalConds > 0) // OR
              {
                conditions_check = false;

                for (int i = 1; totalConds >= i; index++, i++)
                {
                  switch (instructions[index].code)
                  {
                    case InstructionCode::CompareEqual16: // D0
                      conditions_check |=
                        (DoMemoryRead<u16>(instructions[index].address) == instructions[index].value16);
                      break;
                    case InstructionCode::CompareNotEqual16: // D1
                      conditions_check |=
                        (DoMemoryRead<u16>(instructions[index].address) != instructions[index].value16);
                      break;
                    case InstructionCode::CompareLess16: // D2
                      conditions_check |=
                        (DoMemoryRead<u16>(instructions[index].address) < instructions[index].value16);
                      break;
                    case InstructionCode::CompareGreater16: // D3
                      conditions_check |=
                        (DoMemoryRead<u16>(instructions[index].address) > instructions[index].value16);
                      break;
                    case InstructionCode::CompareEqual8: // E0
                      conditions_check |= (DoMemoryRead<u8>(instructions[index].address) == instructions[index].value8);
                      break;
                    case InstructionCode::CompareNotEqual8: // E1
                      conditions_check |= (DoMemoryRead<u8>(instructions[index].address) != instructions[index].value8);
                      break;
                    case InstructionCode::CompareLess8: // E2
                      conditions_check |= (DoMemoryRead<u8>(instructions[index].address) < instructions[index].value8);
                      break;
                    case InstructionCode::CompareGreater8: // E3
                      conditions_check |= (DoMemoryRead<u8>(instructions[index].address) > instructions[index].value8);
                      break;
                    case InstructionCode::ExtCompareEqual32: // A0
                      conditions_check |=
                        (DoMemoryRead<u32>(instructions[index].address) == instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareNotEqual32: // A1
                      conditions_check |=
                        (DoMemoryRead<u32>(instructions[index].address) != instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareLess32: // A2
                      conditions_check |=
                        (DoMemoryRead<u32>(instructions[index].address) < instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareGreater32: // A3
                      conditions_check |=
                        (DoMemoryRead<u32>(instructions[index].address) > instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareBitsSet8: // E4 Internal to F6
                      conditions_check |=
                        (instructions[index].value8 ==
                         (DoMemoryRead<u8>(instructions[index].address) & instructions[index].value8));
                      break;
                    case InstructionCode::ExtCompareBitsClear8: // E5 Internal to F6
                      conditions_check |=
                        ((DoMemoryRead<u8>(instructions[index].address) & instructions[index].value8) == 0);
                      break;
                    case InstructionCode::ExtBitCompareButtons: // D7
                    {
                      const u32 frame_compare_value = instructions[index].address & 0xFFFFu;
                      const u8 cht_reg_no = Truncate8((instructions[index].value32 & 0xFF000000u) >> 24);
                      const bool bit_comparison_type = ((instructions[index].address & 0x100000u) >> 20);
                      const u8 frame_comparison = Truncate8((instructions[index].address & 0xF0000u) >> 16);
                      const u32 check_value = (instructions[index].value32 & 0xFFFFFFu);
                      const u32 value1 = GetControllerButtonBits();
                      const u32 value2 = GetControllerAnalogBits();
                      u32 value = value1 | value2;

                      if ((bit_comparison_type == false && check_value == (value & check_value)) // Check Bits are set
                          ||
                          (bit_comparison_type == true && check_value != (value & check_value))) // Check Bits are clear
                      {
                        cht_register[cht_reg_no] += 1;
                        switch (frame_comparison)
                        {
                          case 0x0: // No comparison on frame count, just do it
                            conditions_check |= true;
                            break;
                          case 0x1: // Check if frame_compare_value == current count
                            conditions_check |= (cht_register[cht_reg_no] == frame_compare_value);
                            break;
                          case 0x2: // Check if frame_compare_value < current count
                            conditions_check |= (cht_register[cht_reg_no] < frame_compare_value);
                            break;
                          case 0x3: // Check if frame_compare_value > current count
                            conditions_check |= (cht_register[cht_reg_no] > frame_compare_value);
                            break;
                          case 0x4: // Check if frame_compare_value != current count
                            conditions_check |= (cht_register[cht_reg_no] != frame_compare_value);
                            break;
                          default:
                            conditions_check |= false;
                            break;
                        }
                      }
                      else
                      {
                        cht_register[cht_reg_no] = 0;
                        conditions_check |= false;
                      }
                      break;
                    }
                    default:
                      ERROR_LOG("Incorrect conditional instruction (see chtdb.txt for supported instructions)");
                      return;
                  }
                }
              }
              else
              {
                ERROR_LOG("Incomplete multi conditional instruction");
                return;
              }
              if (conditions_check == true)
              {
                activate_codes = true;
                break;
              }
              else
              { // parse through to 00000000 FFFF and peek if next line is a F6 type associated with a ELSE
                activate_codes = false;
                // skip to the next separator (00000000 FFFF), or end
                constexpr u64 separator_value = UINT64_C(0x000000000000FFFF);
                constexpr u64 else_value = UINT64_C(0x00000000E15E0000);
                constexpr u64 elseif_value = UINT64_C(0x00000000E15E1F00);
                while (index < count)
                {
                  const u64 bits = instructions[index++].bits;
                  if (bits == separator_value)
                  {
                    const u64 bits_ahead = instructions[index].bits;
                    if ((bits_ahead & 0xFFFFFF00u) == elseif_value)
                    {
                      break;
                    }
                    if ((bits_ahead & 0xFFFF0000u) == else_value)
                    {
                      // index++;
                      activate_codes = true;
                      break;
                    }
                    index--;
                    break;
                  }
                  if ((bits & 0xFFFFFF00u) == elseif_value)
                  {
                    // index--;
                    break;
                  }
                  if ((bits & 0xFFFFFFFFu) == else_value)
                  {
                    // index++;
                    activate_codes = true;
                    break;
                  }
                }
                if (activate_codes == true)
                  break;
              }
            }
            break;
          }
          default:
            activate_codes = false;
            break;
        }

        if (activate_codes)
        {
          // execute following instructions
          continue;
        }

        // skip to the next separator (00000000 FFFF), or end
        constexpr u64 separator_value = UINT64_C(0x000000000000FFFF);
        while (index < count)
        {
          // we don't want to execute the separator instruction
          const u64 bits = instructions[index++].bits;
          if (bits == separator_value)
            break;
        }
      }
      break;

      case InstructionCode::ExtBitCompareButtons: // D7
      {
        index++;
        bool activate_codes;
        const u32 frame_compare_value = inst.address & 0xFFFFu;
        const u8 cht_reg_no = Truncate8((inst.value32 & 0xFF000000u) >> 24);
        const bool bit_comparison_type = ((inst.address & 0x100000u) >> 20);
        const u8 frame_comparison = Truncate8((inst.address & 0xF0000u) >> 16);
        const u32 check_value = (inst.value32 & 0xFFFFFFu);
        const u32 value1 = GetControllerButtonBits();
        const u32 value2 = GetControllerAnalogBits();
        u32 value = value1 | value2;

        if ((bit_comparison_type == false && check_value == (value & check_value))    // Check Bits are set
            || (bit_comparison_type == true && check_value != (value & check_value))) // Check Bits are clear
        {
          cht_register[cht_reg_no] += 1;
          switch (frame_comparison)
          {
            case 0x0: // No comparison on frame count, just do it
              activate_codes = true;
              break;
            case 0x1: // Check if frame_compare_value == current count
              activate_codes = (cht_register[cht_reg_no] == frame_compare_value);
              break;
            case 0x2: // Check if frame_compare_value < current count
              activate_codes = (cht_register[cht_reg_no] < frame_compare_value);
              break;
            case 0x3: // Check if frame_compare_value > current count
              activate_codes = (cht_register[cht_reg_no] > frame_compare_value);
              break;
            case 0x4: // Check if frame_compare_value != current count
              activate_codes = (cht_register[cht_reg_no] != frame_compare_value);
              break;
            default:
              activate_codes = false;
              break;
          }
        }
        else
        {
          cht_register[cht_reg_no] = 0;
          activate_codes = false;
        }

        if (activate_codes)
        {
          // execute following instructions
          continue;
        }

        // skip to the next separator (00000000 FFFF), or end
        constexpr u64 separator_value = UINT64_C(0x000000000000FFFF);
        while (index < count)
        {
          // we don't want to execute the separator instruction
          const u64 bits = instructions[index++].bits;
          if (bits == separator_value)
            break;
        }
      }
      break;

      case InstructionCode::ExtCheatRegistersCompare: // 52
      {
        index++;
        bool activate_codes = false;
        const u8 cht_reg_no1 = Truncate8(inst.address & 0xFFu);
        const u8 cht_reg_no2 = Truncate8((inst.address & 0xFF00u) >> 8);
        const u8 sub_type = Truncate8((inst.first & 0xFF0000u) >> 16);

        switch (sub_type)
        {
          case 0x00:
            activate_codes =
              (Truncate8(cht_register[cht_reg_no2] & 0xFFu) == Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x01:
            activate_codes =
              (Truncate8(cht_register[cht_reg_no2] & 0xFFu) != Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x02:
            activate_codes =
              (Truncate8(cht_register[cht_reg_no2] & 0xFFu) > Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x03:
            activate_codes =
              (Truncate8(cht_register[cht_reg_no2] & 0xFFu) >= Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x04:
            activate_codes =
              (Truncate8(cht_register[cht_reg_no2] & 0xFFu) < Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x05:
            activate_codes =
              (Truncate8(cht_register[cht_reg_no2] & 0xFFu) <= Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x06:
            activate_codes =
              ((Truncate8(cht_register[cht_reg_no2] & 0xFFu) & Truncate8(cht_register[cht_reg_no1] & 0xFFu)) ==
               (Truncate8(cht_register[cht_reg_no1] & 0xFFu)));
            break;
          case 0x07:
            activate_codes =
              ((Truncate8(cht_register[cht_reg_no2] & 0xFFu) & Truncate8(cht_register[cht_reg_no1] & 0xFFu)) !=
               (Truncate8(cht_register[cht_reg_no1] & 0xFFu)));
            break;
          case 0x0A:
            activate_codes =
              ((Truncate8(cht_register[cht_reg_no2] & 0xFFu) & Truncate8(cht_register[cht_reg_no1] & 0xFFu)) ==
               (Truncate8(cht_register[cht_reg_no2] & 0xFFu)));
            break;
          case 0x0B:
            activate_codes =
              ((Truncate8(cht_register[cht_reg_no2] & 0xFFu) & Truncate8(cht_register[cht_reg_no1] & 0xFFu)) !=
               (Truncate8(cht_register[cht_reg_no2] & 0xFFu)));
            break;
          case 0x10:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) == inst.value8);
            break;
          case 0x11:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) != inst.value8);
            break;
          case 0x12:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) > inst.value8);
            break;
          case 0x13:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) >= inst.value8);
            break;
          case 0x14:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) < inst.value8);
            break;
          case 0x15:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) <= inst.value8);
            break;
          case 0x16:
            activate_codes = ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) & inst.value8) == inst.value8);
            break;
          case 0x17:
            activate_codes = ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) & inst.value8) != inst.value8);
            break;
          case 0x18:
            activate_codes =
              ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) > inst.value8) &&
               (Truncate8(cht_register[cht_reg_no1] & 0xFFu) < Truncate8((inst.value32 & 0xFF0000u) >> 16)));
            break;
          case 0x19:
            activate_codes =
              ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) >= inst.value8) &&
               (Truncate8(cht_register[cht_reg_no1] & 0xFFu) <= Truncate8((inst.value32 & 0xFF0000u) >> 16)));
            break;
          case 0x1A:
            activate_codes = ((Truncate8(cht_register[cht_reg_no2] & 0xFFu) & inst.value8) ==
                              Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x1B:
            activate_codes = ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) & inst.value8) !=
                              Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x20:
            activate_codes =
              (DoMemoryRead<u8>(cht_register[cht_reg_no2]) == DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x21:
            activate_codes =
              (DoMemoryRead<u8>(cht_register[cht_reg_no2]) != DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x22:
            activate_codes =
              (DoMemoryRead<u8>(cht_register[cht_reg_no2]) > DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x23:
            activate_codes =
              (DoMemoryRead<u8>(cht_register[cht_reg_no2]) >= DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x24:
            activate_codes =
              (DoMemoryRead<u8>(cht_register[cht_reg_no2]) < DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x25:
            activate_codes =
              (DoMemoryRead<u8>(cht_register[cht_reg_no2]) <= DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x26:
            activate_codes = ((DoMemoryRead<u8>(cht_register[cht_reg_no1]) & inst.value8) == inst.value8);
            break;
          case 0x27:
            activate_codes = ((DoMemoryRead<u8>(cht_register[cht_reg_no1]) & inst.value8) != inst.value8);
            break;
          case 0x28:
            activate_codes =
              ((DoMemoryRead<u8>(cht_register[cht_reg_no1]) > inst.value8) &&
               (DoMemoryRead<u8>(cht_register[cht_reg_no1]) < Truncate8((inst.value32 & 0xFF0000u) >> 16)));
            break;
          case 0x29:
            activate_codes =
              ((DoMemoryRead<u8>(cht_register[cht_reg_no1]) >= inst.value8) &&
               (DoMemoryRead<u8>(cht_register[cht_reg_no1]) <= Truncate8((inst.value32 & 0xFF0000u) >> 16)));
            break;
          case 0x2A:
            activate_codes = ((DoMemoryRead<u8>(cht_register[cht_reg_no1]) & inst.value8) ==
                              DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x2B:
            activate_codes = ((DoMemoryRead<u8>(cht_register[cht_reg_no1]) & inst.value8) !=
                              DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x30:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) == DoMemoryRead<u8>(inst.value32));
            break;
          case 0x31:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) != DoMemoryRead<u8>(inst.value32));
            break;
          case 0x32:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) > DoMemoryRead<u8>(inst.value32));
            break;
          case 0x33:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) >= DoMemoryRead<u8>(inst.value32));
            break;
          case 0x34:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) < DoMemoryRead<u8>(inst.value32));
            break;
          case 0x36:
            activate_codes = ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) & DoMemoryRead<u8>(inst.value32)) ==
                              DoMemoryRead<u8>(inst.value32));
            break;
          case 0x37:
            activate_codes = ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) & DoMemoryRead<u8>(inst.value32)) !=
                              DoMemoryRead<u8>(inst.value32));
            break;
          case 0x3A:
            activate_codes = ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) & DoMemoryRead<u8>(inst.value32)) ==
                              Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x3B:
            activate_codes = ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) & DoMemoryRead<u8>(inst.value32)) !=
                              Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x40:
            activate_codes =
              (Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) == Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x41:
            activate_codes =
              (Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) != Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x42:
            activate_codes =
              (Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) > Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x43:
            activate_codes =
              (Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) >= Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x44:
            activate_codes =
              (Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) < Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x45:
            activate_codes =
              (Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) <= Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x46:
            activate_codes =
              ((Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) & Truncate16(cht_register[cht_reg_no1] & 0xFFFFu)) ==
               Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x47:
            activate_codes =
              ((Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) & Truncate16(cht_register[cht_reg_no1] & 0xFFFFu)) !=
               Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x4A:
            activate_codes =
              ((Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) & Truncate16(cht_register[cht_reg_no1] & 0xFFFFu)) ==
               Truncate16(cht_register[cht_reg_no2] & 0xFFFFu));
            break;
          case 0x4B:
            activate_codes =
              ((Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) & Truncate16(cht_register[cht_reg_no1] & 0xFFFFu)) !=
               Truncate16(cht_register[cht_reg_no2] & 0xFFFFu));
            break;
          case 0x50:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) == inst.value16);
            break;
          case 0x51:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) != inst.value16);
            break;
          case 0x52:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) > inst.value16);
            break;
          case 0x53:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) >= inst.value16);
            break;
          case 0x54:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) < inst.value16);
            break;
          case 0x55:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) <= inst.value16);
            break;
          case 0x56:
            activate_codes = ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) & inst.value16) == inst.value16);
            break;
          case 0x57:
            activate_codes = ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) & inst.value16) != inst.value16);
            break;
          case 0x58:
            activate_codes =
              ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) > inst.value16) &&
               (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) < Truncate16((inst.value32 & 0xFFFF0000u) >> 16)));
            break;
          case 0x59:
            activate_codes =
              ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) >= inst.value16) &&
               (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) <= Truncate16((inst.value32 & 0xFFFF0000u) >> 16)));
            break;
          case 0x5A:
            activate_codes = ((Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) & inst.value16) ==
                              Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x5B:
            activate_codes = ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) & inst.value16) !=
                              Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x60:
            activate_codes =
              (DoMemoryRead<u16>(cht_register[cht_reg_no2]) == DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x61:
            activate_codes =
              (DoMemoryRead<u16>(cht_register[cht_reg_no2]) != DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x62:
            activate_codes =
              (DoMemoryRead<u16>(cht_register[cht_reg_no2]) > DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x63:
            activate_codes =
              (DoMemoryRead<u16>(cht_register[cht_reg_no2]) >= DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x64:
            activate_codes =
              (DoMemoryRead<u16>(cht_register[cht_reg_no2]) < DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x65:
            activate_codes =
              (DoMemoryRead<u16>(cht_register[cht_reg_no2]) <= DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x66:
            activate_codes = ((DoMemoryRead<u16>(cht_register[cht_reg_no1]) & inst.value16) == inst.value16);
            break;
          case 0x67:
            activate_codes = ((DoMemoryRead<u16>(cht_register[cht_reg_no1]) & inst.value16) != inst.value16);
            break;
          case 0x68:
            activate_codes =
              ((DoMemoryRead<u16>(cht_register[cht_reg_no1]) > inst.value16) &&
               (DoMemoryRead<u16>(cht_register[cht_reg_no1]) < Truncate16((inst.value32 & 0xFFFF0000u) >> 16)));
            break;
          case 0x69:
            activate_codes =
              ((DoMemoryRead<u16>(cht_register[cht_reg_no1]) >= inst.value16) &&
               (DoMemoryRead<u16>(cht_register[cht_reg_no1]) <= Truncate16((inst.value32 & 0xFFFF0000u) >> 16)));
            break;
          case 0x6A:
            activate_codes = ((DoMemoryRead<u16>(cht_register[cht_reg_no1]) & inst.value16) ==
                              DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x6B:
            activate_codes = ((DoMemoryRead<u16>(cht_register[cht_reg_no1]) & inst.value16) !=
                              DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x70:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) == DoMemoryRead<u16>(inst.value32));
            break;
          case 0x71:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) != DoMemoryRead<u16>(inst.value32));
            break;
          case 0x72:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) > DoMemoryRead<u16>(inst.value32));
            break;
          case 0x73:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) >= DoMemoryRead<u16>(inst.value32));
            break;
          case 0x74:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) < DoMemoryRead<u16>(inst.value32));
            break;
          case 0x76:
            activate_codes = ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) & DoMemoryRead<u16>(inst.value32)) ==
                              DoMemoryRead<u16>(inst.value32));
            break;
          case 0x77:
            activate_codes = ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) & DoMemoryRead<u16>(inst.value32)) !=
                              DoMemoryRead<u16>(inst.value32));
            break;
          case 0x7A:
            activate_codes = ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) & DoMemoryRead<u16>(inst.value32)) ==
                              Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x7B:
            activate_codes = ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) & DoMemoryRead<u16>(inst.value32)) !=
                              Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x80:
            activate_codes = (cht_register[cht_reg_no2] == cht_register[cht_reg_no1]);
            break;
          case 0x81:
            activate_codes = (cht_register[cht_reg_no2] != cht_register[cht_reg_no1]);
            break;
          case 0x82:
            activate_codes = (cht_register[cht_reg_no2] > cht_register[cht_reg_no1]);
            break;
          case 0x83:
            activate_codes = (cht_register[cht_reg_no2] >= cht_register[cht_reg_no1]);
            break;
          case 0x84:
            activate_codes = (cht_register[cht_reg_no2] < cht_register[cht_reg_no1]);
            break;
          case 0x85:
            activate_codes = (cht_register[cht_reg_no2] <= cht_register[cht_reg_no1]);
            break;
          case 0x86:
            activate_codes = ((cht_register[cht_reg_no2] & cht_register[cht_reg_no1]) == cht_register[cht_reg_no1]);
            break;
          case 0x87:
            activate_codes = ((cht_register[cht_reg_no2] & cht_register[cht_reg_no1]) != cht_register[cht_reg_no1]);
            break;
          case 0x8A:
            activate_codes = ((cht_register[cht_reg_no2] & cht_register[cht_reg_no1]) == cht_register[cht_reg_no2]);
            break;
          case 0x8B:
            activate_codes = ((cht_register[cht_reg_no2] & cht_register[cht_reg_no1]) != cht_register[cht_reg_no2]);
            break;
          case 0x90:
            activate_codes = (cht_register[cht_reg_no1] == inst.value32);
            break;
          case 0x91:
            activate_codes = (cht_register[cht_reg_no1] != inst.value32);
            break;
          case 0x92:
            activate_codes = (cht_register[cht_reg_no1] > inst.value32);
            break;
          case 0x93:
            activate_codes = (cht_register[cht_reg_no1] >= inst.value32);
            break;
          case 0x94:
            activate_codes = (cht_register[cht_reg_no1] < inst.value32);
            break;
          case 0x95:
            activate_codes = (cht_register[cht_reg_no1] <= inst.value32);
            break;
          case 0x96:
            activate_codes = ((cht_register[cht_reg_no1] & inst.value32) == inst.value32);
            break;
          case 0x97:
            activate_codes = ((cht_register[cht_reg_no1] & inst.value32) != inst.value32);
            break;
          case 0x9A:
            activate_codes = ((cht_register[cht_reg_no2] & inst.value32) == cht_register[cht_reg_no1]);
            break;
          case 0x9B:
            activate_codes = ((cht_register[cht_reg_no1] & inst.value32) != cht_register[cht_reg_no1]);
            break;
          case 0xA0:
            activate_codes =
              (DoMemoryRead<u32>(cht_register[cht_reg_no2]) == DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xA1:
            activate_codes =
              (DoMemoryRead<u32>(cht_register[cht_reg_no2]) != DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xA2:
            activate_codes =
              (DoMemoryRead<u32>(cht_register[cht_reg_no2]) > DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xA3:
            activate_codes =
              (DoMemoryRead<u32>(cht_register[cht_reg_no2]) >= DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xA4:
            activate_codes =
              (DoMemoryRead<u32>(cht_register[cht_reg_no2]) < DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xA5:
            activate_codes =
              (DoMemoryRead<u32>(cht_register[cht_reg_no2]) <= DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xA6:
            activate_codes = ((DoMemoryRead<u32>(cht_register[cht_reg_no1]) & inst.value32) == inst.value32);
            break;
          case 0xA7:
            activate_codes = ((DoMemoryRead<u32>(cht_register[cht_reg_no1]) & inst.value32) != inst.value32);
            break;
          case 0xAA:
            activate_codes = ((DoMemoryRead<u32>(cht_register[cht_reg_no1]) & inst.value32) ==
                              DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xAB:
            activate_codes = ((DoMemoryRead<u32>(cht_register[cht_reg_no1]) & inst.value32) !=
                              DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xB0:
            activate_codes = (cht_register[cht_reg_no1] == DoMemoryRead<u32>(inst.value32));
            break;
          case 0xB1:
            activate_codes = (cht_register[cht_reg_no1] != DoMemoryRead<u32>(inst.value32));
            break;
          case 0xB2:
            activate_codes = (cht_register[cht_reg_no1] > DoMemoryRead<u32>(inst.value32));
            break;
          case 0xB3:
            activate_codes = (cht_register[cht_reg_no1] >= DoMemoryRead<u32>(inst.value32));
            break;
          case 0xB4:
            activate_codes = (cht_register[cht_reg_no1] < DoMemoryRead<u32>(inst.value32));
            break;
          case 0xB6:
            activate_codes =
              ((cht_register[cht_reg_no1] & DoMemoryRead<u32>(inst.value32)) == DoMemoryRead<u32>(inst.value32));
            break;
          case 0xB7:
            activate_codes =
              ((cht_register[cht_reg_no1] & DoMemoryRead<u32>(inst.value32)) != DoMemoryRead<u32>(inst.value32));
            break;
          case 0xBA:
            activate_codes =
              ((cht_register[cht_reg_no1] & DoMemoryRead<u32>(inst.value32)) == cht_register[cht_reg_no1]);
            break;
          case 0xBB:
            activate_codes =
              ((cht_register[cht_reg_no1] & DoMemoryRead<u32>(inst.value32)) != cht_register[cht_reg_no1]);
            break;
          default:
            activate_codes = false;
            break;
        }
        if (activate_codes)
        {
          // execute following instructions
          continue;
        }

        // skip to the next separator (00000000 FFFF), or end
        constexpr u64 separator_value = UINT64_C(0x000000000000FFFF);
        while (index < count)
        {
          // we don't want to execute the separator instruction
          const u64 bits = instructions[index++].bits;
          if (bits == separator_value)
            break;
        }
      }
      break;

      case InstructionCode::DelayActivation: // C1
      {
        // A value of around 4000 or 5000 will usually give you a good 20-30 second delay before codes are activated.
        // Frame number * 0.3 -> (20 * 60) * 10 / 3 => 4000
        const u32 comp_value = (System::GetFrameNumber() * 10) / 3;
        if (comp_value < inst.value16)
          index = count;
        else
          index++;
      }
      break;

      case InstructionCode::Slide:
      {
        if ((index + 1) >= instructions.size())
        {
          ERROR_LOG("Incomplete slide instruction");
          return;
        }

        const u32 slide_count = (inst.first >> 8) & 0xFFu;
        const u32 address_increment = inst.first & 0xFFu;
        const u16 value_increment = Truncate16(inst.second);
        const Instruction& inst2 = instructions[index + 1];
        const InstructionCode write_type = inst2.code;
        u32 address = inst2.address;
        u16 value = inst2.value16;

        if (write_type == InstructionCode::ConstantWrite8)
        {
          for (u32 i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<u8>(address, Truncate8(value));
            address += address_increment;
            value += value_increment;
          }
        }
        else if (write_type == InstructionCode::ConstantWrite16)
        {
          for (u32 i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<u16>(address, value);
            address += address_increment;
            value += value_increment;
          }
        }
        else
        {
          ERROR_LOG("Invalid command in second slide parameter 0x{:02X}", static_cast<unsigned>(write_type));
        }

        index += 2;
      }
      break;

      case InstructionCode::ExtImprovedSlide:
      {
        if ((index + 1) >= instructions.size())
        {
          ERROR_LOG("Incomplete slide instruction");
          return;
        }

        const u32 slide_count = inst.first & 0xFFFFu;
        const u32 address_change = (inst.second >> 16) & 0xFFFFu;
        const u16 value_change = Truncate16(inst.second);
        const Instruction& inst2 = instructions[index + 1];
        const InstructionCode write_type = inst2.code;
        const bool address_change_negative = (inst.first >> 20) & 0x1u;
        const bool value_change_negative = (inst.first >> 16) & 0x1u;
        u32 address = inst2.address;
        u32 value = inst2.value32;

        if (write_type == InstructionCode::ConstantWrite8)
        {
          for (u32 i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<u8>(address, Truncate8(value));
            if (address_change_negative)
              address -= address_change;
            else
              address += address_change;
            if (value_change_negative)
              value -= value_change;
            else
              value += value_change;
          }
        }
        else if (write_type == InstructionCode::ConstantWrite16)
        {
          for (u32 i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<u16>(address, Truncate16(value));
            if (address_change_negative)
              address -= address_change;
            else
              address += address_change;
            if (value_change_negative)
              value -= value_change;
            else
              value += value_change;
          }
        }
        else if (write_type == InstructionCode::ExtConstantWrite32)
        {
          for (u32 i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<u32>(address, value);
            if (address_change_negative)
              address -= address_change;
            else
              address += address_change;
            if (value_change_negative)
              value -= value_change;
            else
              value += value_change;
          }
        }
        else
        {
          ERROR_LOG("Invalid command in second slide parameter 0x{:02X}", static_cast<unsigned>(write_type));
        }

        index += 2;
      }
      break;

      case InstructionCode::MemoryCopy:
      {
        if ((index + 1) >= instructions.size())
        {
          ERROR_LOG("Incomplete memory copy instruction");
          return;
        }

        const Instruction& inst2 = instructions[index + 1];
        const u32 byte_count = inst.value16;
        u32 src_address = inst.address;
        u32 dst_address = inst2.address;

        for (u32 i = 0; i < byte_count; i++)
        {
          u8 value = DoMemoryRead<u8>(src_address);
          DoMemoryWrite<u8>(dst_address, value);
          src_address++;
          dst_address++;
        }

        index += 2;
      }
      break;

      default:
      {
        ERROR_LOG("Unhandled instruction code 0x{:02X} ({:08X} {:08X})", static_cast<u8>(inst.code.GetValue()),
                  inst.first, inst.second);
        index++;
      }
      break;
    }
  }
}

void Cheats::GamesharkCheatCode::ApplyOnDisable() const
{
  const u32 count = static_cast<u32>(instructions.size());
  u32 index = 0;
  for (; index < count;)
  {
    const Instruction& inst = instructions[index];
    switch (inst.code)
    {
      case InstructionCode::Nop:
      case InstructionCode::ConstantWrite8:
      case InstructionCode::ConstantWrite16:
      case InstructionCode::ExtConstantWrite32:
      case InstructionCode::ExtConstantBitSet8:
      case InstructionCode::ExtConstantBitSet16:
      case InstructionCode::ExtConstantBitSet32:
      case InstructionCode::ExtConstantBitClear8:
      case InstructionCode::ExtConstantBitClear16:
      case InstructionCode::ExtConstantBitClear32:
      case InstructionCode::ScratchpadWrite16:
      case InstructionCode::ExtScratchpadWrite32:
      case InstructionCode::ExtIncrement32:
      case InstructionCode::ExtDecrement32:
      case InstructionCode::Increment16:
      case InstructionCode::Decrement16:
      case InstructionCode::Increment8:
      case InstructionCode::Decrement8:
      case InstructionCode::ExtConstantForceRange8:
      case InstructionCode::ExtConstantForceRangeLimits16:
      case InstructionCode::ExtConstantForceRangeRollRound16:
      case InstructionCode::ExtConstantSwap16:
      case InstructionCode::DelayActivation: // C1
      case InstructionCode::ExtConstantWriteIfMatch16:
      case InstructionCode::ExtCheatRegisters:
        index++;
        break;

      case InstructionCode::ExtConstantForceRange16:
      case InstructionCode::Slide:
      case InstructionCode::ExtImprovedSlide:
      case InstructionCode::MemoryCopy:
        index += 2;
        break;
      case InstructionCode::ExtFindAndReplace:
        index += 5;
        break;
        // for conditionals, we don't want to skip over in case it changed at some point
      case InstructionCode::ExtCompareEqual32:
      case InstructionCode::ExtCompareNotEqual32:
      case InstructionCode::ExtCompareLess32:
      case InstructionCode::ExtCompareGreater32:
      case InstructionCode::CompareEqual16:
      case InstructionCode::CompareNotEqual16:
      case InstructionCode::CompareLess16:
      case InstructionCode::CompareGreater16:
      case InstructionCode::CompareEqual8:
      case InstructionCode::CompareNotEqual8:
      case InstructionCode::CompareLess8:
      case InstructionCode::CompareGreater8:
      case InstructionCode::CompareButtons: // D4
        index++;
        break;

        // same deal for block conditionals
      case InstructionCode::SkipIfNotEqual16:         // C0
      case InstructionCode::ExtSkipIfNotEqual32:      // A4
      case InstructionCode::SkipIfButtonsNotEqual:    // D5
      case InstructionCode::SkipIfButtonsEqual:       // D6
      case InstructionCode::ExtBitCompareButtons:     // D7
      case InstructionCode::ExtSkipIfNotLess8:        // C3
      case InstructionCode::ExtSkipIfNotGreater8:     // C4
      case InstructionCode::ExtSkipIfNotLess16:       // C5
      case InstructionCode::ExtSkipIfNotGreater16:    // C6
      case InstructionCode::ExtMultiConditionals:     // F6
      case InstructionCode::ExtCheatRegistersCompare: // 52
        index++;
        break;

      case InstructionCode::ExtConstantWriteIfMatchWithRestore16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        const u16 comparevalue = Truncate16(inst.value32 >> 16);
        const u16 newvalue = Truncate16(inst.value32 & 0xFFFFu);
        if (value == newvalue)
          DoMemoryWrite<u16>(inst.address, comparevalue);

        index++;
      }
      break;

      case InstructionCode::ExtConstantWriteIfMatchWithRestore8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        const u8 comparevalue = Truncate8(inst.value16 >> 8);
        const u8 newvalue = Truncate8(inst.value16 & 0xFFu);
        if (value == newvalue)
          DoMemoryWrite<u8>(inst.address, comparevalue);

        index++;
      }
      break;

      [[unlikely]] default:
      {
        ERROR_LOG("Unhandled instruction code 0x{:02X} ({:08X} {:08X})", static_cast<u8>(inst.code.GetValue()),
                  inst.first, inst.second);
        index++;
      }
      break;
    }
  }
}

void Cheats::GamesharkCheatCode::SetOptionValue(u32 value)
{
  for (const auto& [index, bitpos_start, bit_count] : option_instruction_values)
  {
    Instruction& inst = instructions[index];
    const u32 value_mask = ((1u << bit_count) - 1);
    ;
    const u32 fixed_mask = ~(value_mask << bitpos_start);
    inst.second = (inst.second & fixed_mask) | ((value & value_mask) << bitpos_start);
  }
}

std::unique_ptr<Cheats::CheatCode> Cheats::ParseGamesharkCode(CheatCode::Metadata metadata, const std::string_view data,
                                                              Error* error)
{
  return GamesharkCheatCode::Parse(std::move(metadata), data, error);
}
