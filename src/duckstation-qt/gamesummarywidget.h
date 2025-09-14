// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/types.h"
#include <QtWidgets/QWidget>

#include "ui_gamesummarywidget.h"

enum class DiscRegion : u8;

namespace GameList {
struct Entry;
}

class SettingsWindow;

class GameSummaryWidget : public QWidget
{
public:
  GameSummaryWidget(const GameList::Entry* entry, SettingsWindow* dialog, QWidget* parent);
  ~GameSummaryWidget();

  void reloadGameSettings();

private:
  void populateUi(const GameList::Entry* entry);
  void setCustomTitle(const std::string& text);
  void setCustomRegion(int region);
  void setRevisionText(const QString& text);

  void populateTracksInfo();

  void onSeparateDiscSettingsChanged(Qt::CheckState state);
  void onCustomLanguageChanged(int language);
  void onCompatibilityCommentsClicked();
  void onInputProfileChanged(int index);
  void onEditInputProfileClicked();
  void onComputeHashClicked();

  Ui::GameSummaryWidget m_ui;
  SettingsWindow* m_dialog;

  std::string m_path;
  std::string m_redump_search_keyword;
  QString m_compatibility_comments;
};
