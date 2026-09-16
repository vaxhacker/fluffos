---
title: http / http_request
---
# http_request

Send an asynchronous HTTP or HTTPS request. Available with `PACKAGE_HTTP`
(enabled by default on native builds; disabled on WebAssembly). No additional
library is required: it uses the driver's bundled libwebsockets and libevent.

```c
promise http_request(string url, mapping opts | void);
```

The driver keeps serving players during DNS resolution, connection setup,
uploads, and downloads. Use `await` inside an `async` function, or attach
handlers with `promise_then()` / `promise_catch()`.

## Options

| Key | Default | Meaning |
| --- | --- | --- |
| `method` | `"GET"` | `"GET"`, `"POST"`, `"PUT"`, `"DELETE"`, or `"HEAD"` (uppercase). |
| `headers` | `([])` | Mapping of header names to string values. |
| `body` | Absent | String (UTF-8 bytes) or buffer (arbitrary bytes). |
| `timeout` | `http default timeout` | Positive milliseconds for the entire exchange, including DNS and redirects. |
| `max_body` | `http max body` | Maximum response bytes, including zero. May lower but cannot raise the configured ceiling. |
| `follow` | `0` | Set to `1` to follow up to five redirects. |
| `text` | `0` | Set to `1` to return the body as a string. |
| `protocol` | Absent | Reserved for a future WebSocket client; currently raises an error. |

Invalid option keys, types, methods, URLs, or headers raise an LPC error
synchronously. URLs must be absolute `http://` or `https://` URLs with an ASCII
hostname or IP address. Percent-encode spaces/non-ASCII path bytes; URL userinfo
(`user:password@host`) is not accepted. Supply authentication using headers.
Fragments are omitted from requests and the returned URL.

Header names are case-insensitive. Duplicate names, control characters, and
caller-supplied transport headers (`Host`, `Content-Length`, `Transfer-Encoding`,
`Connection`, `Upgrade`, `Expect`, `Proxy-Authorization`, `Trailer`, `TE`) are
rejected. The driver supplies framing and the correct Host header. Request
headers are limited to 32 KiB in total. Responses use a bounded header parser.

## Result

A completed HTTP exchange **fulfills**, including 404, 409, and 500 responses:

```c
([
    "status": 200,
    "headers": ([ "content-type": "application/json", ... ]),
    "body": buffer_value,
    "url": "http://127.0.0.1:5984/world/document",
    "ms": 12,
])
```

Header keys are lowercase. `url` is the final normalized URL, including the
port; `ms` is elapsed wall-clock milliseconds. `HEAD`, 204, and 304 responses
have an empty body. Chunked transfer framing is removed. Content encoding
(such as gzip) is not decoded automatically; request identity encoding if
needed. JSON parsing is separate from transport.

The default buffer preserves all bytes and avoids the string-length ceiling.
The effective response cap is the smallest of `max_body`, `http max body`,
and `maximum buffer size` (or `maximum string length` with `text: 1`). The
request rejects rather than returning a truncated body. Page large CouchDB
queries using `limit` and `startkey`.

Text mode requires NUL-free UTF-8; invalid text rejects with `connect` and a
message explaining that a buffer is required. It performs no charset conversion.

## Rejections

Failures reject with `([ "error": code, "message": description ])`:

| Code | Meaning |
| --- | --- |
| `timeout` | The total deadline expired. |
| `connect` | Connection, protocol, upload, incomplete-response, or text-conversion failure. |
| `dns` | Hostname resolution failed. |
| `tls` | TLS negotiation or certificate verification failed. |
| `too_large` | The response body or transport request-header limit was exceeded. |
| `denied` | Host/master policy denied access, or a redirect exceeded the limit, supplied an invalid target, or downgraded HTTPS. |

TLS verifies the server certificate and hostname using the system trust store.
There is no option to disable verification. The bundled transport currently
cannot verify HTTPS URLs with IPv6 literal hosts or authorities of 128 bytes
or more; these reject with `tls`. HTTP IPv6 literals are supported.
Environment HTTP proxies are not used.

## Access policy and redirects

Both the exact `host:port` configuration entry and the synchronous
[`valid_http`](../../apply/master/valid_http) master apply must approve.
The default empty host list denies all requests, including loopback. For example:

```text
http allowed hosts : 127.0.0.1:5984 db.example:443
http default timeout : 15000
http max body : 400000
```

Entries are separated by whitespace or commas, matched case-insensitively,
and do not support wildcards. Keep the complete config line below 120 bytes
(the current driver config parser limit). IPv6 entries use brackets: `[::1]:5984`.
A permitted hostname authorizes its resolved addresses; this is not an IP-range
filter. Protect credentials and keep the list limited to intended services.

Redirects are off by default. With `follow: 1`, every target is checked against
both policies before DNS or connection setup, using the original caller.
HTTPS-to-HTTP redirects are denied. Authorization and Cookie headers are
removed when the origin changes. A 303 changes the method to GET (except HEAD);
301/302 change POST to GET; 307/308 preserve method and body. Relative Location
values are supported. The timeout covers the whole chain.

As with `async_read()`, LPC may settle the returned promise early. This discards
the eventual driver result; it does **not** cancel the network request. Use the
built-in timeout to bound network work. Driver shutdown cancels pending work.

## Example

```c
async void save_document(string url, buffer json_body) {
    mapping response;
    mixed failure = acatch {
        response = await http_request(url, ([
            "method": "PUT",
            "headers": ([ "Content-Type": "application/json" ]),
            "body": json_body,
        ]));
    };
    if (failure) {
        // Handle transport failure separately from an HTTP status.
        return;
    }
    if (response["status"] == 409) {
        // Fetch the current revision and apply your conflict policy.
    }
}
```

Keep CouchDB authentication policy, `_rev` handling, pagination, retries, and
save queues in LPC. Ordinary HTTP requests also support `_changes` polling and
long polling (choose a suitable timeout). `http_stream()` and
`http_stream_close()` are reserved designs, not implemented efuns.

## See also

[async / await / acatch](../../lpc/constructs/async),
[promise_then](../promises/promise_then),
[Runtime configuration](../../driver/config)
