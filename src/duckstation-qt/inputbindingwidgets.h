// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/input_manager.h"

#include "common/types.h"

#include <QtWidgets/QPushButton>

#include <functional>
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
                     std::string section_name, std::string key_name, const QString display_name = {});
  ~InputBindingWidget();

  static bool isMouseMappingEnabled();

  /// Passes the progress of the binding capture to a widget or dialog, on the UI thread.
  /// Copyable rather than move-only, since each incomplete update queues its own UI thread call with a copy of it.
  using BindingCaptureUpdateFunction = std::function<void(std::span<const InputBindingKey> keys, bool completed)>;

  /// Starts or stops the binding capture of the widget or dialog which is listening for input. Only one runs at once.
  static void startBindingCapture(BindingCaptureUpdateFunction on_update, bool needs_incomplete_updates);
  static void addBindingCaptureKey(InputBindingKey key);
  static void completeBindingCapture();
  static void stopBindingCapture();

  void initialize(SettingsInterface* sif, InputBindingInfo::Type bind_type, std::string section_name,
                  std::string key_name, const QString& display_name);

  const std::vector<std::string>& getBindings() const { return m_bindings; }
  void setHideDeviceName(bool hide);

  void clearBinding();
  void reloadBinding();

Q_SIGNALS:
  void bindingsChanged();

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
  void setNewBinding(std::span<const InputBindingKey> keys);
  void updateElidedText();
  void updateTextAndToolTip();

  void onClicked();
  void onInputListenTimerTimeout();

  void showEffectBindingDialog();

  SettingsInterface* m_sif = nullptr;
  InputBindingInfo::Type m_bind_type = InputBindingInfo::Type::Unknown;
  std::string m_section_name;
  std::string m_key_name;
  QString m_display_name;
  std::vector<std::string> m_bindings;
  QString m_full_text;
  QTimer* m_input_listen_timer = nullptr;
  u32 m_input_listen_remaining_seconds = 0;
  QPoint m_input_listen_start_position{};
  bool m_mouse_mapping_enabled = false;
  bool m_hide_device_name = false;
};
