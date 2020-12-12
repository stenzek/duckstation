#include "gdbconnection.h"
#include "common/log.h"
#include "core/cpu_core.h"
#include "core/gdb_protocol.h"
Log_SetChannel(GDBConnection);

GDBConnection::GDBConnection(QObject *parent, int descriptor)
  : QThread(parent), m_descriptor(descriptor)
{
  Log_InfoPrintf("(%u) Accepted new connection on GDB server", m_descriptor);

  connect(&m_socket, SIGNAL(readyRead()), this, SLOT(receivedData()));
  connect(&m_socket, SIGNAL(disconnected()), this, SLOT(gotDisconnected()));

  if (!m_socket.setSocketDescriptor(m_descriptor)) {
    Log_ErrorPrintf("(%u) Failed to set socket descriptor: %s", m_descriptor, m_socket.errorString().toUtf8().constData());
  }

  CPU::g_state.debug_pause = true;
  CPU::DebugUpdatedInstrumentation();
}

void GDBConnection::gotDisconnected()
{
  Log_InfoPrintf("(%u) Client disconnected", m_descriptor);
  this->exit(0);
}

void GDBConnection::receivedData()
{
  qint64 bytesRead;
  char buffer[256];

  while ((bytesRead = m_socket.read(buffer, sizeof(buffer))) > 0) {
    for (char c : std::string_view(buffer, bytesRead)) {
      m_readBuffer.push_back(c);
      if (GDBProtocol::IsPacketComplete(m_readBuffer)) {
        Log_DebugPrintf("(%u) > %s", m_descriptor, m_readBuffer.c_str());
        writePacket(GDBProtocol::ProcessPacket(m_readBuffer));
        m_readBuffer.erase();
      }
    }
  }
  if (bytesRead == -1) {
    Log_ErrorPrintf("(%u) Failed to read from socket: %s", m_descriptor, m_socket.errorString().toUtf8().constData());
  }
}

void GDBConnection::onDebugPaused()
{
  if (m_seen_resume) {
    m_seen_resume = false;
    // Generate a stop reply packet, insert '?' command to generate it.
    writePacket(GDBProtocol::ProcessPacket("$?#3f"));
  }
}

void GDBConnection::onDebugResumed()
{
  m_seen_resume = true;
}

void GDBConnection::writePacket(std::string_view packet)
{
  Log_DebugPrintf("(%u) < %*s", m_descriptor, packet.length(), packet.data());
  if (m_socket.write(packet.data(), packet.length()) == -1) {
    Log_ErrorPrintf("(%u) Failed to write to socket: %s", m_descriptor, m_socket.errorString().toUtf8().constData());
  }
}
