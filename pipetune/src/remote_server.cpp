/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "remote_server.h"
#include <libsoup/soup.h>
#include <sys/random.h>
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <cstdio>
#include <fstream>
#include <set>
#include <vector>

namespace pipetune {

struct RemotePeer {
  std::shared_ptr<SoupWebsocketConnection> socket;
  cardio::cancellation_source cancellation;
};

struct RemoteServer::Impl {
  RemoteEditor editor;
  SoupServer *server = nullptr;
  std::filesystem::path root;
  std::set<std::string, std::less<>> files;
  std::string origin, authority, token;
  std::map<SoupWebsocketConnection *, std::shared_ptr<RemotePeer>> peers;
  std::vector<cardio::promise<void>> operations;
  std::uint64_t broadcastRevision = 0;
  GSource *broadcast = nullptr;
};

RemoteServer::RemoteServer(RemoteEditor::Read read, RemoteEditor::Apply apply)
    : impl_(std::make_unique<Impl>()) {
  impl_->editor.read = std::move(read);
  impl_->editor.apply = std::move(apply);
}

static std::string newToken() {
  std::array<unsigned char, 24> bytes{};
  for (std::size_t offset = 0; offset < bytes.size();) {
    const auto size = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
    if (size < 0 && errno == EINTR) continue;
    if (size <= 0) throw std::runtime_error("cannot generate the pairing token");
    offset += static_cast<std::size_t>(size);
  }
  auto token = std::string{};
  constexpr auto digits = "0123456789abcdef";
  for (auto byte : bytes) { token += digits[byte >> 4]; token += digits[byte & 15]; }
  return token;
}

static void closePeers(RemoteServer::Impl &impl, unsigned code) {
  auto peers = std::exchange(impl.peers, {});
  for (const auto &[socket, peer] : peers) {
    g_signal_handlers_disconnect_by_data(socket, &impl);
    peer->cancellation.cancel();
    if (soup_websocket_connection_get_state(socket) == SOUP_WEBSOCKET_STATE_OPEN)
      soup_websocket_connection_close(socket, code, nullptr);
  }
}

static void stop(RemoteServer::Impl &impl) {
  closePeers(impl, 1001);
  if (impl.broadcast) {
    g_source_destroy(impl.broadcast);
    g_source_unref(std::exchange(impl.broadcast, nullptr));
  }
  if (impl.server) {
    soup_server_disconnect(impl.server);
    g_clear_object(&impl.server);
  }
  impl.token.clear();
}

RemoteServer::~RemoteServer() { stop(*impl_); }

cardio::promise<void> RemoteServer::shutdown() {
  stop(*impl_);
  for (auto &operation : impl_->operations) co_await operation;
  impl_->operations.clear();
}

static void publish(RemoteServer::Impl &impl) {
  if (impl.peers.empty()) return;
  impl.editor.refresh();
  if (impl.broadcast || impl.editor.revision == impl.broadcastRevision) return;
  impl.broadcast = g_timeout_source_new(100);
  g_source_set_callback(impl.broadcast, [](void *data) -> gboolean {
    auto &source = *static_cast<RemoteServer::Impl *>(data);
    g_source_unref(std::exchange(source.broadcast, nullptr));
    source.broadcastRevision = source.editor.revision;
    for (const auto &[socket, peer] : source.peers)
      if (soup_websocket_connection_get_state(socket) == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_send_text(socket, source.editor.lastState.c_str());
    return G_SOURCE_REMOVE;
  }, &impl, nullptr);
  g_source_attach(impl.broadcast, g_main_context_get_thread_default());
}

bool RemoteServer::hasPeers() const noexcept { return !impl_->peers.empty(); }

void RemoteServer::changed() { publish(*impl_); }

static cardio::promise<void> dispatch(RemoteServer::Impl &impl, std::shared_ptr<RemotePeer> peer, std::string message) {
  auto reply = std::move(co_await impl.editor.request(std::move(message), peer->cancellation.get_cancellation()));
  auto *socket = peer->socket.get();
  if (!peer->cancellation.get_cancellation().is_cancellation_requested() &&
      soup_websocket_connection_get_state(socket) == SOUP_WEBSOCKET_STATE_OPEN) {
    if (!reply.ack.empty()) soup_websocket_connection_send_text(socket, reply.ack.c_str());
    if (!reply.data.empty()) soup_websocket_connection_send_text(socket, reply.data.c_str());
  }
  publish(impl);
}

static void messageReceived(SoupWebsocketConnection *socket, SoupWebsocketDataType type, GBytes *bytes, gpointer data) {
  auto &impl = *static_cast<RemoteServer::Impl *>(data);
  const auto it = impl.peers.find(socket);
  if (it == impl.peers.end()) return;
  if (type != SOUP_WEBSOCKET_DATA_TEXT) {
    soup_websocket_connection_close(socket, 1003, nullptr);
    return;
  }
  std::erase_if(impl.operations, [](const auto &op) { return op.try_result(); });
  if (impl.operations.size() >= 64) {
    soup_websocket_connection_close(socket, 1013, nullptr);
    return;
  }
  gsize length = 0;
  const auto *text = static_cast<const char *>(g_bytes_get_data(bytes, &length));
  impl.operations.push_back(dispatch(impl, it->second, std::string(text, length)));
}

static void connected(SoupServer *, SoupServerMessage *message, const char *, SoupWebsocketConnection *socket, gpointer data) {
  auto &impl = *static_cast<RemoteServer::Impl *>(data);
  const auto *query = g_uri_get_query(soup_server_message_get_uri(message));
  const auto values = std::unique_ptr<GHashTable, decltype(&g_hash_table_unref)>(
      query ? soup_form_decode(query) : nullptr, g_hash_table_unref);
  const auto *token = values ? static_cast<const char *>(g_hash_table_lookup(values.get(), "t")) : nullptr;
  if (!token || impl.token != token) {
    soup_websocket_connection_close(socket, 4401, nullptr);
    return;
  }
  if (impl.peers.size() >= 16) {
    soup_websocket_connection_close(socket, 1013, nullptr);
    return;
  }
  soup_websocket_connection_set_max_incoming_payload_size(socket, 4 * 1024 * 1024);
  soup_websocket_connection_set_keepalive_interval(socket, 15);
  auto peer = std::make_shared<RemotePeer>();
  peer->socket = {static_cast<SoupWebsocketConnection *>(g_object_ref(socket)), g_object_unref};
  impl.peers.emplace(socket, peer);
  g_signal_connect(socket, "message", G_CALLBACK(messageReceived), data);
  g_signal_connect(socket, "closed", G_CALLBACK(+[](SoupWebsocketConnection *closed, gpointer owner) {
    auto &self = *static_cast<RemoteServer::Impl *>(owner);
    const auto it = self.peers.find(closed);
    if (it != self.peers.end()) {
      it->second->cancellation.cancel();
      self.peers.erase(it);
    }
  }), data);
}

static const char *contentType(const std::filesystem::path &path) {
  static const std::map<std::string, const char *> types = {
    {".html","text/html"}, {".js","text/javascript"}, {".mjs","text/javascript"}, {".css","text/css"},
    {".json","application/json"}, {".json5","text/plain"}, {".txt","text/plain"}, {".svg","image/svg+xml"},
    {".png","image/png"}, {".jpg","image/jpeg"}, {".jpeg","image/jpeg"}, {".ico","image/x-icon"},
    {".wasm","application/wasm"}, {".woff2","font/woff2"}, {".effetune_preset","application/json"}};
  const auto it = types.find(path.extension());
  return it == types.end() ? "application/octet-stream" : it->second;
}

static void handleHttp(SoupServer *, SoupServerMessage *message, const char *path, GHashTable *, gpointer data) {
  auto &impl = *static_cast<RemoteServer::Impl *>(data);
  auto *headers = soup_server_message_get_request_headers(message);
  const auto *host = soup_message_headers_get_one(headers, "Host");
  const auto *origin = soup_message_headers_get_one(headers, "Origin");
  if (!host || impl.authority != host || (origin && impl.origin != origin)) {
    soup_server_message_set_status(message, 403, nullptr);
    return;
  }
  if (soup_message_headers_header_contains(headers, "Upgrade", "websocket")) {
    if (std::string_view(path) != "/") soup_server_message_set_status(message, 404, nullptr);
    return;
  }
  const auto method = std::string_view(soup_server_message_get_method(message));
  if (method != "GET" && method != "HEAD") {
    soup_server_message_set_status(message, 405, nullptr);
    return;
  }
  const auto url = std::string_view(path);
  auto relative = std::filesystem::path(url == "/" ? "remote.html" : url.substr(1));
  if (relative.empty() || relative.is_absolute() ||
      std::ranges::any_of(relative, [](const auto &part) { return part.empty() || part.native().front() == '.'; })) {
    soup_server_message_set_status(message, 404, nullptr);
    return;
  }
  if (!impl.files.contains(relative.native())) {
    soup_server_message_set_status(message, 404, nullptr);
    return;
  }
  const auto file = impl.root / relative;
  auto *mapping = g_mapped_file_new(file.c_str(), false, nullptr);
  if (!mapping) {
    soup_server_message_set_status(message, 404, nullptr);
    return;
  }
  auto *response = soup_server_message_get_response_headers(message);
  soup_message_headers_replace(response, "Referrer-Policy", "no-referrer");
  soup_message_headers_replace(response, "X-Content-Type-Options", "nosniff");
  soup_message_headers_replace(response, "Cache-Control", "no-cache");
  if (relative == "remote.html") {
    auto policy = std::string("connect-src 'self' ") + (impl.origin.starts_with("https:") ? "wss://" : "ws://") + impl.authority;
    soup_message_headers_replace(response, "Content-Security-Policy", policy.c_str());
  }
  auto *bytes = g_mapped_file_get_bytes(mapping);
  soup_message_headers_set_content_type(response, contentType(relative), nullptr);
  soup_message_headers_set_content_length(response, g_bytes_get_size(bytes));
  if (method == "GET") soup_message_body_append_bytes(soup_server_message_get_response_body(message), bytes);
  soup_server_message_set_status(message, 200, nullptr);
  g_bytes_unref(bytes);
  g_mapped_file_unref(mapping);
}

std::string RemoteServer::configure(const RemoteListenerOptions &options) {
  auto &impl = *impl_;
  if (options.operation == "disable") { stop(impl); return "{\"ok\":true}"; }
  if (options.operation == "enable") {
    if (impl.server) return makeControlErrorResponse("remote editor already enabled");
    const auto address = std::unique_ptr<GInetAddress, decltype(&g_object_unref)>(
        g_inet_address_new_from_string(options.address.c_str()), g_object_unref);
    if (!address || g_inet_address_get_family(address.get()) != G_SOCKET_FAMILY_IPV4 ||
        g_inet_address_get_is_any(address.get()) || g_inet_address_get_is_multicast(address.get()))
      return makeControlErrorResponse("specify the IPv4 address of a local interface");
    impl.root = remoteEditorDirectory();
    std::ifstream manifest(impl.root / "remote-files.txt");
    impl.files.clear();
    for (std::string name; std::getline(manifest, name);) impl.files.insert(std::move(name));
    if (!impl.files.contains("remote.html")) return makeControlErrorResponse("remote editor assets are unavailable");
    const auto token = newToken();
    const auto listener = std::unique_ptr<SoupServer, decltype(&g_object_unref)>(soup_server_new(nullptr, nullptr), g_object_unref);
    GError *error = nullptr;
    const auto secure = !options.certificate.empty();
    if (secure) {
      auto *cert = g_tls_certificate_new_from_files(options.certificate.c_str(), options.key.c_str(), &error);
      if (cert) { soup_server_set_tls_certificate(listener.get(), cert); g_object_unref(cert); }
    }
    const auto endpoint = std::unique_ptr<GSocketAddress, decltype(&g_object_unref)>(
        g_inet_socket_address_new(address.get(), options.port), g_object_unref);
    if (error || !soup_server_listen(listener.get(), endpoint.get(), secure ? SOUP_SERVER_LISTEN_HTTPS : SoupServerListenOptions{}, &error)) {
      const auto failure = makeControlErrorResponse(error ? error->message : "cannot listen");
      g_clear_error(&error);
      return failure;
    }
    auto *uris = soup_server_get_uris(listener.get());
    const auto port = g_uri_get_port(static_cast<GUri *>(uris->data));
    g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));
    impl.authority = options.address + ((secure && port == 443) || (!secure && port == 80) ? "" : ":" + std::to_string(port));
    impl.origin = (secure ? "https://" : "http://") + impl.authority;
    impl.token = token;
    impl.server = static_cast<SoupServer *>(g_object_ref(listener.get()));
    soup_server_add_handler(impl.server, nullptr, handleHttp, &impl, nullptr);
    soup_server_add_websocket_handler(impl.server, "/", nullptr, nullptr, connected, &impl, nullptr);
  }
  if (!impl.server) return makeControlErrorResponse("remote editor is disabled");
  if (options.operation == "rotate") {
    impl.token = newToken();
    closePeers(impl, 4401);
  }
  return "{\"ok\":true,\"link\":\"" + impl.origin + "/?t=" + impl.token + "\"}";
}
}
