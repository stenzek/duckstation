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
