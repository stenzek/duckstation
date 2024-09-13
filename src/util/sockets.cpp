// SPDX-FileCopyrightText: 2015-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "sockets.h"
#include "platform_misc.h"

#include "common/assert.h"
#include "common/log.h"

#include <algorithm>
#include <cstring>
#include <limits>

#ifndef __APPLE__
#include <malloc.h> // alloca
#else
#include <alloca.h>
#endif

#ifdef _WIN32

#include "common/windows_headers.h"

#include <WS2tcpip.h>
#include <WinSock2.h>

#define SIZE_CAST(x) static_cast<int>(x)
using ssize_t = int;
using nfds_t = ULONG;

#else

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/epoll.h>
#endif

#define ioctlsocket ioctl
#define closesocket close
#define WSAEWOULDBLOCK EAGAIN
#define WSAGetLastError() errno
#define WSAPoll poll
#define SIZE_CAST(x) x

#define SOCKET_ERROR -1
#define INVALID_SOCKET -1
#define SD_BOTH SHUT_RDWR
#endif

Log_SetChannel(Sockets);

static bool SetNonBlocking(SocketDescriptor sd, Error* error)
{
  // switch to nonblocking mode
  unsigned long value = 1;
  if (ioctlsocket(sd, FIONBIO, &value) < 0)
  {
    Error::SetSocket(error, "ioctlsocket() failed: ", WSAGetLastError());
    return false;
  }

  return true;
}

void SocketAddress::SetFromSockaddr(const void* sa, size_t length)
{
  m_length = std::min(static_cast<u32>(length), static_cast<u32>(sizeof(m_data)));
  std::memcpy(m_data, sa, m_length);
  if (m_length < sizeof(m_data))
    std::memset(m_data + m_length, 0, sizeof(m_data) - m_length);
}

bool SocketAddress::IsIPAddress() const
{
  const sockaddr* addr = reinterpret_cast<const sockaddr*>(m_data);
  return (addr->sa_family == AF_INET || addr->sa_family == AF_INET6);
}

std::optional<SocketAddress> SocketAddress::Parse(Type type, const char* address, u32 port, Error* error)
{
  std::optional<SocketAddress> ret = SocketAddress();

  switch (type)
  {
    case Type::IPv4:
    {
      sockaddr_in* sain = reinterpret_cast<sockaddr_in*>(ret->m_data);
      std::memset(sain, 0, sizeof(sockaddr_in));
      sain->sin_family = AF_INET;
      sain->sin_port = htons(static_cast<u16>(port));
      int res = inet_pton(AF_INET, address, &sain->sin_addr);
      if (res == 1)
      {
        ret->m_length = sizeof(sockaddr_in);
      }
      else
      {
        Error::SetSocket(error, "inet_pton() failed: ", WSAGetLastError());
        ret.reset();
      }
    }
    break;

    case Type::IPv6:
    {
      sockaddr_in6* sain6 = reinterpret_cast<sockaddr_in6*>(ret->m_data);
      std::memset(sain6, 0, sizeof(sockaddr_in6));
      sain6->sin6_family = AF_INET;
      sain6->sin6_port = htons(static_cast<u16>(port));
      int res = inet_pton(AF_INET6, address, &sain6->sin6_addr);
      if (res == 1)
      {
        ret->m_length = sizeof(sockaddr_in6);
      }
      else
      {
        Error::SetSocket(error, "inet_pton() failed: ", WSAGetLastError());
        ret.reset();
      }
    }
    break;

#ifndef _WIN32
    case Type::Unix:
    {
      sockaddr_un* sun = reinterpret_cast<sockaddr_un*>(ret->m_data);
      std::memset(sun, 0, sizeof(sockaddr_un));
      sun->sun_family = AF_UNIX;

      const size_t len = std::strlen(address);
      if ((len + 1) <= std::size(sun->sun_path))
      {
        std::memcpy(sun->sun_path, address, len);
        ret->m_length = sizeof(sockaddr_un);
      }
      else
      {
        Error::SetStringFmt(error, "Path length {} exceeds {} bytes.", len, std::size(sun->sun_path));
        ret.reset();
      }
    }
    break;
#endif

    default:
      Error::SetStringView(error, "Unknown address type.");
      ret.reset();
      break;
  }

  return ret;
}

