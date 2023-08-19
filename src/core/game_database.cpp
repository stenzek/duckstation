// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "game_database.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/heterogeneous_containers.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "host.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "system.h"
#include "util/cd_image.h"
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
Log_SetChannel(GameDatabase);

#ifdef _WIN32
#include "common/windows_headers.h"
#endif

namespace GameDatabase {

enum : u32
{
  GAME_DATABASE_CACHE_SIGNATURE = 0x45434C48,
  GAME_DATABASE_CACHE_VERSION = 3,
};

static Entry* GetMutableEntry(const std::string_view& serial);

static bool LoadFromCache();
static bool SaveToCache();

static bool LoadGameDBJson();
static bool ParseJsonEntry(Entry* entry, const rapidjson::Value& value);
static bool ParseJsonCodes(u32 index, const rapidjson::Value& value);
static bool LoadTrackHashes();

std::array<const char*, static_cast<u32>(GameDatabase::Trait::Count)> s_trait_names = {{
  "ForceInterpreter",
  "ForceSoftwareRenderer",
  "ForceSoftwareRendererForReadbacks",
  "ForceInterlacing",
  "DisableTrueColor",
  "DisableUpscaling",
  "DisableScaledDithering",
  "DisableForceNTSCTimings",
  "DisableWidescreen",
  "DisablePGXP",
  "DisablePGXPCulling",
  "DisablePGXPTextureCorrection",
  "DisablePGXPColorCorrection",
  "DisablePGXPDepthBuffer",
  "ForcePGXPVertexCache",
  "ForcePGXPCPUMode",
  "ForceRecompilerMemoryExceptions",
  "ForceRecompilerICache",
  "ForceRecompilerLUTFastmem",
  "IsLibCryptProtected",
}};

static bool s_loaded = false;
static bool s_track_hashes_loaded = false;

static std::vector<GameDatabase::Entry> s_entries;
static UnorderedStringMap<u32> s_code_lookup;

static TrackHashesMap s_track_hashes_map;
} // namespace GameDatabase

void GameDatabase::EnsureLoaded()
{
  if (s_loaded)
    return;

  Common::Timer timer;

  s_loaded = true;

  if (!LoadFromCache())
  {
    s_entries = {};
    s_code_lookup = {};

    LoadGameDBJson();
    SaveToCache();
  }

  Log_InfoPrintf("Database load took %.2f ms", timer.GetTimeMilliseconds());
}

void GameDatabase::Unload()
{
  s_entries = {};
  s_code_lookup = {};
  s_loaded = false;
}

const GameDatabase::Entry* GameDatabase::GetEntryForId(const std::string_view& code)
{
  if (code.empty())
    return nullptr;

  EnsureLoaded();

  auto iter = UnorderedStringMapFind(s_code_lookup, code);
  return (iter != s_code_lookup.end()) ? &s_entries[iter->second] : nullptr;
}

std::string GameDatabase::GetSerialForDisc(CDImage* image)
{
  std::string ret;

  const GameDatabase::Entry* entry = GetEntryForDisc(image);
  if (entry)
    ret = entry->serial;

  return ret;
}

std::string GameDatabase::GetSerialForPath(const char* path)
{
  std::string ret;

  if (System::IsLoadableFilename(path) && !System::IsExeFileName(path) && !System::IsPsfFileName(path))
  {
    std::unique_ptr<CDImage> image(CDImage::Open(path, false, nullptr));
    if (image)
      ret = GetSerialForDisc(image.get());
  }

  return ret;
}

const GameDatabase::Entry* GameDatabase::GetEntryForDisc(CDImage* image)
{
  std::string id;
  System::GetGameDetailsFromImage(image, &id, nullptr);
  if (!id.empty())
  {
    const Entry* entry = GetEntryForId(id);
    if (entry)
      return entry;
  }

  Log_WarningPrintf("No entry found for disc '%s'", id.c_str());
  return nullptr;
}

const GameDatabase::Entry* GameDatabase::GetEntryForSerial(const std::string_view& serial)
{
  EnsureLoaded();

  return GetMutableEntry(serial);
}

GameDatabase::Entry* GameDatabase::GetMutableEntry(const std::string_view& serial)
{
  for (Entry& entry : s_entries)
  {
    if (entry.serial == serial)
      return &entry;
  }

  return nullptr;
}

const char* GameDatabase::GetTraitName(Trait trait)
{
  DebugAssert(trait < Trait::Count);
  return s_trait_names[static_cast<u32>(trait)];
}

