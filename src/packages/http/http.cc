#include "base/package_api.h"

#include "packages/http/http.h"

#include <event2/dns.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/util.h>
#include <libwebsockets.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Mapping = std::unique_ptr<mapping_t, decltype(&free_mapping)>;
constexpr size_t kHeaderLimit = 32 * 1024;
constexpr unsigned kRedirectLimit = 5;

struct Url {
  std::string scheme, host, authority, path, value;
  int port = 0;
};

std::string lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  }
  return s;
}

bool clean_url(const std::string& s) {
  for (unsigned char c : s) {
    if (c <= 32 || c >= 127 || c == '\\') return false;
  }
  return true;
}

bool parse_url(const std::string& s, Url& out) {
  if (!clean_url(s) || s.size() > kHeaderLimit) return false;
  std::unique_ptr<evhttp_uri, decltype(&evhttp_uri_free)> uri(evhttp_uri_parse(s.c_str()),
                                                          evhttp_uri_free);
  if (!uri || !evhttp_uri_get_scheme(uri.get()) || !evhttp_uri_get_host(uri.get()) ||
      evhttp_uri_get_userinfo(uri.get())) return false;
  Url u;
  u.scheme = lower(evhttp_uri_get_scheme(uri.get()));
  if (u.scheme != "http" && u.scheme != "https") return false;
  u.host = lower(evhttp_uri_get_host(uri.get()));
  if (u.host.empty()) return false;
  // libevent preserves the brackets around an IPv6 literal.
  if (u.host.front() == '[' && u.host.back() == ']') {
    u.host = u.host.substr(1, u.host.size() - 2);
    unsigned char addr[16];
    if (evutil_inet_pton(AF_INET6, u.host.c_str(), addr) != 1) return false;
  } else {
    for (unsigned char c : u.host) {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'))
        return false;
    }
  }
  u.port = evhttp_uri_get_port(uri.get());
  if (u.port == -1) u.port = u.scheme == "https" ? 443 : 80;
  if (u.port < 1 || u.port > 65535) return false;
  u.authority = (u.host.find(':') == std::string::npos ? u.host : "[" + u.host + "]") +
                ":" + std::to_string(u.port);
  const char* path = evhttp_uri_get_path(uri.get());
  u.path = path && *path ? path : "/";
  if (u.path.front() != '/') return false;
  if (const char* q = evhttp_uri_get_query(uri.get())) u.path += std::string("?") + q;
  u.value = u.scheme + "://" + u.authority + u.path;
  out = std::move(u);
  return true;
}

std::string redirect_url(const Url& base, const std::string& location) {
  if (location.find("://") != std::string::npos) return location;
  if (location.compare(0, 2, "//") == 0) return base.scheme + ":" + location;
  std::string origin = base.scheme + "://" + base.authority;
  if (location.empty()) return base.value;
  if (location[0] == '/') return origin + location;
  std::string path = base.path.substr(0, base.path.find('?'));
  if (location[0] == '?') return origin + path + location;
  if (location[0] == '#') return base.value + location;
  path.resize(path.rfind('/') + 1);
  path += location;
  // Remove literal dot segments (RFC 3986); escaped segments stay escaped.
  std::string suffix;
  auto end = path.find_first_of("?#");
  if (end != std::string::npos) {
    suffix = path.substr(end);
    path.resize(end);
  }
  std::vector<std::string> parts;
  std::istringstream input(path.substr(1));
  std::string part;
  while (std::getline(input, part, '/')) {
    if (part == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (part != ".") parts.push_back(part);
  }
  std::string result = "/";
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) result += '/';
    result += parts[i];
  }
  if ((path.back() == '/' || part == "." || part == "..") && result.back() != '/') result += '/';
  return origin + result + suffix;
}

