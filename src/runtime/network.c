#include "network_internal.h"
#include "string_internal.h"
#include "string_layout.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

char *zap_network_copy_path(zap_string_t path) {
  if (!path.ptr) {
    return NULL;
  }

  char *buffer = (char *)malloc((size_t)path.len + 1);
  if (!buffer) {
    return NULL;
  }

  memcpy(buffer, path.ptr, (size_t)path.len);
  buffer[path.len] = '\0';
  return buffer;
}

static int zap_net_bind_addrinfo(const char *host, long port, int socktype,
                                 int flags, struct addrinfo **out) {
  if (!out) {
    errno = EINVAL;
    return -1;
  }

  char port_buf[32];
  snprintf(port_buf, sizeof(port_buf), "%ld", port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = socktype;
  hints.ai_flags = flags;

  const char *node = host;
  if (host && (strcmp(host, "") == 0 || strcmp(host, "*") == 0)) {
    node = NULL;
  }

  int rc = getaddrinfo(node, port_buf, &hints, out);
  if (rc != 0) {
    if (rc == EAI_SYSTEM) {
    } else {
      errno = EINVAL;
    }
    return -1;
  }

  errno = 0;
  return 0;
}

long netConnect(zap_string_t host, long port) {
  if (!host.ptr || port <= 0 || port > 65535) {
    errno = EINVAL;
    return -1;
  }

  char *host_buf = zap_network_copy_path(host);
  if (!host_buf) {
    errno = ENOMEM;
    return -1;
  }

  struct addrinfo *res = NULL;
  if (zap_net_bind_addrinfo(host_buf, port, SOCK_STREAM, 0, &res) != 0) {
    free(host_buf);
    return -1;
  }

  long out_fd = -1;
  int last_err = ECONNREFUSED;
  for (struct addrinfo *it = res; it; it = it->ai_next) {
    int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      last_err = errno;
      continue;
    }

    if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
      out_fd = fd;
      last_err = 0;
      break;
    }

    last_err = errno;
    close(fd);
  }

  freeaddrinfo(res);
  free(host_buf);

  errno = last_err;
  return out_fd;
}

long netListen(zap_string_t host, long port) {
  if (port <= 0 || port > 65535) {
    errno = EINVAL;
    return -1;
  }

  char *host_buf = NULL;
  if (host.ptr) {
    host_buf = zap_network_copy_path(host);
    if (!host_buf) {
      errno = ENOMEM;
      return -1;
    }
  }

  struct addrinfo *res = NULL;
  if (zap_net_bind_addrinfo(host_buf ? host_buf : "", port, SOCK_STREAM,
                            AI_PASSIVE, &res) != 0) {
    free(host_buf);
    return -1;
  }

  long out_fd = -1;
  int last_err = EADDRNOTAVAIL;
  for (struct addrinfo *it = res; it; it = it->ai_next) {
    int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      last_err = errno;
      continue;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (bind(fd, it->ai_addr, it->ai_addrlen) != 0) {
      last_err = errno;
      close(fd);
      continue;
    }

    if (listen(fd, 128) != 0) {
      last_err = errno;
      close(fd);
      continue;
    }

    out_fd = fd;
    last_err = 0;
    break;
  }

  freeaddrinfo(res);
  free(host_buf);

  errno = last_err;
  return out_fd;
}

zap_string_t netResolve(zap_string_t host) {
  if (!host.ptr || host.len == 0) {
    errno = EINVAL;
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  char *host_buf = zap_network_copy_path(host);
  if (!host_buf) {
    errno = ENOMEM;
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = NULL;
  int rc = getaddrinfo(host_buf, NULL, &hints, &res);
  free(host_buf);
  if (rc != 0) {
    if (rc == EAI_SYSTEM) {
    } else {
      errno = EINVAL;
    }
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  char ipbuf[INET6_ADDRSTRLEN];
  memset(ipbuf, 0, sizeof(ipbuf));

  for (struct addrinfo *it = res; it; it = it->ai_next) {
    void *addr_ptr = NULL;
    if (it->ai_family == AF_INET) {
      struct sockaddr_in *sa = (struct sockaddr_in *)it->ai_addr;
      addr_ptr = &(sa->sin_addr);
    } else if (it->ai_family == AF_INET6) {
      struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)it->ai_addr;
      addr_ptr = &(sa6->sin6_addr);
    }

    if (addr_ptr &&
        inet_ntop(it->ai_family, addr_ptr, ipbuf, sizeof(ipbuf)) != NULL) {
      break;
    }
  }

  freeaddrinfo(res);

  if (ipbuf[0] == '\0') {
    errno = EADDRNOTAVAIL;
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  errno = 0;
  return zap_string_from_ptrlen(ipbuf, (long)strlen(ipbuf));
}