const char* GameDatabase::GetCompatibilityRatingName(CompatibilityRating rating)
{
  static std::array<const char*, static_cast<int>(CompatibilityRating::Count)> names = {
    {"Unknown", "DoesntBoot", "CrashesInIntro", "CrashesInGame", "GraphicalAudioIssues", "NoIssues"}};
  return names[static_cast<int>(rating)];
}

const char* GameDatabase::GetCompatibilityRatingDisplayName(CompatibilityRating rating)
{
  static constexpr std::array<const char*, static_cast<size_t>(CompatibilityRating::Count)> names = {
    {TRANSLATE_NOOP("GameListCompatibilityRating", "Unknown"),
     TRANSLATE_NOOP("GameListCompatibilityRating", "Doesn't Boot"),
     TRANSLATE_NOOP("GameListCompatibilityRating", "Crashes In Intro"),
     TRANSLATE_NOOP("GameListCompatibilityRating", "Crashes In-Game"),
     TRANSLATE_NOOP("GameListCompatibilityRating", "Graphical/Audio Issues"),
     TRANSLATE_NOOP("GameListCompatibilityRating", "No Issues")}};
  return (rating >= CompatibilityRating::Unknown && rating < CompatibilityRating::Count) ?
           names[static_cast<int>(rating)] :
           "";
}

