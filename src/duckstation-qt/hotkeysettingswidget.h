#pragma once

#include <QtCore/QMap>
#include <QtWidgets/QWidget>
#include <array>
#include <vector>

class QScrollArea;
class QGridLayout;
class QVBoxLayout;

class ControllerSettingsDialog;

class HotkeySettingsWidget : public QWidget
{
  Q_OBJECT

public:
  HotkeySettingsWidget(QWidget* parent, ControllerSettingsDialog* dialog);
  ~HotkeySettingsWidget();

private:
  void createUi();
  void createButtons();

  ControllerSettingsDialog* m_dialog;
  QScrollArea* m_scroll_area = nullptr;
  QWidget* m_container = nullptr;
  QVBoxLayout* m_layout = nullptr;

  QMap<QString, QGridLayout*> m_categories;
};
