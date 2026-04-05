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

void panic(zap_string_t message)
{
    eprintln(message);
    exit(1);
}