SmallString SocketAddress::ToString() const
{
  SmallString ret;

  const sockaddr* sa = reinterpret_cast<const sockaddr*>(m_data);
  switch (sa->sa_family)
  {
    case AF_INET:
    {
      ret.clear();
      ret.reserve(128);
      const char* res =
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(m_data)->sin_addr, ret.data(), ret.buffer_size());
      if (res == nullptr)
        ret.assign("<unknown>");
      else
        ret.update_size();

      ret.append_format(":{}", static_cast<u32>(ntohs(reinterpret_cast<const sockaddr_in*>(m_data)->sin_port)));
    }
    break;

    case AF_INET6:
    {
      ret.clear();
      ret.reserve(128);
      ret.append('[');
      const char* res = inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(m_data)->sin6_addr, ret.data() + 1,
                                  ret.buffer_size() - 1);
      if (res == nullptr)
        ret.assign("<unknown>");
      else
        ret.update_size();

      ret.append_format("]:{}", static_cast<u32>(ntohs(reinterpret_cast<const sockaddr_in6*>(m_data)->sin6_port)));
    }
    break;

#ifndef _WIN32
    case AF_UNIX:
    {
      ret.assign(reinterpret_cast<const sockaddr_un*>(m_data)->sun_path);
    }
    break;
#endif

    default:
    {
      ret.assign("<unknown>");
      break;
    }
  }

  return ret;
}

BaseSocket::BaseSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor)
  : m_multiplexer(multiplexer), m_descriptor(descriptor)
{
}

BaseSocket::~BaseSocket() = default;

SocketMultiplexer::SocketMultiplexer() = default;

SocketMultiplexer::~SocketMultiplexer()
{
  CloseAll();

#ifdef __linux__
  if (m_epoll_fd >= 0)
    close(m_epoll_fd);
#else
  if (m_poll_array)
    std::free(m_poll_array);
#endif
}

std::unique_ptr<SocketMultiplexer> SocketMultiplexer::Create(Error* error)
{
  std::unique_ptr<SocketMultiplexer> ret;
  if (PlatformMisc::InitializeSocketSupport(error))
  {
    ret = std::unique_ptr<SocketMultiplexer>(new SocketMultiplexer());
    if (!ret->Initialize(error))
      ret.reset();
  }

  return ret;
}

bool SocketMultiplexer::Initialize(Error* error)
{
#ifdef __linux__
  m_epoll_fd = epoll_create1(0);
  if (m_epoll_fd < 0)
  {
    Error::SetErrno(error, "epoll_create1() failed: ", errno);
    return false;
  }

  return true;
#else
  return true;
#endif
}

std::shared_ptr<ListenSocket> SocketMultiplexer::InternalCreateListenSocket(const SocketAddress& address,
                                                                            CreateStreamSocketCallback callback,
                                                                            Error* error)
{
  // create and bind socket
  const sockaddr* sa = reinterpret_cast<const sockaddr*>(address.GetData());
  SocketDescriptor descriptor = socket(sa->sa_family, SOCK_STREAM, StreamSocket::GetSocketProtocolForAddress(address));
  if (descriptor == INVALID_SOCKET)
  {
    Error::SetSocket(error, "socket() failed: ", WSAGetLastError());
    return {};
  }

  const int reuseaddr_enable = 1;
  if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseaddr_enable),
                 sizeof(reuseaddr_enable)) < 0)
  {
    WARNING_LOG("Failed to set SO_REUSEADDR: {}", Error::CreateSocket(WSAGetLastError()).GetDescription());
  }

  if (bind(descriptor, sa, address.GetLength()) < 0)
  {
    Error::SetSocket(error, "bind() failed: ", WSAGetLastError());
    closesocket(descriptor);
    return {};
  }

  if (listen(descriptor, 5) < 0)
  {
    Error::SetSocket(error, "listen() failed: ", WSAGetLastError());
    closesocket(descriptor);
    return {};
  }

  if (!SetNonBlocking(descriptor, error))
  {
    closesocket(descriptor);
    return {};
  }

  // create listensocket
  std::shared_ptr<ListenSocket> ret = std::make_shared<ListenSocket>(*this, descriptor, callback);

  // add to list, register for reads
  AddOpenSocket(std::static_pointer_cast<BaseSocket>(ret));
  SetNotificationMask(ret.get(), descriptor, POLLIN);
  return ret;
}

