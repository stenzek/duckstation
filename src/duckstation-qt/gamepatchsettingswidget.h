// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_gamepatchdetailswidget.h"
#include "ui_gamepatchsettingswidget.h"

#include "core/cheats.h"

#include <QtWidgets/QWidget>

namespace GameList {
struct Entry;
}

class SettingsWindow;

class GamePatchDetailsWidget : public QWidget
{
  Q_OBJECT

public:
  GamePatchDetailsWidget(const Cheats::CodeInfo& info, bool enabled, SettingsWindow* dialog, QWidget* parent);
  ~GamePatchDetailsWidget();

Q_SIGNALS:
  void editRequested();
  void removeRequested();

private:
  void onEnabledStateChanged(Qt::CheckState state);
  void setOptionValue(u32 value);

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

  void disableAllPatches();

private:
  void reloadList();

  void onReloadClicked();
  void onAddClicked();
  void editPatch(const std::string& name);
  void removePatch(const std::string& name);

  std::string getPathForSavingPatches(std::string_view existing_name = {}) const;

  Ui::GamePatchSettingsWidget m_ui;
  SettingsWindow* m_dialog;
  Cheats::CodeInfoList m_patches;
};
