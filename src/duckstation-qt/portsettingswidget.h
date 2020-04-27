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
class InputBindingWidget;

class PortSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  PortSettingsWidget(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~PortSettingsWidget();

private Q_SLOTS:
  void onProfileLoaded();

private:
  QtHostInterface* m_host_interface;

  QTabWidget* m_tab_widget;

  struct PortSettingsUI
  {
    QWidget* widget;
    QVBoxLayout* layout;
    QComboBox* controller_type;
    QComboBox* memory_card_type;
    QLineEdit* memory_card_path;
    QWidget* button_binding_container;
    InputBindingWidget* first_button;
  };

  void createUi();
  void reloadBindingButtons();
  void createPortSettingsUi(int index, PortSettingsUI* ui);
  void createPortBindingSettingsUi(int index, PortSettingsUI* ui, ControllerType ctype);
  void onControllerTypeChanged(int index);
  void onBrowseMemoryCardPathClicked(int index);
  void onEjectMemoryCardClicked(int index);
  void onLoadProfileClicked();
  void onSaveProfileClicked();

  std::array<PortSettingsUI, 2> m_port_ui = {};
};
