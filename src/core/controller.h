// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "input_types.h"
#include "types.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

class SettingsInterface;
class StateWrapper;

class Controller
{
public:
  struct ControllerBindingInfo
  {
    const char* name;                    ///< Internal name for the binding.
    const char* display_name;            ///< Localized name for the binding.
    const char* icon_name;               ///< FA/PromptFont icon glyph name, if any.
    const char* image_element;           ///< Element ID in the controller image SVG, if any.
    u32 bind_index;                      ///< Index of the binding in the controller's bind state array.
    InputBindingInfo::Type type;         ///< Type of input for the binding.
    GenericInputBinding generic_mapping; /// Generic input mapping for the binding, if any.
  };

  struct ControllerInfo
  {
    ControllerType type;                             ///< Type of controller.
    const char* name;                                ///< Internal name for the controller type.
    const char* display_name;                        ///< Localized name for the controller type.
    const char* icon_name;                           ///< FA/PromptFont icon glyph name, if any.
    const char* image_name;                          ///< Name of the controller image SVG file, if any.
    std::span<const ControllerBindingInfo> bindings; ///< List of bindings for the controller type.
    std::span<const SettingInfo> settings;           ///< List of settings for the controller type.

    /// Returns localized controller type name.
    std::string_view GetDisplayName() const;

    /// Returns localized controller type name.
    std::string_view GetBindingDisplayName(const ControllerBindingInfo& bi) const;
  };

  /// Default stick deadzone/sensitivity.
  static constexpr float DEFAULT_STICK_DEADZONE = 0.0f;
  static constexpr float DEFAULT_STICK_SENSITIVITY = 1.33f;
  static constexpr float DEFAULT_BUTTON_DEADZONE = 0.25f;

  /// Center position when dealing with axis values.
  static constexpr u8 AXIS_CENTER = 0x7F;

  Controller(u32 index, ControllerType type);
  virtual ~Controller();

  /// Returns the index of the controller in the global pad list.
  ALWAYS_INLINE u32 GetIndex() const { return m_index; }

  /// Returns the type of controller.
  ALWAYS_INLINE ControllerType GetType() const { return m_type; }

  virtual void Reset() = 0;

  /// Saves/loads the transfer state. Derived classes call this before their own state.
  virtual bool DoState(StateWrapper& sw, bool apply_input_state);

  // Resets all state for the transferring to/from the device.
  virtual void ResetTransferState();

  // Returns the value of ACK, as well as filling out_data.
  bool Transfer(const u8 data_in, u8* data_out);

  /// Changes the specified axis state. Values are normalized from -1..1.
  virtual float GetBindState(u32 index) const = 0;

  /// Changes the specified bind state. Values are normalized from -1..1.
  virtual void SetBindState(u32 index, float value) = 0;

  /// Returns a bitmask of the current button states, 1 = on.
  virtual u32 GetButtonStateBits() const = 0;

  /// Returns analog input bytes packed as a u32. Values are specific to controller type.
  virtual std::optional<u32> GetAnalogInputBytes() const;

  /// Loads/refreshes any per-controller settings.
  virtual void LoadSettings(const SettingsInterface& si, const char* section, bool initial) = 0;

  /// Creates a new controller of the specified type.
  static std::unique_ptr<Controller> Create(ControllerType type, u32 index);

  /// Returns a list of all controller types.
  static const std::array<const ControllerInfo*, static_cast<size_t>(ControllerType::Count)>& GetControllerInfoList();

  /// Returns general information for the specified controller type.
  static const ControllerInfo& GetControllerInfo(ControllerType type);
  static const ControllerInfo* GetControllerInfo(std::string_view name);

  /// Converts a global pad index to a multitap port and slot.
  static std::tuple<u32, u32> ConvertPadToPortAndSlot(u32 index);

  /// Converts a multitap port and slot to a global pad index.
  static u32 ConvertPortAndSlotToPad(u32 port, u32 slot);

  /// Returns true if the given pad index is a multitap slot.
  static bool PadIsMultitapSlot(u32 index);
  static bool PortAndSlotIsMultitap(u32 port, u32 slot);

  /// Returns the configuration section for the specified gamepad.
  static std::string GetSettingsSection(u32 pad);

  /// Returns a printable label for a given port.
  static const char* GetPortDisplayName(u32 port, u32 slot, bool mtap);
  static const char* GetPortDisplayName(u32 index, MultitapMode mode);

  /// Returns true if multitap is enabled on the given port.
  static bool IsMultitapEnabledOnPort(u32 port, MultitapMode mode);

  /// Returns a boolean array of which ports have multitap enabled for a given mode.
  static std::array<bool, 2> GetMultitapEnabledPorts(MultitapMode mode);

  /// List of controller indices in the order that they should be displayed.
  static const std::array<u32, NUM_CONTROLLER_AND_CARD_PORTS> PortDisplayOrder;

  /// Returns true if automatic analog mode can be used.
  static bool CanStartInAnalogMode(ControllerType ctype);

  /// Updates an active low button bit, flagging a runahead replay if it changed.
  static void SetButtonPressed(u16& state, u16 bit, bool pressed);

protected:
  enum class TransferState : u8
  {
    Idle,
    Ready,
    Command,
  };

  /// Transmit and receive buffers, not including the first Hi-Z/ack response byte.
  static constexpr u32 MAX_RESPONSE_LENGTH = 8;

  /// Returns the ID byte sent for a command. The low nibble is the number of halfwords after the ID halfword.
  virtual u8 GetIDByte() const = 0;

  /// Starts a command, filling the reply after the ID byte. Returns false if the command isn't supported.
  virtual bool BeginCommand(u8 command) = 0;

  /// Called for each byte after the command byte, before its reply is sent.
  virtual void ProcessCommandByte(u32 step, u8 data_in);

  /// Called after the last byte of a command.
  virtual void EndCommand();

  /// Skips the per-byte transfer state of states before version 105, and resets the transfer.
  void SkipLegacyTransferState(StateWrapper& sw);

  u32 m_index;
  ControllerType m_type;

  TransferState m_transfer_state = TransferState::Idle;
  u8 m_command_step = 0;
  u8 m_response_length = 0;
  std::array<u8, MAX_RESPONSE_LENGTH> m_rx_buffer{};
  std::array<u8, MAX_RESPONSE_LENGTH> m_tx_buffer{};
};