std::shared_ptr<StreamSocket> SocketMultiplexer::InternalConnectStreamSocket(const SocketAddress& address,
                                                                             CreateStreamSocketCallback callback,
                                                                             Error* error)
{
  // create and bind socket
  const sockaddr* sa = reinterpret_cast<const sockaddr*>(address.GetData());
  SocketDescriptor descriptor = socket(sa->sa_family, SOCK_STREAM, StreamSocket::GetSocketProtocolForAddress(address));
  if (descriptor == INVALID_SOCKET)
  {
    Error::SetSocket(error, "socket() failed: ", WSAGetLastError());
    return {};
  }

  if (connect(descriptor, sa, address.GetLength()) < 0)
  {
    Error::SetSocket(error, "connect() failed: ", WSAGetLastError());
    closesocket(descriptor);
    return {};
  }

  if (!SetNonBlocking(descriptor, error))
  {
    closesocket(descriptor);
    return {};
  }

  // create stream socket
  std::shared_ptr<StreamSocket> csocket = callback(*this, descriptor);
  csocket->InitialSetup();
  if (!csocket->IsConnected())
    csocket.reset();

  return csocket;
}

void SocketMultiplexer::AddOpenSocket(std::shared_ptr<BaseSocket> socket)
{
#ifdef __linux__
  struct epoll_event ev = {.events = 0u, .data = {.fd = socket->GetDescriptor()}};
  if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, socket->GetDescriptor(), &ev) != 0) [[unlikely]]
    ERROR_LOG("epoll_ctl() to add socket failed: {}", Error::CreateErrno(errno).GetDescription());
#endif

  std::unique_lock lock(m_open_sockets_lock);
  DebugAssert(m_open_sockets.find(socket->GetDescriptor()) == m_open_sockets.end());
  m_open_sockets.emplace(socket->GetDescriptor(), std::move(socket));
}

void SocketMultiplexer::AddClientSocket(std::shared_ptr<BaseSocket> socket)
{
  AddOpenSocket(std::move(socket));
  m_client_socket_count.fetch_add(1, std::memory_order_acq_rel);
}

void SocketMultiplexer::RemoveOpenSocket(BaseSocket* socket)
{
  std::unique_lock lock(m_open_sockets_lock);
  const auto iter = m_open_sockets.find(socket->GetDescriptor());
  Assert(iter != m_open_sockets.end());
  m_open_sockets.erase(iter);

#ifdef __linux__
  if (epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, socket->GetDescriptor(), nullptr) != 0) [[unlikely]]
    ERROR_LOG("epoll_ctl() to remove socket failed: {}", Error::CreateErrno(errno).GetDescription());
#else
#ifdef _DEBUG
  for (size_t i = 0; i < m_poll_array_active_size; i++)
  {
    pollfd& pfd = m_poll_array[i];
    DebugAssert(pfd.fd != socket->GetDescriptor());
  }
#endif

  // Update size.
  size_t new_active_size = 0;
  for (size_t i = 0; i < m_poll_array_active_size; i++)
    new_active_size = (m_poll_array[i].fd != INVALID_SOCKET) ? (i + 1) : new_active_size;
  m_poll_array_active_size = new_active_size;
#endif
}

void SocketMultiplexer::RemoveClientSocket(BaseSocket* socket)
{
  DebugAssert(m_client_socket_count.load(std::memory_order_acquire) > 0);
  m_client_socket_count.fetch_sub(1, std::memory_order_acq_rel);
  RemoveOpenSocket(socket);
}

bool SocketMultiplexer::HasAnyOpenSockets()
{
  std::unique_lock lock(m_open_sockets_lock);
  return !m_open_sockets.empty();
}

bool SocketMultiplexer::HasAnyClientSockets()
{
  return (GetClientSocketCount() > 0);
}

size_t SocketMultiplexer::GetClientSocketCount()
{
  return m_client_socket_count.load(std::memory_order_acquire);
}

void SocketMultiplexer::CloseAll()
{
  std::unique_lock lock(m_open_sockets_lock);

  while (!m_open_sockets.empty())
  {
    std::shared_ptr<BaseSocket> socket = m_open_sockets.begin()->second;
    lock.unlock();
    socket->Close();
    lock.lock();
  }
}

void SocketMultiplexer::SetNotificationMask(BaseSocket* socket, SocketDescriptor descriptor, u32 events)
{
#ifdef __linux__
  struct epoll_event ev = {.events = events, .data = {.fd = descriptor}};
  if (epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, descriptor, &ev) != 0) [[unlikely]]
    ERROR_LOG("epoll_ctl() for events 0x{:x} failed: {}", events, Error::CreateErrno(errno).GetDescription());