bool allowed(const Url& url, object_t* caller) {
  const char* setting = CONFIG_STR(__HTTP_ALLOWED_HOSTS__);
  std::string list = setting ? setting : "";
  std::replace(list.begin(), list.end(), ',', ' ');
  std::istringstream hosts(list);
  std::string host;
  bool found = false;
  while (hosts >> host) {
    if (lower(host) == url.authority) { found = true; break; }
  }
  if (!found || !caller || (caller->flags & O_DESTRUCTED)) return false;
  copy_and_push_string(url.value.c_str());
  push_object(caller);
  auto* answer = safe_apply_master_ob(APPLY_VALID_HTTP, 2);
  // master_approved deliberately allows bootstrap's -1; HTTP never does.
  return answer != reinterpret_cast<svalue_t*>(-1) &&
         master_approved(answer, "valid_http") && answer->type == T_NUMBER &&
         answer->u.number == 1 && !(caller->flags & O_DESTRUCTED);
}

struct Request {
  uint64_t id = 0;
  Url url;
  std::string method = "GET", body;
  std::map<std::string, std::string> headers, response_headers;
  std::string response, error, message, redirect;
  size_t max_body = 0, sent = 0;
  int timeout = 0, status = 0;
  unsigned redirects = 0;
  bool text = false, follow = false, has_body = false;
  bool started = false, resolved = false, finished = false, queued = false;
  bool eof_body = false, sent_headers = false, rearm_pending = false;
  std::vector<std::string> addresses;
  size_t next_address = 0;
  Clock::time_point begin = Clock::now();
  promise_t* promise = nullptr;
  object_t* caller = nullptr;
  Mapping fallback{nullptr, free_mapping};
  lws* wsi = nullptr;
  evdns_getaddrinfo_request* dns = nullptr;
  event* timer = nullptr;

  ~Request() {
    if (timer) event_free(timer);
    if (promise) free_promise(promise);
    if (caller) free_object(&caller, "http request");
  }
};

lws_context* http_context = nullptr;
evdns_base* resolver = nullptr;
event_base* event_loop = nullptr;
bool stopping = false;
uint64_t next_id = 0;
std::map<uint64_t, std::unique_ptr<Request>> requests;

void dispatch(uint64_t id);
void rearm(uint64_t id);
void queue(Request& r) {
  if (stopping || r.queued) return;
  r.queued = true;
  // No LPC, VM allocation, or refcount manipulation in a network callback.
  add_walltime_event(std::chrono::milliseconds(0), [id = r.id] { dispatch(id); });
}

void fail(Request& r, const char* code, const std::string& message) {
  if (r.finished && r.redirect.empty()) return;
  r.error = code;
  r.message = message;
  r.redirect.clear();
  r.finished = true;
  queue(r);
}

bool header_name(const std::string& name) {
  if (name.empty()) return false;
  for (unsigned char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          std::string("!#$%&'*+-.^_`|~").find(c) != std::string::npos)) return false;
  }
  return true;
}

std::string string_value(const svalue_t& v) {
  return std::string(v.u.string, SVALUE_STRLEN(&v));
}

