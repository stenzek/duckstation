#pragma once
#include "common/types.h"
#include "controller.h"
#include "memory_card.h"
#include "util/state_wrapper.h"
#include <array>

class Multitap final
{
public:
  Multitap();

  void Reset();

  void SetEnable(bool enable, u32 base_index);
  ALWAYS_INLINE bool IsEnabled() const { return m_enabled; };

  bool DoState(StateWrapper& sw);

  void ResetTransferState();
  bool Transfer(const u8 data_in, u8* data_out);
  ALWAYS_INLINE bool IsReadingMemoryCard() { return IsEnabled() && m_transfer_state == TransferState::MemoryCard; };

private:
  ALWAYS_INLINE static constexpr u8 GetMultitapIDByte() { return 0x80; };
  ALWAYS_INLINE static constexpr u8 GetStatusByte() { return 0x5A; };

  bool TransferController(u32 slot, const u8 data_in, u8* data_out) const;
  bool TransferMemoryCard(u32 slot, const u8 data_in, u8* data_out) const;

  enum class TransferState : u8
  {
    Idle,
    MemoryCard,
    ControllerCommand,
    SingleController,
    AllControllers
  };

  TransferState m_transfer_state = TransferState::Idle;
  u8 m_selected_slot = 0;

  u32 m_controller_transfer_step = 0;

  bool m_invalid_transfer_all_command = false;
  bool m_transfer_all_controllers = false;
  bool m_current_controller_done = false;

  std::array<u8, 32> m_transfer_buffer{};

  u32 m_base_index;
  bool m_enabled = false;
};
