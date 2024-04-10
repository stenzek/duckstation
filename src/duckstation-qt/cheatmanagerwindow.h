// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "ui_cheatmanagerwindow.h"

#include "core/cheats.h"

#include <QtCore/QTimer>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QWidget>
#include <optional>

class CheatManagerWindow : public QWidget
{
  Q_OBJECT

public:
  CheatManagerWindow();
  ~CheatManagerWindow();

Q_SIGNALS:
  void closed();

protected:
  void showEvent(QShowEvent* event);
  void closeEvent(QCloseEvent* event);
  void resizeEvent(QResizeEvent* event);
  void resizeColumns();

private Q_SLOTS:
  CheatList* getCheatList() const;
  void updateCheatList();
  void saveCheatList();
  void cheatListCurrentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous);
  void cheatListItemActivated(QTreeWidgetItem* item);
  void cheatListItemChanged(QTreeWidgetItem* item, int column);
  void activateCheat(u32 index);
  void setCheatCheckState(u32 index, bool checked);
  void newCategoryClicked();
  void addCodeClicked();
  void editCodeClicked();
  void deleteCodeClicked();
  void activateCodeClicked();
  void importClicked();
  void importFromFileTriggered();
  void importFromTextTriggered();
  void exportClicked();
  void clearClicked();
  void resetClicked();

private:
  enum : int
  {
    MAX_DISPLAYED_SCAN_RESULTS = 5000
  };

  void connectUi();
  void fillItemForCheatCode(QTreeWidgetItem* item, u32 index, const CheatCode& code);

  QTreeWidgetItem* getItemForCheatIndex(u32 index) const;
  QTreeWidgetItem* getItemForCheatGroup(const QString& group_name) const;
  QTreeWidgetItem* createItemForCheatGroup(const QString& group_name) const;
  QStringList getCheatGroupNames() const;
  int getSelectedCheatIndex() const;

  Ui::CheatManagerWindow m_ui;

  QTimer* m_update_timer = nullptr;
};
