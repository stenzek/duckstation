// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/types.h"
#include <QtWidgets/QWidget>

#include "ui_gamesummarywidget.h"

enum class DiscRegion : u8;

namespace GameDatabase {
struct Entry;
}

class SettingsWindow;

class GameSummaryWidget : public QWidget
{
  Q_OBJECT

public:
  GameSummaryWidget(const std::string& path, const std::string& serial, DiscRegion region,
                    const GameDatabase::Entry* entry, SettingsWindow* dialog, QWidget* parent);
  ~GameSummaryWidget();

  void reloadGameSettings();

protected:
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;

private Q_SLOTS:
  void onSeparateDiscSettingsChanged(Qt::CheckState state);
  void onCustomLanguageChanged(int language);
  void onCompatibilityCommentsClicked();
  void onInputProfileChanged(int index);
  void onEditInputProfileClicked();
  void onComputeHashClicked();

private:
  void populateUi(const std::string& path, const std::string& serial, DiscRegion region,
                  const GameDatabase::Entry* entry);
  void populateCustomAttributes();
  void updateWindowTitle();
  void setCustomTitle(const std::string& text);
  void setCustomRegion(int region);
  void setRevisionText(const QString& text);

  void populateTracksInfo();
  void updateTracksInfoColumnSizes();

  Ui::GameSummaryWidget m_ui;
  SettingsWindow* m_dialog;

  std::string m_path;
  std::string m_redump_search_keyword;
  QString m_compatibility_comments;
};
