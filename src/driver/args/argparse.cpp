#include "argparse.hpp"
#include "args.hpp"

#include "../compiler.hpp"

namespace zap {

void printHelp() {
  out() << "Usage: zapc [options] <file>\n"
           "Options:\n";
#define ZAP_FLAG(ID, STR, MSG, ...)                                            \
  out() << "  " STR;                                                           \
  if constexpr ((2 + (sizeof(STR) - 1)) >= 18) {                               \
    (out() << '\n').indent(18) << MSG;                                         \
  } else {                                                                     \
    out().indent(18 - ((sizeof(STR) - 1) + 2)) << MSG "\n";                    \
  }
#include "flags.inc"
#undef ZAP_FLAG
}

void printVersion() { out() << "Zap Compiler v" << ZAP_VERSION << '\n'; }

namespace args {

enum class ArgTypes : unsigned {
#define ZAP_FLAG(ID, ...) ID,
#include "flags.inc"
#undef ZAP_FLAG
  ARGS_END
};

class ArgConf {
public:
  enum class ArgKind { Flag, Joined, Separate, JoinedSeparate };

private:
  ArgKind _kind;
  ArgTypes _type;

public:
  ArgConf(ArgKind kind, ArgTypes type) noexcept : _kind(kind), _type(type) {}

  ArgKind getKind() const noexcept { return _kind; }

  ArgTypes getType() const noexcept { return _type; }
};

/// Ideally use a specialized string map.
const std::unordered_map<std::string_view, ArgConf> arg_map = {{
#define ZAP_FLAG(ID, STR, MSG, KIND, ...)                                      \
  {STR, ArgConf(ArgConf::ArgKind::KIND, ArgTypes::ID)},
#include "flags.inc"
#undef ZAP_FLAG
}};

struct ArgVal {
  std::string_view original;
  std::string_view optional;
  ArgTypes type;

  bool valid() const noexcept { return original.size(); }

  ArgVal(std::string_view og, std::string_view opt, ArgTypes t) noexcept
      : original(og), optional(opt), type(t) {}
};

class ArgHolder {
  uint16_t _posArray[(unsigned)ArgTypes::ARGS_END]{};
  uint16_t _countArray[(unsigned)ArgTypes::ARGS_END]{};
  const std::vector<ArgVal> &_args;

public:
  ArgHolder(const std::vector<ArgVal> &args) : _args(args) {
    unsigned pos = 0;
    for (const ArgVal &arg : _args) {
      unsigned idx = (unsigned)arg.type;
      _posArray[idx] = pos++;
      _countArray[idx]++;
    }
  }

  unsigned count(ArgTypes t) const noexcept { return _countArray[(unsigned)t]; }

  bool has(ArgTypes t) const noexcept { return count(t); }

  const ArgVal *get(ArgTypes argT) const noexcept {
    if (!has(argT))
      return nullptr;
    return &_args[_posArray[(unsigned)argT]];
  }

