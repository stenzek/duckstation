// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtWidgets/QWidget>

#include "ui_foldersettingswidget.h"

class SettingsWindow;

class FolderSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  FolderSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~FolderSettingsWidget();

private:
  Ui::FolderSettingsWidget m_ui;
};
