#pragma once
#include "core/types.h"
#include <QtWidgets/QPushButton>

class QTimer;

class QtHostInterface;

class InputButtonBindingWidget : public QPushButton
{
  Q_OBJECT

public:
  InputButtonBindingWidget(QtHostInterface* host_interface, QString setting_name, QWidget* parent);
  ~InputButtonBindingWidget();

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private Q_SLOTS:
  void onPressed();
  void onInputListenTimerTimeout();

private:
  bool isListeningForInput() const { return m_input_listen_timer != nullptr; }
  void startListeningForInput();
  void stopListeningForInput();
  void setNewBinding();

  QtHostInterface* m_host_interface;
  QString m_setting_name;
  QString m_current_binding_value;
  QString m_new_binding_value;
  QTimer* m_input_listen_timer = nullptr;
  u32 m_input_listen_remaining_seconds = 0;
};
