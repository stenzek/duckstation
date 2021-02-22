#pragma once
#include "common/timer.h"
#include "common_host_interface.h"
#include <memory>

class HostDisplayTexture;

namespace FrontendCommon {

class SaveStateSelectorUI
{
public:
  static constexpr float DEFAULT_OPEN_TIME = 5.0f;

  SaveStateSelectorUI(CommonHostInterface* host_interface);
  ~SaveStateSelectorUI();

  ALWAYS_INLINE bool IsOpen() const { return m_open; }
  ALWAYS_INLINE void ResetOpenTimer() { m_open_timer.Reset(); }

  void Open(float open_time = DEFAULT_OPEN_TIME);
  void Close();

  void ClearList();
  void RefreshList();

  void RefreshHotkeyLegend();

  const char* GetSelectedStatePath() const;
  s32 GetSelectedStateSlot() const;

  void SelectNextSlot();
  void SelectPreviousSlot();

  void Draw();

  void LoadCurrentSlot();
  void SaveCurrentSlot();

private:
  struct ListEntry
  {
    std::string path;
    std::string game_code;
    std::string title;
    std::string formatted_timestamp;
    std::unique_ptr<HostDisplayTexture> preview_texture;
    s32 slot;
    bool global;
  };

  void InitializePlaceholderListEntry(ListEntry* li, s32 slot, bool global);
  void InitializeListEntry(ListEntry* li, CommonHostInterface::ExtendedSaveStateInfo* ssi);
  std::pair<s32, bool> GetSlotTypeFromSelection(u32 selection) const;

  std::string m_load_legend;
  std::string m_save_legend;
  std::string m_prev_legend;
  std::string m_next_legend;

  CommonHostInterface* m_host_interface;
  std::vector<ListEntry> m_slots;
  u32 m_current_selection = 0;

  Common::Timer m_open_timer;
  float m_open_time = 0.0f;

  bool m_open = false;
};

} // namespace FrontendCommon