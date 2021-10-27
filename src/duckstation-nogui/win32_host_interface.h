#pragma once
#include "common/windows_headers.h"
#include "nogui_host_interface.h"
#include <memory>
#include <vector>

class Win32HostInterface final : public NoGUIHostInterface
{
public:
  Win32HostInterface();
  ~Win32HostInterface();

  bool Initialize();
  void Shutdown();

  static std::unique_ptr<NoGUIHostInterface> Create();

protected:
  void SetMouseMode(bool relative, bool hide_cursor) override;

  bool CreatePlatformWindow() override;
  void DestroyPlatformWindow() override;
  std::optional<WindowInfo> GetPlatformWindowInfo() override;

  std::optional<HostKeyCode> GetHostKeyCode(const std::string_view key_code) const override;

  void PollAndUpdate() override;

private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  bool RegisterWindowClass();
  void ProcessWin32Events();

  HWND m_hwnd{};
};
