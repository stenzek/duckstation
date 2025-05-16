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
  Q_OBJECT

public:
  GamePatchDetailsWidget(std::string name, const std::string& author, const std::string& description, bool enabled,
                         bool disallowed_for_achievements, SettingsWindow* dialog, QWidget* parent);
  ~GamePatchDetailsWidget();

private Q_SLOTS:
  void onEnabledStateChanged(int state);

private:
  Ui::GamePatchDetailsWidget m_ui;
  SettingsWindow* m_dialog;
  std::string m_name;
};

class GamePatchSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  GamePatchSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~GamePatchSettingsWidget();

public Q_SLOTS:
  void disableAllPatches();

private Q_SLOTS:
  void onReloadClicked();

private:
  void reloadList();

  Ui::GamePatchSettingsWidget m_ui;
  SettingsWindow* m_dialog;
};
