#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void printInt(long v)
{
    printf("%ld\n", v);
}

void printFloat(float v)
{
    printf("%f\n", v);
}

void printFloat64(double v)
{
    printf("%f\n", v);
}

long zap_sum_variadic(long count, ...)
{
    va_list args;
    va_start(args, count);
    long sum = 0;
    for (long i = 0; i < count; ++i)
    {
        sum += va_arg(args, long);
    }
    va_end(args);
    return sum;
}

void printBool(_Bool v)
{
    printf("%s\n", v ? "true" : "false");
}

void printChar(char v)
{
    putchar(v);
    putchar('\n');
}

void printStringPtrLen(const char *ptr, long len)
{
    if (!ptr || len <= 0)
    {
        printf("\n");
        return;
    }
    fwrite(ptr, 1, (size_t)len, stdout);
    printf("\n");
}

char *string_concat_ptrlen(const char *a, long a_len, const char *b, long b_len)
{
    long total = a_len + b_len;
    char *out = (char *)malloc((size_t)total + 1);
    if (!out)
        return NULL;
    if (a_len > 0)
        memcpy(out, a, (size_t)a_len);
    if (b_len > 0)
        memcpy(out + a_len, b, (size_t)b_len);
    out[total] = '\0';
    return out;
}

typedef struct {
    const char *ptr;
    long len;
} zap_string_t;

static char *zap_string_to_cstr(zap_string_t s)
{
    size_t len = s.len > 0 ? (size_t)s.len : 0;
    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    if (len > 0 && s.ptr)
        memcpy(out, s.ptr, len);
    out[len] = '\0';
    return out;
}

static long zap_process_argc = 0;
static char **zap_process_argv = NULL;

void __zap_process_set_args(int argc, char **argv)
{
    zap_process_argc = argc;
    zap_process_argv = argv;
}

void println(zap_string_t s)
{
    printStringPtrLen(s.ptr, s.len);
}

