#include "frontend/frontend_session.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct TemporaryDirectory {
    std::filesystem::path path;

    TemporaryDirectory() {
        const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
            / ("zap-outline-graph-" + std::to_string(suffix));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void writeFile(const std::filesystem::path& path, const std::string& source) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << source;
}

zap::frontend::FrontendSession makeSession(
    const std::filesystem::path& stdlib,
    bool includePrelude,
    bool allowEntryErrors = false,
    std::vector<std::filesystem::path>* loadedPaths = nullptr
) {
    zap::frontend::FrontendSessionConfig config{zap::frontend::RuntimePaths(
                                                    {},
                                                    {},
                                                    stdlib,
                                                    {},
                                                    zap::frontend::EnvironmentOverrides::Ignore
                                                ),
        {}};
    config.includePrelude = includePrelude;
    config.allowEntryErrors = allowEntryErrors;
    config.runtimePaths.coreDirOverride = ZAP_TEST_CORE_DIR;
    if (!stdlib.empty())
        config.runtimePaths.stdlibDirOverride = stdlib;
    return zap::frontend::FrontendSession(
        config,
        [loadedPaths](const std::filesystem::path& path) -> std::optional<std::string> {
            if (loadedPaths)
                loadedPaths->push_back(path);
            std::ifstream input(path);
            if (!input)
                return std::nullopt;
            return std::string(
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()
            );
        }
    );
}

void testImportedAndForwardMacros() {
    TemporaryDirectory temporary;
    const auto entry = temporary.path / "main.zp";
    const auto helper = temporary.path / "helper.zp";
    writeFile(entry, "import \"helper.zp\"; fun main() Int { return 1; }");
    writeFile(
        helper,
        "pub macro imported($x: expr) { $x } "
        "fun helper() Int { return 2; }"
    );

    auto session = makeSession({}, false);
    auto project = session.load(entry);
    require(project.loaded && project.modules.size() == 2, "complete import graph was not loaded");
    require(
        project.outlines.at(helper.string()).macros.size() == 1
            && project.outlines.at(helper.string()).macros[0].name.value == "imported",
        "imported macro was not retained in the module outline"
    );
    require(
        project.modules.at(entry.string())->imports.size() == 1
            && project.modules.at(entry.string())->imports[0].targetModuleIds[0] == helper.string(),
        "resolved import was not retained after full parsing"
    );
    require(session.bind(project), "discovered import graph did not bind");

    writeFile(
        entry,
        "fun main() Int { return later!(1); } "
        "macro later($x: expr) { $x } import \"helper.zp\";"
    );
    std::vector<std::filesystem::path> loadedPaths;
    auto permissive = makeSession({}, false, true, &loadedPaths);
    auto forward = permissive.load(entry);
    require(
        forward.outlines.at(entry.string()).macros.size() == 1
            && forward.outlines.at(entry.string()).macros[0].name.value == "later",
        "macro declared after its use was missing from the outline"
    );
    require(
        forward.outlines.count(helper.string()) == 1 && loadedPaths.size() == 2,
        "dependencies were not discovered before parsing the entry module"
    );
}

void testCyclesAndPrelude() {
    TemporaryDirectory temporary;
    const auto entry = temporary.path / "main.zp";
    const auto helper = temporary.path / "helper.zp";
    writeFile(entry, "import \"helper.zp\"; fun main() Int { return 1; }");
    writeFile(helper, "import \"main.zp\";");

    auto cyclic = makeSession({}, false).load(entry);
    require(
        !cyclic.loaded && !cyclic.errors.empty()
            && cyclic.errors.front().find("cyclic import") != std::string::npos,
        "cyclic import was not detected during outline discovery"
    );

    writeFile(helper, "fun helper() Int { return 2; }");
    const auto stdlib = temporary.path / "std";
    const auto prelude = stdlib / "prelude.zp";
    writeFile(prelude, "pub macro prelude_macro() {} ");
    auto withPrelude = makeSession(stdlib, true).load(entry);
    require(
        withPrelude.loaded && withPrelude.modules.size() == 3,
        "implicit prelude was not loaded into the module graph"
    );
    require(
        withPrelude.outlines.at(entry.string()).hasImportPath("std/prelude")
            && withPrelude.outlines.at(prelude.string()).macros.size() == 1,
        "prelude macro was not available in the discovered outlines"
    );
    require(
        withPrelude.macros.find(entry.string(), "prelude_macro") != nullptr,
        "implicit prelude macro was not visible without qualification"
    );
    require(
        withPrelude.modules.at(entry.string())->imports.size() == 2,
        "implicit prelude was not resolved before full parsing"
    );

    writeFile(entry, "import \"std/prelude\"; fun main() Int { return 1; }");
    auto explicitPrelude = makeSession(stdlib, true).load(entry);
    require(
        explicitPrelude.loaded && explicitPrelude.outlines.at(entry.string()).imports.size() == 1
            && explicitPrelude.modules.at(entry.string())->imports.size() == 1,
        "explicit prelude import was duplicated"
    );
}

