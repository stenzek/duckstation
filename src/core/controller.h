#pragma once
#include "types.h"
#include <optional>
#include <string_view>

class StateWrapper;

class Controller
{
public:
  Controller();
  virtual ~Controller();

  virtual void Reset();
  virtual bool DoState(StateWrapper& sw);

  // Resets all state for the transferring to/from the device.
  virtual void ResetTransferState();

  // Returns the value of ACK, as well as filling out_data.
  virtual bool Transfer(const u8 data_in, u8* data_out);

  /// Creates a new controller of the specified type.
  static std::shared_ptr<Controller> Create(std::string_view type_name);

  /// Gets the integer code for a button in the specified controller type.
  static std::optional<s32> GetButtonCodeByName(std::string_view type_name, std::string_view button_name);
};
