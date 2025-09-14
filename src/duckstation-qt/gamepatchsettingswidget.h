// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_gamepatchdetailswidget.h"
#include "ui_gamepatchsettingswidget.h"

#include <QtWidgets/QWidget>

namespace GameList {
struct Entry;
}

class SettingsWindow;

class GamePatchDetailsWidget : public QWidget
{
public:
  GamePatchDetailsWidget(std::string name, const std::string& author, const std::string& description,
                         bool disallowed_for_achievements, bool enabled, SettingsWindow* dialog, QWidget* parent);
  ~GamePatchDetailsWidget();

private:
  void onEnabledStateChanged(Qt::CheckState state);

  Ui::GamePatchDetailsWidget m_ui;
  SettingsWindow* m_dialog;
  std::string m_name;
};

class GamePatchSettingsWidget : public QWidget
{
public:
  GamePatchSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~GamePatchSettingsWidget();

  void disableAllPatches();

private:
  void reloadList();

  void onReloadClicked();

  Ui::GamePatchSettingsWidget m_ui;
  SettingsWindow* m_dialog;
};
