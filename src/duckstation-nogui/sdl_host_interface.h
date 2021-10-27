#pragma once
#include "nogui_host_interface.h"
#include <SDL.h>

class SDLHostInterface final : public NoGUIHostInterface
{
public:
  SDLHostInterface();
  ~SDLHostInterface();

  static std::unique_ptr<SDLHostInterface> Create();

  const char* GetFrontendName() const override;

  bool Initialize() override;
  void Shutdown() override;

  bool RequestRenderWindowSize(s32 new_window_width, s32 new_window_height) override;

  bool IsFullscreen() const override;
  bool SetFullscreen(bool enabled) override;

protected:
  void SetMouseMode(bool relative, bool hide_cursor) override;

  void PollAndUpdate() override;

  std::optional<HostKeyCode> GetHostKeyCode(const std::string_view key_code) const override;

  bool CreatePlatformWindow() override;
  void DestroyPlatformWindow() override;
  std::optional<WindowInfo> GetPlatformWindowInfo() override;

private:
  void HandleSDLEvent(const SDL_Event* event);

  void GetSavedWindowGeometry(int* x, int* y, int* width, int* height);
  void SaveWindowGeometry();

  SDL_Window* m_window = nullptr;
  bool m_fullscreen = false;
  bool m_was_paused_by_focus_loss = false;
};
