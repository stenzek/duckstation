#pragma once
#include "common/types.h"
#include "frontend-common/input_manager.h"
#include "ui_inputbindingdialog.h"
#include <QtWidgets/QDialog>
#include <optional>
#include <string>
#include <vector>

class SettingsInterface;

class InputBindingDialog : public QDialog
{
  Q_OBJECT

public:
  InputBindingDialog(SettingsInterface* sif, std::string section_name, std::string key_name,
                     std::vector<std::string> bindings, QWidget* parent);
  ~InputBindingDialog();

protected Q_SLOTS:
  void onAddBindingButtonClicked();
  void onRemoveBindingButtonClicked();
  void onClearBindingsButtonClicked();
  void onInputListenTimerTimeout();
  void inputManagerHookCallback(InputBindingKey key, float value);

protected:
  enum : u32
  {
    TIMEOUT_FOR_BINDING = 5
  };

  virtual bool eventFilter(QObject* watched, QEvent* event) override;

  virtual void startListeningForInput(u32 timeout_in_seconds);
  virtual void stopListeningForInput();

  bool isListeningForInput() const { return m_input_listen_timer != nullptr; }
  void addNewBinding();

  void updateList();
  void saveListToSettings();

  void hookInputManager();
  void unhookInputManager();

  Ui::InputBindingDialog m_ui;

  SettingsInterface* m_sif;
  std::string m_section_name;
  std::string m_key_name;
  std::vector<std::string> m_bindings;
  std::vector<InputBindingKey> m_new_bindings;

  QTimer* m_input_listen_timer = nullptr;
  u32 m_input_listen_remaining_seconds = 0;
  QPointF m_input_listen_start_position{};
  bool m_mouse_mapping_enabled = false;
};
