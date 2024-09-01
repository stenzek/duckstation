// SPDX-FileCopyrightText: 2015-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/error.h"
#include "common/heap_array.h"
#include "common/small_string.h"
#include "common/threading.h"
#include "common/types.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>

#ifdef _WIN32
using SocketDescriptor = uintptr_t;
#else
using SocketDescriptor = int;
#endif

struct pollfd;

class BaseSocket;
class ListenSocket;
class StreamSocket;
class BufferedStreamSocket;
class SocketMultiplexer;

struct SocketAddress final
{
  enum class Type
  {
    Unknown,
    IPv4,
    IPv6,
    Unix,
  };

  // accessors
  const void* GetData() const { return m_data; }
  u32 GetLength() const { return m_length; }

  // parse interface
  static std::optional<SocketAddress> Parse(Type type, const char* address, u32 port, Error* error);

  // resolve interface
  static std::optional<SocketAddress> Resolve(const char* address, u32 port, Error* error);

  // to string interface
  SmallString ToString() const;

  // initializers
  void SetFromSockaddr(const void* sa, size_t length);

  /// Returns true if the address is IP.
  bool IsIPAddress() const;

private:
  u8 m_data[128] = {};
  u32 m_length = 0;
};

class BaseSocket : public std::enable_shared_from_this<BaseSocket>
{
  friend SocketMultiplexer;

public:
  BaseSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor);
  virtual ~BaseSocket();

  ALWAYS_INLINE SocketDescriptor GetDescriptor() const { return m_descriptor; }

  virtual void Close() = 0;

protected:
  virtual void OnReadEvent() = 0;
  virtual void OnWriteEvent() = 0;
  virtual void OnHangupEvent() = 0;

  SocketMultiplexer& m_multiplexer;
  SocketDescriptor m_descriptor;
};

class SocketMultiplexer final
{
  // TODO: Re-introduce worker threads.

public:
  typedef std::shared_ptr<StreamSocket> (*CreateStreamSocketCallback)(SocketMultiplexer& multiplexer,
                                                                      SocketDescriptor descriptor);
  friend BaseSocket;
  friend ListenSocket;
  friend StreamSocket;
  friend BufferedStreamSocket;

public:
  ~SocketMultiplexer();

  // Factory method.
  static std::unique_ptr<SocketMultiplexer> Create(Error* error);

  // Public interface
  template<class T>
  std::shared_ptr<ListenSocket> CreateListenSocket(const SocketAddress& address, Error* error);
  template<class T>
  std::shared_ptr<T> ConnectStreamSocket(const SocketAddress& address, Error* error);

  // Returns true if any sockets are currently registered.
  bool HasAnyOpenSockets();

  // Returns true if any client sockets are currently connected.
  bool HasAnyClientSockets();

  // Returns the number of current client sockets.
  size_t GetClientSocketCount();

  // Close all sockets on this multiplexer.
  void CloseAll();

  // Poll for events. Returns false if there are no sockets registered.
  bool PollEventsWithTimeout(u32 milliseconds);

protected:
  // Internal interface
  std::shared_ptr<ListenSocket> InternalCreateListenSocket(const SocketAddress& address,
                                                           CreateStreamSocketCallback callback, Error* error);
  std::shared_ptr<StreamSocket> InternalConnectStreamSocket(const SocketAddress& address,
                                                            CreateStreamSocketCallback callback, Error* error);

private:
  // Hide the constructor.
  SocketMultiplexer();

  // Initialization.
  bool Initialize(Error* error);

  // Tracking of open sockets.
  void AddOpenSocket(std::shared_ptr<BaseSocket> socket);
  void AddClientSocket(std::shared_ptr<BaseSocket> socket);
  void RemoveOpenSocket(BaseSocket* socket);
  void RemoveClientSocket(BaseSocket* socket);

  // Register for notifications
  void SetNotificationMask(BaseSocket* socket, SocketDescriptor descriptor, u32 events);

private:
  // We store the fd in the struct to avoid the cache miss reading the object.
  using SocketMap = std::unordered_map<SocketDescriptor, std::shared_ptr<BaseSocket>>;

#ifdef __linux__
  int m_epoll_fd = -1;
#else
  std::mutex m_poll_array_lock;
  pollfd* m_poll_array = nullptr;
  size_t m_poll_array_active_size = 0;
  size_t m_poll_array_max_size = 0;
#endif

  std::mutex m_open_sockets_lock;
  SocketMap m_open_sockets;
  std::atomic_size_t m_client_socket_count{0};
};

