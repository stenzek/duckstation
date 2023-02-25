// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include <QtCore/QThread>
#include <QtNetwork/QTcpSocket>

class GDBServer;

class GDBConnection : public QTcpSocket
{
  Q_OBJECT

public:
  GDBConnection(GDBServer *parent, intptr_t descriptor);

public Q_SLOTS:
  void gotDisconnected();
  void receivedData();
  void onEmulationPaused();
  void onEmulationResumed();

private:
  void writePacket(std::string_view data);

  intptr_t m_descriptor;
  std::string m_readBuffer;
  bool m_seen_resume;
};
