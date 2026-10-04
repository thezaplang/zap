#include "utils/stream.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class RecordingStream final : public zap::Stream {
public:
    std::string output;
    size_t flushes = 0;
    bool failWrite = false;

    explicit RecordingStream(size_t capacity)
        : Stream(capacity) {}

private:
    void internalWrite(const char* data, size_t size) override {
        if (failWrite)
            throw std::runtime_error("write failed");
        output.append(data, size);
    }

    void internalFlush() override { ++flushes; }
};

} // namespace

int main() {
    RecordingStream stream(4);
    require(
        stream.flushes == 0 && stream.getBufferSize() == 4,
        "constructor called a virtual function or lost buffer capacity"
    );
    stream.write("ab", 2);
    require(
        stream.output.empty() && stream.getBufferSize() == 4,
        "writing changed the buffer capacity"
    );
    stream.setBufferSize(4);
    require(
        stream.output.empty() && stream.flushes == 0,
        "same-size resize unnecessarily flushed the stream"
    );
    stream.write("cde", 3);
    require(stream.output == "abcd", "buffer boundary changed byte order");
    stream.write("FGHIJ", 5);
    require(stream.output == "abcdeFGHIJ", "large write lost buffered bytes");
    stream.write("k", 1);
    stream.setBufferSize(2);
    require(
        stream.output == "abcdeFGHIJk" && stream.getBufferSize() == 2,
        "resize discarded pending bytes"
    );
    stream.write("l", 1);
    stream.failWrite = true;
    try {
        stream.setBufferSize(8);
        require(false, "resize swallowed a failed flush");
    } catch (const std::runtime_error&) {
    }
    require(stream.getBufferSize() == 2, "failed resize changed buffer capacity");
    stream.failWrite = false;
    stream.setNoBuffer();
    stream.write("m", 1);
    stream.write(nullptr, 0);
    require(
        stream.output == "abcdeFGHIJklm" && stream.getBufferSize() == 0,
        "unbuffered writes or failed-resize recovery lost bytes"
    );

    RecordingStream pending(8);
    pending.write("x", 1); // Destruction releases storage without virtual dispatch.

    FILE* file = std::tmpfile();
    require(file, "could not create file fixture");
    {
        zap::SFStream fileStream(file, false, 8);
        fileStream << "saved";
    }
    std::rewind(file);
    char text[5];
    require(
        std::fread(text, 1, sizeof(text), file) == sizeof(text)
            && std::string(text, sizeof(text)) == "saved",
        "file-stream destruction did not flush pending bytes"
    );
    std::fclose(file);
}