void GameDatabase::Entry::ApplySettings(Settings& settings, bool display_osd_messages) const
{
  constexpr float osd_duration = 5.0f;

  if (display_active_start_offset.has_value())
    settings.display_active_start_offset = display_active_start_offset.value();
  if (display_active_end_offset.has_value())
    settings.display_active_end_offset = display_active_end_offset.value();
  if (display_line_start_offset.has_value())
    settings.display_line_start_offset = display_line_start_offset.value();
  if (display_line_end_offset.has_value())
    settings.display_line_end_offset = display_line_end_offset.value();
  if (dma_max_slice_ticks.has_value())
    settings.dma_max_slice_ticks = dma_max_slice_ticks.value();
  if (dma_halt_ticks.has_value())
    settings.dma_halt_ticks = dma_halt_ticks.value();
  if (gpu_fifo_size.has_value())
    settings.gpu_fifo_size = gpu_fifo_size.value();
  if (gpu_max_run_ahead.has_value())
    settings.gpu_max_run_ahead = gpu_max_run_ahead.value();
  if (gpu_pgxp_tolerance.has_value())
    settings.gpu_pgxp_tolerance = gpu_pgxp_tolerance.value();
  if (gpu_pgxp_depth_threshold.has_value())
    settings.SetPGXPDepthClearThreshold(gpu_pgxp_depth_threshold.value());

  if (HasTrait(Trait::ForceInterpreter))
  {
    if (display_osd_messages && settings.cpu_execution_mode != CPUExecutionMode::Interpreter)
    {
      Host::AddKeyedOSDMessage("gamedb_force_interpreter",
                               Host::TranslateStdString("OSDMessage", "CPU interpreter forced by game settings."),
                               osd_duration);
    }

    settings.cpu_execution_mode = CPUExecutionMode::Interpreter;
  }

  if (HasTrait(Trait::ForceSoftwareRenderer))
  {
    if (display_osd_messages && settings.gpu_renderer != GPURenderer::Software)
    {
      Host::AddKeyedOSDMessage("gamedb_force_software",
                               Host::TranslateStdString("OSDMessage", "Software renderer forced by game settings."),
                               osd_duration);
    }

    settings.gpu_renderer = GPURenderer::Software;
  }

  if (HasTrait(Trait::ForceSoftwareRendererForReadbacks))
  {
    if (display_osd_messages && settings.gpu_renderer != GPURenderer::Software)
    {
      Host::AddKeyedOSDMessage(
        "gamedb_force_software_rb",
        Host::TranslateStdString("OSDMessage", "Using software renderer for readbacks based on game settings."),
        osd_duration);
    }

    settings.gpu_use_software_renderer_for_readbacks = true;
  }

  if (HasTrait(Trait::ForceInterlacing))
  {
    if (display_osd_messages && settings.gpu_disable_interlacing)
    {
      Host::AddKeyedOSDMessage("gamedb_force_interlacing",
                               Host::TranslateStdString("OSDMessage", "Interlacing forced by game settings."),
                               osd_duration);
    }

    settings.gpu_disable_interlacing = false;
  }

  if (HasTrait(Trait::DisableTrueColor))
  {
    if (display_osd_messages && settings.gpu_true_color)
    {
      Host::AddKeyedOSDMessage("gamedb_disable_true_color",
                               Host::TranslateStdString("OSDMessage", "True color disabled by game settings."),
                               osd_duration);
    }

    settings.gpu_true_color = false;
  }

  if (HasTrait(Trait::DisableUpscaling))
  {
    if (display_osd_messages && settings.gpu_resolution_scale > 1)
    {
      Host::AddKeyedOSDMessage("gamedb_disable_upscaling",
                               Host::TranslateStdString("OSDMessage", "Upscaling disabled by game settings."),
                               osd_duration);
    }

    settings.gpu_resolution_scale = 1;
  }

  if (HasTrait(Trait::DisableScaledDithering))
  {
    if (display_osd_messages && settings.gpu_scaled_dithering)
    {
      Host::AddKeyedOSDMessage("gamedb_disable_scaled_dithering",
                               Host::TranslateStdString("OSDMessage", "Scaled dithering disabled by game settings."),
                               osd_duration);
    }

    settings.gpu_scaled_dithering = false;
  }

  if (HasTrait(Trait::DisableWidescreen))
  {
    if (display_osd_messages &&
        (settings.display_aspect_ratio == DisplayAspectRatio::R16_9 || settings.gpu_widescreen_hack))
    {
      Host::AddKeyedOSDMessage("gamedb_disable_widescreen",
                               Host::TranslateStdString("OSDMessage", "Widescreen disabled by game settings."),
                               osd_duration);
    }

    settings.display_aspect_ratio = DisplayAspectRatio::R4_3;
    settings.gpu_widescreen_hack = false;
  }

  if (HasTrait(Trait::DisableForceNTSCTimings))
  {
    if (display_osd_messages && settings.gpu_force_ntsc_timings)
    {
      Host::AddKeyedOSDMessage(
        "gamedb_disable_force_ntsc_timings",
        Host::TranslateStdString("OSDMessage", "Forcing NTSC Timings disallowed by game settings."), osd_duration);
    }

    settings.gpu_force_ntsc_timings = false;
  }

  if (HasTrait(Trait::DisablePGXP))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable)
    {
      Host::AddKeyedOSDMessage(
        "gamedb_disable_pgxp",
        Host::TranslateStdString("OSDMessage", "PGXP geometry correction disabled by game settings."), osd_duration);
    }

    settings.gpu_pgxp_enable = false;
  }

  if (HasTrait(Trait::DisablePGXPCulling))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_culling)
    {
      Host::AddKeyedOSDMessage("gamedb_disable_pgxp_culling",
                               Host::TranslateStdString("OSDMessage", "PGXP culling disabled by game settings."),
                               osd_duration);
    }

    settings.gpu_pgxp_culling = false;
  }

  if (HasTrait(Trait::DisablePGXPTextureCorrection))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_texture_correction)
    {
      Host::AddKeyedOSDMessage(
        "gamedb_disable_pgxp_texture",
        Host::TranslateStdString("OSDMessage", "PGXP perspective corrected textures disabled by game settings."),
        osd_duration);
    }

    settings.gpu_pgxp_texture_correction = false;
  }

  if (HasTrait(Trait::DisablePGXPColorCorrection))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_texture_correction &&
        settings.gpu_pgxp_color_correction)
    {
      Host::AddKeyedOSDMessage(
        "gamedb_disable_pgxp_texture",
        Host::TranslateStdString("OSDMessage", "PGXP perspective corrected colors disabled by game settings."),
        osd_duration);
    }

    settings.gpu_pgxp_color_correction = false;
  }

  if (HasTrait(Trait::ForcePGXPVertexCache))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && !settings.gpu_pgxp_vertex_cache)
    {
      Host::AddKeyedOSDMessage("gamedb_force_pgxp_vertex_cache",
                               Host::TranslateStdString("OSDMessage", "PGXP vertex cache forced by game settings."),
                               osd_duration);
    }

    settings.gpu_pgxp_vertex_cache = true;
  }

  if (HasTrait(Trait::ForcePGXPCPUMode))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && !settings.gpu_pgxp_cpu)
    {
      Host::AddKeyedOSDMessage("gamedb_force_pgxp_cpu",
                               Host::TranslateStdString("OSDMessage", "PGXP CPU mode forced by game settings."),
                               osd_duration);
    }

    settings.gpu_pgxp_cpu = true;
  }

  if (HasTrait(Trait::DisablePGXPDepthBuffer))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_depth_buffer)
    {
      Host::AddKeyedOSDMessage("gamedb_disable_pgxp_depth",
                               Host::TranslateStdString("OSDMessage", "PGXP Depth Buffer disabled by game settings."),
                               osd_duration);
    }

    settings.gpu_pgxp_depth_buffer = false;
  }

  if (HasTrait(Trait::ForceRecompilerMemoryExceptions))
  {
    Log_WarningPrint("Memory exceptions for recompiler forced by game settings.");
    settings.cpu_recompiler_memory_exceptions = true;
  }

  if (HasTrait(Trait::ForceRecompilerICache))
  {
    Log_WarningPrint("ICache for recompiler forced by game settings.");
    settings.cpu_recompiler_icache = true;
  }

  if (settings.cpu_fastmem_mode == CPUFastmemMode::MMap && HasTrait(Trait::ForceRecompilerLUTFastmem))
  {
    Log_WarningPrint("LUT fastmem for recompiler forced by game settings.");
    settings.cpu_fastmem_mode = CPUFastmemMode::LUT;
  }