void options(Request& r, mapping_t* opts) {
  r.timeout = CONFIG_INT(__HTTP_DEFAULT_TIMEOUT__);
  r.max_body = CONFIG_INT(__HTTP_MAX_BODY__);
  if (opts) {
    for (unsigned i = 0; i <= opts->table_size; ++i) {
      for (auto* node = opts->table[i]; node; node = node->next) {
        const auto& key = node->values[0];
        const auto& v = node->values[1];
        if (key.type != T_STRING) error("http_request: option keys must be strings.\n");
        std::string name = string_value(key);
        if (name == "method") {
          if (v.type != T_STRING) error("http_request: method must be a string.\n");
          r.method = string_value(v);
          if (r.method != "GET" && r.method != "POST" && r.method != "PUT" &&
              r.method != "DELETE" && r.method != "HEAD")
            error("http_request: unsupported method.\n");
        } else if (name == "body") {
          if (v.type == T_STRING) r.body = string_value(v);
          else if (v.type == T_BUFFER)
            r.body.assign(reinterpret_cast<const char*>(v.u.buf->item), v.u.buf->size);
          else error("http_request: body must be a string or buffer.\n");
          r.has_body = true;
        } else if (name == "headers") {
          if (v.type != T_MAPPING) error("http_request: headers must be a mapping.\n");
          size_t total = 0;
          for (unsigned j = 0; j <= v.u.map->table_size; ++j) {
            for (auto* h = v.u.map->table[j]; h; h = h->next) {
              if (h->values[0].type != T_STRING || h->values[1].type != T_STRING)
                error("http_request: headers must map strings to strings.\n");
              std::string name = lower(string_value(h->values[0]));
              std::string value = string_value(h->values[1]);
              if (!header_name(name)) error("http_request: invalid header name.\n");
              for (unsigned char c : value) {
                if ((c < 32 && c != '\t') || c == 127)
                  error("http_request: invalid header value.\n");
              }
              if (name == "host" || name == "content-length" || name == "transfer-encoding" ||
                  name == "connection" || name == "upgrade" || name == "expect" ||
                  name == "proxy-authorization" || name == "trailer" || name == "te")
                error("http_request: reserved transport header '%s'.\n", name.c_str());
              total += name.size() + value.size() + 4;
              if (total > kHeaderLimit) error("http_request: request headers too large.\n");
              if (!r.headers.emplace(name, value).second)
                error("http_request: duplicate header name.\n");
            }
          }
        } else if (name == "timeout" || name == "max_body") {
          if (v.type != T_NUMBER || v.u.number < (name == "timeout" ? 1 : 0) ||
              v.u.number > INT_MAX) error("http_request: invalid %s.\n", name.c_str());
          if (name == "timeout") r.timeout = static_cast<int>(v.u.number);
          else r.max_body = std::min(r.max_body, static_cast<size_t>(v.u.number));
        } else if (name == "text" || name == "follow") {
          if (v.type != T_NUMBER || (v.u.number != 0 && v.u.number != 1))
            error("http_request: %s must be 0 or 1.\n", name.c_str());
          if (name == "text") r.text = v.u.number;
          else r.follow = v.u.number;
        } else if (name == "protocol") {
          error("http_request: protocol is reserved; WebSocket clients are not implemented.\n");
        } else error("http_request: unknown option '%s'.\n", name.c_str());
      }
    }
  }
  r.max_body = std::min(r.max_body, static_cast<size_t>(
      CONFIG_INT(r.text ? __MAX_STRING_LENGTH__ : __MAX_BUFFER_SIZE__)));
}

void custom_header(const char* name, int length, void* opaque) {
  auto& r = *static_cast<Request*>(opaque);
  int n = lws_hdr_custom_length(r.wsi, name, length);
  if (n < 0) return;
  std::vector<char> value(static_cast<size_t>(n) + 1);
  if (lws_hdr_custom_copy(r.wsi, value.data(), n + 1, name, length) < 0) return;
  std::string key(name, length);
  if (!key.empty() && key.back() == ':') key.pop_back();
  r.response_headers[lower(key)] = std::string(value.data(), n);
}

void response_headers(Request& r) {
  for (int i = 0; i < WSI_TOKEN_COUNT; ++i) {
    auto token = static_cast<lws_token_indexes>(i);
    const auto* name = lws_token_to_string(token);
    if (!name) continue;
    std::string key(reinterpret_cast<const char*>(name));
    // Pseudoheaders, request URI, status, and internal tokens are not headers.
    if (key.empty() || key.back() != ':' || key.front() == ':') continue;
    int n = lws_hdr_total_length(r.wsi, token);
    if (!n) continue;
    std::vector<char> value(static_cast<size_t>(n) + 1);
    if (lws_hdr_copy(r.wsi, value.data(), n + 1, token) < 0) continue;
    key.pop_back();
    r.response_headers[lower(key)] = std::string(value.data(), n);
  }
  lws_hdr_custom_name_foreach(r.wsi, custom_header, &r);
}

