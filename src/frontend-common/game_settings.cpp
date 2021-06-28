#include "game_settings.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string.h"
#include "common/string_util.h"
#include "core/host_interface.h"
#include "core/settings.h"
#include <array>
#include <utility>
Log_SetChannel(GameSettings);

#ifdef _WIN32
#include "common/windows_headers.h"
#endif
#include "SimpleIni.h"

namespace GameSettings {

std::array<std::pair<const char*, const char*>, static_cast<u32>(Trait::Count)> s_trait_names = {{
  {"ForceInterpreter", TRANSLATABLE("GameSettingsTrait", "Force Interpreter")},
  {"ForceSoftwareRenderer", TRANSLATABLE("GameSettingsTrait", "Force Software Renderer")},
  {"ForceSoftwareRendererForReadbacks", TRANSLATABLE("GameSettingsTrait", "Force Software Renderer For Readbacks")},
  {"ForceInterlacing", TRANSLATABLE("GameSettingsTrait", "Force Interlacing")},
  {"DisableTrueColor", TRANSLATABLE("GameSettingsTrait", "Disable True Color")},
  {"DisableUpscaling", TRANSLATABLE("GameSettingsTrait", "Disable Upscaling")},
  {"DisableScaledDithering", TRANSLATABLE("GameSettingsTrait", "Disable Scaled Dithering")},
  {"DisableForceNTSCTimings", TRANSLATABLE("GameSettingsTrait", "Disallow Forcing NTSC Timings")},
  {"DisableWidescreen", TRANSLATABLE("GameSettingsTrait", "Disable Widescreen")},
  {"DisablePGXP", TRANSLATABLE("GameSettingsTrait", "Disable PGXP")},
  {"DisablePGXPCulling", TRANSLATABLE("GameSettingsTrait", "Disable PGXP Culling")},
  {"DisablePGXPTextureCorrection", TRANSLATABLE("GameSettingsTrait", "Disable PGXP Texture Correction")},
  {"DisablePGXPDepthBuffer", TRANSLATABLE("GameSettingsTrait", "Disable PGXP Depth Buffer")},
  {"ForcePGXPVertexCache", TRANSLATABLE("GameSettingsTrait", "Force PGXP Vertex Cache")},
  {"ForcePGXPCPUMode", TRANSLATABLE("GameSettingsTrait", "Force PGXP CPU Mode")},
  {"ForceRecompilerMemoryExceptions", TRANSLATABLE("GameSettingsTrait", "Force Recompiler Memory Exceptions")},
  {"ForceRecompilerICache", TRANSLATABLE("GameSettingsTrait", "Force Recompiler ICache")},
  {"ForceRecompilerLUTFastmem", TRANSLATABLE("GameSettingsTrait", "Force Recompiler LUT Fastmem")},
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

static bool ReadStringFromStream(ByteStream* stream, std::string* dest)
{
  u32 size;
  if (!stream->Read2(&size, sizeof(size)))
    return false;

  dest->resize(size);
  if (!stream->Read2(dest->data(), size))
    return false;

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

static bool WriteStringToStream(ByteStream* stream, const std::string& str)
{
  const u32 size = static_cast<u32>(str.size());
  return (stream->Write2(&size, sizeof(size)) && (size == 0 || stream->Write2(str.data(), size)));
}

bool Entry::LoadFromStream(ByteStream* stream)
{
  constexpr u32 num_bytes = (static_cast<u32>(Trait::Count) + 7) / 8;
  std::array<u8, num_bytes> bits;

  if (!stream->Read2(bits.data(), num_bytes) || !ReadOptionalFromStream(stream, &runahead_frames) ||
      !ReadOptionalFromStream(stream, &cpu_overclock_numerator) ||
      !ReadOptionalFromStream(stream, &cpu_overclock_denominator) ||
      !ReadOptionalFromStream(stream, &cpu_overclock_enable) || !ReadOptionalFromStream(stream, &enable_8mb_ram) ||
      !ReadOptionalFromStream(stream, &cdrom_read_speedup) || !ReadOptionalFromStream(stream, &cdrom_seek_speedup) ||
      !ReadOptionalFromStream(stream, &display_active_start_offset) ||
      !ReadOptionalFromStream(stream, &display_active_end_offset) ||
      !ReadOptionalFromStream(stream, &display_line_start_offset) ||
      !ReadOptionalFromStream(stream, &display_line_end_offset) ||
      !ReadOptionalFromStream(stream, &dma_max_slice_ticks) || !ReadOptionalFromStream(stream, &dma_halt_ticks) ||
      !ReadOptionalFromStream(stream, &gpu_fifo_size) || !ReadOptionalFromStream(stream, &gpu_max_run_ahead) ||
      !ReadOptionalFromStream(stream, &gpu_pgxp_tolerance) ||
      !ReadOptionalFromStream(stream, &gpu_pgxp_depth_threshold) ||
      !ReadOptionalFromStream(stream, &display_crop_mode) || !ReadOptionalFromStream(stream, &display_aspect_ratio) ||
      !ReadOptionalFromStream(stream, &gpu_downsample_mode) ||
      !ReadOptionalFromStream(stream, &display_linear_upscaling) ||
      !ReadOptionalFromStream(stream, &display_integer_upscaling) ||
      !ReadOptionalFromStream(stream, &display_force_4_3_for_24bit) ||
      !ReadOptionalFromStream(stream, &display_aspect_ratio_custom_numerator) ||
      !ReadOptionalFromStream(stream, &display_aspect_ratio_custom_denominator) ||
      !ReadOptionalFromStream(stream, &gpu_renderer) || !ReadOptionalFromStream(stream, &gpu_resolution_scale) ||
      !ReadOptionalFromStream(stream, &gpu_multisamples) || !ReadOptionalFromStream(stream, &gpu_per_sample_shading) ||
      !ReadOptionalFromStream(stream, &gpu_true_color) || !ReadOptionalFromStream(stream, &gpu_scaled_dithering) ||
      !ReadOptionalFromStream(stream, &gpu_force_ntsc_timings) ||
      !ReadOptionalFromStream(stream, &gpu_texture_filter) || !ReadOptionalFromStream(stream, &gpu_widescreen_hack) ||
      !ReadOptionalFromStream(stream, &gpu_pgxp) || !ReadOptionalFromStream(stream, &gpu_pgxp_projection_precision) ||
      !ReadOptionalFromStream(stream, &gpu_pgxp_depth_buffer) || !ReadOptionalFromStream(stream, &multitap_mode) ||
      !ReadOptionalFromStream(stream, &controller_1_type) || !ReadOptionalFromStream(stream, &controller_2_type) ||
      !ReadOptionalFromStream(stream, &memory_card_1_type) || !ReadOptionalFromStream(stream, &memory_card_2_type) ||
      !ReadStringFromStream(stream, &memory_card_1_shared_path) ||
      !ReadStringFromStream(stream, &memory_card_2_shared_path) || !ReadStringFromStream(stream, &input_profile_name))
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

  return stream->Write2(bits.data(), num_bytes) && WriteOptionalToStream(stream, runahead_frames) &&
         WriteOptionalToStream(stream, cpu_overclock_numerator) &&
         WriteOptionalToStream(stream, cpu_overclock_denominator) &&
         WriteOptionalToStream(stream, cpu_overclock_enable) && WriteOptionalToStream(stream, enable_8mb_ram) &&
         WriteOptionalToStream(stream, cdrom_read_speedup) && WriteOptionalToStream(stream, cdrom_seek_speedup) &&
         WriteOptionalToStream(stream, display_active_start_offset) &&
         WriteOptionalToStream(stream, display_active_end_offset) &&
         WriteOptionalToStream(stream, display_line_start_offset) &&
         WriteOptionalToStream(stream, display_line_end_offset) && WriteOptionalToStream(stream, dma_max_slice_ticks) &&
         WriteOptionalToStream(stream, dma_halt_ticks) && WriteOptionalToStream(stream, gpu_fifo_size) &&
         WriteOptionalToStream(stream, gpu_max_run_ahead) && WriteOptionalToStream(stream, gpu_pgxp_tolerance) &&
         WriteOptionalToStream(stream, gpu_pgxp_depth_threshold) && WriteOptionalToStream(stream, display_crop_mode) &&
         WriteOptionalToStream(stream, display_aspect_ratio) && WriteOptionalToStream(stream, gpu_downsample_mode) &&
         WriteOptionalToStream(stream, display_linear_upscaling) &&
         WriteOptionalToStream(stream, display_integer_upscaling) &&
         WriteOptionalToStream(stream, display_force_4_3_for_24bit) &&
         WriteOptionalToStream(stream, display_aspect_ratio_custom_numerator) &&
         WriteOptionalToStream(stream, display_aspect_ratio_custom_denominator) &&
         WriteOptionalToStream(stream, gpu_renderer) && WriteOptionalToStream(stream, gpu_resolution_scale) &&
         WriteOptionalToStream(stream, gpu_multisamples) && WriteOptionalToStream(stream, gpu_per_sample_shading) &&
         WriteOptionalToStream(stream, gpu_true_color) && WriteOptionalToStream(stream, gpu_scaled_dithering) &&
         WriteOptionalToStream(stream, gpu_force_ntsc_timings) && WriteOptionalToStream(stream, gpu_texture_filter) &&
         WriteOptionalToStream(stream, gpu_widescreen_hack) && WriteOptionalToStream(stream, gpu_pgxp) &&
         WriteOptionalToStream(stream, gpu_pgxp_projection_precision) &&
         WriteOptionalToStream(stream, gpu_pgxp_depth_buffer) && WriteOptionalToStream(stream, multitap_mode) &&
         WriteOptionalToStream(stream, controller_1_type) && WriteOptionalToStream(stream, controller_2_type) &&
         WriteOptionalToStream(stream, memory_card_1_type) && WriteOptionalToStream(stream, memory_card_2_type) &&
         WriteStringToStream(stream, memory_card_1_shared_path) &&
         WriteStringToStream(stream, memory_card_2_shared_path) && WriteStringToStream(stream, input_profile_name);
}

static void ParseIniSection(Entry* entry, const char* section, const CSimpleIniA& ini)
{
  for (u32 trait = 0; trait < static_cast<u32>(Trait::Count); trait++)
  {
    if (ini.GetBoolValue(section, s_trait_names[trait].first, false))
      entry->AddTrait(static_cast<Trait>(trait));
  }

  const char* cvalue = ini.GetValue(section, "RunaheadFrameCount", nullptr);
  if (cvalue)
    entry->runahead_frames = StringUtil::FromChars<u32>(cvalue);

  cvalue = ini.GetValue(section, "CPUOverclockNumerator", nullptr);
  if (cvalue)
    entry->cpu_overclock_numerator = StringUtil::FromChars<u32>(cvalue);
  cvalue = ini.GetValue(section, "CPUOverclockDenominator", nullptr);
  if (cvalue)
    entry->cpu_overclock_denominator = StringUtil::FromChars<u32>(cvalue);
  cvalue = ini.GetValue(section, "CPUOverclockEnable", nullptr);
  if (cvalue)
    entry->cpu_overclock_enable = StringUtil::FromChars<bool>(cvalue);
  cvalue = ini.GetValue(section, "Enable8MBRAM", nullptr);
  if (cvalue)
    entry->enable_8mb_ram = StringUtil::FromChars<bool>(cvalue);

  cvalue = ini.GetValue(section, "CDROMReadSpeedup", nullptr);
  if (cvalue)
    entry->cdrom_read_speedup = StringUtil::FromChars<u32>(cvalue);
  cvalue = ini.GetValue(section, "CDROMSeekSpeedup", nullptr);
  if (cvalue)
    entry->cdrom_seek_speedup = StringUtil::FromChars<u32>(cvalue);

  long lvalue = ini.GetLongValue(section, "DisplayActiveStartOffset", 0);
  if (lvalue != 0)
    entry->display_active_start_offset = static_cast<s16>(lvalue);
  lvalue = ini.GetLongValue(section, "DisplayActiveEndOffset", 0);
  if (lvalue != 0)
    entry->display_active_end_offset = static_cast<s16>(lvalue);
  lvalue = ini.GetLongValue(section, "DisplayLineStartOffset", 0);
  if (lvalue != 0)
    entry->display_line_start_offset = static_cast<s8>(lvalue);
  lvalue = ini.GetLongValue(section, "DisplayLineEndOffset", 0);
  if (lvalue != 0)
    entry->display_line_end_offset = static_cast<s8>(lvalue);
  lvalue = ini.GetLongValue(section, "DMAMaxSliceTicks", 0);
  if (lvalue > 0)
    entry->dma_max_slice_ticks = static_cast<u32>(lvalue);
  lvalue = ini.GetLongValue(section, "DMAHaltTicks", 0);
  if (lvalue > 0)
    entry->dma_halt_ticks = static_cast<u32>(lvalue);
  lvalue = ini.GetLongValue(section, "GPUFIFOSize", 0);
  if (lvalue > 0)
    entry->gpu_fifo_size = static_cast<u32>(lvalue);
  lvalue = ini.GetLongValue(section, "GPUMaxRunAhead", 0);
  if (lvalue > 0)
    entry->gpu_max_run_ahead = static_cast<u32>(lvalue);
  float fvalue = static_cast<float>(ini.GetDoubleValue(section, "GPUPGXPTolerance", -1.0f));
  if (fvalue >= 0.0f)
    entry->gpu_pgxp_tolerance = fvalue;
  fvalue = static_cast<float>(ini.GetDoubleValue(section, "GPUPGXPDepthThreshold", -1.0f));
  if (fvalue > 0.0f)
    entry->gpu_pgxp_depth_threshold = fvalue;

  cvalue = ini.GetValue(section, "DisplayCropMode", nullptr);
  if (cvalue)
    entry->display_crop_mode = Settings::ParseDisplayCropMode(cvalue);
  cvalue = ini.GetValue(section, "DisplayAspectRatio", nullptr);
  if (cvalue)
    entry->display_aspect_ratio = Settings::ParseDisplayAspectRatio(cvalue);
  lvalue = ini.GetLongValue(section, "CustomAspectRatioNumerator", 0);
  if (lvalue != 0)
  {
    entry->display_aspect_ratio_custom_numerator =
      static_cast<u16>(std::clamp<long>(lvalue, 1, std::numeric_limits<u16>::max()));
  }
  lvalue = ini.GetLongValue(section, "CustomAspectRatioDenominator", 0);
  if (lvalue != 0)
  {
    entry->display_aspect_ratio_custom_denominator =
      static_cast<u16>(std::clamp<long>(lvalue, 1, std::numeric_limits<u16>::max()));
  }
  cvalue = ini.GetValue(section, "GPURenderer", nullptr);
  if (cvalue)
    entry->gpu_renderer = Settings::ParseRendererName(cvalue);
  cvalue = ini.GetValue(section, "GPUDownsampleMode", nullptr);
  if (cvalue)
    entry->gpu_downsample_mode = Settings::ParseDownsampleModeName(cvalue);
  cvalue = ini.GetValue(section, "DisplayLinearUpscaling", nullptr);
  if (cvalue)
    entry->display_linear_upscaling = StringUtil::FromChars<bool>(cvalue);
  cvalue = ini.GetValue(section, "DisplayIntegerUpscaling", nullptr);
  if (cvalue)
    entry->display_integer_upscaling = StringUtil::FromChars<bool>(cvalue);
  cvalue = ini.GetValue(section, "DisplayForce4_3For24Bit", nullptr);
  if (cvalue)
    entry->display_force_4_3_for_24bit = StringUtil::FromChars<bool>(cvalue);

  cvalue = ini.GetValue(section, "GPUResolutionScale", nullptr);
  if (cvalue)
    entry->gpu_resolution_scale = StringUtil::FromChars<u32>(cvalue);
  cvalue = ini.GetValue(section, "GPUMultisamples", nullptr);
  if (cvalue)
    entry->gpu_multisamples = StringUtil::FromChars<u32>(cvalue);
  cvalue = ini.GetValue(section, "GPUPerSampleShading", nullptr);
  if (cvalue)
    entry->gpu_per_sample_shading = StringUtil::FromChars<bool>(cvalue);
  cvalue = ini.GetValue(section, "GPUTrueColor", nullptr);
  if (cvalue)
    entry->gpu_true_color = StringUtil::FromChars<bool>(cvalue);
  cvalue = ini.GetValue(section, "GPUScaledDithering", nullptr);
  if (cvalue)
    entry->gpu_scaled_dithering = StringUtil::FromChars<bool>(cvalue);
  cvalue = ini.GetValue(section, "GPUTextureFilter", nullptr);
  if (cvalue)
    entry->gpu_texture_filter = Settings::ParseTextureFilterName(cvalue);
  cvalue = ini.GetValue(section, "GPUForceNTSCTimings", nullptr);
  if (cvalue)
    entry->gpu_force_ntsc_timings = StringUtil::FromChars<bool>(cvalue);
  cvalue = ini.GetValue(section, "GPUWidescreenHack", nullptr);
  if (cvalue)
    entry->gpu_widescreen_hack = StringUtil::FromChars<bool>(cvalue);
  cvalue = ini.GetValue(section, "GPUPGXP", nullptr);
  if (cvalue)
    entry->gpu_pgxp = StringUtil::FromChars<bool>(cvalue);
  cvalue = ini.GetValue(section, "GPUPGXPPreserveProjFP", nullptr);
  if (cvalue)
    entry->gpu_pgxp_projection_precision = StringUtil::FromChars<bool>(cvalue);
  cvalue = ini.GetValue(section, "GPUPGXPDepthBuffer", nullptr);
  if (cvalue)
    entry->gpu_pgxp_depth_buffer = StringUtil::FromChars<bool>(cvalue);

  cvalue = ini.GetValue(section, "MultitapMode", nullptr);
  if (cvalue)
    entry->multitap_mode = Settings::ParseMultitapModeName(cvalue);
  cvalue = ini.GetValue(section, "Controller1Type", nullptr);
  if (cvalue)
    entry->controller_1_type = Settings::ParseControllerTypeName(cvalue);
  cvalue = ini.GetValue(section, "Controller2Type", nullptr);
  if (cvalue)
    entry->controller_2_type = Settings::ParseControllerTypeName(cvalue);

  cvalue = ini.GetValue(section, "MemoryCard1Type", nullptr);
  if (cvalue)
    entry->memory_card_1_type = Settings::ParseMemoryCardTypeName(cvalue);
  cvalue = ini.GetValue(section, "MemoryCard2Type", nullptr);
  if (cvalue)
    entry->memory_card_2_type = Settings::ParseMemoryCardTypeName(cvalue);
  cvalue = ini.GetValue(section, "MemoryCard1SharedPath");
  if (cvalue)
    entry->memory_card_1_shared_path = cvalue;
  cvalue = ini.GetValue(section, "MemoryCard2SharedPath");
  if (cvalue)
    entry->memory_card_2_shared_path = cvalue;
  cvalue = ini.GetValue(section, "InputProfileName");
  if (cvalue)
    entry->input_profile_name = cvalue;
}

static void StoreIniSection(const Entry& entry, const char* section, CSimpleIniA& ini)
{
  for (u32 trait = 0; trait < static_cast<u32>(Trait::Count); trait++)
  {
    if (entry.HasTrait(static_cast<Trait>(trait)))
      ini.SetBoolValue(section, s_trait_names[trait].first, true);
  }

  if (entry.runahead_frames.has_value())
    ini.SetLongValue(section, "RunaheadFrameCount", static_cast<long>(entry.runahead_frames.value()));

  if (entry.cpu_overclock_numerator.has_value())
    ini.SetLongValue(section, "CPUOverclockNumerator", static_cast<long>(entry.cpu_overclock_numerator.value()));
  if (entry.cpu_overclock_denominator.has_value())
    ini.SetLongValue(section, "CPUOverclockDenominator", static_cast<long>(entry.cpu_overclock_denominator.value()));
  if (entry.cpu_overclock_enable.has_value())
    ini.SetBoolValue(section, "CPUOverclockEnable", entry.cpu_overclock_enable.value());
  if (entry.enable_8mb_ram.has_value())
    ini.SetBoolValue(section, "Enable8MBRAM", entry.enable_8mb_ram.value());

  if (entry.cdrom_read_speedup.has_value())
    ini.SetLongValue(section, "CDROMReadSpeedup", static_cast<long>(entry.cdrom_read_speedup.value()));
  if (entry.cdrom_seek_speedup.has_value())
    ini.SetLongValue(section, "CDROMSeekSpeedup", static_cast<long>(entry.cdrom_seek_speedup.value()));

  if (entry.display_active_start_offset.has_value())
    ini.SetLongValue(section, "DisplayActiveStartOffset", entry.display_active_start_offset.value());
  if (entry.display_active_end_offset.has_value())
    ini.SetLongValue(section, "DisplayActiveEndOffset", entry.display_active_end_offset.value());
  if (entry.display_line_start_offset.has_value())
    ini.SetLongValue(section, "DisplayLineStartOffset", entry.display_line_start_offset.value());
  if (entry.display_line_end_offset.has_value())
    ini.SetLongValue(section, "DisplayLineEndOffset", entry.display_line_end_offset.value());
  if (entry.dma_max_slice_ticks.has_value())
    ini.SetLongValue(section, "DMAMaxSliceTicks", static_cast<long>(entry.dma_max_slice_ticks.value()));
  if (entry.dma_halt_ticks.has_value())
    ini.SetLongValue(section, "DMAHaltTicks", static_cast<long>(entry.dma_halt_ticks.value()));
  if (entry.gpu_fifo_size.has_value())
    ini.SetLongValue(section, "GPUFIFOSize", static_cast<long>(entry.gpu_fifo_size.value()));
  if (entry.gpu_max_run_ahead.has_value())
    ini.SetLongValue(section, "GPUMaxRunAhead", static_cast<long>(entry.gpu_max_run_ahead.value()));
  if (entry.gpu_pgxp_tolerance.has_value())
    ini.SetDoubleValue(section, "GPUPGXPTolerance", static_cast<double>(entry.gpu_pgxp_tolerance.value()));
  if (entry.gpu_pgxp_depth_threshold.has_value())
    ini.SetDoubleValue(section, "GPUPGXPDepthThreshold", static_cast<double>(entry.gpu_pgxp_depth_threshold.value()));

  if (entry.display_crop_mode.has_value())
    ini.SetValue(section, "DisplayCropMode", Settings::GetDisplayCropModeName(entry.display_crop_mode.value()));
  if (entry.display_aspect_ratio.has_value())
  {
    ini.SetValue(section, "DisplayAspectRatio",
                 Settings::GetDisplayAspectRatioName(entry.display_aspect_ratio.value()));
  }
  if (entry.display_aspect_ratio_custom_numerator.has_value())
  {
    ini.SetLongValue(section, "CustomAspectRatioNumerator",
                     static_cast<long>(entry.display_aspect_ratio_custom_numerator.value()));
  }
  if (entry.display_aspect_ratio_custom_denominator.has_value())
  {
    ini.SetLongValue(section, "CustomAspectRatioDenominator",
                     static_cast<long>(entry.display_aspect_ratio_custom_denominator.value()));
  }
  if (entry.gpu_renderer.has_value())
    ini.SetValue(section, "GPURenderer", Settings::GetRendererName(entry.gpu_renderer.value()));
  if (entry.gpu_downsample_mode.has_value())
  {
    ini.SetValue(section, "GPUDownsampleMode", Settings::GetDownsampleModeName(entry.gpu_downsample_mode.value()));
  }
  if (entry.display_linear_upscaling.has_value())
    ini.SetValue(section, "DisplayLinearUpscaling", entry.display_linear_upscaling.value() ? "true" : "false");
  if (entry.display_integer_upscaling.has_value())
    ini.SetValue(section, "DisplayIntegerUpscaling", entry.display_integer_upscaling.value() ? "true" : "false");
  if (entry.display_force_4_3_for_24bit.has_value())
    ini.SetValue(section, "DisplayForce4_3For24Bit", entry.display_force_4_3_for_24bit.value() ? "true" : "false");

  if (entry.gpu_resolution_scale.has_value())
    ini.SetLongValue(section, "GPUResolutionScale", static_cast<s32>(entry.gpu_resolution_scale.value()));
  if (entry.gpu_multisamples.has_value())
    ini.SetLongValue(section, "GPUMultisamples", static_cast<s32>(entry.gpu_multisamples.value()));
  if (entry.gpu_per_sample_shading.has_value())
    ini.SetValue(section, "GPUPerSampleShading", entry.gpu_per_sample_shading.value() ? "true" : "false");
  if (entry.gpu_true_color.has_value())
    ini.SetValue(section, "GPUTrueColor", entry.gpu_true_color.value() ? "true" : "false");
  if (entry.gpu_scaled_dithering.has_value())
    ini.SetValue(section, "GPUScaledDithering", entry.gpu_scaled_dithering.value() ? "true" : "false");
  if (entry.gpu_texture_filter.has_value())
    ini.SetValue(section, "GPUTextureFilter", Settings::GetTextureFilterName(entry.gpu_texture_filter.value()));
  if (entry.gpu_force_ntsc_timings.has_value())
    ini.SetValue(section, "GPUForceNTSCTimings", entry.gpu_force_ntsc_timings.value() ? "true" : "false");
  if (entry.gpu_widescreen_hack.has_value())
    ini.SetValue(section, "GPUWidescreenHack", entry.gpu_widescreen_hack.value() ? "true" : "false");
  if (entry.gpu_pgxp.has_value())
    ini.SetValue(section, "GPUPGXP", entry.gpu_pgxp.value() ? "true" : "false");
  if (entry.gpu_pgxp_projection_precision.has_value())
    ini.SetValue(section, "GPUPGXPPreserveProjFP", entry.gpu_pgxp_projection_precision.value() ? "true" : "false");
  if (entry.gpu_pgxp_depth_buffer.has_value())
    ini.SetValue(section, "GPUPGXPDepthBuffer", entry.gpu_pgxp_depth_buffer.value() ? "true" : "false");

  if (entry.multitap_mode.has_value())
    ini.SetValue(section, "MultitapMode", Settings::GetMultitapModeName(entry.multitap_mode.value()));
  if (entry.controller_1_type.has_value())
    ini.SetValue(section, "Controller1Type", Settings::GetControllerTypeName(entry.controller_1_type.value()));
  if (entry.controller_2_type.has_value())
    ini.SetValue(section, "Controller2Type", Settings::GetControllerTypeName(entry.controller_2_type.value()));
  if (entry.memory_card_1_type.has_value())
    ini.SetValue(section, "MemoryCard1Type", Settings::GetMemoryCardTypeName(entry.memory_card_1_type.value()));
  if (entry.memory_card_2_type.has_value())
    ini.SetValue(section, "MemoryCard2Type", Settings::GetMemoryCardTypeName(entry.memory_card_2_type.value()));
  if (!entry.memory_card_1_shared_path.empty())
    ini.SetValue(section, "MemoryCard1SharedPath", entry.memory_card_1_shared_path.c_str());
  if (!entry.memory_card_2_shared_path.empty())
    ini.SetValue(section, "MemoryCard2SharedPath", entry.memory_card_2_shared_path.c_str());
  if (!entry.input_profile_name.empty())
    ini.SetValue(section, "InputProfileName", entry.input_profile_name.c_str());
}

u32 Entry::GetUserSettingsCount() const
{
  u32 count = 0;

  count += BoolToUInt32(runahead_frames.has_value());
  count += BoolToUInt32(cpu_overclock_numerator.has_value());
  count += BoolToUInt32(cpu_overclock_denominator.has_value());
  count += BoolToUInt32(cpu_overclock_enable.has_value());
  count += BoolToUInt32(enable_8mb_ram.has_value());
  count += BoolToUInt32(cdrom_read_speedup.has_value());
  count += BoolToUInt32(cdrom_seek_speedup.has_value());
  count += BoolToUInt32(display_crop_mode.has_value());
  count += BoolToUInt32(display_aspect_ratio.has_value());
  count += BoolToUInt32(gpu_downsample_mode.has_value());
  count += BoolToUInt32(display_linear_upscaling.has_value());
  count += BoolToUInt32(display_integer_upscaling.has_value());
  count += BoolToUInt32(display_force_4_3_for_24bit.has_value());
  count += BoolToUInt32(gpu_renderer.has_value());
  count += BoolToUInt32(gpu_resolution_scale.has_value());
  count += BoolToUInt32(gpu_multisamples.has_value());
  count += BoolToUInt32(gpu_per_sample_shading.has_value());
  count += BoolToUInt32(gpu_true_color.has_value());
  count += BoolToUInt32(gpu_scaled_dithering.has_value());
  count += BoolToUInt32(gpu_force_ntsc_timings.has_value());
  count += BoolToUInt32(gpu_texture_filter.has_value());
  count += BoolToUInt32(gpu_widescreen_hack.has_value());
  count += BoolToUInt32(gpu_pgxp.has_value());
  count += BoolToUInt32(gpu_pgxp_projection_precision.has_value());
  count += BoolToUInt32(gpu_pgxp_depth_buffer.has_value());
  count += BoolToUInt32(multitap_mode.has_value());
  count += BoolToUInt32(controller_1_type.has_value());
  count += BoolToUInt32(controller_2_type.has_value());
  count += BoolToUInt32(memory_card_1_type.has_value());
  count += BoolToUInt32(memory_card_2_type.has_value());
  count += BoolToUInt32(!memory_card_1_shared_path.empty());
  count += BoolToUInt32(!memory_card_2_shared_path.empty());
  count += BoolToUInt32(!input_profile_name.empty());

  return count;
}

static std::optional<std::string> GetEntryValueForKey(const Entry& entry, const std::string_view& key)
{
  if (key == "RunaheadFrameCount")
  {
    if (!entry.runahead_frames.has_value())
      return std::nullopt;

    return std::to_string(entry.runahead_frames.value());
  }
  else if (key == "CPUOverclock")
  {
    if (!entry.cpu_overclock_enable.has_value())
      return std::nullopt;

    return std::to_string(Settings::CPUOverclockFractionToPercent(entry.cpu_overclock_numerator.value_or(1),
                                                                  entry.cpu_overclock_denominator.value_or(1)));
  }
  else if (key == "Enable8MBRAM")
  {
    if (!entry.enable_8mb_ram.has_value())
      return std::nullopt;
    else
      return entry.enable_8mb_ram.value() ? "true" : "false";
  }
  else if (key == "CDROMReadSpeedup")
  {
    if (!entry.cdrom_read_speedup.has_value())
      return std::nullopt;
    else
      return std::to_string(entry.cdrom_read_speedup.value());
  }
  else if (key == "CDROMSeekSpeedup")
  {
    if (!entry.cdrom_seek_speedup.has_value())
      return std::nullopt;
    else
      return std::to_string(entry.cdrom_seek_speedup.value());
  }
  else if (key == "DisplayCropMode")
  {
    if (!entry.display_crop_mode.has_value())
      return std::nullopt;
    else
      return Settings::GetDisplayCropModeName(entry.display_crop_mode.value());
  }
  else if (key == "DisplayAspectRatio")
  {
    if (!entry.display_aspect_ratio.has_value())
      return std::nullopt;
    else
      return Settings::GetDisplayAspectRatioName(entry.display_aspect_ratio.value());
  }
  else if (key == "CustomAspectRatioNumerator")
  {
    if (!entry.display_aspect_ratio_custom_numerator.has_value())
      return std::nullopt;
    else
      return std::to_string(entry.display_aspect_ratio_custom_numerator.value());
  }
  else if (key == "CustomAspectRatioDenominator")
  {
    if (!entry.display_aspect_ratio_custom_denominator.has_value())
      return std::nullopt;
    else
      return std::to_string(entry.display_aspect_ratio_custom_denominator.value());
  }
  else if (key == "GPURenderer")
  {
    if (!entry.gpu_renderer.has_value())
      return std::nullopt;
    else
      return Settings::GetRendererName(entry.gpu_renderer.value());
  }
  else if (key == "GPUDownsampleMode")
  {
    if (!entry.gpu_downsample_mode.has_value())
      return std::nullopt;
    else
      return Settings::GetDownsampleModeName(entry.gpu_downsample_mode.value());
  }
  else if (key == "DisplayLinearUpscaling")
  {
    if (!entry.display_linear_upscaling.has_value())
      return std::nullopt;
    else
      return entry.display_linear_upscaling.value() ? "true" : "false";
  }
  else if (key == "DisplayIntegerUpscaling")
  {
    if (!entry.display_integer_upscaling.has_value())
      return std::nullopt;
    else
      return entry.display_integer_upscaling.value() ? "true" : "false";
  }
  else if (key == "DisplayForce4_3For24Bit")
  {
    if (!entry.display_force_4_3_for_24bit.has_value())
      return std::nullopt;
    else
      return entry.display_force_4_3_for_24bit.value() ? "true" : "false";
  }
  else if (key == "GPUResolutionScale")
  {
    if (!entry.gpu_resolution_scale.has_value())
      return std::nullopt;
    else
      return std::to_string(entry.gpu_resolution_scale.value());
  }
  else if (key == "GPUMSAA")
  {
    if (!entry.gpu_multisamples.has_value())
    {
      return std::nullopt;
    }
    else
    {
      return StringUtil::StdStringFromFormat("%u%s", entry.gpu_multisamples.value(),
                                             entry.gpu_per_sample_shading.value_or(false) ? "-ssaa" : "");
    }
  }
  else if (key == "GPUTrueColor")
  {
    if (!entry.gpu_true_color.has_value())
      return std::nullopt;
    else
      return entry.gpu_true_color.value() ? "true" : "false";
  }
  else if (key == "GPUScaledDithering")
  {
    if (!entry.gpu_scaled_dithering.has_value())
      return std::nullopt;
    else
      return entry.gpu_scaled_dithering.value() ? "true" : "false";
  }
  else if (key == "GPUTextureFilter")
  {
    if (!entry.gpu_texture_filter.has_value())
      return std::nullopt;
    else
      return Settings::GetTextureFilterName(entry.gpu_texture_filter.value());
  }
  else if (key == "GPUForceNTSCTimings")
  {
    if (!entry.gpu_force_ntsc_timings.has_value())
      return std::nullopt;
    else
      return entry.gpu_force_ntsc_timings.value() ? "true" : "false";
  }
  else if (key == "GPUWidescreenHack")
  {
    if (!entry.gpu_widescreen_hack.has_value())
      return std::nullopt;
    else
      return entry.gpu_widescreen_hack.value() ? "true" : "false";
  }
  else if (key == "GPUPGXP")
  {
    if (!entry.gpu_pgxp.has_value())
      return std::nullopt;
    else
      return entry.gpu_pgxp.value() ? "true" : "false";
  }
  else if (key == "GPUPGXPPreserveProjFP")
  {
    if (!entry.gpu_pgxp_projection_precision.has_value())
      return std::nullopt;
    else
      return entry.gpu_pgxp_projection_precision.value() ? "true" : "false";
  }
  else if (key == "GPUPGXPDepthBuffer")
  {
    if (!entry.gpu_pgxp_depth_buffer.has_value())
      return std::nullopt;
    else
      return entry.gpu_pgxp_depth_buffer.value() ? "true" : "false";
  }
  else if (key == "MultitapMode")
  {
    if (!entry.multitap_mode.has_value())
      return std::nullopt;
    else
      return Settings::GetMultitapModeName(entry.multitap_mode.value());
  }
  else if (key == "Controller1Type")
  {
    if (!entry.controller_1_type.has_value())
      return std::nullopt;
    else
      return Settings::GetControllerTypeName(entry.controller_1_type.value());
  }
  else if (key == "Controller2Type")
  {
    if (!entry.controller_2_type.has_value())
      return std::nullopt;
    else
      return Settings::GetControllerTypeName(entry.controller_2_type.value());
  }
  else if (key == "MemoryCard1Type")
  {
    if (!entry.memory_card_1_type.has_value())
      return std::nullopt;
    else
      return Settings::GetMemoryCardTypeName(entry.memory_card_1_type.value());
  }
  else if (key == "MemoryCard2Type")
  {
    if (!entry.memory_card_2_type.has_value())
      return std::nullopt;
    else
      return Settings::GetMemoryCardTypeName(entry.memory_card_2_type.value());
  }
  else if (key == "MemoryCard1SharedPath")
  {
    if (entry.memory_card_1_shared_path.empty())
      return std::nullopt;
    else
      return entry.memory_card_1_shared_path;
  }
  else if (key == "MemoryCard2SharedPath")
  {
    if (entry.memory_card_2_shared_path.empty())
      return std::nullopt;
    else
      return entry.memory_card_2_shared_path;
  }
  else if (key == "InputProfileName")
  {
    if (entry.input_profile_name.empty())
      return std::nullopt;
    else
      return entry.input_profile_name;
  }
  else if (key == "ForceSoftwareRenderer")
  {
    return entry.HasTrait(Trait::ForceSoftwareRenderer) ? "true" : "false";
  }
  else if (key == "ForceSoftwareRendererForReadbacks")
  {
    return entry.HasTrait(Trait::ForceSoftwareRendererForReadbacks) ? "true" : "false";
  }
  else if (key == "DisableWidescreen")
  {
    return entry.HasTrait(Trait::DisableWidescreen) ? "true" : "false";
  }
  else if (key == "ForcePGXPVertexCache")
  {
    return entry.HasTrait(Trait::ForcePGXPVertexCache) ? "true" : "false";
  }
  else if (key == "ForcePGXPCPUMode")
  {
    return entry.HasTrait(Trait::ForcePGXPCPUMode) ? "true" : "false";
  }
  else
  {
    Log_ErrorPrintf("Unknown key: %s", std::string(key).c_str());
    return std::nullopt;
  }
}

static void SetEntryValueForKey(Entry& entry, const std::string_view& key, const std::optional<std::string>& value)
{
  if (key == "RunaheadFrameCount")
  {
    if (!value.has_value())
      entry.runahead_frames.reset();
    else
      entry.runahead_frames = StringUtil::FromChars<u32>(value.value());
  }
  else if (key == "CPUOverclock")
  {
    if (!value.has_value())
    {
      entry.cpu_overclock_numerator.reset();
      entry.cpu_overclock_denominator.reset();
      entry.cpu_overclock_enable.reset();
    }
    else
    {
      u32 numerator, denominator;
      Settings::CPUOverclockPercentToFraction(StringUtil::FromChars<u32>(value.value()).value_or(100), &numerator,
                                              &denominator);
      entry.cpu_overclock_numerator = numerator;
      entry.cpu_overclock_denominator = denominator;
      entry.cpu_overclock_enable = true;
    }
  }
  else if (key == "Enable8MBRAM")
  {
    if (!value.has_value())
      entry.enable_8mb_ram.reset();
    else
      entry.enable_8mb_ram = StringUtil::FromChars<bool>(value.value()).value_or(false);
  }
  else if (key == "CDROMReadSpeedup")
  {
    if (!value.has_value())
      entry.cdrom_read_speedup.reset();
    else
      entry.cdrom_read_speedup = StringUtil::FromChars<u32>(value.value());
  }
  else if (key == "CDROMSeekSpeedup")
  {
    if (!value.has_value())
      entry.cdrom_seek_speedup.reset();
    else
      entry.cdrom_seek_speedup = StringUtil::FromChars<u32>(value.value());
  }
  else if (key == "DisplayCropMode")
  {
    if (!value.has_value())
      entry.display_crop_mode.reset();
    else
      entry.display_crop_mode = Settings::ParseDisplayCropMode(value->c_str());
  }
  else if (key == "DisplayAspectRatio")
  {
    if (!value.has_value())
      entry.display_aspect_ratio.reset();
    else
      entry.display_aspect_ratio = Settings::ParseDisplayAspectRatio(value->c_str());
  }
  else if (key == "CustomAspectRatioNumerator")
  {
    if (!value.has_value())
      entry.display_aspect_ratio_custom_numerator.reset();
    else
      entry.display_aspect_ratio_custom_numerator = StringUtil::FromChars<u16>(value.value());
  }
  else if (key == "CustomAspectRatioDenominator")
  {
    if (!value.has_value())
      entry.display_aspect_ratio_custom_denominator.reset();
    else
      entry.display_aspect_ratio_custom_denominator = StringUtil::FromChars<u16>(value.value());
  }
  else if (key == "GPURenderer")
  {
    if (!value.has_value())
      entry.gpu_renderer.reset();
    else
      entry.gpu_renderer = Settings::ParseRendererName(value->c_str());
  }
  else if (key == "GPUDownsampleMode")
  {
    if (!value.has_value())
      entry.gpu_downsample_mode.reset();
    else
      entry.gpu_downsample_mode = Settings::ParseDownsampleModeName(value->c_str());
  }
  else if (key == "DisplayLinearUpscaling")
  {
    if (!value.has_value())
      entry.display_linear_upscaling.reset();
    else
      entry.display_linear_upscaling = StringUtil::FromChars<bool>(value.value()).value_or(false);
  }
  else if (key == "DisplayIntegerUpscaling")
  {
    if (!value.has_value())
      entry.display_integer_upscaling.reset();
    else
      entry.display_integer_upscaling = StringUtil::FromChars<bool>(value.value()).value_or(false);
  }
  else if (key == "DisplayForce4_3For24Bit")
  {
    if (!value.has_value())
      entry.display_force_4_3_for_24bit.reset();
    else
      entry.display_force_4_3_for_24bit = StringUtil::FromChars<bool>(value.value()).value_or(false);
  }
  else if (key == "GPUResolutionScale")
  {
    if (!value.has_value())
      entry.gpu_resolution_scale.reset();
    else
      entry.gpu_resolution_scale = StringUtil::FromChars<u32>(value.value());
  }
  else if (key == "GPUMSAA")
  {
    if (!value.has_value())
    {
      entry.gpu_multisamples.reset();
      entry.gpu_per_sample_shading.reset();
    }
    else
    {
      if (StringUtil::EndsWith(value.value(), "-ssaa"))
      {
        entry.gpu_multisamples =
          StringUtil::FromChars<u32>(std::string_view(value.value()).substr(0, value->length() - 5));
        entry.gpu_per_sample_shading = true;
      }
      else
      {
        entry.gpu_multisamples = StringUtil::FromChars<u32>(value.value());
        entry.gpu_per_sample_shading = false;
      }
    }
  }
  else if (key == "GPUTrueColor")
  {
    if (!value.has_value())
      entry.gpu_true_color.reset();
    else
      entry.gpu_true_color = StringUtil::FromChars<bool>(value.value()).value_or(false);
  }
  else if (key == "GPUScaledDithering")
  {
    if (!value.has_value())
      entry.gpu_scaled_dithering.reset();
    else
      entry.gpu_scaled_dithering = StringUtil::FromChars<bool>(value.value()).value_or(false);
  }
  else if (key == "GPUTextureFilter")
  {
    if (!value.has_value())
      entry.gpu_texture_filter.reset();
    else
      entry.gpu_texture_filter = Settings::ParseTextureFilterName(value->c_str());
  }
  else if (key == "GPUForceNTSCTimings")
  {
    if (!value.has_value())
      entry.gpu_force_ntsc_timings.reset();
    else
      entry.gpu_force_ntsc_timings = StringUtil::FromChars<bool>(value.value()).value_or(false);
  }
  else if (key == "GPUWidescreenHack")
  {
    if (!value.has_value())
      entry.gpu_widescreen_hack.reset();
    else
      entry.gpu_widescreen_hack = StringUtil::FromChars<bool>(value.value()).value_or(false);
  }
  else if (key == "GPUPGXP")
  {
    if (!value.has_value())
      entry.gpu_pgxp.reset();
    else
      entry.gpu_pgxp = StringUtil::FromChars<bool>(value.value()).value_or(false);
  }
  else if (key == "GPUPGXPPreserveProjFP")
  {
    if (!value.has_value())
      entry.gpu_pgxp_projection_precision.reset();
    else
      entry.gpu_pgxp_projection_precision = StringUtil::FromChars<bool>(value.value()).value_or(false);
  }
  else if (key == "GPUPGXPDepthBuffer")
  {
    if (!value.has_value())
      entry.gpu_pgxp_depth_buffer.reset();
    else
      entry.gpu_pgxp_depth_buffer = StringUtil::FromChars<bool>(value.value()).value_or(false);
  }
  else if (key == "MultitapMode")
  {
    if (!value.has_value())
      entry.multitap_mode.reset();
    else
      entry.multitap_mode = Settings::ParseMultitapModeName(value->c_str());
  }
  else if (key == "Controller1Type")
  {
    if (!value.has_value())
      entry.controller_1_type.reset();
    else
      entry.controller_1_type = Settings::ParseControllerTypeName(value->c_str());
  }
  else if (key == "Controller2Type")
  {
    if (!value.has_value())
      entry.controller_2_type.reset();
    else
      entry.controller_2_type = Settings::ParseControllerTypeName(value->c_str());
  }
  else if (key == "MemoryCard1Type")
  {
    if (!value.has_value())
      entry.memory_card_1_type.reset();
    else
      entry.memory_card_1_type = Settings::ParseMemoryCardTypeName(value->c_str());
  }
  else if (key == "MemoryCard2Type")
  {
    if (!value.has_value())
      entry.memory_card_2_type.reset();
    else
      entry.memory_card_2_type = Settings::ParseMemoryCardTypeName(value->c_str());
  }
  else if (key == "MemoryCard1SharedPath")
  {
    if (!value.has_value())
      entry.memory_card_1_shared_path.clear();
    else
      entry.memory_card_1_shared_path = value.value();
  }
  else if (key == "MemoryCard2SharedPath")
  {
    if (!value.has_value())
      entry.memory_card_1_shared_path.clear();
    else
      entry.memory_card_1_shared_path = value.value();
  }
  else if (key == "InputProfileName")
  {
    if (!value.has_value())
      entry.input_profile_name.clear();
    else
      entry.input_profile_name = value.value();
  }
  else if (key == "ForceSoftwareRenderer")
  {
    if (!value.has_value() || !StringUtil::FromChars<bool>(value.value()).value_or(false))
      entry.RemoveTrait(Trait::ForceSoftwareRenderer);
    else
      entry.AddTrait(Trait::ForceSoftwareRenderer);
  }
  else if (key == "ForceSoftwareRendererForReadbacks")
  {
    if (!value.has_value() || !StringUtil::FromChars<bool>(value.value()).value_or(false))
      entry.RemoveTrait(Trait::ForceSoftwareRendererForReadbacks);
    else
      entry.AddTrait(Trait::ForceSoftwareRendererForReadbacks);
  }
  else if (key == "DisableWidescreen")
  {
    if (!value.has_value() || !StringUtil::FromChars<bool>(value.value()).value_or(false))
      entry.RemoveTrait(Trait::DisableWidescreen);
    else
      entry.AddTrait(Trait::DisableWidescreen);
  }
  else if (key == "ForcePGXPVertexCache")
  {
    if (!value.has_value() || !StringUtil::FromChars<bool>(value.value()).value_or(false))
      entry.RemoveTrait(Trait::ForcePGXPVertexCache);
    else
      entry.AddTrait(Trait::ForcePGXPVertexCache);
  }
  else if (key == "ForcePGXPCPUMode")
  {
    if (!value.has_value() || !StringUtil::FromChars<bool>(value.value()).value_or(false))
      entry.RemoveTrait(Trait::ForcePGXPCPUMode);
    else
      entry.AddTrait(Trait::ForcePGXPCPUMode);
  }
  else
  {
    Log_ErrorPrintf("Unknown key: %s", std::string(key).c_str());
  }
}

Database::Database() = default;

Database::~Database() = default;

const GameSettings::Entry* Database::GetEntry(const std::string& code) const
{
  auto it = m_entries.find(code);
  return (it != m_entries.end()) ? &it->second : nullptr;
}

bool Database::Load(const std::string_view& ini_data)
{
  CSimpleIniA ini;
  SI_Error err = ini.LoadData(ini_data.data(), ini_data.size());
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

  Log_InfoPrintf("Loaded settings for %zu games", sections.size());
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

std::optional<std::string> Entry::GetValueForKey(const std::string_view& key) const
{
  return GetEntryValueForKey(*this, key);
}

void Entry::SetValueForKey(const std::string_view& key, const std::optional<std::string>& value)
{
  SetEntryValueForKey(*this, key, value);
}

void Entry::ApplySettings(bool display_osd_messages) const
{
  constexpr float osd_duration = 10.0f;

  if (runahead_frames.has_value())
    g_settings.runahead_frames = runahead_frames.value();
  if (cpu_overclock_numerator.has_value())
    g_settings.cpu_overclock_numerator = cpu_overclock_numerator.value();
  if (cpu_overclock_denominator.has_value())
    g_settings.cpu_overclock_denominator = cpu_overclock_denominator.value();
  if (cpu_overclock_enable.has_value())
    g_settings.cpu_overclock_enable = cpu_overclock_enable.value();
  if (enable_8mb_ram.has_value())
    g_settings.enable_8mb_ram = enable_8mb_ram.value();
  g_settings.UpdateOverclockActive();

  if (cdrom_read_speedup.has_value())
    g_settings.cdrom_read_speedup = cdrom_read_speedup.value();
  if (cdrom_seek_speedup.has_value())
    g_settings.cdrom_seek_speedup = cdrom_seek_speedup.value();

  if (display_active_start_offset.has_value())
    g_settings.display_active_start_offset = display_active_start_offset.value();
  if (display_active_end_offset.has_value())
    g_settings.display_active_end_offset = display_active_end_offset.value();
  if (display_line_start_offset.has_value())
    g_settings.display_line_start_offset = display_line_start_offset.value();
  if (display_line_end_offset.has_value())
    g_settings.display_line_end_offset = display_line_end_offset.value();
  if (dma_max_slice_ticks.has_value())
    g_settings.dma_max_slice_ticks = dma_max_slice_ticks.value();
  if (dma_halt_ticks.has_value())
    g_settings.dma_halt_ticks = dma_halt_ticks.value();
  if (gpu_fifo_size.has_value())
    g_settings.gpu_fifo_size = gpu_fifo_size.value();
  if (gpu_max_run_ahead.has_value())
    g_settings.gpu_max_run_ahead = gpu_max_run_ahead.value();
  if (gpu_pgxp_tolerance.has_value())
    g_settings.gpu_pgxp_tolerance = gpu_pgxp_tolerance.value();
  if (gpu_pgxp_depth_threshold.has_value())
    g_settings.SetPGXPDepthClearThreshold(gpu_pgxp_depth_threshold.value());

  if (display_crop_mode.has_value())
    g_settings.display_crop_mode = display_crop_mode.value();
  if (display_aspect_ratio.has_value())
    g_settings.display_aspect_ratio = display_aspect_ratio.value();
  if (display_aspect_ratio_custom_numerator.has_value())
    g_settings.display_aspect_ratio_custom_numerator = display_aspect_ratio_custom_numerator.value();
  if (display_aspect_ratio_custom_denominator.has_value())
    g_settings.display_aspect_ratio_custom_denominator = display_aspect_ratio_custom_denominator.value();
  if (gpu_downsample_mode.has_value())
    g_settings.gpu_downsample_mode = gpu_downsample_mode.value();
  if (display_linear_upscaling.has_value())
    g_settings.display_linear_filtering = display_linear_upscaling.value();
  if (display_integer_upscaling.has_value())
    g_settings.display_integer_scaling = display_integer_upscaling.value();
  if (display_force_4_3_for_24bit.has_value())
    g_settings.display_force_4_3_for_24bit = display_force_4_3_for_24bit.value();

  if (gpu_renderer.has_value())
    g_settings.gpu_renderer = gpu_renderer.value();
  if (gpu_resolution_scale.has_value())
    g_settings.gpu_resolution_scale = gpu_resolution_scale.value();
  if (gpu_multisamples.has_value())
    g_settings.gpu_multisamples = gpu_multisamples.value();
  if (gpu_per_sample_shading.has_value())
    g_settings.gpu_per_sample_shading = gpu_per_sample_shading.value();
  if (gpu_true_color.has_value())
    g_settings.gpu_true_color = gpu_true_color.value();
  if (gpu_scaled_dithering.has_value())
    g_settings.gpu_scaled_dithering = gpu_scaled_dithering.value();
  if (gpu_force_ntsc_timings.has_value())
    g_settings.gpu_force_ntsc_timings = gpu_force_ntsc_timings.value();
  if (gpu_texture_filter.has_value())
    g_settings.gpu_texture_filter = gpu_texture_filter.value();
  if (gpu_widescreen_hack.has_value())
    g_settings.gpu_widescreen_hack = gpu_widescreen_hack.value();
  if (gpu_pgxp.has_value())
    g_settings.gpu_pgxp_enable = gpu_pgxp.value();
  if (gpu_pgxp_projection_precision.has_value())
    g_settings.gpu_pgxp_preserve_proj_fp = gpu_pgxp_projection_precision.value();
  if (gpu_pgxp_depth_buffer.has_value())
    g_settings.gpu_pgxp_depth_buffer = gpu_pgxp_depth_buffer.value();

  if (multitap_mode.has_value())
    g_settings.multitap_mode = multitap_mode.value();
  if (controller_1_type.has_value())
    g_settings.controller_types[0] = controller_1_type.value();
  if (controller_2_type.has_value())
    g_settings.controller_types[1] = controller_2_type.value();

  if (memory_card_1_type.has_value())
    g_settings.memory_card_types[0] = memory_card_1_type.value();
  if (!memory_card_1_shared_path.empty())
    g_settings.memory_card_paths[0] = memory_card_1_shared_path;
  if (memory_card_2_type.has_value())
    g_settings.memory_card_types[1] = memory_card_2_type.value();
  if (!memory_card_1_shared_path.empty())
    g_settings.memory_card_paths[1] = memory_card_2_shared_path;

  if (HasTrait(Trait::ForceInterpreter))
  {
    if (display_osd_messages && g_settings.cpu_execution_mode != CPUExecutionMode::Interpreter)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage", "CPU interpreter forced by game settings."), osd_duration);
    }

    g_settings.cpu_execution_mode = CPUExecutionMode::Interpreter;
  }

  if (HasTrait(Trait::ForceSoftwareRenderer))
  {
    if (display_osd_messages && g_settings.gpu_renderer != GPURenderer::Software)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage", "Software renderer forced by game settings."), osd_duration);
    }

    g_settings.gpu_renderer = GPURenderer::Software;
  }

  if (HasTrait(Trait::ForceInterlacing))
  {
    if (display_osd_messages && g_settings.gpu_disable_interlacing)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage", "Interlacing forced by game settings."), osd_duration);
    }

    g_settings.gpu_disable_interlacing = false;
  }

  if (HasTrait(Trait::DisableTrueColor))
  {
    if (display_osd_messages && g_settings.gpu_true_color)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage", "True color disabled by game settings."), osd_duration);
    }

    g_settings.gpu_true_color = false;
  }

  if (HasTrait(Trait::DisableUpscaling))
  {
    if (display_osd_messages && g_settings.gpu_resolution_scale > 1)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage", "Upscaling disabled by game settings."), osd_duration);
    }

    g_settings.gpu_resolution_scale = 1;
  }

