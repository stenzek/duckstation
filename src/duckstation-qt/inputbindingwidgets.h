// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/types.h"
#include "util/input_manager.h"
#include <QtWidgets/QPushButton>
#include <optional>

class QTimer;

class ControllerSettingsWindow;
class SettingsInterface;

class InputBindingWidget : public QPushButton
{
  Q_OBJECT

public:
  explicit InputBindingWidget(QWidget* parent);
  InputBindingWidget(QWidget* parent, SettingsInterface* sif, InputBindingInfo::Type bind_type,
                     std::string section_name, std::string key_name);
  ~InputBindingWidget();

  static bool isMouseMappingEnabled();
  static bool isSensorMappingEnabled();
  static bool isSensorBinding(InputBindingKey key);
  static void logInputEvent(InputBindingInfo::Type bind_type, InputBindingKey key, float value, float initial_value,
                            float min_value);

  void initialize(SettingsInterface* sif, InputBindingInfo::Type bind_type, std::string section_name,
                  std::string key_name);

  void clearBinding();
  void reloadBinding();

protected:
  enum : u32
  {
    TIMEOUT_FOR_SINGLE_BINDING = 5,
    TIMEOUT_FOR_ALL_BINDING = 10
  };

  virtual bool eventFilter(QObject* watched, QEvent* event) override;
  virtual bool event(QEvent* event) override;
  virtual void mouseReleaseEvent(QMouseEvent* e) override;
  virtual void resizeEvent(QResizeEvent* e) override;

  virtual void startListeningForInput(u32 timeout_in_seconds);
  virtual void stopListeningForInput();
  virtual void openDialog();

  bool isListeningForInput() const { return m_input_listen_timer != nullptr; }
  void setNewBinding();
  void updateElidedText();
  void updateTextAndToolTip();

  void hookInputManager();
  void unhookInputManager();

  void onClicked();
  void onInputListenTimerTimeout();
  void inputManagerHookCallback(InputBindingKey key, float value);

  void showEffectBindingDialog();

  SettingsInterface* m_sif = nullptr;
  InputBindingInfo::Type m_bind_type = InputBindingInfo::Type::Unknown;
  std::string m_section_name;
  std::string m_key_name;
  std::vector<std::string> m_bindings;
  std::vector<InputBindingKey> m_new_bindings;
  std::vector<std::pair<InputBindingKey, std::pair<float, float>>> m_value_ranges;
  QString m_full_text;
  QTimer* m_input_listen_timer = nullptr;
  u32 m_input_listen_remaining_seconds = 0;
  QPoint m_input_listen_start_position{};
  bool m_mouse_mapping_enabled = false;
  bool m_sensor_mapping_enabled = false;
};
