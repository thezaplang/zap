#include "utils/stream.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace zap {

ColorOverride color_override = ColorOverride::AUTO;

Stream::Stream(size_t bufferSize)
    : buffer_(bufferSize ? std::make_unique<BufferChar[]>(bufferSize) : nullptr),
      capacity_(bufferSize) {}

void Stream::setBufferSize(size_t bufferSize) {
    if (bufferSize == getBufferSize())
        return;

    auto buffer = bufferSize ? std::make_unique<BufferChar[]>(bufferSize) : nullptr;
    flush();
    buffer_ = std::move(buffer);
    capacity_ = bufferSize;
}

Stream& Stream::write(const BufferChar* ptr, size_t size) {
    if (!size)
        return *this;
    if (!capacity_) {
        internalWrite(ptr, size);
        return *this;
    }
    if (size >= capacity_) {
        flush();
        internalWrite(ptr, size);
        return *this;
    }

    size_t bytesLeft = size;
    while (bytesLeft) {
        if (used_ == capacity_) {
            flush();
        }
        size_t toCpy = std::min(bytesLeft, capacity_ - used_);
        std::memcpy(buffer_.get() + used_, ptr, toCpy);
        used_ += toCpy;
        ptr += toCpy;
        bytesLeft -= toCpy;
    }

    return *this;
}

struct HandleColors {
    bool stdout_color;
    bool stdout_tty;
    bool stderr_color;
    bool stderr_tty;

    HandleColors();
};

static HandleColors standard_stream_colors;

class StdoutStream : public SFStream {
public:
    using SFStream::SFStream;

    bool hasColors() const override { return standard_stream_colors.stdout_color; }

    bool onTTY() const override { return standard_stream_colors.stdout_tty; }
};

class StderrStream : public SFStream {
public:
    using SFStream::SFStream;

    bool hasColors() const override { return standard_stream_colors.stderr_color; }

    bool onTTY() const override { return standard_stream_colors.stderr_tty; }
};

Stream& err() {
    static StderrStream stderrStream(stderr, false);
    return stderrStream;
}

Stream& out() {
    static StdoutStream stdoutStream(stdout, false);
    return stdoutStream;
}

Stream& Stream::indent(unsigned n) {
    constexpr const char space_buffer[0x10] =
        {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    while (n > sizeof(space_buffer)) {
        write(space_buffer, sizeof(space_buffer));
        n -= sizeof(space_buffer);
    }
    return write(space_buffer, n);
}

/// This is where it is platform dependent.
HandleColors::HandleColors() {
    auto envTruthy = [](const char* name) -> bool {
        const char* value = std::getenv(name);
        return value && *value;
    };

    auto terminalSupportsColor = [&]() -> bool {
        if (color_override == ColorOverride::ALWAYS) {
            return true;
        }
        if (color_override == ColorOverride::NEVER) {
            return false;
        }

        if (envTruthy("NO_COLOR")) {
            return false;
        }

        const char* term = std::getenv("TERM");
        if (!term || !*term || std::strcmp(term, "dumb") == 0) {
            return false;
        }

        return true;
    };

#if defined(__unix__) || defined(__APPLE__)
    stdout_tty = ::isatty(fileno(stdout));
    stderr_tty = ::isatty(fileno(stderr));
#else
    stdout_tty = false;
    stderr_tty = false;
#endif

    bool colorsEnabled = terminalSupportsColor();
    stdout_color = stdout_tty && colorsEnabled;
    stderr_color = stderr_tty && colorsEnabled;

    if (color_override == ColorOverride::ALWAYS) {
        stdout_color = true;
        stderr_color = true;
    }
}

} // namespace zap
