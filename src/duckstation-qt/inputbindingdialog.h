// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_inputbindingalldialog.h"
#include "ui_inputbindingdialog.h"

#include "util/input_manager.h"

#include "common/types.h"

#include <QtCore/QElapsedTimer>
#include <QtWidgets/QDialog>
#include <optional>
#include <string>
#include <vector>

class SettingsInterface;

class InputBindingDialog : public QDialog
{
  Q_OBJECT

public:
  InputBindingDialog(SettingsInterface* sif, InputBindingInfo::Type bind_type, std::string section_name,
                     std::string key_name, std::vector<std::string> bindings, const QString& display_name,
                     QWidget* parent);
  ~InputBindingDialog();

protected:
  enum : u32
  {
    TIMEOUT_FOR_BINDING = 5
  };

  virtual bool eventFilter(QObject* watched, QEvent* event) override;

  virtual void startListeningForInput(u32 timeout_in_seconds);
  virtual void stopListeningForInput();

  bool isListeningForInput() const { return m_input_listen_timer != nullptr; }
  void addNewBinding(std::span<const InputBindingKey> keys);

  void updateList();
  void saveListToSettings();

  void onSensitivityChanged(int value);
  void onResetDeadzoneClicked();
  void onDeadzoneChanged(int value);
  void onResetSensitivityClicked();

  void onAddBindingButtonClicked();
  void onRemoveBindingButtonClicked();
  void onClearBindingsButtonClicked();
  void onInputListenTimerTimeout();

  Ui::InputBindingDialog m_ui;

  SettingsInterface* m_sif;
  InputBindingInfo::Type m_bind_type;
  std::string m_section_name;
  std::string m_key_name;
  std::vector<std::string> m_bindings;

  QTimer* m_input_listen_timer = nullptr;
  u32 m_input_listen_remaining_seconds = 0;
  QString m_input_listen_binding;
  QPoint m_input_listen_start_position{};
  bool m_mouse_mapping_enabled = false;
};

class InputBindingWidget;

/// Asks for each binding of a controller in turn, through the binding widgets which show them.
class InputBindingAllDialog final : public QDialog
{
  Q_OBJECT

public:
  struct Step
  {
    InputBindingWidget* widget;
    QString display_name;
    InputBindingInfo::Type type;
  };

  InputBindingAllDialog(std::vector<Step> steps, const QString& title, const QIcon& icon, QWidget* parent);
  ~InputBindingAllDialog() override;

  void done(int result) override;

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void showEvent(QShowEvent* event) override;

private:
  void startStep(u32 step, bool continue_sequence);
  void nextStep();
  void updateStatus();
  void onTimerTimeout();

  std::vector<Step> m_steps;
  Ui::InputBindingAllDialog m_ui;

  QTimer* m_timer;
  QElapsedTimer m_step_timer;
  QString m_capture_binding;
  std::vector<u32> m_step_keys;
  u32 m_step = 0;
  u32 m_capture_id = 0;
  bool m_listening = false;
};
