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
  const bool traversal = m_ui.traversal->isChecked();
  QDialog::accept();

  g_emu_thread->createNetplaySession(nickname.trimmed(), port, players, password, inputdelay, traversal);
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
  connect(m_ui.inputDelayTraversal, &QSpinBox::valueChanged, this, &JoinNetplaySessionDialog::updateState);
  connect(m_ui.nickname, &QLineEdit::textChanged, this, &JoinNetplaySessionDialog::updateState);
  connect(m_ui.nicknameTraversal, &QLineEdit::textChanged, this, &JoinNetplaySessionDialog::updateState);
  connect(m_ui.password, &QLineEdit::textChanged, this, &JoinNetplaySessionDialog::updateState);
  connect(m_ui.passwordTraversal, &QLineEdit::textChanged, this, &JoinNetplaySessionDialog::updateState);
  connect(m_ui.hostname, &QLineEdit::textChanged, this, &JoinNetplaySessionDialog::updateState);
  connect(m_ui.hostCode, &QLineEdit::textChanged, this, &JoinNetplaySessionDialog::updateState);
  connect(m_ui.tabConnectMode, &QTabWidget::currentChanged, this, &JoinNetplaySessionDialog::updateState);

  connect(m_ui.buttonBox->button(QDialogButtonBox::Ok), &QAbstractButton::clicked, this,
          &JoinNetplaySessionDialog::accept);
  connect(m_ui.buttonBox->button(QDialogButtonBox::Cancel), &QAbstractButton::clicked, this,
          &JoinNetplaySessionDialog::reject);

  updateState();
}

JoinNetplaySessionDialog::~JoinNetplaySessionDialog() = default;

void JoinNetplaySessionDialog::accept()
{
  const bool direct_mode = m_ui.tabTraversal->isHidden();
  const bool valid = direct_mode ? validate() : validateTraversal();
  if (!valid)
    return;

  int port = m_ui.port->value();
  int inputdelay = direct_mode ? m_ui.inputDelay->value() : m_ui.inputDelayTraversal->value();
  const QString& nickname = direct_mode ? m_ui.nickname->text() : m_ui.nicknameTraversal->text();
  const QString& password = direct_mode ? m_ui.password->text() : m_ui.passwordTraversal->text();
  const QString& hostname = m_ui.hostname->text();
  const QString& hostcode = m_ui.hostCode->text();
  const bool spectating = m_ui.spectating->isChecked();
  QDialog::accept();

  g_emu_thread->joinNetplaySession(nickname.trimmed(), hostname.trimmed(), port, password, spectating, inputdelay,
                                   !direct_mode, hostcode.trimmed());
}

bool JoinNetplaySessionDialog::validate()
{
  const int port = m_ui.port->value();
  const int inputdelay = m_ui.inputDelay->value();
  const QString& nickname = m_ui.nickname->text();
  const QString& hostname = m_ui.hostname->text();
  return (!nickname.isEmpty() && !hostname.isEmpty() && port > 0 && port <= 65535 && inputdelay >= 0 &&
          inputdelay <= 10);
}

bool JoinNetplaySessionDialog::validateTraversal()
{
  const int inputdelay = m_ui.inputDelayTraversal->value();
  const QString& nickname = m_ui.nicknameTraversal->text();
  const QString& hostcode = m_ui.hostCode->text();
  return (!nickname.isEmpty() && !hostcode.isEmpty() && inputdelay >= 0 && inputdelay <= 10);
}

void JoinNetplaySessionDialog::updateState()
{
  m_ui.buttonBox->button(QDialogButtonBox::Ok)
    ->setEnabled(m_ui.tabTraversal->isHidden() ? validate() : validateTraversal());
}
