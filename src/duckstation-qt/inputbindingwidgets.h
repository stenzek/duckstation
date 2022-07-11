#pragma once
#include "common/types.h"
#include "frontend-common/input_manager.h"
#include <QtWidgets/QPushButton>
#include <optional>

class QTimer;

class ControllerSettingsDialog;
class SettingsInterface;

class InputBindingWidget : public QPushButton
{
  Q_OBJECT

public:
  InputBindingWidget(QWidget* parent);
  InputBindingWidget(QWidget* parent, SettingsInterface* sif, std::string section_name, std::string key_name);
  ~InputBindingWidget();

  static bool isMouseMappingEnabled();

  void initialize(SettingsInterface* sif, std::string section_name, std::string key_name);

public Q_SLOTS:
  void clearBinding();
  void reloadBinding();

protected Q_SLOTS:
  void onClicked();
  void onInputListenTimerTimeout();
  void inputManagerHookCallback(InputBindingKey key, float value);

protected:
  enum : u32
  {
    TIMEOUT_FOR_SINGLE_BINDING = 5,
    TIMEOUT_FOR_ALL_BINDING = 10
  };

  virtual bool eventFilter(QObject* watched, QEvent* event) override;
  virtual bool event(QEvent* event) override;
  virtual void mouseReleaseEvent(QMouseEvent* e) override;

  virtual void startListeningForInput(u32 timeout_in_seconds);
  virtual void stopListeningForInput();
  virtual void openDialog();

  bool isListeningForInput() const { return m_input_listen_timer != nullptr; }
  void setNewBinding();
  void updateText();

  void hookInputManager();
  void unhookInputManager();

  SettingsInterface* m_sif = nullptr;
  std::string m_section_name;
  std::string m_key_name;
  std::vector<std::string> m_bindings;
  std::vector<InputBindingKey> m_new_bindings;
  QTimer* m_input_listen_timer = nullptr;
  u32 m_input_listen_remaining_seconds = 0;
  QPointF m_input_listen_start_position{};
  bool m_mouse_mapping_enabled = false;
};

class InputVibrationBindingWidget : public QPushButton
{
  Q_OBJECT

public:
  InputVibrationBindingWidget(QWidget* parent);
  InputVibrationBindingWidget(QWidget* parent, ControllerSettingsDialog* dialog, std::string section_name,
                              std::string key_name);
  ~InputVibrationBindingWidget();

  void setKey(ControllerSettingsDialog* dialog, std::string section_name, std::string key_name);

public Q_SLOTS:
  void clearBinding();

protected Q_SLOTS:
  void onClicked();

protected:
  virtual void mouseReleaseEvent(QMouseEvent* e) override;

private:
  std::string m_section_name;
  std::string m_key_name;
  std::string m_binding;

  ControllerSettingsDialog* m_dialog;
};