int callback(lws* wsi, lws_callback_reasons reason, void*, void* in, size_t len) {
  auto* r = static_cast<Request*>(lws_get_opaque_user_data(wsi));
  if (!r) return 0;
  if (reason == LWS_CALLBACK_WSI_DESTROY) {
    r->wsi = nullptr;
    if (!r->finished) fail(*r, "connect", "Connection closed before the response completed.");
    queue(*r);
    return 0;
  }
  if (stopping) return 0;
  // Returning -1 after capturing a redirect deliberately closes this hop.
  // lws reports that as a connection error too; it must not overwrite it.
  if (r->finished) return 0;
  switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
      r->sent_headers = true;
      auto** p = static_cast<unsigned char**>(in);
      auto* end = *p + len;
      for (const auto& h : r->headers) {
        std::string name = h.first + ":";
        if (lws_add_http_header_by_name(wsi,
                reinterpret_cast<const unsigned char*>(name.c_str()),
                reinterpret_cast<const unsigned char*>(h.second.data()),
                static_cast<int>(h.second.size()), p, end)) {
          fail(*r, "too_large", "Request headers exceed the transport limit.");
          return -1;
        }
      }
      if (r->has_body || r->method == "POST" || r->method == "PUT") {
        if (lws_add_http_header_content_length(wsi, r->body.size(), p, end)) return -1;
      }
      if (!r->body.empty()) {
        lws_client_http_body_pending(wsi, 1);
        lws_callback_on_writable(wsi);
      }
      break;
    }
    case LWS_CALLBACK_CLIENT_HTTP_WRITEABLE: {
      // lws_write() never returns short: a partial send is buffered by lws,
      // which re-arms POLLOUT itself and calls back here once it has drained.
      // A choke can also be reported with nothing buffered (the kernel took
      // the whole chunk but the send queue is now past its POLLOUT mark).
      // lws leaves the body wait to "user code" in that case, so the request
      // must ask for the next writable callback itself or it sits until the
      // deadline; bodies above the socket send buffer stalled this way.
      // Asking from inside this callback is unreliable with lws's libevent
      // integration (the wakeup only arrives with lws's periodic service),
      // so the request is re-armed from the driver's own event loop instead.
      while (r->sent < r->body.size()) {
        size_t n = std::min(size_t(16384), r->body.size() - r->sent);
        unsigned char data[LWS_PRE + 16384];
        memcpy(data + LWS_PRE, r->body.data() + r->sent, n);
        bool last = r->sent + n == r->body.size();
        if (lws_write(wsi, data + LWS_PRE, n, last ? LWS_WRITE_HTTP_FINAL : LWS_WRITE_HTTP) <
            static_cast<int>(n)) {
          fail(*r, "connect", "Failed to send the request body.");
          return -1;
        }
        r->sent += n;
        if (last) lws_client_http_body_pending(wsi, 0);
        if (lws_send_pipe_choked(wsi)) break;
      }
      if (r->sent < r->body.size() && !r->rearm_pending) {
        r->rearm_pending = true;
        add_walltime_event(std::chrono::milliseconds(0), [id = r->id] { rearm(id); });
      }
      break;
    }
    case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP: {
      r->status = static_cast<int>(lws_http_client_http_response(wsi));
      response_headers(*r);
      if (r->follow && (r->status == 301 || r->status == 302 || r->status == 303 ||
                        r->status == 307 || r->status == 308)) {
        auto it = r->response_headers.find("location");
        if (it != r->response_headers.end()) {
          r->redirect = redirect_url(r->url, it->second);
          r->finished = true;
          queue(*r);
          return -1;
        }
      }
      if (r->method == "HEAD" || r->status == 204 || r->status == 304) {
        r->finished = true;
        queue(*r);
        return -1;
      }
      auto length = r->response_headers.find("content-length");
      r->eof_body = length == r->response_headers.end() &&
                    !r->response_headers.count("transfer-encoding");
      if (length != r->response_headers.end()) {
        size_t n = 0;
        for (unsigned char c : length->second) {
          if (c < '0' || c > '9') {
            fail(*r, "connect", "Invalid Content-Length.");
            return -1;
          }
          size_t digit = c - '0';
          if (digit > r->max_body || n > (r->max_body - digit) / 10) {
            fail(*r, "too_large", "Response exceeds max_body.");
            return -1;
          }
          n = n * 10 + digit;
        }
      }
      break;
    }
    case LWS_CALLBACK_RECEIVE_CLIENT_HTTP: {
      char data[LWS_PRE + 16384];
      char* p = data + LWS_PRE;
      int n = 16384;
      if (lws_http_client_read(wsi, &p, &n) < 0) return -1;
      break;
    }
    case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
      if (len > r->max_body - r->response.size()) {
        fail(*r, "too_large", "Response exceeds max_body.");
        return -1;
      }
      r->response.append(static_cast<const char*>(in), len);
      break;
    case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
      r->finished = true;
      queue(*r);
      return -1;
    case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
      if (!r->finished && r->eof_body && r->status) {
        r->finished = true;
        queue(*r);
      }
      break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
      std::string message = in ? std::string(static_cast<const char*>(in), len) :
                                "Connection failed.";
      std::string folded = lower(message);
      const char* code = folded.find("ssl") != std::string::npos ||
                         folded.find("tls") != std::string::npos ||
                         folded.find("cert") != std::string::npos ? "tls" : "connect";
      fail(*r, code, message);
      break;
    }
    default: break;
  }
  return 0;
}