#else
  std::unique_lock lock(m_poll_array_lock);
  size_t free_slot = m_poll_array_active_size;
  for (size_t i = 0; i < m_poll_array_active_size; i++)
  {
    pollfd& pfd = m_poll_array[i];
    if (pfd.fd != descriptor)
    {
      free_slot = (pfd.fd < 0 && free_slot != m_poll_array_active_size) ? i : free_slot;
      continue;
    }

    // unbinding?
    if (events != 0)
      pfd.events = static_cast<short>(events);
    else
      pfd.fd = INVALID_SOCKET;

    return;
  }

  // don't create entries for null masks
  if (events == 0)
    return;

  // need to grow the array?
  if (free_slot == m_poll_array_max_size)
  {
    const size_t new_size = std::max(free_slot + 1, free_slot * 2);
    pollfd* new_array = static_cast<pollfd*>(std::realloc(m_poll_array, sizeof(pollfd) * new_size));
    if (!new_array)
      Panic("Memory allocation failed.");

    for (size_t i = m_poll_array_max_size; i < new_size; i++)
      new_array[i] = {.fd = INVALID_SOCKET, .events = 0, .revents = 0};
    m_poll_array = new_array;
    m_poll_array_max_size = new_size;
  }

  m_poll_array[free_slot] = {.fd = descriptor, .events = static_cast<short>(events), .revents = 0};
  m_poll_array_active_size = free_slot + 1;
#endif
}

bool SocketMultiplexer::PollEventsWithTimeout(u32 milliseconds)
{
#ifdef __linux__
  constexpr int MAX_EVENTS = 128;
  struct epoll_event events[MAX_EVENTS];

  const int nevents = epoll_wait(m_epoll_fd, events, MAX_EVENTS, static_cast<int>(milliseconds));
  if (nevents <= 0)
    return false;

  // find sockets that triggered, we use an array here so we can avoid holding the lock, and if a socket disconnects
  using PendingSocketPair = std::pair<std::shared_ptr<BaseSocket>, u32>;
  PendingSocketPair* triggered_sockets =
    reinterpret_cast<PendingSocketPair*>(alloca(sizeof(PendingSocketPair) * static_cast<size_t>(nevents)));
  size_t num_triggered_sockets = 0;
  {
    std::unique_lock open_lock(m_open_sockets_lock);
    for (int i = 0; i < nevents; i++)
    {
      const epoll_event& ev = events[i];
      const auto iter = m_open_sockets.find(ev.data.fd);
      if (iter == m_open_sockets.end()) [[unlikely]]
      {
        ERROR_LOG("Attempting to look up unknown socket {}, this should never happen.", ev.data.fd);
        continue;
      }

      // we add a reference here in case the read kills it with a write pending, or something like that
      new (&triggered_sockets[num_triggered_sockets++]) PendingSocketPair(iter->second->shared_from_this(), ev.events);
    }
  }

  // fire events
  for (size_t i = 0; i < num_triggered_sockets; i++)
  {
    PendingSocketPair& psp = triggered_sockets[i];

    // fire events
    if (psp.second & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
    {
      psp.first->OnHangupEvent();
    }
    else
    {
      if (psp.second & EPOLLIN)
        psp.first->OnReadEvent();
      if (psp.second & EPOLLOUT)
        psp.first->OnWriteEvent();
    }

    psp.first.~shared_ptr();
  }

  return true;
#else
  std::unique_lock lock(m_poll_array_lock);
  if (m_poll_array_active_size == 0)
    return false;

  const int res = WSAPoll(m_poll_array, static_cast<nfds_t>(m_poll_array_active_size), milliseconds);
  if (res <= 0)
    return false;

  // find sockets that triggered, we use an array here so we can avoid holding the lock, and if a socket disconnects
  using PendingSocketPair = std::pair<std::shared_ptr<BaseSocket>, u32>;
  PendingSocketPair* triggered_sockets =
    reinterpret_cast<PendingSocketPair*>(alloca(sizeof(PendingSocketPair) * static_cast<size_t>(res)));
  size_t num_triggered_sockets = 0;
  {
    std::unique_lock open_lock(m_open_sockets_lock);
    for (size_t i = 0; i < m_poll_array_active_size; i++)
    {
      const pollfd& pfd = m_poll_array[i];
      if (pfd.revents == 0)
        continue;

      const auto iter = m_open_sockets.find(pfd.fd);
      if (iter == m_open_sockets.end()) [[unlikely]]
      {
        ERROR_LOG("Attempting to look up unknown socket {}, this should never happen.", pfd.fd);
        continue;
      }

      // we add a reference here in case the read kills it with a write pending, or something like that
      new (&triggered_sockets[num_triggered_sockets++])
        PendingSocketPair(iter->second->shared_from_this(), pfd.revents);
    }
  }

  // release lock so connections etc can acquire it
  lock.unlock();

  // fire events
  for (size_t i = 0; i < num_triggered_sockets; i++)
  {
    PendingSocketPair& psp = triggered_sockets[i];

    // fire events
    if (psp.second & (POLLHUP | POLLERR))
    {
      psp.first->OnHangupEvent();
    }
    else
    {
      if (psp.second & POLLIN)
        psp.first->OnReadEvent();
      if (psp.second & POLLOUT)
        psp.first->OnWriteEvent();
    }

    psp.first.~shared_ptr();
  }

  return true;
#endif
}

ListenSocket::ListenSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor,
                           SocketMultiplexer::CreateStreamSocketCallback accept_callback)
  : BaseSocket(multiplexer, descriptor), m_accept_callback(accept_callback)
{
  // get local address
  sockaddr_storage sa;
  socklen_t salen = sizeof(sa);
  if (getsockname(m_descriptor, reinterpret_cast<sockaddr*>(&sa), &salen) == 0)
    m_local_address.SetFromSockaddr(&sa, salen);
}

