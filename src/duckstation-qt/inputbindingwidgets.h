#pragma once
#include "core/types.h"
#include <QtWidgets/QPushButton>

class QTimer;

class QtHostInterface;

class InputBindingWidget : public QPushButton
{
  Q_OBJECT

public:
  InputBindingWidget(QtHostInterface* host_interface, std::string section_name, std::string key_name, QWidget* parent);
  ~InputBindingWidget();

  ALWAYS_INLINE InputBindingWidget* getNextWidget() const { return m_next_widget; }
  ALWAYS_INLINE void setNextWidget(InputBindingWidget* widget) { m_next_widget = widget; }

public Q_SLOTS:
  void beginRebindAll();
  void clearBinding();
  void reloadBinding();

protected Q_SLOTS:
  void onClicked();
  void onInputListenTimerTimeout();

protected:
  enum : u32
  {
    TIMEOUT_FOR_SINGLE_BINDING = 5,
    TIMEOUT_FOR_ALL_BINDING = 10
  };

  virtual bool eventFilter(QObject* watched, QEvent* event) override;
  virtual void mouseReleaseEvent(QMouseEvent* e) override;

  virtual void startListeningForInput(u32 timeout_in_seconds);
  virtual void stopListeningForInput();

  bool isListeningForInput() const { return m_input_listen_timer != nullptr; }
  void setNewBinding();

  QtHostInterface* m_host_interface;
  std::string m_section_name;
  std::string m_key_name;
  std::string m_current_binding_value;
  std::string m_new_binding_value;
  QTimer* m_input_listen_timer = nullptr;
  u32 m_input_listen_remaining_seconds = 0;

  InputBindingWidget* m_next_widget = nullptr;
  bool m_is_binding_all = false;
};

class InputButtonBindingWidget : public InputBindingWidget
{
  Q_OBJECT

public:
  InputButtonBindingWidget(QtHostInterface* host_interface, std::string section_name, std::string key_name, QWidget* parent);
  ~InputButtonBindingWidget();

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private Q_SLOTS:
  void bindToControllerAxis(int controller_index, int axis_index, bool positive);
  void bindToControllerButton(int controller_index, int button_index);

protected:
  void startListeningForInput(u32 timeout_in_seconds) override;
  void stopListeningForInput() override;
  void hookControllerInput();
  void unhookControllerInput();
};

class InputAxisBindingWidget : public InputBindingWidget
{
  Q_OBJECT

public:
  InputAxisBindingWidget(QtHostInterface* host_interface, std::string section_name, std::string key_name, QWidget* parent);
  ~InputAxisBindingWidget();

private Q_SLOTS:
  void bindToControllerAxis(int controller_index, int axis_index);

protected:
  void startListeningForInput(u32 timeout_in_seconds) override;
  void stopListeningForInput() override;
  void hookControllerInput();
  void unhookControllerInput();
};

class InputRumbleBindingWidget : public InputBindingWidget
{
  Q_OBJECT

public:
  InputRumbleBindingWidget(QtHostInterface* host_interface, std::string section_name, std::string key_name, QWidget* parent);
  ~InputRumbleBindingWidget();

private Q_SLOTS:
  void bindToControllerRumble(int controller_index);

protected:
  void startListeningForInput(u32 timeout_in_seconds) override;
  void stopListeningForInput() override;
  void hookControllerInput();
  void unhookControllerInput();
};