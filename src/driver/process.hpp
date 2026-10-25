#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace zap::process {

enum class Termination {
    Exited,
    Signaled,
    LaunchFailed
};

struct Result {
    Termination termination = Termination::LaunchFailed;
    int code = 0;
    std::error_code error;

    bool succeeded() const noexcept { return termination == Termination::Exited && code == 0; }
};

Result execute(const std::filesystem::path& executable, const std::vector<std::string>& arguments);

} // namespace zap::process
