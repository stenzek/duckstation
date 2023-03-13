// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "core/types.h"
#include "gdbconnection.h"
#include <QtNetwork/QTcpServer>

class GDBServer : public QTcpServer
{
  Q_OBJECT

public:
  GDBServer(QObject* parent = nullptr);
  ~GDBServer();

public Q_SLOTS:
  void start(quint16 port);
  void stop();

protected:
  void incomingConnection(qintptr socketDescriptor) override;
};
