#include "string_internal.h"
#include "string_layout.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

long zap_sum_variadic(long count, ...) {
  va_list args;
  va_start(args, count);
  long sum = 0;
  for (long i = 0; i < count; ++i) {
    sum += va_arg(args, long);
  }
  va_end(args);
  return sum;
}

static long zap_process_argc = 0;
static char **zap_process_argv = NULL;

void __zap_process_set_args(int argc, char **argv) {
  zap_process_argc = argc;
  zap_process_argv = argv;
}

long zap_printf(zap_string_t format, ...) {
  char *fmt = zap_string_to_cstr(format);
  if (!fmt)
    return -1;

  va_list args;
  va_start(args, format);
  long written = vprintf(fmt, args);
  va_end(args);

  free(fmt);
  return written;
}

long zap_printfln(zap_string_t format, ...) {
  char *fmt = zap_string_to_cstr(format);
  if (!fmt)
    return -1;

  va_list args;
  va_start(args, format);
  long written = vprintf(fmt, args);
  va_end(args);

  free(fmt);

  if (written < 0)
    return written;
  if (printf("\n") < 0)
    return -1;
  return written + 1;
}

zap_string_t getln() {
  char *line = NULL;
  size_t len = 0;
  ssize_t read = getline(&line, &len, stdin);
  if (read == -1) {
    free(line);
    return (zap_string_t){.ptr = NULL, .len = 0};
  }
  // Remove newline if present
  if (read > 0 && line[read - 1] == '\n') {
    line[--read] = '\0';
  }
  char *owned = zap_string_alloc_owned((size_t)read);
  if (!owned) {
    free(line);
    return (zap_string_t){.ptr = NULL, .len = 0};
  }
  if (read > 0) {
    memcpy(owned, line, (size_t)read);
  }
  owned[read] = '\0';
  free(line);
  zap_string_t result = {.ptr = owned, .len = (long)read};
  return result;
}

long argc() { return zap_process_argc; }

zap_string_t argv(long i) {
  if (i < 0 || i >= zap_process_argc || !zap_process_argv ||
      !zap_process_argv[i]) {
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  const char *arg = zap_process_argv[i];
  return zap_string_from_cstr(arg);
}