#define BIT_FOR(ctype) (static_cast<u32>(1) << static_cast<u32>(ctype))

  if (supported_controllers != 0 && supported_controllers != static_cast<u32>(-1))
  {
    for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    {
      const ControllerType ctype = settings.controller_types[i];
      if (ctype == ControllerType::None)
        continue;

      if (supported_controllers & BIT_FOR(ctype))
        continue;

      // Special case: Dualshock is permitted when not supported as long as it's in digital mode.
      if (ctype == ControllerType::AnalogController &&
          (supported_controllers & BIT_FOR(ControllerType::DigitalController)) != 0)
      {
        settings.controller_disable_analog_mode_forcing = true;
        continue;
      }

      if (display_osd_messages)
      {
        SmallString supported_controller_string;
        for (u32 j = 0; j < static_cast<u32>(ControllerType::Count); j++)
        {
          const ControllerType supported_ctype = static_cast<ControllerType>(j);
          if ((supported_controllers & BIT_FOR(supported_ctype)) == 0)
            continue;

          if (!supported_controller_string.IsEmpty())
            supported_controller_string.AppendString(", ");

          supported_controller_string.AppendString(
            Host::TranslateString("ControllerType", Settings::GetControllerTypeDisplayName(supported_ctype)));
        }

        Host::AddKeyedFormattedOSDMessage(
          "gamedb_controller_unsupported", 30.0f,
          Host::TranslateString("OSDMessage",
                                "Controller in port %u (%s) is not supported for %s.\nSupported controllers: "
                                "%s\nPlease configure a supported controller from the list above."),
          i + 1u, Host::TranslateString("ControllerType", Settings::GetControllerTypeDisplayName(ctype)).GetCharArray(),
          System::GetGameTitle().c_str(), supported_controller_string.GetCharArray());
      }
    }
  }

#undef BIT_FOR
}