void testMalformedImportStillDiscoversLaterDependency() {
    TemporaryDirectory temporary;
    const auto entry = temporary.path / "main.zp";
    const auto helper = temporary.path / "helper.zp";
    writeFile(entry, "import \"broken.zp\"\nimport \"helper.zp\";");
    writeFile(helper, "fun helper() Int { return 2; }");

    std::vector<std::filesystem::path> loadedPaths;
    auto session = makeSession({}, false, true, &loadedPaths);
    auto project = session.load(entry);
    require(
        project.outlines.at(entry.string()).imports.size() == 1
            && project.outlines.at(entry.string()).imports[0]->path == "helper.zp"
            && project.outlines.count(helper.string()) == 1 && loadedPaths.size() == 2,
        "malformed import prevented later dependency discovery"
    );
}

void testMacroVisibilityAliasesAndReexports() {
    TemporaryDirectory temporary;
    const auto leaf = temporary.path / "leaf.zp";
    const auto facade = temporary.path / "facade.zp";
    const auto plain = temporary.path / "plain.zp";
    const auto entry = temporary.path / "main.zp";
    writeFile(
        leaf,
        "pub macro shown() {} macro hidden() {} "
        "pub fun ordinary() Int { return 2; }"
    );
    writeFile(facade, "pub import \"leaf.zp\" as leaf { shown as renamed }; ");
    writeFile(plain, "pub import \"leaf.zp\";");
    writeFile(
        entry,
        "import \"facade.zp\" as api; "
        "import \"plain.zp\" as plain; "
        "import \"leaf.zp\" as direct { shown as local, "
        "ordinary as value }; "
        "fun local() Int { return 1; } "
        "fun main() Int { return local() + value(); }"
    );

    auto session = makeSession({}, false);
    auto project = session.load(entry);
    require(project.loaded, "macro imports did not load");
    require(
        project.macros.find(entry.string(), "local") != nullptr
            && project.macros.findQualified(entry.string(), "direct", "shown") != nullptr
            && project.macros.findQualified(entry.string(), "api", "renamed") != nullptr
            && project.macros.findQualified(entry.string(), "plain", "shown") != nullptr
            && project.macros.findQualified(entry.string(), "direct", "hidden") == nullptr
            && project.macros.find(entry.string(), "hidden") == nullptr,
        "macro visibility, aliases, or re-exports resolved incorrectly"
    );
    require(session.bind(project), "macro-only selective import interfered with runtime binding");
}

void testMacroNameConflictsAreDeterministic() {
    TemporaryDirectory temporary;
    const auto entry = temporary.path / "main.zp";
    writeFile(temporary.path / "first.zp", "pub macro same() {}");
    writeFile(temporary.path / "second.zp", "pub macro same() {}");
    writeFile(
        entry,
        "pub import \"first.zp\" { same as duplicate }; "
        "pub import \"second.zp\" { same as duplicate }; "
        "fun main() Int { return 1; }"
    );

    auto session = makeSession({}, false);
    auto project = session.load(entry);
    require(!project.loaded, "conflicting macro imports were accepted");
    size_t conflicts = 0;
    for (const auto& diagnostic : project.diagnostics) {
        if (diagnostic.message.find("Macro signature for 'duplicate' conflicts")
            != std::string::npos)
            ++conflicts;
    }
    require(conflicts == 1, "macro conflict was not reported exactly once");

    auto permissive = makeSession({}, false, true).load(entry);
    require(
        permissive.loaded && !permissive.diagnostics.empty(),
        "entry diagnostics unexpectedly prevented permissive loading"
    );
}

} // namespace

int main() {
    testImportedAndForwardMacros();
    testCyclesAndPrelude();
    testMalformedImportStillDiscoversLaterDependency();
    testMacroVisibilityAliasesAndReexports();
    testMacroNameConflictsAreDeterministic();
    return 0;
}
