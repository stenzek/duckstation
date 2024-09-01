// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/timer.h"
#include "common/types.h"
#include "qtprogresscallback.h"
#include "ui_selectdiscdialog.h"
#include <QtWidgets/QDialog>
#include <array>
#include <memory>
#include <string>

class SelectDiscDialog final : public QDialog
{
  Q_OBJECT

public:
  SelectDiscDialog(const std::string& disc_set_name, QWidget* parent = nullptr);
  ~SelectDiscDialog();

  ALWAYS_INLINE const std::string& getSelectedDiscPath() { return m_selected_path; }

protected:
  void resizeEvent(QResizeEvent* ev);

private Q_SLOTS:
  void onListItemActivated(const QTreeWidgetItem* item);
  void updateStartEnabled();
  void onSelectClicked();
  void onCancelClicked();

private:
  void populateList(const std::string& disc_set_name);

  Ui::SelectDiscDialog m_ui;
  std::string m_selected_path;
};
