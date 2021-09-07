#pragma once
#include "core/cheats.h"
#include "ui_cheatmanagerdialog.h"
#include <QtCore/QTimer>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <optional>

class CheatManagerDialog : public QDialog
{
  Q_OBJECT

public:
  CheatManagerDialog(QWidget* parent);
  ~CheatManagerDialog();

protected:
  void showEvent(QShowEvent* event);
  void resizeEvent(QResizeEvent* event);

private Q_SLOTS:
  void resizeColumns();

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

  void addToWatchClicked();
  void addManualWatchAddressClicked();
  void removeWatchClicked();
  void scanCurrentItemChanged(QTableWidgetItem* current, QTableWidgetItem* previous);
  void watchCurrentItemChanged(QTableWidgetItem* current, QTableWidgetItem* previous);
  void scanItemChanged(QTableWidgetItem* item);
  void watchItemChanged(QTableWidgetItem* item);
  void updateScanValue();
  void updateScanUi();

private:
  enum : int
  {
    MAX_DISPLAYED_SCAN_RESULTS = 5000
  };

  void setupAdditionalUi();
  void connectUi();
  void setUpdateTimerEnabled(bool enabled);
  void updateResults();
  void updateResultsValues();
  void updateWatch();
  void updateWatchValues();
  void fillItemForCheatCode(QTreeWidgetItem* item, u32 index, const CheatCode& code);

  QTreeWidgetItem* getItemForCheatIndex(u32 index) const;
  QTreeWidgetItem* getItemForCheatGroup(const QString& group_name) const;
  QTreeWidgetItem* createItemForCheatGroup(const QString& group_name) const;
  QStringList getCheatGroupNames() const;
  int getSelectedCheatIndex() const;
  int getSelectedResultIndexFirst() const;
  int getSelectedResultIndexLast() const;
  int getSelectedWatchIndexFirst() const;
  int getSelectedWatchIndexLast() const;

  Ui::CheatManagerDialog m_ui;

  MemoryScan m_scanner;
  MemoryWatchList m_watch;

  QTimer* m_update_timer = nullptr;
};
