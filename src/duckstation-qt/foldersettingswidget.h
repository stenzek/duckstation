#pragma once

#include <QtWidgets/QWidget>

#include "ui_foldersettingswidget.h"

class SettingsDialog;

class FolderSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  FolderSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~FolderSettingsWidget();

private:
  Ui::FolderSettingsWidget m_ui;
};
