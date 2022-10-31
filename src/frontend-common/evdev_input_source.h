#pragma once
#include "core/types.h"
#include "input_source.h"
#include <array>
#include <functional>
#include <libevdev/libevdev.h>
#include <mutex>
#include <vector>

class EvdevInputSource final : public InputSource
{
public:
  EvdevInputSource();
  ~EvdevInputSource() override;

  bool Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  void UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  bool ReloadDevices() override;
  void Shutdown() override;

  void PollEvents() override;
  std::vector<std::pair<std::string, std::string>> EnumerateDevices() override;
  std::vector<InputBindingKey> EnumerateMotors() override;
  bool GetGenericBindingMapping(const std::string_view& device, GenericInputBindingMapping* mapping) override;
  void UpdateMotorState(InputBindingKey key, float intensity) override;
  void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                        float small_intensity) override;

  std::optional<InputBindingKey> ParseKeyString(const std::string_view& device,
                                                const std::string_view& binding) override;
  std::string ConvertKeyToString(InputBindingKey key) override;

private:
  struct ControllerData
  {
    ControllerData(int fd_, struct libevdev* obj_);
    ControllerData(const ControllerData&) = delete;
    ControllerData(ControllerData&& move);
    ~ControllerData();

    ControllerData& operator=(const ControllerData&) = delete;
    ControllerData& operator=(ControllerData&& move);

    struct libevdev* obj = nullptr;
    int fd = -1;
    int controller_id = 0;
    u32 num_motors = 0;

    float deadzone = 0.25f;

    struct Axis
    {
      std::string name;
      u32 id;
      s32 min;
      s32 range;
      u32 neg_button;
      u32 pos_button;
      GenericInputBinding neg_generic;
      GenericInputBinding pos_generic;
      float last_value;
    };

    struct Button
    {
      Button() = default;
      Button(std::string name_, u32 id_, GenericInputBinding generic_)
        : name(std::move(name_)), id(id_), generic(generic_)
      {
      }

      std::string name;
      u32 id;
      GenericInputBinding generic;
    };

    std::string uniq;
    std::string name;
    std::vector<Axis> axes;
    std::vector<Button> buttons;
  };

  ControllerData* GetControllerById(int id);
  ControllerData* GetControllerByUniq(const std::string_view& uniq);
  bool InitializeController(int index, ControllerData* cd);
  void HandleControllerEvents(ControllerData* cd);

  std::vector<ControllerData> m_controllers;
};
