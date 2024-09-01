// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "core/cheats.h"
#include "ui_cheatcodeeditordialog.h"

class CheatCodeEditorDialog : public QDialog
{
  Q_OBJECT

public:
  CheatCodeEditorDialog(const QStringList& group_names, CheatCode* code, QWidget* parent);
  ~CheatCodeEditorDialog();

private Q_SLOTS:
  void saveClicked();
  void cancelClicked();

private:
  void setupAdditionalUi(const QStringList& group_names);
  void fillUi();
  void connectUi();

  CheatCode* m_code;

  Ui::CheatCodeEditorDialog m_ui;
};
