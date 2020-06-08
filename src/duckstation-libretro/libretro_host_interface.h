#pragma once
#include "core/host_interface.h"
#include "core/system.h"
#include "libretro.h"

class LibretroHostInterface : public HostInterface
{
public:
  LibretroHostInterface();
  ~LibretroHostInterface() override;

  static bool SetCoreOptions();

  bool Initialize() override;
  void Shutdown() override;

  void ReportError(const char* message) override;
  void ReportMessage(const char* message) override;
  bool ConfirmMessage(const char* message) override;

  const retro_hw_render_callback& GetHWRenderCallback() const { return m_hw_render_callback; }

  // Called by frontend
  void retro_get_system_av_info(struct retro_system_av_info* info);
  bool retro_load_game(const struct retro_game_info* game);
  void retro_run_frame();
  unsigned retro_get_region();

protected:
  bool AcquireHostDisplay() override;
  void ReleaseHostDisplay() override;
  std::unique_ptr<AudioStream> CreateAudioStream(AudioBackend backend) override;

private:
  void LoadSettings();
  void UpdateSettings();
  void UpdateControllers();
  void UpdateControllersDigitalController(u32 index);

  // Hardware renderer setup.
  bool RequestHardwareRendererContext();

  static void HardwareRendererContextReset();
  static void HardwareRendererContextDestroy();

  retro_hw_render_callback m_hw_render_callback = {};
};

extern LibretroHostInterface g_libretro_host_interface;

// libretro callbacks
extern retro_environment_t g_retro_environment_callback;
extern retro_video_refresh_t g_retro_video_refresh_callback;
extern retro_audio_sample_t g_retro_audio_sample_callback;
extern retro_audio_sample_batch_t g_retro_audio_sample_batch_callback;
extern retro_input_poll_t g_retro_input_poll_callback;
extern retro_input_state_t g_retro_input_state_callback;
