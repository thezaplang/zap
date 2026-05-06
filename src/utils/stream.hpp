#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace zap {

/// @brief Allows to override if colors are allowed or not.
enum class ColorOverride : uint8_t { AUTO, ALWAYS, NEVER };
extern ColorOverride color_override;

enum class Color {
  BLACK = 30,
  RED,
  GREEN,
  YELLOW,
  BLUE,
  MAGENTA,
  CYAN,
  WHITE,
  BRIGHT_BLACK = 90,
  BRIGHT_RED,
  BRIGHT_GREEN,
  BRIGHT_YELLOW,
  BRIGHT_BLUE,
  BRIGHT_MAGENTA,
  BRIGHT_CYAN,
  BRIGHT_WHITE
};

/// @brief A base stream class.
class Stream {
public:
  using BufferChar = char;
  using BufferType = BufferChar *;

  /// @brief Default buffer size if the buffer size arg is not set.
  constexpr static size_t DEFAULT_BUFFER_SIZE = 0x2000;

private:
  BufferType start, cur, end;

  /// @brief Implementation for the write function, depends on the class.
  virtual void internalWrite(const BufferChar *ptr, size_t size) = 0;

  virtual void internalFlush() {}

public:
  Stream(const Stream &) = delete;
  Stream &operator=(const Stream &) = delete;

  /// @brief Default stream constructor.
  /// @param bufferSize What buffer size should it be, 0 for unbuffered.
  Stream(size_t bufferSize = DEFAULT_BUFFER_SIZE);

  size_t getBufferSize() const noexcept { return end - cur; }

  /// @brief Writes the provided characters to the stream.
  /// @param ptr Pointer to the characters.
  /// @param size How many of them.
  Stream &write(const BufferChar *ptr, size_t size);

  /// @brief Sets the buffer size of the stream.
  void setBufferSize(size_t bufferSize = DEFAULT_BUFFER_SIZE);

  /// @brief Makes the stream unbuffered.
  void setNoBuffer() noexcept { setBufferSize(0); }

  void flush() {
    if (cur != start) {
      internalWrite(start, cur - start);
      cur = start;
    }
    internalFlush();
  }

  /// @brief Returns whether or not the stream is in a console/terminal.
  virtual bool onTTY() const { return false; }

  /// @brief Returns whether or not this stream supports colors.
  virtual bool hasColors() const { return false; }

  virtual ~Stream() noexcept { setNoBuffer(); }

  Stream &operator<<(std::nullptr_t) { return write("null", 4); }

  Stream &operator<<(char c) { return write(&c, sizeof(c)); }

  Stream &operator<<(unsigned char c) { return *this << char(c); }

  Stream &operator<<(signed char c) { return *this << char(c); }

  Stream &operator<<(bool b) {
    return b ? write("true", 4) : write("false", 5);
  }

  Stream &operator<<(const void *ptr) {
    char buff[sizeof(void *) * 2 + 2];

    auto res = std::to_chars(buff, buff + sizeof(buff), (uintptr_t)ptr, 16);
    if (res.ec == std::errc()) {
      write(buff, res.ptr - buff);
    }

    return *this;
  }

  Stream &operator<<(const char *str) { return write(str, strlen(str)); }

  Stream &operator<<(const std::string &str) {
    return write(str.data(), str.size());
  }

  Stream &operator<<(std::string_view str) {
    return write(str.data(), str.size());
  }

  Stream &resetColor() {
    if (hasColors()) {
      write("\033[0m", 4);
    }
    return *this;
  }

  template <typename T>
  std::enable_if_t<std::is_arithmetic_v<T>, Stream &> operator<<(T val) {
    char
        buff[(std::is_floating_point_v<T> ? std::numeric_limits<T>::max_digits10
                                          : std::numeric_limits<T>::digits10) +
             0x20];

    auto result = std::to_chars(buff, buff + sizeof(buff), val);
    if (result.ec == std::errc()) {
      write(buff, result.ptr - buff);
    }

    return *this;
  }

  Stream &indent(unsigned n);

  Stream &changeColor(Color col, bool bold = false, bool bg = false) {
    if (hasColors()) {
      int code = int(col);

      if (bg) {
        code += 10;
      }

      *this << "\033[";
      if (bold)
        *this << "1;";
      *this << code << "m";
    }
    return *this;
  }

  Stream &operator<<(Color col) { return changeColor(col); }
};

/// @brief Standard FILE* stream.
class SFStream : public Stream {
  FILE *file;
  bool close;

  void internalWrite(const BufferChar *ptr, size_t size) override {
    if (file)
      fwrite(ptr, 1, size, file);
  }

  void internalFlush() override {
    if (file)
      fflush(file);
  }

public:
  /// @brief Opens a new stream to a cstdio file.
  /// @param f Stream to stream to.
  /// @param closeFile whether or not this stream should take ownership.
  /// @param bufferSize Set to 0 because most cstdio files already have one.
  SFStream(FILE *f, bool closeFile, size_t bufferSize = 0)
      : Stream(bufferSize), file(f), close(closeFile) {}

  ~SFStream() override {
    setNoBuffer();
    if (close)
      fclose(file);
  }
};

extern Stream &err();
extern Stream &out();

} // namespace zap