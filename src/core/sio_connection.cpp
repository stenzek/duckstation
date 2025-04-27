#include "sio_connection.h"
#include "common/log.h"
#include "common/small_string.h"
#include <cstdarg>
LOG_CHANNEL(SIO);

#ifdef _WIN32
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define SOCKET_ERROR_WOULD_BLOCK WSAEWOULDBLOCK
#else
#define SOCKET_ERROR_WOULD_BLOCK EWOULDBLOCK
#define INVALID_SOCKET -1
#endif

static void CloseSocket(SocketType fd)
{
#ifdef _WIN32
  closesocket(fd);
#else
  close(fd);
#endif
}

static int GetSocketError()
{
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

static void PrintSocketError(const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);

  SmallString str;
  str.vsprintf(format, ap);
  va_end(ap);

  ERROR_LOG("{}: {}", str, GetSocketError());
}

static bool SetSocketNonblocking(SocketType socket, bool nonblocking)
{
#ifdef WIN32
  u_long value = nonblocking ? 1 : 0;
  if (ioctlsocket(socket, FIONBIO, &value) != 0)
  {
    PrintSocketError("ioctlsocket(%s)", nonblocking ? "nonblocking" : "blocking");
    return false;
  }

  return true;
#else
  return false;
#endif
}

SIOSocketConnection::SIOSocketConnection(std::string hostname, u32 port)
  : m_hostname(std::move(hostname)), m_port(port), m_client_fd(INVALID_SOCKET)
{
}

SIOSocketConnection::~SIOSocketConnection()
{
  if (m_client_fd != INVALID_SOCKET)
    CloseSocket(m_client_fd);

#ifdef WIN32
  if (m_client_event != NULL)
    WSACloseEvent(m_client_event);

  if (m_want_write_event != NULL)
    CloseHandle(m_want_write_event);

  if (m_sockets_initialized)
    WSACleanup();
#endif
}

bool SIOSocketConnection::Initialize()
{
#ifdef _WIN32
  WSADATA wd = {};
  if (WSAStartup(MAKEWORD(2, 0), &wd) != 0)
  {
    PrintSocketError("WSAStartup() failed");
    return false;
  }

  m_sockets_initialized = true;
#endif

#ifdef _WIN32
  m_client_event = WSACreateEvent();
  m_want_write_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (m_client_event == NULL || m_want_write_event == NULL)
    return false;
#endif

  return true;
}

u32 SIOSocketConnection::Read(void* buffer, u32 buffer_size, u32 min_size)
{
  std::unique_lock lock(m_buffer_mutex);
  if (m_read_buffer.empty() || m_client_fd < 0)
  {
    m_data_ready.store(false);
    return 0;
  }

  if (m_read_buffer.size() < min_size)
    return 0;

  const u32 to_read = std::min<u32>(static_cast<u32>(m_read_buffer.size()), buffer_size);
  if (to_read > 0)
  {
    std::memcpy(buffer, m_read_buffer.data(), to_read);
    if (to_read == m_read_buffer.size())
    {
      m_read_buffer.clear();
    }
    else
    {
      const size_t new_size = m_read_buffer.size() - to_read;
      std::memmove(&m_read_buffer[0], &m_read_buffer[to_read], new_size);
      m_read_buffer.resize(new_size);
    }
  }

  m_data_ready.store(!m_read_buffer.empty());
  return to_read;
}

u32 SIOSocketConnection::Write(const void* buffer, u32 buffer_size)
{
  std::unique_lock lock(m_buffer_mutex);
  if (m_client_fd < 0)
    return 0;

  // TODO: Max buffer size
  const u32 to_write = buffer_size;
  const size_t current_size = m_write_buffer.size();
  m_write_buffer.resize(m_write_buffer.size() + buffer_size);
  std::memcpy(&m_write_buffer[current_size], buffer, buffer_size);

#ifdef _WIN32
  SetEvent(m_want_write_event);
#else
#endif

  return to_write;
}

void SIOSocketConnection::StartThread()
{
  m_thread = std::thread([this]() { SocketThread(); });
}

void SIOSocketConnection::ShutdownThread()
{
  if (!m_thread.joinable())
    return;

  m_thread_shutdown.store(true);

#ifdef _WIN32
  SetEvent(m_want_write_event);
#endif

  m_thread.join();
}

