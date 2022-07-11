#include "gdbserver.h"
#include "gdbconnection.h"
#include "common/log.h"
#include "qthost.h"
Log_SetChannel(GDBServer);

GDBServer::GDBServer(QObject *parent, u16 port)
    : QTcpServer(parent)
{
  if (listen(QHostAddress::LocalHost, port)) {
    Log_InfoPrintf("GDB server listening on TCP port %u", port);
  }
  else {
    Log_InfoPrintf("Failed to listen on TCP port %u for GDB server: %s", port, errorString().toUtf8().constData());
  }
}

GDBServer::~GDBServer()
{
  Log_InfoPrint("GDB server stopped");
  for (auto* thread : m_connections) {
    thread->quit();
    thread->wait();
    delete thread;
  }
}

void GDBServer::incomingConnection(qintptr descriptor)
{
  Log_InfoPrint("Accepted connection on GDB server");
  GDBConnection *thread = new GDBConnection(this, descriptor);
  connect(g_emu_thread, &EmuThread::systemPaused, thread, &GDBConnection::onEmulationPaused);
  connect(g_emu_thread, &EmuThread::systemResumed, thread, &GDBConnection::onEmulationResumed);
  thread->start();
  m_connections.push_back(thread);
}