void dns_result(int error, evutil_addrinfo* result, void* opaque) {
  auto& r = *static_cast<Request*>(opaque);
  r.dns = nullptr;
  std::unique_ptr<evutil_addrinfo, decltype(&evutil_freeaddrinfo)> owned(
      result, evutil_freeaddrinfo);
  if (stopping || r.finished) { queue(r); return; }
  if (error) {
    fail(r, "dns", evutil_gai_strerror(error));
    return;
  }
  for (auto* a = result; a; a = a->ai_next) {
    char host[128];
    const void* addr = a->ai_family == AF_INET ?
        static_cast<void*>(&reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr) :
        a->ai_family == AF_INET6 ?
        static_cast<void*>(&reinterpret_cast<sockaddr_in6*>(a->ai_addr)->sin6_addr) : nullptr;
    if (addr && evutil_inet_ntop(a->ai_family, addr, host, sizeof(host)))
      r.addresses.emplace_back(host);
  }
  if (r.addresses.empty()) fail(r, "dns", "No usable address for host.");
  else { r.resolved = true; queue(r); }
}

void start(Request& r) {
  r.started = true;
  if (!http_context || !resolver) { fail(r, "connect", "HTTP client is unavailable."); return; }
  // This lws release uses a 128-byte hostname scratch buffer and splits at
  // the first colon for certificate verification. Refuse unsupported TLS
  // authorities instead of validating a truncated/different hostname.
  if (r.url.scheme == "https" && (r.url.authority.size() >= 128 ||
                                  r.url.host.find(':') != std::string::npos)) {
    fail(r, "tls", "This transport cannot verify IPv6 literals or long TLS hostnames.");
    return;
  }
  evutil_addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  r.dns = evdns_getaddrinfo(resolver, r.url.host.c_str(), nullptr, &hints, dns_result, &r);
}

void connect(Request& r) {
  lws_client_connect_info info{};
  info.context = http_context;
  info.address = r.addresses[r.next_address++].c_str();
  info.port = r.url.port;
  info.host = r.url.authority.c_str();
  info.path = r.url.path.c_str();
  info.method = r.method.c_str();
  info.local_protocol_name = "fluffos-http";
  info.alpn = "http/1.1";
  info.opaque_user_data = &r;
  info.pwsi = &r.wsi;
  info.ssl_connection = LCCSCF_HTTP_NO_FOLLOW_REDIRECT | LCCSCF_HTTP_NO_CACHE_CONTROL;
  if (r.url.scheme == "https") info.ssl_connection |= LCCSCF_USE_SSL;
  // The address is numeric: lws cannot block on DNS. host retains the real
  // authority for Host, SNI and certificate verification.
  if (!lws_client_connect_via_info(&info)) {
    r.wsi = nullptr;
    fail(r, "connect", "Unable to create HTTP connection.");
  }
}

Mapping rejection(const char* code, const char* message) {
  Mapping m(allocate_mapping(2), free_mapping);
  add_mapping_string(m.get(), "error", code);
  add_mapping_string(m.get(), "message", message);
  return m;
}

svalue_t* mapping_slot(mapping_t* m, const char* name) {
  svalue_t key = const0;
  key.type = T_STRING;
  key.subtype = STRING_CONSTANT;
  key.u.string = name;
  // find_for_insert interns (and ref-bumps) this temporary key before
  // inserting it. Release its own ref on both success and error unwind.
  std::unique_ptr<svalue_t, void (*)(svalue_t*)> cleanup(
      &key, [](svalue_t* value) { free_svalue(value, "HTTP mapping key"); });
  return find_for_insert(m, &key, 1);
}

