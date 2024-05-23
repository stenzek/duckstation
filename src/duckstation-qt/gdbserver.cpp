// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "gdbserver.h"
#include "common/log.h"
#include "gdbconnection.h"
#include "qthost.h"
Log_SetChannel(GDBServer);

GDBServer::GDBServer(QObject* parent) : QTcpServer(parent)
{
}

GDBServer::~GDBServer()
{
  stop();
}

void GDBServer::start(quint16 port)
{
  if (isListening())
  {
    return;
  }

  if (!listen(QHostAddress::LocalHost, port))
  {
    Log_ErrorFmt("Failed to listen on TCP port {} for GDB server: {}", port, errorString().toUtf8().constData());
    return;
  }

  Log_InfoFmt("GDB server listening on TCP port {}", port);
}

void GDBServer::stop()
{
  if (isListening())
  {
    close();
    Log_InfoPrint("GDB server stopped");
  }

  for (QObject* connection : children())
  {
    connection->deleteLater();
  }
}

void GDBServer::incomingConnection(qintptr descriptor)
{
  new GDBConnection(this, descriptor);
}