static void GetTimestamps(u64* gamedb_ts, u64* gamesettings_ts, u64* compat_ts)
{
  *gamedb_ts = Host::GetResourceFileTimestamp("database/gamedb.json").value_or(0);
  *gamesettings_ts = Host::GetResourceFileTimestamp("database/gamesettings.ini").value_or(0);
  *compat_ts = Host::GetResourceFileTimestamp("database/compatibility.xml").value_or(0);
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

static std::string GetCacheFile()
{
  return Path::Combine(EmuFolders::Cache, "gamedb.cache");
}

bool GameDatabase::LoadFromCache()
{
  std::unique_ptr<ByteStream> stream(
    ByteStream::OpenFile(GetCacheFile().c_str(), BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED));
  if (!stream)
  {
    Log_DevPrintf("Cache does not exist, loading full database.");
    return false;
  }

  u64 gamedb_ts, gamesettings_ts, compat_ts;
  GetTimestamps(&gamedb_ts, &gamesettings_ts, &compat_ts);

  u32 signature, version, num_entries, num_codes;
  u64 file_gamedb_ts, file_gamesettings_ts, file_compat_ts;
  if (!stream->ReadU32(&signature) || !stream->ReadU32(&version) || !stream->ReadU64(&file_gamedb_ts) ||
      !stream->ReadU64(&file_gamesettings_ts) || !stream->ReadU64(&file_compat_ts) || !stream->ReadU32(&num_entries) ||
      !stream->ReadU32(&num_codes) || signature != GAME_DATABASE_CACHE_SIGNATURE ||
      version != GAME_DATABASE_CACHE_VERSION)
  {
    Log_DevPrintf("Cache header is corrupted or version mismatch.");
    return false;
  }

  if (gamedb_ts != file_gamedb_ts || gamesettings_ts != file_gamesettings_ts || compat_ts != file_compat_ts)
  {
    Log_DevPrintf("Cache is out of date, recreating.");
    return false;
  }

  s_entries.reserve(num_entries);

  for (u32 i = 0; i < num_entries; i++)
  {
    Entry& entry = s_entries.emplace_back();

    constexpr u32 num_bytes = (static_cast<u32>(Trait::Count) + 7) / 8;
    std::array<u8, num_bytes> bits;
    u8 compatibility;

    if (!stream->ReadSizePrefixedString(&entry.serial) || !stream->ReadSizePrefixedString(&entry.title) ||
        !stream->ReadSizePrefixedString(&entry.genre) || !stream->ReadSizePrefixedString(&entry.developer) ||
        !stream->ReadSizePrefixedString(&entry.publisher) || !stream->ReadU64(&entry.release_date) ||
        !stream->ReadU8(&entry.min_players) || !stream->ReadU8(&entry.max_players) ||
        !stream->ReadU8(&entry.min_blocks) || !stream->ReadU8(&entry.max_blocks) ||
        !stream->ReadU32(&entry.supported_controllers) || !stream->ReadU8(&compatibility) ||
        compatibility >= static_cast<u8>(GameDatabase::CompatibilityRating::Count) ||
        !stream->Read2(bits.data(), num_bytes) ||
        !ReadOptionalFromStream(stream.get(), &entry.display_active_start_offset) ||
        !ReadOptionalFromStream(stream.get(), &entry.display_active_end_offset) ||
        !ReadOptionalFromStream(stream.get(), &entry.display_line_start_offset) ||
        !ReadOptionalFromStream(stream.get(), &entry.display_line_end_offset) ||
        !ReadOptionalFromStream(stream.get(), &entry.dma_max_slice_ticks) ||
        !ReadOptionalFromStream(stream.get(), &entry.dma_halt_ticks) ||
        !ReadOptionalFromStream(stream.get(), &entry.gpu_fifo_size) ||
        !ReadOptionalFromStream(stream.get(), &entry.gpu_max_run_ahead) ||
        !ReadOptionalFromStream(stream.get(), &entry.gpu_pgxp_tolerance) ||
        !ReadOptionalFromStream(stream.get(), &entry.gpu_pgxp_depth_threshold))
    {
      Log_DevPrintf("Cache entry is corrupted.");
      return false;
    }

    entry.compatibility = static_cast<GameDatabase::CompatibilityRating>(compatibility);
    entry.traits.reset();
    for (u32 j = 0; j < static_cast<int>(Trait::Count); j++)
    {
      if ((bits[j / 8] & (1u << (j % 8))) != 0)
        entry.traits[j] = true;
    }
  }

  for (u32 i = 0; i < num_codes; i++)
  {
    std::string code;
    u32 index;
    if (!stream->ReadSizePrefixedString(&code) || !stream->ReadU32(&index) ||
        index >= static_cast<u32>(s_entries.size()))
    {
      Log_DevPrintf("Cache code entry is corrupted.");
      return false;
    }

    s_code_lookup.emplace(std::move(code), index);
  }

  return true;
}

bool GameDatabase::SaveToCache()
{
  u64 gamedb_ts, gamesettings_ts, compat_ts;
  GetTimestamps(&gamedb_ts, &gamesettings_ts, &compat_ts);

  std::unique_ptr<ByteStream> stream(
    ByteStream::OpenFile(GetCacheFile().c_str(), BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_WRITE |
                                                   BYTESTREAM_OPEN_TRUNCATE | BYTESTREAM_OPEN_STREAMED));
  if (!stream)
    return false;

  bool result = stream->WriteU32(GAME_DATABASE_CACHE_SIGNATURE);
  result = result && stream->WriteU32(GAME_DATABASE_CACHE_VERSION);
  result = result && stream->WriteU64(static_cast<u64>(gamedb_ts));
  result = result && stream->WriteU64(static_cast<u64>(gamesettings_ts));
  result = result && stream->WriteU64(static_cast<u64>(compat_ts));

  result = result && stream->WriteU32(static_cast<u32>(s_entries.size()));
  result = result && stream->WriteU32(static_cast<u32>(s_code_lookup.size()));

  for (const Entry& entry : s_entries)
  {
    result = result && stream->WriteSizePrefixedString(entry.serial);
    result = result && stream->WriteSizePrefixedString(entry.title);
    result = result && stream->WriteSizePrefixedString(entry.genre);
    result = result && stream->WriteSizePrefixedString(entry.developer);
    result = result && stream->WriteSizePrefixedString(entry.publisher);
    result = result && stream->WriteU64(entry.release_date);
    result = result && stream->WriteU8(entry.min_players);
    result = result && stream->WriteU8(entry.max_players);
    result = result && stream->WriteU8(entry.min_blocks);
    result = result && stream->WriteU8(entry.max_blocks);
    result = result && stream->WriteU32(entry.supported_controllers);
    result = result && stream->WriteU8(static_cast<u8>(entry.compatibility));

    constexpr u32 num_bytes = (static_cast<u32>(Trait::Count) + 7) / 8;
    std::array<u8, num_bytes> bits;
    bits.fill(0);
    for (u32 j = 0; j < static_cast<int>(Trait::Count); j++)
    {
      if (entry.traits[j])
        bits[j / 8] |= (1u << (j % 8));
    }

    result = result && stream->Write2(bits.data(), num_bytes);

    result = result && WriteOptionalToStream(stream.get(), entry.display_active_start_offset);
    result = result && WriteOptionalToStream(stream.get(), entry.display_active_end_offset);
    result = result && WriteOptionalToStream(stream.get(), entry.display_line_start_offset);
    result = result && WriteOptionalToStream(stream.get(), entry.display_line_end_offset);
    result = result && WriteOptionalToStream(stream.get(), entry.dma_max_slice_ticks);
    result = result && WriteOptionalToStream(stream.get(), entry.dma_halt_ticks);
    result = result && WriteOptionalToStream(stream.get(), entry.gpu_fifo_size);
    result = result && WriteOptionalToStream(stream.get(), entry.gpu_max_run_ahead);
    result = result && WriteOptionalToStream(stream.get(), entry.gpu_pgxp_tolerance);
    result = result && WriteOptionalToStream(stream.get(), entry.gpu_pgxp_depth_threshold);
  }

  for (const auto& it : s_code_lookup)
  {
    result = result && stream->WriteSizePrefixedString(it.first);
    result = result && stream->WriteU32(it.second);
  }

  result = result && stream->Flush();
  return true;
}

//////////////////////////////////////////////////////////////////////////
// JSON Parsing
//////////////////////////////////////////////////////////////////////////

static bool GetStringFromObject(const rapidjson::Value& object, const char* key, std::string* dest)
{
  dest->clear();
  auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsString())
    return false;

  dest->assign(member->value.GetString(), member->value.GetStringLength());
  return true;
}