  std::vector<const ArgVal *> getAll(ArgTypes argT) const noexcept {
    if (!has(argT))
      return {};
    std::vector<const ArgVal *> vec;
    for (const ArgVal &arg : _args) {
      if (arg.type == argT)
        vec.emplace_back(&arg);
    }
    return vec;
  }
};

ParseResult parse(const std::vector<std::string_view> &cmdline,
                  CmdlineArgs &args) {
  bool ok = true;
  std::vector<ArgVal> argsvec;

  for (size_t i = 1; i < cmdline.size(); i++) {
    std::string_view original(cmdline[i]);

    if (original[0] == '-') {
      if (auto it = arg_map.find(original); it != arg_map.end()) {
        const ArgConf &conf = it->second;

        if (conf.getKind() == ArgConf::ArgKind::Flag) {
          argsvec.emplace_back(original, std::string_view(), conf.getType());
        } else {
          if (i + 1 == cmdline.size()) {
            reportError("missing value for flag '", it->first, "'");
            ok = false;
            break;
          }
          argsvec.emplace_back(original, cmdline[i + 1], conf.getType());
          i++;
        }
      } else {
        const ArgConf *maybeConf = nullptr;
        size_t offset = 0;
#define ZAP_FLAG(ID, STR, MSG, KIND, ...)                                      \
  if constexpr (ArgConf::ArgKind::KIND == ArgConf::ArgKind::Joined ||          \
                ArgConf::ArgKind::KIND == ArgConf::ArgKind::JoinedSeparate) {  \
    if (original.size() >= (sizeof(STR) - 1)) {                                \
      auto it = arg_map.find(original.substr(0, (sizeof(STR) - 1)));           \
      if (it != arg_map.end()) {                                               \
        maybeConf = &it->second;                                               \
        offset = sizeof(STR) - 1;                                              \
        goto match_found;                                                      \
      }                                                                        \
    }                                                                          \
  }
#include "flags.inc"
#undef ZAP_FLAG
        reportError("unknown flag '", original, "' encountered");
        ok = false;
      match_found:
        argsvec.emplace_back(original, original.substr(offset),
                             maybeConf->getType());
      }
    } else {
      args.inputs.emplace_back(original);
    }
  }

  if (!ok)
    return ParseResult::Failed;

  ArgHolder holder(argsvec);

  if (holder.has(ArgTypes::Help)) {
    printHelp();
    return ParseResult::SkipCompilation;
  }
  if (holder.has(ArgTypes::Version)) {
    printVersion();
    return ParseResult::SkipCompilation;
  }

  args.output.implicit = !holder.has(ArgTypes::Output);
  args.incStdlib = !holder.has(ArgTypes::NoStdlib);
  args.incPrelude = !holder.has(ArgTypes::NoPrelude);

  bool emitS = holder.has(ArgTypes::CompileOnlyS);
  bool compileOnly = holder.has(ArgTypes::CompileOnly);
  bool emitLLVM = holder.has(ArgTypes::EmitLLVM);
  bool emitZIR = holder.has(ArgTypes::EmitZIR);

  if (holder.has(ArgTypes::OptLevel)) {
    std::string_view optL = holder.get(ArgTypes::OptLevel)->optional;

    if (optL == "0") {
      args.optLevel = OptLevel::O0;
    } else if (optL == "1") {
      args.optLevel = OptLevel::O1;
    } else if (optL == "2") {
      args.optLevel = OptLevel::O2;
    } else if (optL == "3") {
      args.optLevel = OptLevel::O3;
    } else {
      reportError("unknown optimization value '", optL, "' encountered");
      ok = false;
    }
  } else
    args.optLevel = OptLevel::O0;

  if (!ok)
    return ParseResult::Failed;

  for (const ArgVal *arg : holder.getAll(ArgTypes::LinkDir)) {
    std::string val = "-L";
    val += arg->optional;

    args.linkerArgs.emplace_back(std::move(val));
  }

  for (const ArgVal *arg : holder.getAll(ArgTypes::LinkLib)) {
    std::string val = "-l";
    val += arg->optional;

    args.linkerArgs.emplace_back(std::move(val));
  }

  for (const ArgVal *arg : holder.getAll(ArgTypes::ImportMap)) {
    std::string_view entry = arg->optional;
    auto eq = entry.find('=');
    if (eq == std::string_view::npos) {
      reportError("--import-map requires format @alias=path, got: ", entry);
      ok = false;
      continue;
    }
    args.importMap[std::string(entry.substr(0, eq))] =
        std::string(entry.substr(eq + 1));
  }

  if (!ok)
    return ParseResult::Failed;

  if (emitZIR) {
    args.output.type = OutputType::ZIR;
  } else if (emitLLVM) {
    args.output.type = emitS ? OutputType::TEXT_LLVM : OutputType::LLVM;
  } else if (emitS) {
    args.output.type = OutputType::ASM;
  } else if (compileOnly) {
    args.output.type = OutputType::OBJECT;
  }

  if ((emitLLVM || emitZIR) && !args.output.implicit && (emitLLVM == emitZIR)) {
    reportError("cannot use -o when emitting multiple text outputs");
    return ParseResult::Failed;
  }

  args.output.path = holder.has(ArgTypes::Output)
                         ? holder.get(ArgTypes::Output)->optional
                         : "a.out";

  if (args.inputs.empty()) {
    reportError("no input files");
    return ParseResult::Failed;
  }

  return ParseResult::Success;
}

ParseResult parse(int argc, char **argv, CmdlineArgs &args) {
  std::vector<std::string_view> views;
  views.reserve(argc);

  for (int i = 0; i < argc; ++i) {
    views.emplace_back(argv[i]);
  }

  return parse(views, args);
}

ParseResult parse(const std::vector<std::string> &argv, CmdlineArgs &args) {
  std::vector<std::string_view> views;
  views.reserve(argv.size());

  for (const auto &arg : argv) {
    views.emplace_back(arg);
  }

  return parse(views, args);
}

} // namespace args
} // namespace zap