void settle(Request& r) {
  if (r.promise->state != PROMISE_PENDING || r.promise->resolving) return;
  if (r.error.empty() && r.text &&
      (r.response.find('\0') != std::string::npos ||
       !u8_validate(reinterpret_cast<const uint8_t*>(r.response.data()), r.response.size()))) {
    r.error = "connect";
    r.message = "Response is not NUL-free UTF-8; request a buffer instead.";
  }
  Mapping result(nullptr, free_mapping);
  if (!r.error.empty()) result = rejection(r.error.c_str(), r.message.c_str());
  else {
    result.reset(allocate_mapping(5));
    add_mapping_pair(result.get(), "status", r.status);
    add_mapping_string(result.get(), "url", r.url.value.c_str());
    add_mapping_pair(result.get(), "ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                         Clock::now() - r.begin).count());
    auto* slot = mapping_slot(result.get(), "headers");
    auto* headers = allocate_mapping(r.response_headers.size());
    slot->type = T_MAPPING;
    slot->u.map = headers;
    for (const auto& h : r.response_headers) {
      add_mapping_string(headers, h.first.c_str(), h.second.c_str());
    }
    if (r.text) add_mapping_string(result.get(), "body", r.response.c_str());
    else {
      slot = mapping_slot(result.get(), "body");
      auto* body = allocate_buffer(r.response.size());
      if (!r.response.empty()) memcpy(body->item, r.response.data(), r.response.size());
      slot->type = T_BUFFER;
      slot->u.buf = body;
    }
  }
  svalue_t value = const0;
  value.type = T_MAPPING;
  value.u.map = result.get();
  promise_settle(r.promise, &value, !r.error.empty());
}

void progress(Request& r) {
  if (r.finished) {
    if (r.dns) { evdns_getaddrinfo_cancel(r.dns); return; }
    if (r.wsi) {
      // This runs on the tick path, outside every lws callback. Close now:
      // an async kill depends on lws's service timer being rearmed and can
      // otherwise leave a timed-out request waiting on its peer forever.
      lws_set_timeout(r.wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_SYNC);
      return;
    }
    // Try other resolved addresses only before sending any HTTP request.
    // Never replay a request that could already have changed server state.
    if (r.error == "connect" && !r.sent_headers && r.next_address < r.addresses.size()) {
      r.finished = false;
      r.error.clear();
      r.message.clear();
      connect(r);
      return;
    }
    if (!r.redirect.empty()) {
      Url next;
      if (++r.redirects > kRedirectLimit || !parse_url(r.redirect, next) ||
          (r.url.scheme == "https" && next.scheme != "https")) {
        fail(r, "denied", "Redirect limit, invalid URL, or TLS downgrade.");
      } else if (!allowed(next, r.caller)) {
        fail(r, "denied", "Redirect denied by HTTP access policy.");
      } else {
        if (r.url.scheme != next.scheme || r.url.authority != next.authority) {
          r.headers.erase("authorization");
          r.headers.erase("cookie");
        }
        if ((r.status == 303 && r.method != "HEAD") ||
            ((r.status == 301 || r.status == 302) && r.method == "POST")) {
          r.method = "GET";
          r.body.clear();
          r.has_body = false;
          r.headers.erase("content-type");
          r.headers.erase("content-encoding");
        }
        r.url = std::move(next);
        r.redirect.clear();
        r.response_headers.clear();
        r.response.clear();
        r.addresses.clear();
        r.next_address = r.sent = 0;
        r.status = 0;
        r.resolved = r.finished = r.eof_body = r.sent_headers = false;
        start(r);
        return;
      }
    }
    settle(r);
    requests.erase(r.id);
    return;
  }
  if (!r.started) start(r);
  else if (r.resolved && !r.wsi) connect(r);
}

void rearm(uint64_t id) {
  auto it = requests.find(id);
  if (stopping || it == requests.end()) return;
  auto& r = *it->second;
  r.rearm_pending = false;
  if (r.wsi && !r.finished && r.sent < r.body.size()) lws_callback_on_writable(r.wsi);
}

void dispatch(uint64_t id) {
  auto it = requests.find(id);
  if (stopping || it == requests.end()) return;
  auto& r = *it->second;
  r.queued = false;
  error_context_t econ;
  save_context(&econ);
  try {
    set_eval(max_eval_cost);
    progress(r);
  } catch (const char*) {
    restore_context(&econ);
    // Response construction can throw (e.g. a mapping-size limit changed).
    // This preallocated rejection survives that unwind without another build.
    svalue_t value = const0;
    value.type = T_MAPPING;
    value.u.map = r.fallback.get();
    promise_settle(r.promise, &value, 1);
    r.finished = true;
    r.redirect.clear();
    queue(r);
  }
  pop_context(&econ);
}
}  // namespace

