#include "network_internal.h"
#include "string_internal.h"
#include "string_layout.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

static long zap_tls_last_error_code = 0;

typedef struct {
  SSL_CTX *context;
  SSL *ssl;
  int fd;
} zap_tls_session_t;

static void zap_tls_session_free(zap_tls_session_t *session) {
  if (!session) {
    return;
  }
  if (session->ssl) {
    SSL_shutdown(session->ssl);
    SSL_free(session->ssl);
  }
  if (session->context) {
    SSL_CTX_free(session->context);
  }
  if (session->fd >= 0) {
    close(session->fd);
  }
  free(session);
}

long zap_tls_connect(zap_string_t host, long port) {
  const long fd = netConnect(host, port);
  if (fd < 0) {
    zap_tls_last_error_code = errno;
    return 0;
  }

  char *host_buffer = zap_network_copy_path(host);
  if (!host_buffer) {
    close((int)fd);
    zap_tls_last_error_code = ENOMEM;
    return 0;
  }

  zap_tls_session_t *session = calloc(1, sizeof(*session));
  if (!session) {
    free(host_buffer);
    close((int)fd);
    zap_tls_last_error_code = ENOMEM;
    return 0;
  }
  session->fd = (int)fd;
  session->context = SSL_CTX_new(TLS_client_method());
  if (!session->context ||
      SSL_CTX_set_default_verify_paths(session->context) != 1) {
    free(host_buffer);
    zap_tls_session_free(session);
    zap_tls_last_error_code = EIO;
    return 0;
  }
  SSL_CTX_set_verify(session->context, SSL_VERIFY_PEER, NULL);
  session->ssl = SSL_new(session->context);
  if (!session->ssl ||
      SSL_set_tlsext_host_name(session->ssl, host_buffer) != 1 ||
      SSL_set1_host(session->ssl, host_buffer) != 1 ||
      SSL_set_fd(session->ssl, session->fd) != 1 ||
      SSL_connect(session->ssl) != 1) {
    free(host_buffer);
    zap_tls_session_free(session);
    zap_tls_last_error_code = EIO;
    return 0;
  }
  free(host_buffer);

  if (SSL_get_verify_result(session->ssl) != X509_V_OK) {
    zap_tls_session_free(session);
    zap_tls_last_error_code = EACCES;
    return 0;
  }

  zap_tls_last_error_code = 0;
  return (long)(intptr_t)session;
}

long zap_tls_send(long handle, zap_string_t data) {
  zap_tls_session_t *session = (zap_tls_session_t *)(intptr_t)handle;
  if (!session || !session->ssl || !data.ptr) {
    zap_tls_last_error_code = EINVAL;
    return -1;
  }

  size_t total = 0;
  const size_t target = data.len > 0 ? (size_t)data.len : 0;
  while (total < target) {
    const size_t remaining = target - total;
    const int request = remaining > INT_MAX ? INT_MAX : (int)remaining;
    const int written = SSL_write(session->ssl, data.ptr + total, request);
    if (written <= 0) {
      zap_tls_last_error_code = EIO;
      return -1;
    }
    total += (size_t)written;
  }
  zap_tls_last_error_code = 0;
  return (long)total;
}

zap_string_t zap_tls_recv(long handle, long max_len) {
  zap_tls_session_t *session = (zap_tls_session_t *)(intptr_t)handle;
  if (!session || !session->ssl || max_len <= 0) {
    zap_tls_last_error_code = EINVAL;
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  const size_t capacity = (size_t)max_len;
  char *buffer = zap_string_alloc_owned(capacity);
  if (!buffer) {
    zap_tls_last_error_code = ENOMEM;
    return (zap_string_t){.ptr = NULL, .len = 0};
  }
  const int request = capacity > INT_MAX ? INT_MAX : (int)capacity;
  const int received = SSL_read(session->ssl, buffer, request);
  if (received <= 0) {
    const int ssl_error = SSL_get_error(session->ssl, received);
    zap_string_release_ptr(buffer);
    if (ssl_error == SSL_ERROR_ZERO_RETURN) {
      zap_tls_last_error_code = 0;
    } else {
      zap_tls_last_error_code = EIO;
    }
    return (zap_string_t){.ptr = NULL, .len = 0};
  }
  buffer[received] = '\0';
  zap_tls_last_error_code = 0;
  return (zap_string_t){.ptr = buffer, .len = received};
}

long zap_tls_close(long handle) {
  zap_tls_session_t *session = (zap_tls_session_t *)(intptr_t)handle;
  if (!session) {
    zap_tls_last_error_code = EINVAL;
    return EINVAL;
  }
  zap_tls_session_free(session);
  zap_tls_last_error_code = 0;
  return 0;
}

long zap_tls_last_error() { return zap_tls_last_error_code; }