static bool GetBoolFromObject(const rapidjson::Value& object, const char* key, bool* dest)
{
  *dest = false;

  auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsBool())
    return false;

  *dest = member->value.GetBool();
  return true;
}

template<typename T>
static bool GetUIntFromObject(const rapidjson::Value& object, const char* key, T* dest)
{
  *dest = 0;

  auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsUint())
    return false;

  *dest = static_cast<T>(member->value.GetUint());
  return true;
}

static bool GetArrayOfStringsFromObject(const rapidjson::Value& object, const char* key, std::vector<std::string>* dest)
{
  dest->clear();
  auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsArray())
    return false;

  for (const rapidjson::Value& str : member->value.GetArray())
  {
    if (str.IsString())
    {
      dest->emplace_back(str.GetString(), str.GetStringLength());
    }
  }
  return true;
}

template<typename T>
static std::optional<T> GetOptionalIntFromObject(const rapidjson::Value& object, const char* key)
{
  auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsInt())
    return std::nullopt;

  return static_cast<T>(member->value.GetInt());
}

template<typename T>
static std::optional<T> GetOptionalUIntFromObject(const rapidjson::Value& object, const char* key)
{
  auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsUint())
    return std::nullopt;

  return static_cast<T>(member->value.GetUint());
}

static std::optional<float> GetOptionalFloatFromObject(const rapidjson::Value& object, const char* key)
{
  auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsFloat())
    return std::nullopt;

  return member->value.GetFloat();
}

bool GameDatabase::LoadGameDBJson()
{
  std::optional<std::string> gamedb_data(Host::ReadResourceFileToString("gamedb.json"));
  if (!gamedb_data.has_value())
  {
    Log_ErrorPrintf("Failed to read game database");
    return false;
  }

  // TODO: Parse in-place, avoid string allocations.
  std::unique_ptr<rapidjson::Document> json = std::make_unique<rapidjson::Document>();
  json->Parse(gamedb_data->c_str(), gamedb_data->size());
  if (json->HasParseError())
  {
    Log_ErrorPrintf("Failed to parse game database: %s at offset %zu",
                    rapidjson::GetParseError_En(json->GetParseError()), json->GetErrorOffset());
    return false;
  }

  if (!json->IsArray())
  {
    Log_ErrorPrintf("Document is not an array");
    return false;
  }

  const auto& jarray = json->GetArray();
  s_entries.reserve(jarray.Size());

  for (const rapidjson::Value& current : json->GetArray())
  {
    // TODO: binary sort
    const u32 index = static_cast<u32>(s_entries.size());
    Entry& entry = s_entries.emplace_back();
    if (!ParseJsonEntry(&entry, current))
    {
      s_entries.pop_back();
      continue;
    }

    ParseJsonCodes(index, current);
  }

  Log_InfoPrintf("Loaded %zu entries and %zu codes from database", s_entries.size(), s_code_lookup.size());
  return true;
}

