#pragma once
#include "sio.h"
#include "types.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "common/windows_headers.h"
#include <WinSock2.h>
#endif

#ifdef _WIN32
using SocketType = SOCKET;
#else
using SocketType = int;
#endif

class SIOConnection
{
public:
  virtual ~SIOConnection() = default;

  static std::unique_ptr<SIOConnection> CreateSocketServer(std::string hostname, u32 port);
  static std::unique_ptr<SIOConnection> CreateSocketClient(std::string hostname, u32 port);

  ALWAYS_INLINE bool HasData() const { return m_data_ready.load(); }
  ALWAYS_INLINE bool IsConnected() const { return m_connected.load(); }

  virtual u32 Read(void* buffer, u32 buffer_size, u32 min_size) = 0;
  virtual u32 Write(const void* buffer, u32 buffer_size) = 0;

protected:
  std::atomic_bool m_connected{false};
  std::atomic_bool m_data_ready{false};
};

class SIOSocketConnection : public SIOConnection
{
public:
  SIOSocketConnection(std::string hostname, u32 port);
  ~SIOSocketConnection() override;

  virtual bool Initialize();

  u32 Read(void* buffer, u32 buffer_size, u32 min_size) override;
  u32 Write(const void* buffer, u32 buffer_size) override;

protected:
  virtual void SocketThread() = 0;

  void StartThread();
  void ShutdownThread();

  void HandleRead();
  void HandleWrite();
  void HandleClose();
  void Disconnect();

  std::string m_hostname;
  std::thread m_thread;
  std::atomic_bool m_thread_shutdown{false};
  u32 m_port = 0;
  SocketType m_client_fd;

  std::mutex m_buffer_mutex;
  std::vector<u8> m_read_buffer;
  std::vector<u8> m_write_buffer;

#ifdef _WIN32
  HANDLE m_client_event = NULL;
  HANDLE m_want_write_event = NULL;
  bool m_sockets_initialized = false;
#endif
};

class SIOSocketServerConnection : public SIOSocketConnection
{
public:
  SIOSocketServerConnection(std::string hostname, u32 port);
  ~SIOSocketServerConnection() override;

  bool Initialize() override;

protected:
  void SocketThread() override;

  void HandleAccept();
  
  SocketType m_accept_fd;

#ifdef _WIN32
  HANDLE m_accept_event = NULL;
#endif
};

class SIOSocketClientConnection : public SIOSocketConnection
{
public:
  SIOSocketClientConnection(std::string hostname, u32 port);
  ~SIOSocketClientConnection() override;

  bool Initialize() override;

protected:
  void SocketThread() override;
};
