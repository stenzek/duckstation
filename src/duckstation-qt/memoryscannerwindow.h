// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "ui_memoryscannerwindow.h"

#include "core/cheats.h"

#include <QtCore/QTimer>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QWidget>
#include <optional>

class MemoryScannerWindow : public QWidget
{
  Q_OBJECT

public:
  MemoryScannerWindow();
  ~MemoryScannerWindow();

Q_SIGNALS:
  void closed();

protected:
  void showEvent(QShowEvent* event);
  void closeEvent(QCloseEvent* event);
  void resizeEvent(QResizeEvent* event);

private Q_SLOTS:
  void onSystemStarted();
  void onSystemDestroyed();

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
    MAX_DISPLAYED_SCAN_RESULTS = 5000,
    SCAN_INTERVAL = 100,
  };

  void connectUi();
  void enableUi(bool enabled);
  void resizeColumns();
  void updateResults();
  void updateResultsValues();
  void updateWatch();
  void updateWatchValues();

  int getSelectedResultIndexFirst() const;
  int getSelectedResultIndexLast() const;
  int getSelectedWatchIndexFirst() const;
  int getSelectedWatchIndexLast() const;

  Ui::MemoryScannerWindow m_ui;

  MemoryScan m_scanner;
  MemoryWatchList m_watch;

  QTimer* m_update_timer = nullptr;
};
