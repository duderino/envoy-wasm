#pragma once

#include <future>

#include "envoy/api/api.h"
#include "envoy/event/dispatcher.h"
#include "envoy/http/codec.h"
#include "envoy/network/address.h"
#include "envoy/thread/thread.h"

#include "common/api/api_impl.h"
#include "common/common/thread.h"
#include "common/http/codec_client.h"
#include "common/network/raw_buffer_socket.h"
#include "common/stats/isolated_store_impl.h"

#include "test/test_common/test_time.h"
#include "test/test_common/utility.h"

#include "fmt/printf.h"
#include "stress_test_common.h"

namespace Envoy {
namespace Stress {

class ClientStream;
class ClientConnection;
class Client;
typedef std::unique_ptr<ClientStream> ClientStreamPtr;
typedef std::shared_ptr<ClientStream> ClientStreamSharedPtr;
typedef std::unique_ptr<ClientConnection> ClientConnectionPtr;
typedef std::shared_ptr<ClientConnection> ClientConnectionSharedPtr;
typedef std::unique_ptr<Client> ClientPtr;
typedef std::shared_ptr<Client> ClientSharedPtr;

enum class ClientConnectionState {
  CONNECTED, // Connection established. Non-Terminal. Will be followed by one
             // of the codes below.
  IDLE,      // Connection has no active streams. Non-Terminal. Close it, use it,
             // or put it in a pool.
};

enum class ClientCloseReason {
  CONNECT_FAILED, // Connection could not be established
  REMOTE_CLOSE,   // Peer closed or connection was reset after it was
                  // established.
  LOCAL_CLOSE     // This process decided to close the connection.
};

enum class ClientCallbackResult {
  CONTINUE, // Leave the connection open
  CLOSE     // Close the connection.
};

/**
 * Handle a non-terminal connection event asynchronously.
 *
 * @param connection The connection with the event
 * @param state The state of the connection (connected or idle).
 */
typedef std::function<ClientCallbackResult(ClientConnection& connection,
                                           ClientConnectionState state)>
    ClientConnectCallback;

/**
 * Handle a terminal connection close event asynchronously.
 *
 * @param connection The connection that was closed
 * @param reason The reason the connection was closed
 */
typedef std::function<void(ClientConnection& connection, ClientCloseReason reason)>
    ClientCloseCallback;

/**
 * Handle a response asynchronously.
 *
 * @param connection The connection that received the response.
 * @param response_headers The response headers or null if timed out.
 * @param latency Latency in nanoseconds of the response... or timeout.
 */
typedef std::function<void(ClientConnection& connection, Http::HeaderMapPtr&& response_headers,
                           std::chrono::microseconds latency)>
    ClientResponseCallback;

class ClientConnection : public Network::ConnectionCallbacks,
                         public Http::ConnectionCallbacks,
                         public Event::DeferredDeletable,
                         protected Logger::Loggable<Logger::Id::testing> {
public:
  ClientConnection(Client& client, uint32_t id, ClientConnectCallback& connect_callback,
                   ClientCloseCallback& close_callback, TimeSource& time_source,
                   std::shared_ptr<Event::Dispatcher>& dispatcher);
  ClientConnection(const ClientConnection&) = delete;

  ClientConnection& operator=(const ClientConnection&) = delete;
  ~ClientConnection() override;

  const std::string& name() const;

  uint32_t id() const;

  virtual Network::ClientConnection& networkConnection() PURE;

  virtual Http::ClientConnection& httpConnection() PURE;

  Event::Dispatcher& dispatcher();

  /**
   * Asynchronously send a request. On HTTP1.1 connections at most one request
   * can be outstanding on a connection. For HTTP2 multiple requests may
   * outstanding.
   *
   * @param request_headers
   * @param callback
   */
  virtual void sendRequest(const Http::HeaderMap& request_headers, ClientResponseCallback& callback,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(5'000));

  /**
   * For internal use
   *
   * @param stream_id
   */
  void removeStream(uint32_t stream_id);

  //
  // Network::ConnectionCallbacks
  //

  void onEvent(Network::ConnectionEvent event) override;

  void onAboveWriteBufferHighWatermark() override;

  void onBelowWriteBufferLowWatermark() override;

  //
  // Http::ConnectionCallbacks
  //

  void onGoAway() override;

private:
  ClientStream& newStream(ClientResponseCallback& callback);

  Client& client_;
  uint32_t id_;
  ClientConnectCallback& connect_callback_;
  ClientCloseCallback& close_callback_;
  TimeSource& time_source_;
  std::shared_ptr<Event::Dispatcher> dispatcher_;
  bool established_{false};

  std::mutex streams_lock_;
  std::unordered_map<uint32_t, ClientStreamPtr> streams_;
  std::atomic<uint32_t> stream_counter_{0U};
};

class Client : Logger::Loggable<Logger::Id::testing> {
public:
  explicit Client(const std::string& name);
  Client(const Client&) = delete;

  Client& operator=(const Client&) = delete;
  virtual ~Client();

  const std::string& name() const;

  /**
   * Start the client's dispatcher in a background thread. This is a noop if
   * the client has already been started. This will block until the dispatcher
   * is running on another thread.
   */
  void start();

  /**
   * Execute something on the client's dispatcher's background thread.
   *
   * @param callback The callback to execute
   */
  void post(std::function<void()> callback);

  /**
   * Stop the client's dispatcher and join the background thread. This will
   * block until the background thread exits.
   */
  void stop();

  /**
   * For internal use
   */
  void releaseConnection(uint32_t id);

  /**
   * For internal use
   */
  void releaseConnection(ClientConnection& connection);

  /**
   * Asynchronously connect to a peer. The connect_callback will be called on
   * successful connection establishment and also on idle state, giving the
   * caller the opportunity to reuse or close connections. The close_callback
   * will be called after the connection is closed, giving the caller the
   * opportunity to cleanup additional resources, etc.
   */
  void connect(Network::TransportSocketFactory& socket_factory,
               Http::CodecClient::Type http_version,
               Network::Address::InstanceConstSharedPtr& address,
               const Network::ConnectionSocket::OptionsSharedPtr& sockopts,
               ClientConnectCallback& connect_callback, ClientCloseCallback& close_callback);

  Stats::Store& store();

private:
  std::atomic<bool> is_running_{false};
  std::string name_;
  Stats::IsolatedStoreImpl stats_store_;
  Thread::ThreadPtr thread_;
  Event::TestRealTimeSystem time_system_;
  Api::Impl api_;
  std::shared_ptr<Event::Dispatcher> dispatcher_;

  std::mutex connections_lock_;
  std::unordered_map<uint32_t, ClientConnectionPtr> connections_;
  uint32_t connection_counter_{0U};
};

#define ALL_LOAD_GENERATOR_STATS(COUNTER, HISTOGRAM)                                               \
  COUNTER(class_2xx)                                                                               \
  COUNTER(class_3xx)                                                                               \
  COUNTER(class_4xx)                                                                               \
  COUNTER(class_5xx)                                                                               \
  COUNTER(connect_failures)                                                                        \
  COUNTER(connect_successes)                                                                       \
  COUNTER(responses_received)                                                                      \
  COUNTER(response_timeouts)                                                                       \
  COUNTER(local_closes)                                                                            \
  COUNTER(remote_closes)                                                                           \
  HISTOGRAM(class_2xx_latency_usec)

class LoadGenerator : Logger::Loggable<Logger::Id::testing> {
public:
  struct Stats {
    ALL_LOAD_GENERATOR_STATS(GENERATE_COUNTER_STRUCT, GENERATE_HISTOGRAM_STRUCT)
  };

  /**
   *  A wrapper around Client and its callbacks that implements a simple load
   * generator.
   *
   * @param socket_factory Socket factory (use for plain TCP vs. TLS)
   * @param http_version HTTP version (h1 vs h2)
   * @param address Address (ip addr, port, ip protocol version) to connect to
   * @param sockopts Socket options for the client sockets. Use default if
   * null.
   */
  LoadGenerator(Client& client, Network::TransportSocketFactory& socket_factory,
                Http::CodecClient::Type http_version,
                Network::Address::InstanceConstSharedPtr& address,
                const Network::ConnectionSocket::OptionsSharedPtr& sockopts = nullptr);
  LoadGenerator(const LoadGenerator&) = delete;
  void operator=(const LoadGenerator&) = delete;
  virtual ~LoadGenerator() = default;

  /**
   * Generate load and block until all connections have finished (successfully
   * or otherwise).
   *
   * @param connections Connections to create
   * @param requests Total requests across all connections to send
   * @param request The request to send
   * @param timeout The time in msec to wait to receive a response after sending
   * each request.
   */
  void run(uint32_t connections, uint32_t requests, Http::HeaderMapPtr&& request,
           std::chrono::milliseconds timeout = std::chrono::milliseconds(5'000));

  uint32_t connectionsEstablished() const;

  /**
   * Access the metrics maintained by the load generator.
   *
   * @return The load generator's metrics
   */
  const Stats& stats() const;

private:
  uint32_t connections_to_initiate_{0};
  uint32_t requests_to_send_{0};
  Http::HeaderMapPtr request_;
  Client& client_;
  Network::TransportSocketFactory& socket_factory_;
  Http::CodecClient::Type http_version_;
  Network::Address::InstanceConstSharedPtr address_;
  const Network::ConnectionSocket::OptionsSharedPtr sockopts_;

  ClientConnectCallback connect_callback_;
  ClientResponseCallback response_callback_;
  ClientCloseCallback close_callback_;
  std::chrono::milliseconds timeout_{std::chrono::milliseconds(0)};
  // Counters needed for control flow are not implemented as stats.
  std::atomic<int32_t> requests_remaining_{0};
  std::atomic<uint32_t> connections_established_{0};
  std::atomic<uint32_t> connections_closed_{0};
  std::unique_ptr<Stats> stats_;
  std::promise<bool> promise_all_connections_closed_;
};

typedef std::unique_ptr<LoadGenerator> LoadGeneratorPtr;

} // namespace Stress
} // namespace Envoy