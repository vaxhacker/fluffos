---
title: master / valid_http
---
# valid_http

Authorize an outbound HTTP request made by `http_request()`.

```c
mixed valid_http(string url, object caller);
```

Return integer `1` to approve. The driver also requires the normalized
`host:port` to appear in `http allowed hosts`; an empty list denies everything.
`url` is the normalized absolute URL (explicit port, no fragment). `caller`
is the object that originally invoked the efun.

The apply runs before connecting, and again for every followed redirect.
A missing master, missing apply, runtime error, destructed caller, or promise
return denies access. This apply must decide synchronously: it cannot await.
A denied request rejects with `([ "error": "denied", "message": ... ])`.

```c
int valid_http(string url, object caller) {
    return caller == find_object("/daemon/couch");
}
```

The configuration and this apply are independent checks. Use the master to
restrict callers and URL paths; keep retry and database policies in LPC.

See [http_request](../../efun/http/http_request).
