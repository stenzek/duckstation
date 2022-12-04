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
  GDBServer(QObject* parent, u16 port);
  ~GDBServer();

protected:
  void incomingConnection(qintptr socketDescriptor) override;

private:
  std::list<GDBConnection*> m_connections;
};
