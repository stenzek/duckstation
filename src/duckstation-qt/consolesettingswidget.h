#pragma once

#include <QtWidgets/QWidget>

#include "ui_consolesettingswidget.h"

class ConsoleSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ConsoleSettingsWidget(QWidget* parent = nullptr);
  ~ConsoleSettingsWidget();

private:
  Ui::ConsoleSettingsWidget m_ui;
};