long zap_printf(zap_string_t format, ...)
{
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

long zap_printfln(zap_string_t format, ...)
{
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

void eprintln(zap_string_t s)
{
    if (!s.ptr || s.len <= 0)
    {
        fputc('\n', stderr);
        return;
    }
    fwrite(s.ptr, 1, (size_t)s.len, stderr);
    fputc('\n', stderr);
}

void println_cstr(const char *s)
{
    if (!s)
    {
        printf("\n");
        return;
    }
    puts(s);
}

zap_string_t getLn()
{
    char *line = NULL;
    size_t len = 0;
    size_t read = getline(&line, &len, stdin);
    if (read == -1)
    {
        free(line);
        return (zap_string_t){.ptr = NULL, .len = 0};
    }
    // Remove newline if present
    if (read > 0 && line[read - 1] == '\n')
    {
        line[--read] = '\0';
    }
    zap_string_t result = {.ptr = line, .len = read};
    return result;
}

long argc()
{
    return zap_process_argc;
}

zap_string_t argv(long i)
{
    if (i < 0 || i >= zap_process_argc || !zap_process_argv || !zap_process_argv[i])
    {
        return (zap_string_t){.ptr = "", .len = 0};
    }

    const char *arg = zap_process_argv[i];
    return (zap_string_t){.ptr = arg, .len = (long)strlen(arg)};
}

long len(zap_string_t s)
{
    return s.len;
}

char at(zap_string_t s, long i)
{
    if (!s.ptr || i < 0 || i >= s.len)
    {
        return '\0';
    }
    return s.ptr[i];
}

zap_string_t slice(zap_string_t s, long start, long length)
{
    if (!s.ptr || s.len <= 0 || length <= 0 || start >= s.len)
    {
        return (zap_string_t){.ptr = "", .len = 0};
    }

    if (start < 0)
    {
        start = 0;
    }

    long available = s.len - start;
    if (available < 0)
    {
        available = 0;
    }
    if (length > available)
    {
        length = available;
    }

    char *out = (char *)malloc((size_t)length + 1);
    if (!out)
    {
        return (zap_string_t){.ptr = NULL, .len = 0};
    }

    if (length > 0)
    {
        memcpy(out, s.ptr + start, (size_t)length);
    }
    out[length] = '\0';
    return (zap_string_t){.ptr = out, .len = length};
}

_Bool eq(zap_string_t a, zap_string_t b)
{
    if (a.len != b.len)
    {
        return 0;
    }

    if (a.len == 0)
    {
        return 1;
    }

    if (!a.ptr || !b.ptr)
    {
        return 0;
    }

    return memcmp(a.ptr, b.ptr, (size_t)a.len) == 0;
}

long exec(zap_string_t cmd)
{
    if (!cmd.ptr)
    {
        return -1;
    }

    char *buffer = (char *)malloc((size_t)cmd.len + 1);
    if (!buffer)
    {
        return -1;
    }

    memcpy(buffer, cmd.ptr, (size_t)cmd.len);
    buffer[cmd.len] = '\0';

    int result = system(buffer);
    free(buffer);

    if (result == -1)
    {
        return -1;
    }

    if (WIFEXITED(result))
    {
        return WEXITSTATUS(result);
    }

    return result;
}

zap_string_t cwd()
{
    char *dir = getcwd(NULL, 0);
    if (!dir)
    {
        return (zap_string_t){.ptr = "", .len = 0};
    }

    return (zap_string_t){.ptr = dir, .len = (long)strlen(dir)};
}

static int zap_stat_path(zap_string_t path, struct stat *st)
{
    if (!path.ptr)
    {
        return -1;
    }

    char *buffer = (char *)malloc((size_t)path.len + 1);
    if (!buffer)
    {
        return -1;
    }

    memcpy(buffer, path.ptr, (size_t)path.len);
    buffer[path.len] = '\0';

    int result = stat(buffer, st);
    free(buffer);
    return result;
}

static char *zap_copy_path(zap_string_t path)
{
    if (!path.ptr)
    {
        return NULL;
    }

    char *buffer = (char *)malloc((size_t)path.len + 1);
    if (!buffer)
    {
        return NULL;
    }

    memcpy(buffer, path.ptr, (size_t)path.len);
    buffer[path.len] = '\0';
    return buffer;
}

_Bool exists(zap_string_t path)
{
    struct stat st;
    return zap_stat_path(path, &st) == 0;
}

_Bool isFile(zap_string_t path)
{
    struct stat st;
    if (zap_stat_path(path, &st) != 0)
    {
        return 0;
    }

    return S_ISREG(st.st_mode);
}

_Bool isDir(zap_string_t path)
{
    struct stat st;
    if (zap_stat_path(path, &st) != 0)
    {
        return 0;
    }

    return S_ISDIR(st.st_mode);
}

long zap_fs_mkdir(zap_string_t path)
{
    char *buffer = zap_copy_path(path);
    if (!buffer)
    {
        return ENOMEM;
    }

    if (mkdir(buffer, 0777) == 0)
    {
        chmod(buffer, 0777);
        free(buffer);
        return 0;
    }

    int err = errno;
    free(buffer);
    return err;
}

zap_string_t readFile(zap_string_t path)
{
    char *buffer = zap_copy_path(path);
    if (!buffer)
    {
        return (zap_string_t){.ptr = "", .len = 0};
    }

    FILE *file = fopen(buffer, "rb");
    free(buffer);
    if (!file)
    {
        return (zap_string_t){.ptr = "", .len = 0};
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return (zap_string_t){.ptr = "", .len = 0};
    }

    long size = ftell(file);
    if (size < 0)
    {
        fclose(file);
        return (zap_string_t){.ptr = "", .len = 0};
    }

    if (fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return (zap_string_t){.ptr = "", .len = 0};
    }

    char *content = (char *)malloc((size_t)size + 1);
    if (!content)
    {
        fclose(file);
        return (zap_string_t){.ptr = "", .len = 0};
    }

    size_t read = fread(content, 1, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size)
    {
        free(content);
        return (zap_string_t){.ptr = "", .len = 0};
    }

    content[size] = '\0';
    return (zap_string_t){.ptr = content, .len = size};
}

long writeFile(zap_string_t path, zap_string_t content)
{
    char *buffer = zap_copy_path(path);
    if (!buffer)
    {
        return ENOMEM;
    }

    FILE *file = fopen(buffer, "wb");
    free(buffer);
    if (!file)
    {
        return errno;
    }

    size_t written = fwrite(content.ptr, 1, (size_t)content.len, file);
    if (fclose(file) != 0)
    {
        return errno;
    }

    if (written != (size_t)content.len)
    {
        return EIO;
    }

    return 0;
}

static zap_string_t zap_copy_string_range(const char *start, size_t len)
{
    char *out = (char *)malloc(len + 1);
    if (!out)
    {
        return (zap_string_t){.ptr = "", .len = 0};
    }

    if (len > 0)
    {
        memcpy(out, start, len);
    }
    out[len] = '\0';
    return (zap_string_t){.ptr = out, .len = (long)len};
}

zap_string_t parent(zap_string_t path)
{
    if (!path.ptr || path.len == 0)
    {
        return (zap_string_t){.ptr = ".", .len = 1};
    }

    long end = path.len;
    while (end > 1 && path.ptr[end - 1] == '/')
    {
        --end;
    }

    long slash = end - 1;
    while (slash >= 0 && path.ptr[slash] != '/')
    {
        --slash;
    }

    if (slash < 0)
    {
        return (zap_string_t){.ptr = ".", .len = 1};
    }

    if (slash == 0)
    {
        return (zap_string_t){.ptr = "/", .len = 1};
    }

    return zap_copy_string_range(path.ptr, (size_t)slash);
}

zap_string_t zap_path_basename(zap_string_t path)
{
    if (!path.ptr || path.len == 0)
    {
        return (zap_string_t){.ptr = ".", .len = 1};
    }

    long end = path.len;
    while (end > 1 && path.ptr[end - 1] == '/')
    {
        --end;
    }

    if (end == 1 && path.ptr[0] == '/')
    {
        return (zap_string_t){.ptr = "/", .len = 1};
    }

    long start = end - 1;
    while (start >= 0 && path.ptr[start] != '/')
    {
        --start;
    }
    ++start;

    return zap_copy_string_range(path.ptr + start, (size_t)(end - start));
}

double zapMathSqrt(double x)
{
    return sqrt(x);
}

double zapMathFloor(double x)
{
    return floor(x);
}

double zapMathCeil(double x)
{
    return ceil(x);
}
