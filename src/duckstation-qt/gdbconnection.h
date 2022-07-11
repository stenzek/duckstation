#pragma once
#include <QtCore/QThread>
#include <QtNetwork/QTcpSocket>

class GDBConnection : public QThread
{
  Q_OBJECT

public:
  GDBConnection(QObject *parent, int descriptor);

public Q_SLOTS:
  void gotDisconnected();
  void receivedData();
  void onEmulationPaused();
  void onEmulationResumed();

private:
  void writePacket(std::string_view data);

  int m_descriptor;
  QTcpSocket m_socket;
  std::string m_readBuffer;
  bool m_seen_resume;
};
