#pragma once
#include "core/types.h"
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QWidget>
#include <array>
#include <vector>

class QTimer;

class QtHostInterface;
class InputBindingWidget;

class ControllerSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  ControllerSettingsWidget(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~ControllerSettingsWidget();

public Q_SLOTS:
  void updateMultitapControllerTitles();

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
    QScrollArea* bindings_scroll_area;
    QWidget* bindings_container;
    InputBindingWidget* first_button;
  };

  static MultitapMode getMultitapMode();

  QString getTabTitleForPort(u32 index, MultitapMode mode) const;

  void createUi();
  void reloadBindingButtons();
  void createPortSettingsUi(int index, PortSettingsUI* ui, MultitapMode multitap_mode);
  void createPortBindingSettingsUi(int index, PortSettingsUI* ui, ControllerType ctype);
  void onControllerTypeChanged(int index);
  void onLoadProfileClicked();
  void onSaveProfileClicked();

  std::array<PortSettingsUI, NUM_CONTROLLER_AND_CARD_PORTS> m_port_ui = {};
};