void SIOSocketConnection::HandleRead()
{
  std::unique_lock lock(m_buffer_mutex);

  size_t current_size = m_read_buffer.size();
  size_t buffer_size = std::max<size_t>(m_read_buffer.size() * 2, 128);
  m_read_buffer.resize(buffer_size);

  int nbytes = recv(m_client_fd, reinterpret_cast<char*>(&m_read_buffer[current_size]),
                    static_cast<int>(buffer_size - current_size), 0);
  if (nbytes <= 0)
  {
    m_read_buffer.resize(current_size);
    if (GetSocketError() == SOCKET_ERROR_WOULD_BLOCK)
      return;

    PrintSocketError("recv() failed");
    Disconnect();
    return;
  }
  else if (nbytes == 0)
  {
    INFO_LOG("Client disconnected.");
    Disconnect();
    return;
  }

  m_read_buffer.resize(current_size + static_cast<size_t>(nbytes));
  m_data_ready.store(true);
}

void SIOSocketConnection::HandleWrite()
{
  std::unique_lock lock(m_buffer_mutex);
  if (m_write_buffer.empty())
    return;

  int nbytes =
    send(m_client_fd, reinterpret_cast<const char*>(m_write_buffer.data()), static_cast<int>(m_write_buffer.size()), 0);
  if (nbytes < 0)
  {
    if (GetSocketError() == SOCKET_ERROR_WOULD_BLOCK)
      return;

    PrintSocketError("send() failed");
    Disconnect();
    return;
  }

  if (nbytes == static_cast<int>(m_write_buffer.size()))
  {
    m_write_buffer.clear();
    return;
  }

  const size_t new_size = m_write_buffer.size() - static_cast<size_t>(nbytes);
  std::memmove(&m_write_buffer[0], &m_write_buffer[static_cast<size_t>(nbytes)], new_size);
  m_write_buffer.resize(new_size);
}

void SIOSocketConnection::HandleClose()
{
  INFO_LOG("Client disconnected.");
  Disconnect();
}

void SIOSocketConnection::Disconnect()
{
  CloseSocket(m_client_fd);
  m_client_fd = INVALID_SOCKET;
  m_read_buffer.clear();
  m_write_buffer.clear();
  m_connected.store(false);
  m_data_ready.store(false);
}

std::unique_ptr<SIOConnection> SIOConnection::CreateSocketServer(std::string hostname, u32 port)
{
  std::unique_ptr<SIOSocketServerConnection> server(new SIOSocketServerConnection(std::move(hostname), port));
  if (!server->Initialize())
    return {};

  return server;
}

SIOSocketServerConnection::SIOSocketServerConnection(std::string hostname, u32 port)
  : SIOSocketConnection(std::move(hostname), port), m_accept_fd(INVALID_SOCKET)
{
}

SIOSocketServerConnection::~SIOSocketServerConnection()
{
  ShutdownThread();

  if (m_accept_fd != INVALID_SOCKET)
    CloseSocket(m_accept_fd);

  if (m_accept_event != NULL)
    WSACloseEvent(m_accept_event);
}

