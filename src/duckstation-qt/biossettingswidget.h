#pragma once
#include "core/types.h"
#include <QtWidgets/QWidget>

#include "ui_biossettingswidget.h"

class QtHostInterface;
class SettingsDialog;

class BIOSSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit BIOSSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog);
  ~BIOSSettingsWidget();

private Q_SLOTS:
  void refreshList();
  void browseSearchDirectory();
  void openSearchDirectory();

private:
  Ui::BIOSSettingsWidget m_ui;

  QtHostInterface* m_host_interface;
};
