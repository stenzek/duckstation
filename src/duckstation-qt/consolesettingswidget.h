#pragma once

#include <QtWidgets/QWidget>

#include "ui_consolesettingswidget.h"

class QtHostInterface;

class ConsoleSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ConsoleSettingsWidget(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~ConsoleSettingsWidget();

private Q_SLOTS:
  void onBrowseBIOSPathButtonClicked();

private:
  Ui::ConsoleSettingsWidget m_ui;

  QtHostInterface* m_host_interface;
};
