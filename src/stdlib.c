#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void println(zap_string_t s)
{
    printStringPtrLen(s.ptr, s.len);
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

long stringLen(zap_string_t s)
{
    return s.len;
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

zap_string_t from_char(char c)
{
    char *out = (char *)malloc(2);
    if (!out)
    {
        return (zap_string_t){.ptr = NULL, .len = 0};
    }

    out[0] = c;
    out[1] = '\0';
    return (zap_string_t){.ptr = out, .len = 1};
}

zap_string_t pushChar(zap_string_t s, char c)
{
    char *out = string_concat_ptrlen(s.ptr, s.len, &c, 1);
    if (!out)
    {
        return (zap_string_t){.ptr = NULL, .len = 0};
    }
    return (zap_string_t){.ptr = out, .len = s.len + 1};
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

void panic(zap_string_t message)
{
    eprintln(message);
    exit(1);
}
