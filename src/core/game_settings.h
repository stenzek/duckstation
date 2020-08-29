#pragma once
#include "types.h"
#include <bitset>
#include <optional>
#include <string>
#include <unordered_map>

class ByteStream;

namespace GameSettings {
enum class Trait : u32
{
  ForceInterpreter,
  ForceSoftwareRenderer,
  ForceInterlacing,
  DisableTrueColor,
  DisableUpscaling,
  DisableScaledDithering,
  DisableWidescreen,
  DisablePGXP,
  DisablePGXPCulling,
  ForcePGXPVertexCache,
  ForcePGXPCPUMode,
  ForceDigitalController,
  ForceRecompilerMemoryExceptions,
  ForceRecompilerICache,

  Count
};

const char* GetTraitName(Trait trait);
const char* GetTraitDisplayName(Trait trait);

struct Entry
{
  std::bitset<static_cast<int>(Trait::Count)> traits{};
  std::optional<s16> display_active_start_offset;
  std::optional<s16> display_active_end_offset;

  // user settings
  std::optional<DisplayCropMode> display_crop_mode;
  std::optional<DisplayAspectRatio> display_aspect_ratio;
  std::optional<ControllerType> controller_1_type;
  std::optional<ControllerType> controller_2_type;
  std::optional<bool> gpu_widescreen_hack;

  ALWAYS_INLINE bool HasTrait(Trait trait) const { return traits[static_cast<int>(trait)]; }
  ALWAYS_INLINE void AddTrait(Trait trait) { traits[static_cast<int>(trait)] = true; }
  ALWAYS_INLINE void RemoveTrait(Trait trait) { traits[static_cast<int>(trait)] = false; }
  ALWAYS_INLINE void SetTrait(Trait trait, bool enabled) { traits[static_cast<int>(trait)] = enabled; }

  bool HasAnySettings() const;

  bool LoadFromStream(ByteStream* stream);
  bool SaveToStream(ByteStream* stream) const;

  void ApplySettings(bool display_osd_messages) const;
};

class Database
{
public:
  Database();
  ~Database();

  const Entry* GetEntry(const std::string& code) const;
  void SetEntry(const std::string& code, const std::string& name, const Entry& entry, const char* save_path);

  bool Load(const char* path);

private:
  std::unordered_map<std::string, Entry> m_entries;
};

}; // namespace GameSettings