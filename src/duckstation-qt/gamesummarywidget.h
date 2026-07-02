// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/cd_image_hasher.h"

#include "common/types.h"

#include <QtWidgets/QWidget>

#include "ui_gamesummarywidget.h"

enum class DiscRegion : u8;

namespace GameList {
struct Entry;
}

namespace GameDatabase {
struct TrackVerificationResult;
}

class ProgressCallback;
class SettingsWindow;

class GameSummaryWidget : public QWidget
{
  Q_OBJECT

public:
  GameSummaryWidget(const GameList::Entry* entry, SettingsWindow* dialog, QWidget* parent);
  ~GameSummaryWidget();

  void reloadGameSettings();

private:
  void populateUi(const GameList::Entry* entry);
  void setCustomTitle(const std::string& text);
  void setCustomDiscSetTitle(const std::string& text);
  void setCustomRegion(int region);

  void populateTracksInfo();
  void disableWidgetsForRuntimeScannedEntry();

  void onSeparateDiscSettingsChanged(Qt::CheckState state);
  void onChangeSerialClicked();
  void onCustomLanguageChanged(int language);
  void onCompatibilityCommentsClicked();
  void onInputProfileChanged(int index);
  void onEditInputProfileClicked();
  void onComputeHashClicked();

  void processHashResults(const GameDatabase::TrackVerificationResult& verification, bool result, bool cancelled,
                          const Error& error);

  Ui::GameSummaryWidget m_ui;
  SettingsWindow* m_dialog;

  std::string m_path;
  QString m_compatibility_comments;
};