ListenSocket::~ListenSocket()
{
  DebugAssert(m_descriptor == INVALID_SOCKET);
}

void ListenSocket::Close()
{
  if (m_descriptor < 0)
    return;

  m_multiplexer.SetNotificationMask(this, m_descriptor, 0);
  m_multiplexer.RemoveOpenSocket(this);
  closesocket(m_descriptor);
  m_descriptor = INVALID_SOCKET;
}

void ListenSocket::OnReadEvent()
{
  // connection incoming
  sockaddr_storage sa;
  socklen_t salen = sizeof(sa);
  SocketDescriptor new_descriptor = accept(m_descriptor, reinterpret_cast<sockaddr*>(&sa), &salen);
  if (new_descriptor == INVALID_SOCKET)
  {
    ERROR_LOG("accept() returned {}", Error::CreateSocket(WSAGetLastError()).GetDescription());
    return;
  }

  Error error;
  if (!SetNonBlocking(new_descriptor, &error))
  {
    ERROR_LOG("Failed to set just-connected socket to nonblocking: {}", error.GetDescription());
    closesocket(new_descriptor);
    return;
  }

  // create socket, we release our own reference.
  std::shared_ptr<StreamSocket> client = m_accept_callback(m_multiplexer, new_descriptor);
  if (!client)
  {
    closesocket(new_descriptor);
    return;
  }

  m_num_connections_accepted++;
  client->InitialSetup();
}

void ListenSocket::OnWriteEvent()
{
  ERROR_LOG("Unexpected OnWriteEvent() in ListenSocket {}", m_local_address.ToString());
}

void ListenSocket::OnHangupEvent()
{
  ERROR_LOG("Unexpected OnHangupEvent() in ListenSocket {}", m_local_address.ToString());
}

StreamSocket::StreamSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor)
  : BaseSocket(multiplexer, descriptor)
{
  // get local address
  sockaddr_storage sa;
  socklen_t salen = sizeof(sa);
  if (getsockname(m_descriptor, reinterpret_cast<sockaddr*>(&sa), &salen) == 0)
    m_local_address.SetFromSockaddr(&sa, salen);

  // get remote address
  salen = sizeof(sockaddr_storage);
  if (getpeername(m_descriptor, reinterpret_cast<sockaddr*>(&sa), &salen) == 0)
    m_remote_address.SetFromSockaddr(&sa, salen);
}

StreamSocket::~StreamSocket()
{
  DebugAssert(m_descriptor == INVALID_SOCKET);
}

u32 StreamSocket::GetSocketProtocolForAddress(const SocketAddress& sa)
{
  const sockaddr* ssa = reinterpret_cast<const sockaddr*>(sa.GetData());
  return (ssa->sa_family == AF_INET || ssa->sa_family == AF_INET6) ? IPPROTO_TCP : 0;
}

