#include "process.hpp"

#include <cerrno>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;
#endif

namespace zap::process {
namespace {

std::vector<char*> makeArgv(std::vector<std::string>& storage) {
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& argument : storage) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    return argv;
}

} // namespace

Result execute(const std::filesystem::path& executable, const std::vector<std::string>& arguments) {
    std::vector<std::string> argvStorage;
    argvStorage.reserve(arguments.size() + 1);
    argvStorage.push_back(executable.string());
    argvStorage.insert(argvStorage.end(), arguments.begin(), arguments.end());
    auto argv = makeArgv(argvStorage);
    const auto& executableText = argvStorage.front();

#ifdef _WIN32
    const auto status = _spawnv(_P_WAIT, executableText.c_str(), argv.data());
    if (status == -1) {
        return {Termination::LaunchFailed, 0, std::error_code(errno, std::generic_category())};
    }
    return {Termination::Exited, static_cast<int>(status), {}};
#else
    pid_t child = 0;
    const int spawnError =
        posix_spawn(&child, executableText.c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawnError != 0) {
        return {Termination::LaunchFailed, 0, std::error_code(spawnError, std::generic_category())};
    }

    int status = 0;
    while (waitpid(child, &status, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }
        return {Termination::LaunchFailed, 0, std::error_code(errno, std::generic_category())};
    }

    if (WIFEXITED(status)) {
        return {Termination::Exited, WEXITSTATUS(status), {}};
    }
    if (WIFSIGNALED(status)) {
        return {Termination::Signaled, WTERMSIG(status), {}};
    }
    return {Termination::LaunchFailed, 0, std::make_error_code(std::errc::state_not_recoverable)};
#endif
}

} // namespace zap::process