  if (HasTrait(Trait::DisableScaledDithering))
  {
    if (display_osd_messages && g_settings.gpu_scaled_dithering)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage", "Scaled dithering disabled by game settings."),
        osd_duration);
    }

    g_settings.gpu_scaled_dithering = false;
  }

  if (HasTrait(Trait::DisableWidescreen))
  {
    if (display_osd_messages &&
        (g_settings.display_aspect_ratio == DisplayAspectRatio::R16_9 || g_settings.gpu_widescreen_hack))
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage", "Widescreen disabled by game settings."), osd_duration);
    }

    g_settings.display_aspect_ratio = DisplayAspectRatio::R4_3;
    g_settings.gpu_widescreen_hack = false;
  }

  if (HasTrait(Trait::DisableForceNTSCTimings))
  {
    if (display_osd_messages && g_settings.gpu_force_ntsc_timings)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage", "Forcing NTSC Timings disallowed by game settings."),
        osd_duration);
    }

    g_settings.gpu_force_ntsc_timings = false;
  }

  if (HasTrait(Trait::DisablePGXP))
  {
    if (display_osd_messages && g_settings.gpu_pgxp_enable)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage", "PGXP geometry correction disabled by game settings."),
        osd_duration);
    }

    g_settings.gpu_pgxp_enable = false;
  }

  if (HasTrait(Trait::DisablePGXPCulling))
  {
    if (display_osd_messages && g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_culling)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage", "PGXP culling disabled by game settings."), osd_duration);
    }

    g_settings.gpu_pgxp_culling = false;
  }

  if (HasTrait(Trait::DisablePGXPTextureCorrection))
  {
    if (display_osd_messages && g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_texture_correction)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage", "PGXP texture correction disabled by game settings."),
        osd_duration);
    }

    g_settings.gpu_pgxp_texture_correction = false;
  }

  if (HasTrait(Trait::ForcePGXPVertexCache))
  {
    if (display_osd_messages && g_settings.gpu_pgxp_enable && !g_settings.gpu_pgxp_vertex_cache)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage", "PGXP vertex cache forced by game settings."), osd_duration);
    }

    g_settings.gpu_pgxp_vertex_cache = true;
  }

  if (HasTrait(Trait::ForcePGXPCPUMode))
  {
    if (display_osd_messages && g_settings.gpu_pgxp_enable && !g_settings.gpu_pgxp_cpu)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage", "PGXP CPU mode forced by game settings."), osd_duration);
    }

    g_settings.gpu_pgxp_cpu = true;
  }

  if (HasTrait(Trait::DisablePGXPDepthBuffer))
  {
    if (display_osd_messages && g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_depth_buffer)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString("OSDMessage", "PGXP Depth Buffer disabled by game settings."),
        osd_duration);
    }

    g_settings.gpu_pgxp_depth_buffer = false;
  }

  if (HasTrait(Trait::ForceSoftwareRenderer))
  {
    Log_WarningPrint("Using software renderer for readbacks.");
    g_settings.gpu_renderer = GPURenderer::Software;
  }

  if (HasTrait(Trait::ForceRecompilerMemoryExceptions))
  {
    Log_WarningPrint("Memory exceptions for recompiler forced by game settings.");
    g_settings.cpu_recompiler_memory_exceptions = true;
  }

  if (HasTrait(Trait::ForceRecompilerICache))
  {
    Log_WarningPrint("ICache for recompiler forced by game settings.");
    g_settings.cpu_recompiler_icache = true;
  }

  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::MMap && HasTrait(Trait::ForceRecompilerLUTFastmem))
  {
    Log_WarningPrint("LUT fastmem for recompiler forced by game settings.");
    g_settings.cpu_fastmem_mode = CPUFastmemMode::LUT;
  }
}

} // namespace GameSettings