bool SIOSocketServerConnection::Initialize()
{
  if (!SIOSocketConnection::Initialize())
    return false;

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u16>(m_port));

  m_accept_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (m_accept_fd == INVALID_SOCKET)
  {
    PrintSocketError("socket() failed");
    return false;
  }

  if (bind(m_accept_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
  {
    PrintSocketError("bind() failed");
    return false;
  }

  if (listen(m_accept_fd, 1) != 0)
  {
    PrintSocketError("listen() failed");
    return false;
  }

#ifdef _WIN32
  SetSocketNonblocking(m_accept_fd, true);

  m_accept_event = WSACreateEvent();
  if (m_accept_event == NULL)
    return false;

  if (WSAEventSelect(m_accept_fd, m_accept_event, FD_ACCEPT) != 0)
  {
    PrintSocketError("WSAEventSelect(FD_ACCEPT) failed");
    return false;
  }
#endif

  StartThread();
  return true;
}

void SIOSocketServerConnection::SocketThread()
{
  while (!m_thread_shutdown.load())
  {
#ifdef _WIN32
    const HANDLE event_handles[] = {m_want_write_event, m_accept_event, m_client_event};
    const DWORD res = WSAWaitForMultipleEvents(countof(event_handles), event_handles, FALSE, 1000, FALSE);
    if (res == WAIT_TIMEOUT)
      continue;

    WSANETWORKEVENTS ev;
    if (WSAEnumNetworkEvents(m_accept_fd, m_accept_event, &ev) == 0)
    {
      if (ev.lNetworkEvents & FD_ACCEPT)
        HandleAccept();
    }

    if (m_client_fd != INVALID_SOCKET)
    {
      if (WSAEnumNetworkEvents(m_client_fd, m_client_event, &ev) == 0)
      {
        if (ev.lNetworkEvents & FD_READ)
          HandleRead();
        if (ev.lNetworkEvents & FD_WRITE)
          HandleWrite();
        if (ev.lNetworkEvents & FD_CLOSE)
          HandleClose();
      }
    }

    if (m_client_fd != INVALID_SOCKET && res == WSA_WAIT_EVENT_0)
      HandleWrite();
#else
#endif
  }
}

void SIOSocketServerConnection::HandleAccept()
{
  sockaddr client_address = {};
  int client_address_len = sizeof(client_address);
  SocketType new_socket = accept(m_accept_fd, &client_address, &client_address_len);
  if (new_socket == INVALID_SOCKET)
  {
    if (GetSocketError() != SOCKET_ERROR_WOULD_BLOCK)
      PrintSocketError("accept() failed");

    return;
  }

  if (m_client_fd != INVALID_SOCKET)
  {
    static const char error[] = "Client already connected.";
    WARNING_LOG("Dropping client connection because we're already connected");

    // we already have a client
    SetSocketNonblocking(new_socket, false);
    send(new_socket, error, sizeof(error) - 1, 0);
    CloseSocket(new_socket);
    return;
  }

  SetSocketNonblocking(new_socket, true);

#ifdef _WIN32
  if (WSAEventSelect(new_socket, m_client_event, FD_READ | FD_WRITE | FD_CLOSE) != 0)
  {
    PrintSocketError("WSAEventSelect(FD_READ | FD_WRITE | FD_CLOSE) failed");
    CloseSocket(new_socket);
  }
#endif

  std::unique_lock lock(m_buffer_mutex);
  INFO_LOG("Client connection accepted: {}", new_socket);
  m_client_fd = new_socket;
  m_connected.store(true);
}

SIOSocketClientConnection::SIOSocketClientConnection(std::string hostname, u32 port)
  : SIOSocketConnection(std::move(hostname), port)
{
}

SIOSocketClientConnection::~SIOSocketClientConnection()
{
  ShutdownThread();
}

bool SIOSocketClientConnection::Initialize()
{
  if (!SIOSocketConnection::Initialize())
    return false;

  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* ai;
  int err = getaddrinfo(m_hostname.c_str(), TinyString::from_format("{}", m_port), &hints, &ai);
  if (err != 0)
  {
    ERROR_LOG("getaddrinfo({}:{}) failed: {}", m_hostname, m_port, err);
    return false;
  }

  m_client_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (m_client_fd == INVALID_SOCKET)
  {
    PrintSocketError("socket() failed");
    freeaddrinfo(ai);
    return false;
  }

  err = connect(m_client_fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
  freeaddrinfo(ai);
  if (err != 0)
  {
    PrintSocketError("connect() failed");
    return false;
  }

  SetSocketNonblocking(m_client_fd, true);

#ifdef _WIN32
  if (WSAEventSelect(m_client_fd, m_client_event, FD_READ | FD_WRITE | FD_CLOSE) != 0)
  {
    PrintSocketError("WSAEventSelect(FD_READ | FD_WRITE | FD_CLOSE) failed");
    CloseSocket(m_client_fd);
  }
#endif

  m_connected.store(true);
  StartThread();
  return true;
}

void SIOSocketClientConnection::SocketThread()
{
  while (!m_thread_shutdown.load())
  {
#ifdef _WIN32
    HANDLE event_handles[] = {m_want_write_event, m_client_event};
    DWORD res = WSAWaitForMultipleEvents(countof(event_handles), event_handles, FALSE, 1000, FALSE);
    if (res == WAIT_TIMEOUT)
      continue;

    WSANETWORKEVENTS ev;
    if (m_client_fd != INVALID_SOCKET)
    {
      if (WSAEnumNetworkEvents(m_client_fd, m_client_event, &ev) == 0)
      {
        if (ev.lNetworkEvents & FD_READ)
          HandleRead();
        if (ev.lNetworkEvents & FD_WRITE)
          HandleWrite();
        if (ev.lNetworkEvents & FD_CLOSE)
          HandleClose();
      }
    }

    if (m_client_fd != INVALID_SOCKET && res == WSA_WAIT_EVENT_0)
      HandleWrite();
#else
#endif
  }
}

std::unique_ptr<SIOConnection> SIOConnection::CreateSocketClient(std::string hostname, u32 port)
{
  std::unique_ptr<SIOSocketClientConnection> server(new SIOSocketClientConnection(std::move(hostname), port));
  if (!server->Initialize())
    return {};

  return server;
}
