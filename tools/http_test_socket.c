/* Linux-only fault fixture for test_http_idle_upload.py. Limit the client's
 * real TCP send buffer and prove that lws encountered real backpressure.
 * No readiness result is synthesized and no timer or socket wakes the driver.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>

static int upload_fd = -1;
static unsigned chokes;

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
  static int (*real_connect)(int, const struct sockaddr *, socklen_t);
  if (!real_connect) real_connect = dlsym(RTLD_NEXT, "connect");
  if (addr->sa_family == AF_INET || addr->sa_family == AF_INET6) {
    int type, size = 4096;
    socklen_t type_len = sizeof(type);
    if (!getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_len) && type == SOCK_STREAM) {
      if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size))) {
        perror("HTTP test SO_SNDBUF");
        return -1;
      }
      upload_fd = fd;
    }
  }
  return real_connect(fd, addr, len);
}

int poll(struct pollfd *fds, nfds_t count, int timeout) {
  static int (*real_poll)(struct pollfd *, nfds_t, int);
  if (!real_poll) real_poll = dlsym(RTLD_NEXT, "poll");
  int result = real_poll(fds, count, timeout);
  if (count == 1 && fds[0].fd == upload_fd && fds[0].events == POLLOUT &&
      timeout == 0 && result == 0)
    chokes++;
  return result;
}

__attribute__((destructor)) static void report_chokes(void) {
  fprintf(stderr, "HTTP_TEST_SOCKET chokes=%u\n", chokes);
}