void init_http(event_base* base) {
  event_loop = base;
  resolver = evdns_base_new(base, EVDNS_BASE_INITIALIZE_NAMESERVERS);
#ifdef _WIN32
  // Match the core resolver: libevent needs an explicit hosts-file load.
  if (resolver) evdns_base_load_hosts(resolver, nullptr);
#endif
  static lws_protocols protocols[2]{};
  protocols[0].name = "fluffos-http";
  protocols[0].callback = callback;
  protocols[0].rx_buffer_size = 16384;
  void* loops[] = {base};
  lws_context_creation_info info{};
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = protocols;
  info.foreign_loops = loops;
  info.options = LWS_SERVER_OPTION_LIBEVENT | LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
  info.pt_serv_buf_size = 128 * 1024;
  info.max_http_header_data2 = 65535;
  // Do not inherit an environment proxy that bypasses the host policy or
  // introduces synchronous proxy DNS inside lws.
  info.http_proxy_address = ":0";
#ifdef LWS_WITH_SOCKS5
  info.socks_proxy_address = "";
#endif
  info.pcontext = &http_context;
  http_context = lws_create_context(&info);
  if (!http_context) debug_message("HTTP client context initialization failed.\n");
}

void http_cleanup() {
  stopping = true;
  for (auto& item : requests) {
    auto& r = *item.second;
    if (r.dns) evdns_getaddrinfo_cancel(r.dns);
  }
  if (resolver) { evdns_base_free(resolver, 1); resolver = nullptr; }
  if (http_context) lws_context_destroy(http_context);
  requests.clear();
}

#ifdef DEBUGMALLOC_EXTENSIONS
void mark_http_requests() {
  for (auto& item : requests) {
    auto& r = *item.second;
    if (r.promise) r.promise->extra_ref++;
    if (r.caller) r.caller->extra_ref++;
    if (r.fallback) r.fallback->extra_ref++;
    // checkmemory's TAG_PROMISE / TAG_MAPPING walks mark their children.
    // Rewalking here would double-count the reactions and parked frames.
  }
}
#endif

void f_http_request() {
  const int nargs = st_num_arg;
  auto* args = sp - nargs + 1;
  auto request = std::make_unique<Request>();
  auto& r = *request;
  if (!parse_url(string_value(args[0]), r.url)) error("http_request: invalid HTTP(S) URL.\n");
  options(r, nargs == 2 ? args[1].u.map : nullptr);
  object_t* caller = current_object;
  bool approved = allowed(r.url, caller);
  r.fallback = rejection("connect", "Unable to construct HTTP response.");
  r.promise = promise_alloc();
  r.caller = caller;
  add_ref(caller, "http request");
  r.id = ++next_id;
  r.timer = evtimer_new(event_loop, [](evutil_socket_t, short, void* arg) {
    auto& r = *static_cast<Request*>(arg);
    fail(r, "timeout", "HTTP request deadline exceeded.");
  }, &r);
  if (!r.timer) error("http_request: unable to allocate deadline.\n");
  timeval deadline{r.timeout / 1000, (r.timeout % 1000) * 1000};
  // libevent measures a timeout from the time it cached when this loop pass
  // began, and this LPC may have been running for longer than the timeout
  // since. Measure it from now, or the deadline can have passed already.
  event_base_update_cache_time(event_loop);
  if (evtimer_add(r.timer, &deadline)) error("http_request: unable to schedule deadline.\n");
  requests.emplace(r.id, std::move(request));
  if (!approved) fail(r, "denied", "HTTP request denied by access policy.");
  else queue(r);
  pop_n_elems(nargs);
  r.promise->ref++;
  push_refed_promise(r.promise);
}
