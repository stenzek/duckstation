#pragma once
#include "common/drm_display.h"
#include "nogui_host_interface.h"
#include <memory>
#include <vector>
#include <libevdev/libevdev.h>

class DRMHostInterface final : public NoGUIHostInterface
{
public:
  DRMHostInterface();
  ~DRMHostInterface();

  bool Initialize();
  void Shutdown();

  bool IsFullscreen() const override;
  bool SetFullscreen(bool enabled) override;

  static std::unique_ptr<NoGUIHostInterface> Create();

protected:
  virtual void FixIncompatibleSettings(bool display_osd_messages) override;

  bool CreatePlatformWindow() override;
  void DestroyPlatformWindow() override;
  std::optional<WindowInfo> GetPlatformWindowInfo() override;

  std::optional<HostKeyCode> GetHostKeyCode(const std::string_view key_code) const override;

  void PollAndUpdate() override;

private:
  static void SIGTERMHandler(int sig);

  void OpenEVDevFDs();
  void CloseEVDevFDs();
  void PollEvDevKeyboards();
  void SetImGuiKeyMap();

  std::unique_ptr<DRMDisplay> m_drm_display;

  struct EvDevKeyboard
  {
    struct libevdev* obj;
    int fd;
  };

  std::vector<EvDevKeyboard> m_evdev_keyboards;
};
