#include "common/assert.h"
#include "common/log.h"
#include "libretro_host_interface.h"
#include "scmversion/scmversion.h"
Log_SetChannel(Main);

RETRO_API unsigned retro_api_version(void)
{
  return RETRO_API_VERSION;
}

RETRO_API void retro_init(void)
{
  // default log to stdout until we get an interface
  Log::SetConsoleOutputParams(true, nullptr, LOGLEVEL_INFO);

  if (!g_libretro_host_interface.Initialize())
    Panic("Host interface initialization failed");
}

RETRO_API void retro_deinit(void)
{
  g_libretro_host_interface.Shutdown();
}

RETRO_API void retro_get_system_info(struct retro_system_info* info)
{
  std::memset(info, 0, sizeof(*info));

#if defined(_DEBUGFAST)
  info->library_name = "DuckStation DebugFast";
#elif defined(_DEBUG)
  info->library_name = "DuckStation Debug";
#else
  info->library_name = "DuckStation";
#endif

  info->library_version = g_scm_tag_str;
  info->valid_extensions = "exe|cue|bin|chd|psf|m3u";
  info->need_fullpath = true;
  info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info)
{
  g_libretro_host_interface.retro_get_system_av_info(info);
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
  Log_ErrorPrintf("retro_set_controller_port_device(%u, %u)", port, device);
}

RETRO_API void retro_reset(void)
{
  Log_InfoPrint("retro_reset()");
  g_libretro_host_interface.ResetSystem();
}

RETRO_API void retro_run(void)
{
  g_libretro_host_interface.retro_run_frame();
}

RETRO_API size_t retro_serialize_size(void)
{
  return g_libretro_host_interface.retro_serialize_size();
}

RETRO_API bool retro_serialize(void* data, size_t size)
{
  return g_libretro_host_interface.retro_serialize(data, size);
}

RETRO_API bool retro_unserialize(const void* data, size_t size)
{
  return g_libretro_host_interface.retro_unserialize(data, size);
}

RETRO_API void retro_cheat_reset(void)
{
  Log_InfoPrint("retro_cheat_reset()");
  g_libretro_host_interface.retro_cheat_reset();
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
  Log_InfoPrintf("retro_cheat_set(%u, %u, %s)", index, enabled, code);
  g_libretro_host_interface.retro_cheat_set(index, enabled, code);
}

RETRO_API bool retro_load_game(const struct retro_game_info* game)
{
  Log_InfoPrintf("retro_load_game(%s)", game->path);
  return g_libretro_host_interface.retro_load_game(game);
}

RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info)
{
  Log_ErrorPrintf("retro_load_game_special()");
  return false;
}

RETRO_API void retro_unload_game(void)
{
  g_libretro_host_interface.DestroySystem();
}

RETRO_API unsigned retro_get_region(void)
{
  return g_libretro_host_interface.retro_get_region();
}

RETRO_API void* retro_get_memory_data(unsigned id)
{
  return g_libretro_host_interface.retro_get_memory_data(id);
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
  return g_libretro_host_interface.retro_get_memory_size(id);
}

RETRO_API void retro_set_environment(retro_environment_t f)
{
  g_retro_environment_callback = f;
  g_libretro_host_interface.InitInterfaces();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t f)
{
  g_retro_video_refresh_callback = f;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t f)
{
  g_retro_audio_sample_callback = f;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t f)
{
  g_retro_audio_sample_batch_callback = f;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t f)
{
  g_retro_input_poll_callback = f;
}

RETRO_API void retro_set_input_state(retro_input_state_t f)
{
  g_retro_input_state_callback = f;
}
