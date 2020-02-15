#pragma once
#include "core/types.h"
#include <QtWidgets/QTabWidget>
#include <QtCore/QMap>
#include <array>
#include <vector>

class QtHostInterface;
class QGridLayout;

class HotkeySettingsWidget : public QWidget
{
  Q_OBJECT

public:
  HotkeySettingsWidget(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~HotkeySettingsWidget();

private:
  void createUi();
  void createButtons();

  QtHostInterface* m_host_interface;

  QTabWidget* m_tab_widget;

  struct Category
  {
    QWidget* container;
    QGridLayout* layout;
  };
  QMap<QString, Category> m_categories;
};

