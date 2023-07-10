// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "netplaydialogs.h"
#include "qthost.h"

#include "core/netplay.h"

#include <QtWidgets/QPushButton>

CreateNetplaySessionDialog::CreateNetplaySessionDialog(QWidget* parent) : QDialog(parent)
{
  m_ui.setupUi(this);

  connect(m_ui.maxPlayers, &QSpinBox::valueChanged, this, &CreateNetplaySessionDialog::updateState);
  connect(m_ui.port, &QSpinBox::valueChanged, this, &CreateNetplaySessionDialog::updateState);
  connect(m_ui.inputDelay, &QSpinBox::valueChanged, this, &CreateNetplaySessionDialog::updateState);
  connect(m_ui.nickname, &QLineEdit::textChanged, this, &CreateNetplaySessionDialog::updateState);
  connect(m_ui.password, &QLineEdit::textChanged, this, &CreateNetplaySessionDialog::updateState);

  connect(m_ui.buttonBox->button(QDialogButtonBox::Ok), &QAbstractButton::clicked, this,
          &CreateNetplaySessionDialog::accept);
  connect(m_ui.buttonBox->button(QDialogButtonBox::Cancel), &QAbstractButton::clicked, this,
          &CreateNetplaySessionDialog::reject);

  updateState();
}

CreateNetplaySessionDialog::~CreateNetplaySessionDialog() = default;

void CreateNetplaySessionDialog::accept()
{
  if (!validate())
    return;

  const int players = m_ui.maxPlayers->value();
  const int port = m_ui.port->value();
  const int inputdelay = m_ui.inputDelay->value();
  const QString& nickname = m_ui.nickname->text();
  const QString& password = m_ui.password->text();
  QDialog::accept();

  g_emu_thread->createNetplaySession(nickname.trimmed(), port, players, password, inputdelay);
}

bool CreateNetplaySessionDialog::validate()
{
  const int players = m_ui.maxPlayers->value();
  const int port = m_ui.port->value();
  const int inputdelay = m_ui.inputDelay->value();
  const QString& nickname = m_ui.nickname->text();
  return (!nickname.isEmpty() && players >= 2 && players <= Netplay::MAX_PLAYERS && port > 0 && port <= 65535 &&
          inputdelay >= 0 && inputdelay <= 10);
}

void CreateNetplaySessionDialog::updateState()
{
  m_ui.buttonBox->button(QDialogButtonBox::Ok)->setEnabled(validate());
}

JoinNetplaySessionDialog::JoinNetplaySessionDialog(QWidget* parent)
{
  m_ui.setupUi(this);

  connect(m_ui.port, &QSpinBox::valueChanged, this, &JoinNetplaySessionDialog::updateState);
  connect(m_ui.inputDelay, &QSpinBox::valueChanged, this, &JoinNetplaySessionDialog::updateState);
  connect(m_ui.nickname, &QLineEdit::textChanged, this, &JoinNetplaySessionDialog::updateState);
  connect(m_ui.password, &QLineEdit::textChanged, this, &JoinNetplaySessionDialog::updateState);
  connect(m_ui.hostname, &QLineEdit::textChanged, this, &JoinNetplaySessionDialog::updateState);

  connect(m_ui.buttonBox->button(QDialogButtonBox::Ok), &QAbstractButton::clicked, this,
          &JoinNetplaySessionDialog::accept);
  connect(m_ui.buttonBox->button(QDialogButtonBox::Cancel), &QAbstractButton::clicked, this,
          &JoinNetplaySessionDialog::reject);

  updateState();
}

JoinNetplaySessionDialog::~JoinNetplaySessionDialog() = default;

void JoinNetplaySessionDialog::accept()
{
  if (!validate())
    return;

  const int port = m_ui.port->value();
  const int inputdelay = m_ui.inputDelay->value();
  const QString& nickname = m_ui.nickname->text();
  const QString& hostname = m_ui.hostname->text();
  const QString& password = m_ui.password->text();
  const bool spectating = m_ui.spectating->isChecked();
  QDialog::accept();

  g_emu_thread->joinNetplaySession(nickname.trimmed(), hostname.trimmed(), port, password, spectating, inputdelay);
}

bool JoinNetplaySessionDialog::validate()
{
  const int port = m_ui.port->value();
  const int inputdelay = m_ui.inputDelay->value();
  const QString& nickname = m_ui.nickname->text();
  const QString& hostname = m_ui.hostname->text();
  return (!nickname.isEmpty() && !hostname.isEmpty() && port > 0 && port <= 65535 && inputdelay >= 0 && inputdelay <= 10);
}

void JoinNetplaySessionDialog::updateState()
{
  m_ui.buttonBox->button(QDialogButtonBox::Ok)->setEnabled(validate());
}
