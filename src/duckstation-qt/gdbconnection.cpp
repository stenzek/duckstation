// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "gdbconnection.h"
#include "common/log.h"
#include "core/gdb_protocol.h"
#include "qthost.h"
Log_SetChannel(GDBConnection);

GDBConnection::GDBConnection(GDBServer* parent, intptr_t descriptor) : QTcpSocket(parent), m_descriptor(descriptor)
{
  if (!setSocketDescriptor(descriptor))
  {
    Log_ErrorFmt("{} failed to set socket descriptor: {}", descriptor, errorString().toStdString());
    deleteLater();
    return;
  }

  connect(g_emu_thread, &EmuThread::systemPaused, this, &GDBConnection::onEmulationPaused);
  connect(g_emu_thread, &EmuThread::systemResumed, this, &GDBConnection::onEmulationResumed);
  connect(this, &QTcpSocket::readyRead, this, &GDBConnection::receivedData);
  connect(this, &QTcpSocket::disconnected, this, &GDBConnection::gotDisconnected);

  Log_InfoFmt("{} client connected", m_descriptor);

  m_seen_resume = System::IsPaused();
  g_emu_thread->setSystemPaused(true);
}

void GDBConnection::gotDisconnected()
{
  Log_InfoFmt("{} client disconnected", m_descriptor);
  deleteLater();
}

void GDBConnection::receivedData()
{
  qint64 bytesRead;
  char buffer[256];

  while ((bytesRead = read(buffer, sizeof(buffer))) > 0)
  {
    for (char c : std::string_view(buffer, bytesRead))
    {
      m_readBuffer.push_back(c);

      if (GDBProtocol::IsPacketInterrupt(m_readBuffer))
      {
        Log_DebugFmt("{} > Interrupt request", m_descriptor);
        g_emu_thread->setSystemPaused(true);
        m_readBuffer.erase();
      }
      else if (GDBProtocol::IsPacketContinue(m_readBuffer))
      {
        Log_DebugFmt("{} > Continue request", m_descriptor);
        g_emu_thread->setSystemPaused(false);
        m_readBuffer.erase();
      }
      else if (GDBProtocol::IsPacketComplete(m_readBuffer))
      {
        Log_DebugFmt("{} > %s", m_descriptor, m_readBuffer.c_str());
        writePacket(GDBProtocol::ProcessPacket(m_readBuffer));
        m_readBuffer.erase();
      }
    }
  }
  if (bytesRead == -1)
  {
    Log_ErrorFmt("{} failed to read from socket: %s", m_descriptor, errorString().toStdString());
  }
}

void GDBConnection::onEmulationPaused()
{
  if (m_seen_resume)
  {
    m_seen_resume = false;
    // Generate a stop reply packet, insert '?' command to generate it.
    writePacket(GDBProtocol::ProcessPacket("$?#3f"));
  }
}

void GDBConnection::onEmulationResumed()
{
  m_seen_resume = true;
  // Send ack, in case GDB sent a continue request.
  writePacket("+");
}

void GDBConnection::writePacket(std::string_view packet)
{
  Log_DebugFmt("{} < {}", m_descriptor, packet);
  if (write(packet.data(), packet.length()) == -1)
    Log_ErrorFmt("{} failed to write to socket: {}", m_descriptor, errorString().toStdString());
}