bool GameDatabase::ParseJsonEntry(Entry* entry, const rapidjson::Value& value)
{
  if (!value.IsObject())
  {
    Log_WarningPrintf("entry is not an object");
    return false;
  }

  if (!GetStringFromObject(value, "serial", &entry->serial) || !GetStringFromObject(value, "name", &entry->title) ||
      entry->serial.empty())
  {
    Log_ErrorPrintf("Missing serial or title for entry");
    return false;
  }

  GetStringFromObject(value, "genre", &entry->genre);
  GetStringFromObject(value, "developer", &entry->developer);
  GetStringFromObject(value, "publisher", &entry->publisher);

  GetUIntFromObject(value, "minPlayers", &entry->min_players);
  GetUIntFromObject(value, "maxPlayers", &entry->max_players);
  GetUIntFromObject(value, "minBlocks", &entry->min_blocks);
  GetUIntFromObject(value, "maxBlocks", &entry->max_blocks);

  entry->release_date = 0;
  {
    std::string release_date;
    if (GetStringFromObject(value, "releaseDate", &release_date))
    {
      std::istringstream iss(release_date);
      struct tm parsed_time = {};
      iss >> std::get_time(&parsed_time, "%Y-%m-%d");
      if (!iss.fail())
      {
        parsed_time.tm_isdst = 0;
#ifdef _WIN32
        entry->release_date = _mkgmtime(&parsed_time);
#else
        entry->release_date = timegm(&parsed_time);
#endif
      }
    }
  }

  entry->supported_controllers = ~0u;
  const auto controllers = value.FindMember("controllers");
  if (controllers != value.MemberEnd())
  {
    if (controllers->value.IsArray())
    {
      bool first = true;
      for (const rapidjson::Value& controller : controllers->value.GetArray())
      {
        if (!controller.IsString())
        {
          Log_WarningPrintf("controller is not a string");
          return false;
        }

        std::optional<ControllerType> ctype = Settings::ParseControllerTypeName(controller.GetString());
        if (!ctype.has_value())
        {
          Log_WarningPrintf("Invalid controller type '%s'", controller.GetString());
          continue;
        }

        if (first)
        {
          entry->supported_controllers = 0;
          first = false;
        }

        entry->supported_controllers |= (1u << static_cast<u32>(ctype.value()));
      }
    }
    else
    {
      Log_WarningPrintf("controllers is not an array");
    }
  }

  const auto compatibility = value.FindMember("compatibility");
  if (compatibility != value.MemberEnd())
  {
    if (compatibility->value.IsObject())
    {
      u32 rating;
      if (GetUIntFromObject(compatibility->value, "rating", &rating) &&
          rating < static_cast<u32>(CompatibilityRating::Count))
      {
        entry->compatibility = static_cast<CompatibilityRating>(rating);
      }
    }
    else
    {
      Log_WarningPrintf("compatibility is not an object");
    }
  }

  const auto traits = value.FindMember("traits");
  if (traits != value.MemberEnd())
  {
    if (traits->value.IsObject())
    {
      const auto& traitsobj = traits->value;
      for (u32 trait = 0; trait < static_cast<u32>(Trait::Count); trait++)
      {
        bool bvalue;
        if (GetBoolFromObject(traitsobj, s_trait_names[trait], &bvalue) && bvalue)
          entry->traits[trait] = bvalue;
      }

      entry->display_active_start_offset = GetOptionalIntFromObject<s16>(traitsobj, "DisplayActiveStartOffset");
      entry->display_active_end_offset = GetOptionalIntFromObject<s16>(traitsobj, "DisplayActiveEndOffset");
      entry->display_line_start_offset = GetOptionalIntFromObject<s8>(traitsobj, "DisplayLineStartOffset");
      entry->display_line_end_offset = GetOptionalIntFromObject<s8>(traitsobj, "DisplayLineEndOffset");
      entry->dma_max_slice_ticks = GetOptionalUIntFromObject<u32>(traitsobj, "DMAMaxSliceTicks");
      entry->dma_halt_ticks = GetOptionalUIntFromObject<u32>(traitsobj, "DMAHaltTicks");
      entry->gpu_fifo_size = GetOptionalUIntFromObject<u32>(traitsobj, "GPUFIFOSize");
      entry->gpu_max_run_ahead = GetOptionalUIntFromObject<u32>(traitsobj, "GPUMaxRunAhead");
      entry->gpu_pgxp_tolerance = GetOptionalFloatFromObject(traitsobj, "GPUPGXPTolerance");
      entry->gpu_pgxp_depth_threshold = GetOptionalFloatFromObject(traitsobj, "GPUPGXPDepthThreshold");
    }
    else
    {
      Log_WarningPrintf("traits is not an object");
    }
  }

  return true;
}