void StreamSocket::InitialSetup()
{
  // register for notifications
  m_multiplexer.AddClientSocket(shared_from_this());
  m_multiplexer.SetNotificationMask(this, m_descriptor, POLLIN);

  // trigger connected notification
  std::unique_lock lock(m_lock);
  OnConnected();
}

size_t StreamSocket::Read(void* buffer, size_t buffer_size)
{
  std::unique_lock lock(m_lock);
  if (!m_connected)
    return 0;

  // try a read
  const ssize_t len = recv(m_descriptor, static_cast<char*>(buffer), SIZE_CAST(buffer_size), 0);
  if (len <= 0)
  {
    // Check for EAGAIN
    if (len < 0 && WSAGetLastError() == WSAEWOULDBLOCK)
    {
      // Not an error. Just means no data is available.
      return 0;
    }

    // error
    CloseWithError();
    return 0;
  }

  return len;
}

size_t StreamSocket::Write(const void* buffer, size_t buffer_size)
{
  std::unique_lock lock(m_lock);
  if (!m_connected)
    return 0;

  // try a write
  const ssize_t len = send(m_descriptor, static_cast<const char*>(buffer), SIZE_CAST(buffer_size), 0);
  if (len <= 0)
  {
    // Check for EAGAIN
    if (len < 0 && WSAGetLastError() == WSAEWOULDBLOCK)
    {
      // Not an error. Just means no data is available.
      return 0;
    }

    // error
    CloseWithError();
    return 0;
  }

  return len;
}

size_t StreamSocket::WriteVector(const void** buffers, const size_t* buffer_lengths, size_t num_buffers)
{
  std::unique_lock lock(m_lock);
  if (!m_connected || num_buffers == 0)
    return 0;

#ifdef _WIN32

  WSABUF* bufs = static_cast<WSABUF*>(alloca(sizeof(WSABUF) * num_buffers));
  for (size_t i = 0; i < num_buffers; i++)
  {
    bufs[i].buf = (CHAR*)buffers[i];
    bufs[i].len = (ULONG)buffer_lengths[i];
  }

  DWORD bytesSent = 0;
  if (WSASend(m_descriptor, bufs, (DWORD)num_buffers, &bytesSent, 0, nullptr, nullptr) == SOCKET_ERROR)
  {
    if (WSAGetLastError() != WSAEWOULDBLOCK)
    {
      // Socket error.
      CloseWithError();
      return 0;
    }
  }

  return static_cast<size_t>(bytesSent);

#else // _WIN32

  iovec* bufs = static_cast<iovec*>(alloca(sizeof(iovec) * num_buffers));
  for (size_t i = 0; i < num_buffers; i++)
  {
    bufs[i].iov_base = (void*)buffers[i];
    bufs[i].iov_len = buffer_lengths[i];
  }

  ssize_t res = writev(m_descriptor, bufs, num_buffers);
  if (res < 0)
  {
    if (errno != EAGAIN)
    {
      // Socket error.
      CloseWithError();
      return 0;
    }

    res = 0;
  }

  return static_cast<size_t>(res);

#endif
}

bool StreamSocket::SetNagleBuffering(bool enabled, Error* error /* = nullptr */)
{
  if (!m_local_address.IsIPAddress())
  {
    Error::SetStringView(error, "Attempting to disable nagle on a non-IP socket.");
    return false;
  }

  int disable = enabled ? 0 : 1;
  if (setsockopt(m_descriptor, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&disable), sizeof(disable)) != 0)
  {
    Error::SetSocket(error, "setsockopt(TCP_NODELAY) failed: ", WSAGetLastError());
    return false;
  }

  return true;
}

void StreamSocket::Close()
{
  std::unique_lock lock(m_lock);
  if (!m_connected)
    return;

  m_multiplexer.SetNotificationMask(this, m_descriptor, 0);
  m_multiplexer.RemoveClientSocket(this);
  shutdown(m_descriptor, SD_BOTH);
  closesocket(m_descriptor);
  m_descriptor = INVALID_SOCKET;
  m_connected = false;

  OnDisconnected(Error::CreateString("Connection explicitly closed."));
}

void StreamSocket::CloseWithError()
{
  std::unique_lock lock(m_lock);
  DebugAssert(m_connected);

  Error error;
  const int error_code = WSAGetLastError();
  if (error_code == 0)
    error.SetStringView("Connection closed by peer.");
  else
    error.SetSocket(error_code);

  m_multiplexer.SetNotificationMask(this, m_descriptor, 0);
  m_multiplexer.RemoveClientSocket(this);
  closesocket(m_descriptor);
  m_descriptor = INVALID_SOCKET;
  m_connected = false;

  OnDisconnected(error);
}

