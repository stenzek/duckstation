#include "game_settings.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string.h"
#include "common/string_util.h"
#include "host_interface.h"
#include "settings.h"
#include <array>
#include <utility>
Log_SetChannel(GameSettings);

#ifdef WIN32
#include "common/windows_headers.h"
#endif
#include "SimpleIni.h"

namespace GameSettings {

std::array<std::pair<const char*, const char*>, static_cast<u32>(Trait::Count)> s_trait_names = {{
  {"ForceInterpreter", TRANSLATABLE("GameSettingsTrait", "Force Interpreter")},
  {"ForceSoftwareRenderer", TRANSLATABLE("GameSettingsTrait", "Force Software Renderer")},
  {"EnableInterlacing", TRANSLATABLE("GameSettingsTrait", "Enable Interlacing")},
  {"DisableTrueColor", TRANSLATABLE("GameSettingsTrait", "Disable True Color")},
  {"DisableUpscaling", TRANSLATABLE("GameSettingsTrait", "Disable Upscaling")},
  {"DisableScaledDithering", TRANSLATABLE("GameSettingsTrait", "Disable Scaled Dithering")},
  {"DisableWidescreen", TRANSLATABLE("GameSettingsTrait", "Disable Widescreen")},
  {"DisablePGXP", TRANSLATABLE("GameSettingsTrait", "Disable PGXP")},
  {"DisablePGXPCulling", TRANSLATABLE("GameSettingsTrait", "Disable PGXP Culling")},
  {"EnablePGXPVertexCache", TRANSLATABLE("GameSettingsTrait", "Enable PGXP Vertex Cache")},
  {"EnablePGXPCPUMode", TRANSLATABLE("GameSettingsTrait", "Enable PGXP CPU Mode")},
  {"ForceDigitalController", TRANSLATABLE("GameSettingsTrait", "Force Digital Controller")},
  {"EnableRecompilerMemoryExceptions", TRANSLATABLE("GameSettingsTrait", "Enable Recompiler Memory Exceptions")},
}};

const char* GetTraitName(Trait trait)
{
  DebugAssert(trait < Trait::Count);
  return s_trait_names[static_cast<u32>(trait)].first;
}

const char* GetTraitDisplayName(Trait trait)
{
  DebugAssert(trait < Trait::Count);
  return s_trait_names[static_cast<u32>(trait)].second;
}

bool Entry::HasAnySettings() const
{
  return traits.any() || display_active_start_offset >= 0 || display_active_end_offset > 0;
}

template<typename T>
bool ReadOptionalFromStream(ByteStream* stream, std::optional<T>* dest)
{
  bool has_value;
  if (!stream->Read2(&has_value, sizeof(has_value)))
    return false;

  if (!has_value)
    return true;

  T value;
  if (!stream->Read2(&value, sizeof(T)))
    return false;

  *dest = value;
  return true;
}

template<typename T>
bool WriteOptionalToStream(ByteStream* stream, const std::optional<T>& src)
{
  const bool has_value = src.has_value();
  if (!stream->Write2(&has_value, sizeof(has_value)))
    return false;

  if (!has_value)
    return true;

  return stream->Write2(&src.value(), sizeof(T));
}

bool Entry::LoadFromStream(ByteStream* stream)
{
  constexpr u32 num_bytes = (static_cast<u32>(Trait::Count) + 7) / 8;
  std::array<u8, num_bytes> bits;

  if (!stream->Read2(bits.data(), num_bytes) || !ReadOptionalFromStream(stream, &display_active_start_offset) ||
      !ReadOptionalFromStream(stream, &display_active_end_offset) ||
      !ReadOptionalFromStream(stream, &display_crop_mode) || !ReadOptionalFromStream(stream, &display_aspect_ratio) ||
      !ReadOptionalFromStream(stream, &controller_1_type) || !ReadOptionalFromStream(stream, &controller_2_type))
  {
    return false;
  }

  traits.reset();
  for (u32 i = 0; i < static_cast<int>(Trait::Count); i++)
  {
    if ((bits[i / 8] & (1u << (i % 8))) != 0)
      AddTrait(static_cast<Trait>(i));
  }

  return true;
}

bool Entry::SaveToStream(ByteStream* stream) const
{
  constexpr u32 num_bytes = (static_cast<u32>(Trait::Count) + 7) / 8;
  std::array<u8, num_bytes> bits;
  bits.fill(0);
  for (u32 i = 0; i < static_cast<int>(Trait::Count); i++)
  {
    if (HasTrait(static_cast<Trait>(i)))
      bits[i / 8] |= (1u << (i % 8));
  }

  return stream->Write2(bits.data(), num_bytes) && WriteOptionalToStream(stream, display_active_start_offset) &&
         WriteOptionalToStream(stream, display_active_end_offset) && WriteOptionalToStream(stream, display_crop_mode) &&
         WriteOptionalToStream(stream, display_aspect_ratio) && WriteOptionalToStream(stream, controller_1_type) &&
         WriteOptionalToStream(stream, controller_2_type);
}

static void ParseIniSection(Entry* entry, const char* section, const CSimpleIniA& ini)
{
  for (u32 trait = 0; trait < static_cast<u32>(Trait::Count); trait++)
  {
    if (ini.GetBoolValue(section, s_trait_names[trait].first, false))
      entry->AddTrait(static_cast<Trait>(trait));
  }

  long lvalue = ini.GetLongValue(section, "DisplayActiveStartOffset", 0);
  if (lvalue != 0)
    entry->display_active_start_offset = static_cast<s16>(lvalue);
  lvalue = ini.GetLongValue(section, "DisplayActiveEndOffset", 0);
  if (lvalue != 0)
    entry->display_active_end_offset = static_cast<s16>(lvalue);

  const char* cvalue = ini.GetValue(section, "DisplayCropMode", nullptr);
  if (cvalue)
    entry->display_crop_mode = Settings::ParseDisplayCropMode(cvalue);
  cvalue = ini.GetValue(section, "DisplayAspectRatio", nullptr);
  if (cvalue)
    entry->display_aspect_ratio = Settings::ParseDisplayAspectRatio(cvalue);

  cvalue = ini.GetValue(section, "Controller1Type", nullptr);
  if (cvalue)
    entry->controller_1_type = Settings::ParseControllerTypeName(cvalue);
  cvalue = ini.GetValue(section, "Controller2Type", nullptr);
  if (cvalue)
    entry->controller_2_type = Settings::ParseControllerTypeName(cvalue);

  cvalue = ini.GetValue(section, "GPUWidescreenHack", nullptr);
  if (cvalue)
    entry->gpu_widescreen_hack = StringUtil::FromChars<bool>(cvalue);
}

static void StoreIniSection(const Entry& entry, const char* section, CSimpleIniA& ini)
{
  for (u32 trait = 0; trait < static_cast<u32>(Trait::Count); trait++)
  {
    if (entry.HasTrait(static_cast<Trait>(trait)))
      ini.SetBoolValue(section, s_trait_names[trait].first, true);
  }

  if (entry.display_active_start_offset.has_value())
    ini.SetLongValue(section, "DisplayActiveStartOffset", entry.display_active_start_offset.value());

  if (entry.display_active_end_offset.has_value())
    ini.SetLongValue(section, "DisplayActiveEndOffset", entry.display_active_end_offset.value());

  if (entry.display_crop_mode.has_value())
    ini.SetValue(section, "DisplayCropMode", Settings::GetDisplayCropModeName(entry.display_crop_mode.value()));
  if (entry.display_aspect_ratio.has_value())
  {
    ini.SetValue(section, "DisplayAspectRatio",
                 Settings::GetDisplayAspectRatioName(entry.display_aspect_ratio.value()));
  }

  if (entry.controller_1_type.has_value())
    ini.SetValue(section, "Controller1Type", Settings::GetControllerTypeName(entry.controller_1_type.value()));
  if (entry.controller_2_type.has_value())
    ini.SetValue(section, "Controller2Type", Settings::GetControllerTypeName(entry.controller_2_type.value()));

  if (entry.gpu_widescreen_hack.has_value())
    ini.SetValue(section, "GPUWidescreenHack", entry.gpu_widescreen_hack.value() ? "true" : "false");
}

Database::Database() = default;

Database::~Database() = default;

const GameSettings::Entry* Database::GetEntry(const std::string& code) const
{
  auto it = m_entries.find(code);
  return (it != m_entries.end()) ? &it->second : nullptr;
}

bool Database::Load(const char* path)
{
  auto fp = FileSystem::OpenManagedCFile(path, "rb");
  if (!fp)
    return false;

  CSimpleIniA ini;
  SI_Error err = ini.LoadFile(fp.get());
  if (err != SI_OK)
  {
    Log_ErrorPrintf("Failed to parse game settings ini: %d", static_cast<int>(err));
    return false;
  }

  std::list<CSimpleIniA::Entry> sections;
  ini.GetAllSections(sections);
  for (const CSimpleIniA::Entry& section_entry : sections)
  {
    std::string code(section_entry.pItem);
    auto it = m_entries.find(code);
    if (it != m_entries.end())
    {
      ParseIniSection(&it->second, code.c_str(), ini);
      continue;
    }

    Entry entry;
    ParseIniSection(&entry, code.c_str(), ini);
    m_entries.emplace(std::move(code), std::move(entry));
  }

  Log_InfoPrintf("Loaded settings for %zu games from '%s'", sections.size(), path);
  return true;
}

void Database::SetEntry(const std::string& code, const std::string& name, const Entry& entry, const char* save_path)
{
  if (save_path)
  {
    CSimpleIniA ini;
    if (FileSystem::FileExists(save_path))
    {
      auto fp = FileSystem::OpenManagedCFile(save_path, "rb");
      if (fp)
      {
        SI_Error err = ini.LoadFile(fp.get());
        if (err != SI_OK)
          Log_ErrorPrintf("Failed to parse game settings ini: %d. Contents will be lost.", static_cast<int>(err));
      }
      else
      {
        Log_ErrorPrintf("Failed to open existing settings ini: '%s'", save_path);
      }
    }

    ini.Delete(code.c_str(), nullptr, false);
    ini.SetValue(code.c_str(), nullptr, nullptr, SmallString::FromFormat("# %s (%s)", code.c_str(), name.c_str()),
                 false);
    StoreIniSection(entry, code.c_str(), ini);

    const bool did_exist = FileSystem::FileExists(save_path);
    auto fp = FileSystem::OpenManagedCFile(save_path, "wb");
    if (fp)
    {
      // write file comment so simpleini doesn't get confused
      if (!did_exist)
        std::fputs("# DuckStation Game Settings\n\n", fp.get());

      SI_Error err = ini.SaveFile(fp.get());
      if (err != SI_OK)
        Log_ErrorPrintf("Failed to save game settings ini: %d", static_cast<int>(err));
    }
    else
    {
      Log_ErrorPrintf("Failed to open settings ini for saving: '%s'", save_path);
    }
  }

  auto it = m_entries.find(code);
  if (it != m_entries.end())
    it->second = entry;
  else
    m_entries.emplace(code, entry);
}

void Entry::ApplySettings(bool display_osd_messages) const
{
  constexpr float osd_duration = 10.0f;

  if (display_active_start_offset.has_value())
    g_settings.display_active_start_offset = display_active_start_offset.value();
  if (display_active_end_offset.has_value())
    g_settings.display_active_end_offset = display_active_end_offset.value();

  if (display_crop_mode.has_value())
    g_settings.display_crop_mode = display_crop_mode.value();
  if (display_aspect_ratio.has_value())
    g_settings.display_aspect_ratio = display_aspect_ratio.value();
  if (controller_1_type.has_value())
    g_settings.controller_types[0] = controller_1_type.value();
  if (controller_2_type.has_value())
    g_settings.controller_types[1] = controller_2_type.value();
  if (gpu_widescreen_hack.has_value())
    g_settings.gpu_widescreen_hack = gpu_widescreen_hack.value();

  if (HasTrait(Trait::ForceInterpreter))
  {
    if (display_osd_messages && g_settings.cpu_execution_mode != CPUExecutionMode::Interpreter)
      g_host_interface->AddOSDMessage("CPU execution mode forced to interpreter by game settings.", osd_duration);

    g_settings.cpu_execution_mode = CPUExecutionMode::Interpreter;
  }

  if (HasTrait(Trait::ForceSoftwareRenderer))
  {
    if (display_osd_messages && g_settings.gpu_renderer != GPURenderer::Software)
      g_host_interface->AddOSDMessage("GPU renderer forced to software by game settings.", osd_duration);

    g_settings.gpu_renderer = GPURenderer::Software;
  }

  if (HasTrait(Trait::EnableInterlacing))
  {
    if (display_osd_messages && g_settings.gpu_disable_interlacing)
      g_host_interface->AddOSDMessage("Interlacing enabled by game settings.", osd_duration);

    g_settings.gpu_disable_interlacing = false;
  }

  if (HasTrait(Trait::DisableTrueColor))
  {
    if (display_osd_messages && g_settings.gpu_true_color)
      g_host_interface->AddOSDMessage("True color disabled by game settings.", osd_duration);

    g_settings.gpu_true_color = false;
  }

  if (HasTrait(Trait::DisableUpscaling))
  {
    if (display_osd_messages && g_settings.gpu_resolution_scale > 1)
      g_host_interface->AddOSDMessage("Upscaling disabled by game settings.", osd_duration);

    g_settings.gpu_resolution_scale = 1;
  }

  if (HasTrait(Trait::DisableScaledDithering))
  {
    if (display_osd_messages && g_settings.gpu_scaled_dithering)
      g_host_interface->AddOSDMessage("Scaled dithering disabled by game settings.", osd_duration);

    g_settings.gpu_scaled_dithering = false;
  }

  if (HasTrait(Trait::DisableWidescreen))
  {
    if (display_osd_messages &&
        (g_settings.display_aspect_ratio == DisplayAspectRatio::R16_9 || g_settings.gpu_widescreen_hack))
    {
      g_host_interface->AddOSDMessage("Widescreen disabled by game settings.", osd_duration);
    }

    g_settings.display_aspect_ratio = DisplayAspectRatio::R4_3;
    g_settings.gpu_widescreen_hack = false;
  }

  if (HasTrait(Trait::DisablePGXP))
  {
    if (display_osd_messages && g_settings.gpu_pgxp_enable)
      g_host_interface->AddOSDMessage("PGXP geometry correction disabled by game settings.", osd_duration);

    g_settings.gpu_pgxp_enable = false;
  }

  if (HasTrait(Trait::DisablePGXPCulling))
  {
    if (display_osd_messages && g_settings.gpu_pgxp_culling)
      g_host_interface->AddOSDMessage("PGXP culling disabled by game settings.", osd_duration);

    g_settings.gpu_pgxp_culling = false;
  }

  if (HasTrait(Trait::EnablePGXPVertexCache))
  {
    if (display_osd_messages && g_settings.gpu_pgxp_enable && !g_settings.gpu_pgxp_vertex_cache)
      g_host_interface->AddOSDMessage("PGXP vertex cache enabled by game settings.", osd_duration);

    g_settings.gpu_pgxp_vertex_cache = true;
  }

  if (HasTrait(Trait::EnablePGXPCPUMode))
  {
    if (display_osd_messages && g_settings.gpu_pgxp_enable && !g_settings.gpu_pgxp_cpu)
      g_host_interface->AddOSDMessage("PGXP CPU mode enabled by game settings.", osd_duration);

    g_settings.gpu_pgxp_cpu = true;
  }

  if (HasTrait(Trait::ForceDigitalController))
  {
    for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    {
      if (g_settings.controller_types[i] != ControllerType::None &&
          g_settings.controller_types[i] != ControllerType::DigitalController)
      {
        if (display_osd_messages)
        {
          g_host_interface->AddFormattedOSDMessage(osd_duration, "Controller %u changed to digital by game settings.",
                                                   i + 1u);
        }

        g_settings.controller_types[i] = ControllerType::DigitalController;
      }
    }
  }

  if (HasTrait(Trait::EnableRecompilerMemoryExceptions))
  {
    if (display_osd_messages && g_settings.cpu_execution_mode == CPUExecutionMode::Recompiler &&
        !g_settings.cpu_recompiler_memory_exceptions)
    {
      g_host_interface->AddOSDMessage("Recompiler memory exceptions enabled by game settings.", osd_duration);
    }

    g_settings.cpu_recompiler_memory_exceptions = true;
  }

  // TODO: Overscan settings.
}

} // namespace GameSettings