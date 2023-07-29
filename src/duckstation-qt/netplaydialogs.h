// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "ui_createnetplaysessiondialog.h"
#include "ui_joinnetplaysessiondialog.h"

#include <QtWidgets/QDialog>

class CreateNetplaySessionDialog : public QDialog
{
  Q_OBJECT

public:
  CreateNetplaySessionDialog(QWidget* parent);
  ~CreateNetplaySessionDialog();

public Q_SLOTS:
  void accept() override;

private Q_SLOTS:
  void updateState();

private:
  bool validate();

  Ui::CreateNetplaySessionDialog m_ui;
};

class JoinNetplaySessionDialog : public QDialog
{
  Q_OBJECT

public:
  JoinNetplaySessionDialog(QWidget* parent);
  ~JoinNetplaySessionDialog();

public Q_SLOTS:
  void accept() override;

private Q_SLOTS:
  void updateState();

private:
  bool validate();
  bool validateTraversal();

private:
  Ui::JoinNetplaySessionDialog m_ui;
};