bool GameDatabase::ParseJsonCodes(u32 index, const rapidjson::Value& value)
{
  auto member = value.FindMember("codes");
  if (member == value.MemberEnd())
  {
    Log_WarningPrintf("codes member is missing");
    return false;
  }

  if (!member->value.IsArray())
  {
    Log_WarningPrintf("codes is not an array");
    return false;
  }

  u32 added = 0;
  for (const rapidjson::Value& current_code : member->value.GetArray())
  {
    if (!current_code.IsString())
    {
      Log_WarningPrintf("code is not a string");
      continue;
    }

    const std::string_view code(current_code.GetString(), current_code.GetStringLength());
    auto iter = UnorderedStringMapFind(s_code_lookup, code);
    if (iter != s_code_lookup.end())
    {
      Log_WarningPrintf("Duplicate code '%.*s'", static_cast<int>(code.size()), code.data());
      continue;
    }

    s_code_lookup.emplace(code, index);
    added++;
  }

  return (added > 0);
}

void GameDatabase::EnsureTrackHashesMapLoaded()
{
  if (s_track_hashes_loaded)
    return;

  s_track_hashes_loaded = true;
  LoadTrackHashes();
}

bool GameDatabase::LoadTrackHashes()
{
  std::optional<std::string> gamedb_data(Host::ReadResourceFileToString("gamedb.json"));
  if (!gamedb_data.has_value())
  {
    Log_ErrorPrintf("Failed to read game database");
    return false;
  }

  // TODO: Parse in-place, avoid string allocations.
  std::unique_ptr<rapidjson::Document> json = std::make_unique<rapidjson::Document>();
  json->Parse(gamedb_data->c_str(), gamedb_data->size());
  if (json->HasParseError())
  {
    Log_ErrorPrintf("Failed to parse game database: %s at offset %zu",
                    rapidjson::GetParseError_En(json->GetParseError()), json->GetErrorOffset());
    return false;
  }

  if (!json->IsArray())
  {
    Log_ErrorPrintf("Document is not an array");
    return false;
  }

  s_track_hashes_map = {};

  for (const rapidjson::Value& current : json->GetArray())
  {
    if (!current.IsObject())
    {
      Log_WarningPrintf("entry is not an object");
      continue;
    }

    std::vector<std::string> codes;
    if (!GetArrayOfStringsFromObject(current, "codes", &codes))
    {
      Log_WarningPrintf("codes member is missing");
      continue;
    }

    auto track_data = current.FindMember("track_data");
    if (track_data == current.MemberEnd())
    {
      Log_WarningPrintf("track_data member is missing");
      continue;
    }

    if (!track_data->value.IsArray())
    {
      Log_WarningPrintf("track_data is not an array");
      continue;
    }

    uint32_t revision = 0;
    for (const rapidjson::Value& track_revisions : track_data->value.GetArray())
    {
      if (!track_revisions.IsObject())
      {
        Log_WarningPrintf("track_data is not an array of object");
        continue;
      }

      auto tracks = track_revisions.FindMember("tracks");
      if (tracks == track_revisions.MemberEnd())
      {
        Log_WarningPrintf("tracks member is missing");
        continue;
      }

      if (!tracks->value.IsArray())
      {
        Log_WarningPrintf("tracks is not an array");
        continue;
      }

      std::string revisionString;
      GetStringFromObject(track_revisions, "version", &revisionString);

      for (const rapidjson::Value& track : tracks->value.GetArray())
      {
        auto md5_field = track.FindMember("md5");
        if (md5_field == track.MemberEnd() || !md5_field->value.IsString())
        {
          continue;
        }

        auto md5 = CDImageHasher::HashFromString(
          std::string_view(md5_field->value.GetString(), md5_field->value.GetStringLength()));
        if (md5)
        {
          s_track_hashes_map.emplace(std::piecewise_construct, std::forward_as_tuple(md5.value()),
                                     std::forward_as_tuple(codes, revisionString, revision));
        }
      }
      revision++;
    }
  }

  return true;
}

const GameDatabase::TrackHashesMap& GameDatabase::GetTrackHashesMap()
{
  EnsureTrackHashesMapLoaded();
  return s_track_hashes_map;
}
