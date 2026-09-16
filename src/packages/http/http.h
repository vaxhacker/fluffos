#ifndef PACKAGES_HTTP_H
#define PACKAGES_HTTP_H

void init_http(struct event_base* base);
void http_cleanup();
#ifdef DEBUGMALLOC_EXTENSIONS
void mark_http_requests();
#endif

#endif
