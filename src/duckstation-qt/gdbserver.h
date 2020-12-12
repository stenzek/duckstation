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

public Q_SLOTS:
  void onDebugPaused();
  void onDebugResumed();

Q_SIGNALS:
  void debugPause();
  void debugResume();

protected:
  void incomingConnection(qintptr socketDescriptor) override;

private:
  std::list<GDBConnection*> m_connections;
};