void StreamSocket::OnReadEvent()
{
  // forward through
  std::unique_lock lock(m_lock);
  if (m_connected)
    OnRead();
}

void StreamSocket::OnWriteEvent()
{
  // shouldn't be called
}

void StreamSocket::OnHangupEvent()
{
  std::unique_lock lock(m_lock);
  if (!m_connected)
    return;

  m_multiplexer.SetNotificationMask(this, m_descriptor, 0);
  m_multiplexer.RemoveClientSocket(this);
  closesocket(m_descriptor);
  m_descriptor = INVALID_SOCKET;
  m_connected = false;

  OnDisconnected(Error::CreateString("Connection closed by peer."));
}

BufferedStreamSocket::BufferedStreamSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor,
                                           size_t receive_buffer_size /* = 16384 */,
                                           size_t send_buffer_size /* = 16384 */)
  : StreamSocket(multiplexer, descriptor), m_receive_buffer(receive_buffer_size), m_send_buffer(send_buffer_size)
{
}

BufferedStreamSocket::~BufferedStreamSocket()
{
}

std::unique_lock<std::recursive_mutex> BufferedStreamSocket::GetLock()
{
  return std::unique_lock(m_lock);
}

std::span<const u8> BufferedStreamSocket::AcquireReadBuffer() const
{
  return std::span<const u8>(m_receive_buffer.data() + m_receive_buffer_offset, m_receive_buffer_size);
}

void BufferedStreamSocket::ReleaseReadBuffer(size_t bytes_consumed)
{
  DebugAssert(bytes_consumed <= m_receive_buffer_size);
  m_receive_buffer_offset += static_cast<u32>(bytes_consumed);
  m_receive_buffer_size -= static_cast<u32>(bytes_consumed);

  // Anything left? If not, reset offset.
  m_receive_buffer_offset = (m_receive_buffer_size == 0) ? 0 : m_receive_buffer_offset;
}

std::span<u8> BufferedStreamSocket::AcquireWriteBuffer(size_t wanted_bytes, bool allow_smaller /* = false */)
{
  if (!m_connected)
    return {};

  // If to get the desired space, we need to move backwards, do so.
  if ((m_send_buffer_offset + m_send_buffer_size + wanted_bytes) > m_send_buffer.size())
  {
    if ((m_send_buffer_size + wanted_bytes) > m_send_buffer.size() && !allow_smaller)
    {
      // Not enough space.
      return {};
    }

    // Shuffle buffer backwards.
    std::memmove(m_send_buffer.data(), m_send_buffer.data() + m_send_buffer_offset, m_send_buffer_size);
    m_send_buffer_offset = 0;
  }

  DebugAssert((m_send_buffer_offset + m_send_buffer_size + wanted_bytes) <= m_send_buffer.size());
  return std::span<u8>(m_send_buffer.data() + m_send_buffer_offset + m_send_buffer_size,
                       m_send_buffer.size() - m_send_buffer_offset - m_send_buffer_size);
}

