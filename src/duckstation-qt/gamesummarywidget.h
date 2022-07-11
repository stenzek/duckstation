#pragma once
#include "common/types.h"
#include <QtWidgets/QWidget>

#include "ui_gamesummarywidget.h"

enum class DiscRegion : u8;

namespace GameDatabase {
struct Entry;
}

class SettingsDialog;

class GameSummaryWidget : public QWidget
{
  Q_OBJECT

public:
  GameSummaryWidget(const std::string& path, const std::string& serial, DiscRegion region,
                    const GameDatabase::Entry* entry, SettingsDialog* dialog, QWidget* parent);
  ~GameSummaryWidget();

private Q_SLOTS:
  void onInputProfileChanged(int index);
  void onComputeHashClicked();

private:
  void populateUi(const std::string& path, const std::string& serial, DiscRegion region,
                  const GameDatabase::Entry* entry);
  void populateTracksInfo();

  Ui::GameSummaryWidget m_ui;
  SettingsDialog* m_dialog;

  std::string m_path;
  std::string m_redump_search_keyword;
};
