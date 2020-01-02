#pragma once
#include "core/types.h"
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QWidget>
#include <array>
#include <vector>

class QTimer;

class QtHostInterface;
class InputButtonBindingWidget;

class PortSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  PortSettingsWidget(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~PortSettingsWidget();

private:
  QtHostInterface* m_host_interface;

  QTabWidget* m_tab_widget;

  struct PortSettingsUI
  {
    QWidget* widget;
    QVBoxLayout* layout;
    QComboBox* controller_type;
    QLineEdit* memory_card_path;
    QPushButton* memory_card_path_browse;
    QWidget* button_binding_container;
  };

  void createUi();
  void createPortSettingsUi(int index, PortSettingsUI* ui);
  void createPortBindingSettingsUi(int index, PortSettingsUI* ui);
  void onControllerTypeChanged(int index);

  std::array<PortSettingsUI, 2> m_port_ui = {};
};

class InputButtonBindingWidget : public QPushButton
{
  Q_OBJECT

public:
  InputButtonBindingWidget(QtHostInterface* host_interface, QString setting_name, ControllerType controller_type,
                           QWidget* parent);
  ~InputButtonBindingWidget();

protected:
  void keyPressEvent(QKeyEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;
  
private Q_SLOTS:
  void onPressed();
  void onInputListenTimerTimeout();

private:
  bool isListeningForInput() const { return m_input_listen_timer != nullptr; }
  void startListeningForInput();
  void stopListeningForInput();

  QtHostInterface* m_host_interface;
  QString m_setting_name;
  QString m_current_binding_value;
  ControllerType m_controller_type;
  QTimer* m_input_listen_timer = nullptr;
  u32 m_input_listen_remaining_seconds = 0;
};