void BufferedStreamSocket::ReleaseWriteBuffer(size_t bytes_written, bool commit /* = true */)
{
  if (!m_connected)
    return;

  DebugAssert((m_send_buffer_offset + m_send_buffer_size + bytes_written) <= m_send_buffer.size());
  m_send_buffer_size += static_cast<u32>(bytes_written);

  // Send as much as we can.
  if (commit && m_send_buffer_size > 0)
  {
    const ssize_t res = send(m_descriptor, reinterpret_cast<const char*>(m_send_buffer.data() + m_send_buffer_offset),
                             SIZE_CAST(m_send_buffer_size), 0);
    if (res < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
    {
      CloseWithError();
      return;
    }

    m_send_buffer_offset += static_cast<size_t>(res);
    m_send_buffer_size -= static_cast<size_t>(res);
    if (m_send_buffer_size == 0)
    {
      m_send_buffer_offset = 0;
    }
    else
    {
      // Register for writes to finish it off.
      m_multiplexer.SetNotificationMask(this, m_descriptor, POLLIN | POLLOUT);
    }
  }
}

size_t BufferedStreamSocket::Read(void* buffer, size_t buffer_size)
{
  // Read from receive buffer.
  const std::span<const u8> rdbuf = AcquireReadBuffer();
  if (rdbuf.empty())
    return 0;

  const size_t bytes_to_read = std::min(rdbuf.size(), buffer_size);
  std::memcpy(buffer, rdbuf.data(), bytes_to_read);
  ReleaseReadBuffer(bytes_to_read);
  return bytes_to_read;
}

size_t BufferedStreamSocket::Write(const void* buffer, size_t buffer_size)
{
  if (!m_connected)
    return 0;

  // Read from receive buffer.
  const std::span<u8> wrbuf = AcquireWriteBuffer(buffer_size, true);
  if (wrbuf.empty())
    return 0;

  const size_t bytes_to_write = std::min(wrbuf.size(), buffer_size);
  std::memcpy(wrbuf.data(), buffer, bytes_to_write);
  ReleaseWriteBuffer(bytes_to_write);
  return bytes_to_write;
}

size_t BufferedStreamSocket::WriteVector(const void** buffers, const size_t* buffer_lengths, size_t num_buffers)
{
  if (!m_connected || num_buffers == 0)
    return 0;

  size_t total_size = 0;
  for (size_t i = 0; i < num_buffers; i++)
    total_size += buffer_lengths[i];

  const std::span<u8> wrbuf = AcquireWriteBuffer(total_size, true);
  if (wrbuf.empty())
    return 0;

  size_t written_bytes = 0;
  for (size_t i = 0; i < num_buffers; i++)
  {
    const size_t bytes_to_write = std::min(wrbuf.size() - written_bytes, buffer_lengths[i]);
    if (bytes_to_write == 0)
      break;

    std::memcpy(&wrbuf[written_bytes], buffers[i], bytes_to_write);
    written_bytes += buffer_lengths[i];
  }

  return written_bytes;
}

void BufferedStreamSocket::Close()
{
  StreamSocket::Close();

  m_receive_buffer_offset = 0;
  m_receive_buffer_size = 0;
  m_send_buffer_offset = 0;
  m_send_buffer_size = 0;
}

void BufferedStreamSocket::OnReadEvent()
{
  std::unique_lock lock(m_lock);
  if (!m_connected)
    return;

  // Pull as many bytes as possible into the read buffer.
  for (;;)
  {
    const size_t buffer_space = m_receive_buffer.size() - m_receive_buffer_offset - m_receive_buffer_size;
    if (buffer_space == 0) [[unlikely]]
    {
      // If we're here again, it means OnRead() didn't consume the data, and we overflowed.
      ERROR_LOG("Receive buffer overflow, dropping client {}.", GetRemoteAddress().ToString());
      CloseWithError();
      return;
    }

    const ssize_t res = recv(
      m_descriptor, reinterpret_cast<char*>(m_receive_buffer.data() + m_receive_buffer_offset + m_receive_buffer_size),
      SIZE_CAST(buffer_space), 0);
    if (res <= 0 && WSAGetLastError() != WSAEWOULDBLOCK)
    {
      CloseWithError();
      return;
    }

    m_receive_buffer_size += static_cast<size_t>(res);
    OnRead();

    // Are we at the end?
    if ((m_receive_buffer_offset + m_receive_buffer_size) == m_receive_buffer.size())
    {
      // Try to claw back some of the buffer, and try reading again.
      if (m_receive_buffer_offset > 0)
      {
        std::memmove(m_receive_buffer.data(), m_receive_buffer.data() + m_receive_buffer_offset, m_receive_buffer_size);
        m_receive_buffer_offset = 0;
        continue;
      }
    }

    break;
  }
}

void BufferedStreamSocket::OnWriteEvent()
{
  std::unique_lock lock(m_lock);
  if (!m_connected)
    return;

  // Send as much as we can.
  if (m_send_buffer_size > 0)
  {
    const ssize_t res = send(m_descriptor, reinterpret_cast<const char*>(m_send_buffer.data() + m_send_buffer_offset),
                             SIZE_CAST(m_send_buffer_size), 0);
    if (res < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
    {
      CloseWithError();
      return;
    }

    m_send_buffer_offset += static_cast<size_t>(res);
    m_send_buffer_size -= static_cast<size_t>(res);
    if (m_send_buffer_size == 0)
      m_send_buffer_offset = 0;
  }

  OnWrite();

  if (m_send_buffer_size == 0)
  {
    // Are we done? Switch back to reads only.
    m_multiplexer.SetNotificationMask(this, m_descriptor, POLLIN);
  }
}

void BufferedStreamSocket::OnWrite()
{
}
