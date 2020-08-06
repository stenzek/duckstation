#pragma once
#include "common/progress_callback.h"
#include "windows_headers.h"

class Win32ProgressCallback final : public BaseProgressCallback
{
public:
  Win32ProgressCallback();

  void PushState() override;
  void PopState() override;

  void SetCancellable(bool cancellable) override;
  void SetTitle(const char* title) override;
  void SetStatusText(const char* text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void DisplayError(const char* message) override;
  void DisplayWarning(const char* message) override;
  void DisplayInformation(const char* message) override;
  void DisplayDebugMessage(const char* message) override;

  void ModalError(const char* message) override;
  bool ModalConfirmation(const char* message) override;
  void ModalInformation(const char* message) override;
  
private:
  enum : int
  {
    WINDOW_WIDTH = 600,
    WINDOW_HEIGHT = 300,
    WINDOW_MARGIN = 10,
    SUBWINDOW_WIDTH = WINDOW_WIDTH - 20 - WINDOW_MARGIN - WINDOW_MARGIN,
  };

  bool Create();
  void Destroy();
  void Redraw(bool force);
  void PumpMessages();

  static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

  HWND m_window_hwnd{};
  HWND m_text_hwnd{};
  HWND m_progress_hwnd{};
  HWND m_list_box_hwnd{};

  int m_last_progress_percent = -1;
};
