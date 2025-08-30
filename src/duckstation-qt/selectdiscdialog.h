// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_selectdiscdialog.h"

#include <QtWidgets/QDialog>

#include <string>

namespace GameDatabase {
struct DiscSetEntry;
}

class SelectDiscDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit SelectDiscDialog(const GameDatabase::DiscSetEntry* dsentry, bool localized_titles,
                            QWidget* parent = nullptr);
  ~SelectDiscDialog();

  ALWAYS_INLINE const std::string& getSelectedDiscPath() { return m_selected_path; }

private Q_SLOTS:
  void onListItemActivated(const QTreeWidgetItem* item);
  void updateStartEnabled();
  void onSelectClicked();
  void onCancelClicked();

private:
  void populateList(const GameDatabase::DiscSetEntry* dsentry, bool localized_titles);

  Ui::SelectDiscDialog m_ui;
  std::string m_selected_path;
};