template<class T>
std::shared_ptr<ListenSocket> SocketMultiplexer::CreateListenSocket(const SocketAddress& address, Error* error)
{
  const CreateStreamSocketCallback callback = [](SocketMultiplexer& multiplexer,
                                                 SocketDescriptor descriptor) -> std::shared_ptr<StreamSocket> {
    return std::static_pointer_cast<StreamSocket>(std::make_shared<T>(multiplexer, descriptor));
  };
  return InternalCreateListenSocket(address, callback, error);
}

template<class T>
std::shared_ptr<T> SocketMultiplexer::ConnectStreamSocket(const SocketAddress& address, Error* error)
{
  const CreateStreamSocketCallback callback = [](SocketMultiplexer& multiplexer,
                                                 SocketDescriptor descriptor) -> std::shared_ptr<StreamSocket> {
    return std::static_pointer_cast<StreamSocket>(std::make_shared<T>(multiplexer, descriptor));
  };
  return std::static_pointer_cast<T>(InternalConnectStreamSocket(address, callback, error));
}

class ListenSocket final : public BaseSocket
{
  friend SocketMultiplexer;

public:
  ListenSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor,
               SocketMultiplexer::CreateStreamSocketCallback accept_callback);
  virtual ~ListenSocket() override;

  const SocketAddress* GetLocalAddress() const { return &m_local_address; }
  u32 GetConnectionsAccepted() const { return m_num_connections_accepted; }

  void Close() override final;

protected:
  void OnReadEvent() override final;
  void OnWriteEvent() override final;
  void OnHangupEvent() override final;

private:
  SocketMultiplexer::CreateStreamSocketCallback m_accept_callback;
  SocketAddress m_local_address = {};
  u32 m_num_connections_accepted = 0;
};

class StreamSocket : public BaseSocket
{
public:
  StreamSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor);
  virtual ~StreamSocket() override;

  static u32 GetSocketProtocolForAddress(const SocketAddress& sa);

  virtual void Close() override;

  // Accessors
  const SocketAddress& GetLocalAddress() const { return m_local_address; }
  const SocketAddress& GetRemoteAddress() const { return m_remote_address; }
  bool IsConnected() const { return m_connected; }

  // Read/write
  size_t Read(void* buffer, size_t buffer_size);
  size_t Write(const void* buffer, size_t buffer_size);
  size_t WriteVector(const void** buffers, const size_t* buffer_lengths, size_t num_buffers);

  /// Disables Nagle's buffering algorithm, i.e. TCP_NODELAY.
  bool SetNagleBuffering(bool enabled, Error* error = nullptr);

protected:
  virtual void OnConnected() = 0;
  virtual void OnDisconnected(const Error& error) = 0;
  virtual void OnRead() = 0;

  virtual void OnReadEvent() override;
  virtual void OnWriteEvent() override;
  virtual void OnHangupEvent() override;

  void CloseWithError();

private:
  void InitialSetup();

  SocketAddress m_local_address = {};
  SocketAddress m_remote_address = {};
  std::recursive_mutex m_lock;
  bool m_connected = true;

  // Ugly, but needed in order to call the events.
  friend SocketMultiplexer;
  friend ListenSocket;
  friend BufferedStreamSocket;
};

class BufferedStreamSocket : public StreamSocket
{
public:
  BufferedStreamSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor, size_t receive_buffer_size = 16384,
                       size_t send_buffer_size = 16384);
  virtual ~BufferedStreamSocket() override;

  // Must hold the lock when not part of OnRead().
  std::unique_lock<std::recursive_mutex> GetLock();
  std::span<const u8> AcquireReadBuffer() const;
  void ReleaseReadBuffer(size_t bytes_consumed);
  std::span<u8> AcquireWriteBuffer(size_t wanted_bytes, bool allow_smaller = false);
  void ReleaseWriteBuffer(size_t bytes_written, bool commit = true);

  // Hide StreamSocket read/write methods.
  size_t Read(void* buffer, size_t buffer_size);
  size_t Write(const void* buffer, size_t buffer_size);
  size_t WriteVector(const void** buffers, const size_t* buffer_lengths, size_t num_buffers);
  virtual void Close() override;

protected:
  void OnReadEvent() override final;
  void OnWriteEvent() override final;
  virtual void OnWrite();

private:
  std::vector<u8> m_receive_buffer;
  size_t m_receive_buffer_offset = 0;
  size_t m_receive_buffer_size = 0;

  std::vector<u8> m_send_buffer;
  size_t m_send_buffer_offset = 0;
  size_t m_send_buffer_size = 0;
};
