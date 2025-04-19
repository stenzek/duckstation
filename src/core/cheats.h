// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/bitfield.h"

#include "types.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Error;

namespace Cheats {
enum class CodeType : u8
{
  Gameshark,
  Count
};

enum class CodeActivation : u8
{
  Manual,
  EndFrame,
  Count,
};

enum class FileFormat : u8
{
  Unknown,
  DuckStation,
  PCSX,
  Libretro,
  EPSXe,
  Count
};

using CodeOption = std::pair<std::string, u32>;
using CodeOptionList = std::vector<CodeOption>;

/// Contains all the information required to present a cheat code to the user.
struct CodeInfo
{
  std::string name;
  std::string author;
  std::string description;
  std::string body;
  CodeOptionList options;
  u16 option_range_start = 0;
  u16 option_range_end = 0;
  u32 file_offset_start = 0;
  u32 file_offset_body_start = 0;
  u32 file_offset_end = 0;
  CodeType type = CodeType::Gameshark;
  CodeActivation activation = CodeActivation::EndFrame;
  bool from_database = false;

  std::string_view GetNamePart() const;
  std::string_view GetNameParentPart() const;

  bool HasOptionChoices() const { return (!options.empty()); }
  bool HasOptionRange() const { return (option_range_end > option_range_start); }
  std::string_view MapOptionValueToName(u32 value) const;
  std::string_view MapOptionValueToName(const std::string_view value) const;
  u32 MapOptionNameToValue(const std::string_view opt_name) const;
};

using CodeInfoList = std::vector<CodeInfo>;

/// Returns the internal identifier for a code type.
extern const char* GetTypeName(CodeType type);

/// Returns the human-readable name for a code type.
extern const char* GetTypeDisplayName(CodeType type);

/// Parses an internal identifier, returning the code type.
extern std::optional<CodeType> ParseTypeName(const std::string_view str);

/// Returns the internal identifier for a code activation.
extern const char* GetActivationName(CodeActivation activation);

/// Returns the human-readable name for a code activation.
extern const char* GetActivationDisplayName(CodeActivation activation);

/// Parses an internal identifier, returning the activation type.
extern std::optional<CodeActivation> ParseActivationName(const std::string_view str);

/// Returns a list of all available cheats/patches for a given game.
extern CodeInfoList GetCodeInfoList(const std::string_view serial, std::optional<GameHash> hash, bool cheats,
                                    bool load_from_database, bool sort_by_name);

/// Returns a list of all unique prefixes/groups for a cheat list.
extern std::vector<std::string_view> GetCodeListUniquePrefixes(const CodeInfoList& list, bool include_empty);

/// Searches for a given code by name.
extern const CodeInfo* FindCodeInInfoList(const CodeInfoList& list, const std::string_view name);

/// Searches for a given code by name.
extern CodeInfo* FindCodeInInfoList(CodeInfoList& list, const std::string_view name);

/// Formats the given cheat code in the format that it would be saved to a file.
extern std::string FormatCodeForFile(const CodeInfo& code);

/// Imports all codes from the provided string.
extern bool ImportCodesFromString(CodeInfoList* dst, const std::string_view file_contents, FileFormat file_format,
                                  bool stop_on_error, Error* error);

/// Exports codes to the given file, in DuckStation format.
extern bool ExportCodesToFile(std::string path, const CodeInfoList& codes, Error* error);

/// Adds, updates, or removes the specified code from the file, rewriting it. If code is null, it will be removed.
extern bool UpdateCodeInFile(const char* path, const std::string_view name, const CodeInfo* code, Error* error);

/// Updates or adds multiple codes to the file, rewriting it.
extern bool SaveCodesToFile(const char* path, const CodeInfoList& codes, Error* error);

/// Removes any .cht files for the specified game.
extern void RemoveAllCodes(const std::string_view serial, const std::string_view title, std::optional<GameHash> hash);

/// Returns the path to a new cheat/patch cht for the specified serial and hash.
extern std::string GetChtFilename(const std::string_view serial, std::optional<GameHash> hash, bool cheats);

/// Reloads cheats and game patches. The parameters control the degree to which data is reloaded.
extern void ReloadCheats(bool reload_files, bool reload_enabled_list, bool verbose, bool verbose_if_changed,
                         bool show_disabled_codes);

/// Releases all cheat-related state.
extern void UnloadAll();

/// Returns true if any patches have setting overrides specified.
extern bool HasAnySettingOverrides();

/// Applies setting changes based on patches.
extern void ApplySettingOverrides();

/// Applies all currently-registered frame end cheat codes.
extern void ApplyFrameEndCodes();

/// Returns true if cheats are enabled in the current game's configuration.
extern bool AreCheatsEnabled();

/// Enumerates the names of all manually-activated codes.
extern bool EnumerateManualCodes(std::function<bool(const std::string& name)> callback);

/// Invokes/applies the specified manually-activated code.
extern bool ApplyManualCode(const std::string_view name);

/// Returns the number of active patches.
extern u32 GetActivePatchCount();

/// Returns the number of active cheats.
extern u32 GetActiveCheatCount();

// Config sections/keys to use to enable patches.
extern const char* PATCHES_CONFIG_SECTION;
extern const char* CHEATS_CONFIG_SECTION;
extern const char* PATCH_ENABLE_CONFIG_KEY;

} // namespace Cheats
