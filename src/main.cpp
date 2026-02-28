#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
extern char** environ;
#endif

#include "frontend.h"
#include "llvm_backend.h"
#include "repl.h"
#include "version.h"

namespace {

enum class JitBackendKind {
  kLlvm,
};

void print_usage() {
  std::cout << "holyc <command> [options]\n"
            << "\n"
            << "Commands:\n"
            << "  --version            Print compiler version\n"
            << "  --print-strict-mode  Print strict-mode default\n"
            << "  check <file> [--mode=jit|aot] [--strict|--permissive]\n"
            << "                       Parse + semantic-check only\n"
            << "  preprocess <file> [--mode=jit|aot] [--strict|--permissive]\n"
            << "                       Print preprocessed HolyC\n"
            << "  ast-dump <file> [--mode=jit|aot] [--strict|--permissive]\n"
            << "                       Parse HolyC and print AST\n"
            << "  emit-hir <file> [--mode=jit|aot] [--strict|--permissive]\n"
            << "                       Emit lowered HIR dump\n"
            << "  emit-llvm <file> [--mode=jit|aot] [--strict|--permissive]\n"
            << "                       Emit textual LLVM IR\n"
            << "  jit <file> [--strict|--permissive] [--jit-backend=llvm]\n"
            << "            [--jit-session=<name>] [--jit-reset] [--opt-level=0|1|2|3|s|z]\n"
            << "                       Execute supported subset in-process\n"
            << "  repl [--strict|--permissive] [--jit-session=<name>] [--jit-reset]\n"
            << "       [--opt-level=0|1|2|3|s|z]\n"
            << "                       Start interactive JIT-backed HolyC REPL\n"
            << "  build <file> [-o out] [--target=<triple>] [--artifact-dir=<dir>]\n"
            << "               [--keep-temps] [--strict|--permissive] [--opt-level=0|1|2|3|s|z]\n"
            << "                       Build executable via host toolchain/LLVM\n"
            << "  run <file> [--target=<triple>] [--artifact-dir=<dir>] [--keep-temps]\n"
            << "            [--strict|--permissive] [--opt-level=0|1|2|3|s|z]\n"
            << "                       Build and run executable\n";
}

bool ReadFile(std::string_view path, std::string* out) {
  std::ifstream input{std::string(path)};
  if (!input.is_open()) {
    return false;
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  *out = buffer.str();
  return true;
}

bool WriteFile(std::string_view path, std::string_view contents) {
  std::ofstream output{std::string(path)};
  if (!output.is_open()) {
    return false;
  }
  output << contents;
  return output.good();
}

#if !defined(__unix__) && !defined(__APPLE__)
std::string ShellQuote(std::string_view value) {
  std::string out;
  out.push_back('\'');
  for (char c : value) {
    if (c == '\'') {
      out.append("'\\''");
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}
#endif

int RunProgram(const std::vector<std::string>& args, std::string* error_out = nullptr) {
  if (args.empty()) {
    if (error_out != nullptr) {
      *error_out = "empty command";
    }
    return -1;
  }

#if defined(__unix__) || defined(__APPLE__)
  std::vector<char*> argv;
  argv.resize(args.size());
  std::transform(args.begin(), args.end(), argv.begin(), [](const std::string& arg) {
    return const_cast<char*>(arg.c_str());
  });
  argv.push_back(nullptr);

  pid_t pid = 0;
  const int spawn_rc = posix_spawnp(&pid, args[0].c_str(), nullptr, nullptr, argv.data(), environ);
  if (spawn_rc != 0) {
    if (error_out != nullptr) {
      *error_out = "failed to spawn " + args[0] + ": " + std::strerror(spawn_rc);
    }
    return -1;
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    if (error_out != nullptr) {
      *error_out = "failed waiting for " + args[0] + ": " + std::strerror(errno);
    }
    return -1;
  }

  if (WIFEXITED(status)) {
    const int exit_code = WEXITSTATUS(status);
    if (exit_code != 0 && error_out != nullptr) {
      *error_out = args[0] + " exited with status " + std::to_string(exit_code);
    }
    return exit_code;
  }
  if (WIFSIGNALED(status)) {
    if (error_out != nullptr) {
      *error_out = args[0] + " terminated by signal " + std::to_string(WTERMSIG(status));
    }
    return -1;
  }
  if (error_out != nullptr) {
    *error_out = args[0] + " failed unexpectedly";
  }
  return -1;
#else
  std::string cmd;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i > 0) {
      cmd.push_back(' ');
    }
    cmd.append(ShellQuote(args[i]));
  }
  const int rc = std::system(cmd.c_str());
  if (rc != 0 && error_out != nullptr) {
    *error_out = args[0] + " exited with non-zero status";
  }
  return rc;
#endif
}

std::string BasenameNoExt(std::string_view path) {
  const std::string p(path);
  const std::size_t slash = p.find_last_of("/\\");
  const std::string file = (slash == std::string::npos) ? p : p.substr(slash + 1);
  const std::size_t dot = file.find_last_of('.');
  if (dot == std::string::npos) {
    return file;
  }
  return file.substr(0, dot);
}

std::string ArtifactBaseName(std::string_view output_path) {
  const std::filesystem::path output{std::string(output_path)};
  const std::string filename = output.filename().string();
  if (!filename.empty()) {
    return filename;
  }
  return BasenameNoExt(output_path);
}

bool TryParseModeArg(std::string_view arg, holyc::frontend::ExecutionMode* mode_out,
                     std::string* error) {
  constexpr std::string_view prefix = "--mode=";
  if (arg.substr(0, prefix.size()) != prefix) {
    return false;
  }

  const std::string_view value = arg.substr(prefix.size());
  if (value == "jit") {
    *mode_out = holyc::frontend::ExecutionMode::kJit;
    return true;
  }
  if (value == "aot") {
    *mode_out = holyc::frontend::ExecutionMode::kAot;
    return true;
  }

  *error = "error: invalid --mode value (expected jit or aot): " + std::string(value);
  return true;
}

bool TryParseStrictArg(std::string_view arg, bool* strict_mode_out) {
  if (arg == "--strict") {
    *strict_mode_out = true;
    return true;
  }
  if (arg == "--permissive") {
    *strict_mode_out = false;
    return true;
  }
  return false;
}

bool TryParseTargetArg(std::string_view arg, std::string* target_out) {
  constexpr std::string_view prefix = "--target=";
  if (arg.substr(0, prefix.size()) != prefix) {
    return false;
  }
  *target_out = std::string(arg.substr(prefix.size()));
  return true;
}

bool TryParseArtifactDirArg(std::string_view arg, std::string* artifact_dir_out) {
  constexpr std::string_view prefix = "--artifact-dir=";
  if (arg.substr(0, prefix.size()) != prefix) {
    return false;
  }
  *artifact_dir_out = std::string(arg.substr(prefix.size()));
  return true;
}

bool TryParseJitSessionArg(std::string_view arg, std::string* session_out) {
  constexpr std::string_view prefix = "--jit-session=";
  if (arg.substr(0, prefix.size()) != prefix) {
    return false;
  }
  *session_out = std::string(arg.substr(prefix.size()));
  return true;
}

bool TryParseJitBackendArg(std::string_view arg, JitBackendKind* backend_out,
                           std::string* error) {
  constexpr std::string_view prefix = "--jit-backend=";
  if (arg.substr(0, prefix.size()) != prefix) {
    return false;
  }

  const std::string_view value = arg.substr(prefix.size());
  if (value == "llvm") {
    *backend_out = JitBackendKind::kLlvm;
    return true;
  }

  *error = "error: invalid --jit-backend value (expected llvm): " +
           std::string(value);
  return true;
}

bool TryParseOptLevelArg(std::string_view arg, holyc::llvm_backend::OptLevel* opt_level_out,
                         std::string* error) {
  constexpr std::string_view prefix = "--opt-level=";
  if (arg.substr(0, prefix.size()) != prefix) {
    return false;
  }

  const std::string_view value = arg.substr(prefix.size());
  if (value == "0") {
    *opt_level_out = holyc::llvm_backend::OptLevel::kO0;
    return true;
  }
  if (value == "1") {
    *opt_level_out = holyc::llvm_backend::OptLevel::kO1;
    return true;
  }
  if (value == "2") {
    *opt_level_out = holyc::llvm_backend::OptLevel::kO2;
    return true;
  }
  if (value == "3") {
    *opt_level_out = holyc::llvm_backend::OptLevel::kO3;
    return true;
  }
  if (value == "s") {
    *opt_level_out = holyc::llvm_backend::OptLevel::kOs;
    return true;
  }
  if (value == "z") {
    *opt_level_out = holyc::llvm_backend::OptLevel::kOz;
    return true;
  }

  *error = "error: invalid --opt-level value (expected 0|1|2|3|s|z): " +
           std::string(value);
  return true;
}

bool TryParseTimePhasesArg(std::string_view arg, bool* enabled_out,
                           std::string* json_path_out, std::string* error) {
  if (arg == "--time-phases") {
    *enabled_out = true;
    return true;
  }
  constexpr std::string_view prefix = "--time-phases-json=";
  if (arg.substr(0, prefix.size()) != prefix) {
    return false;
  }
  const std::string_view value = arg.substr(prefix.size());
  if (value.empty()) {
    *error = "error: --time-phases-json requires a non-empty file path";
    return true;
  }
  *enabled_out = true;
  *json_path_out = std::string(value);
  return true;
}

template <typename Fn>
auto RunTimedPhase(std::vector<holyc::frontend::PhaseTiming>* phase_timings,
                   std::string_view phase_name, Fn&& fn) -> decltype(fn()) {
  const auto start = std::chrono::steady_clock::now();
  auto result = fn();
  const auto end = std::chrono::steady_clock::now();
  if (phase_timings != nullptr) {
    phase_timings->push_back(holyc::frontend::PhaseTiming{
        std::string(phase_name),
        std::chrono::duration<double>(end - start).count(),
    });
  }
  return result;
}

std::string EscapeJson(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

void PrintPhaseTimings(std::string_view command,
                       const std::vector<holyc::frontend::PhaseTiming>& phase_timings) {
  if (phase_timings.empty()) {
    return;
  }
  std::cerr << "phase timings [" << command << "]\n";
  for (const holyc::frontend::PhaseTiming& phase : phase_timings) {
    std::cerr << "  " << std::setw(24) << std::left << phase.name << " "
              << std::fixed << std::setprecision(6) << phase.seconds << " s\n";
  }
}

bool WritePhaseTimingsJson(std::string_view output_path, std::string_view command,
                           const std::vector<holyc::frontend::PhaseTiming>& phase_timings,
                           std::string* error_out) {
  std::error_code ec;
  const std::filesystem::path json_path{std::string(output_path)};
  if (!json_path.parent_path().empty()) {
    std::filesystem::create_directories(json_path.parent_path(), ec);
    if (ec) {
      if (error_out != nullptr) {
        *error_out = "failed to create timing output directory: " + json_path.parent_path().string();
      }
      return false;
    }
  }

  std::ofstream out(json_path);
  if (!out.is_open()) {
    if (error_out != nullptr) {
      *error_out = "failed to open timing output file: " + std::string(output_path);
    }
    return false;
  }

  out << "{\n"
      << "  \"command\": \"" << EscapeJson(command) << "\",\n"
      << "  \"phases\": [\n";
  for (std::size_t i = 0; i < phase_timings.size(); ++i) {
    const holyc::frontend::PhaseTiming& phase = phase_timings[i];
    out << "    {\"name\":\"" << EscapeJson(phase.name) << "\",\"seconds\":"
        << std::fixed << std::setprecision(9) << phase.seconds << "}";
    if (i + 1 < phase_timings.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n"
      << "}\n";

  if (!out.good()) {
    if (error_out != nullptr) {
      *error_out = "failed writing timing output file: " + std::string(output_path);
    }
    return false;
  }
  return true;
}

void MaybeReportPhaseTimings(std::string_view command, bool enabled,
                             std::string_view json_path,
                             const std::vector<holyc::frontend::PhaseTiming>& phase_timings) {
  if (!enabled) {
    return;
  }
  PrintPhaseTimings(command, phase_timings);
  if (!json_path.empty()) {
    std::string error;
    if (!WritePhaseTimingsJson(json_path, command, phase_timings, &error)) {
      std::cerr << "warning: " << error << "\n";
    }
  }
}

int BuildExecutable(std::string_view input_path, std::string_view output_path,
                    std::string_view artifact_dir, std::string_view target_triple,
                    bool strict_mode, bool keep_temps,
                    holyc::llvm_backend::OptLevel opt_level,
                    std::vector<holyc::frontend::PhaseTiming>* phase_timings = nullptr) {
  std::string input_text;
  const bool read_ok = RunTimedPhase(phase_timings, "read-source",
                                     [&]() { return ReadFile(input_path, &input_text); });
  if (!read_ok) {
    std::cerr << "error: cannot read file: " << input_path << "\n";
    return 2;
  }

  const holyc::frontend::ParseResult ir =
      holyc::frontend::EmitLlvmIr(input_text, input_path, holyc::frontend::ExecutionMode::kAot,
                                  strict_mode, phase_timings);
  if (!ir.ok) {
    std::cerr << ir.output << "\n";
    return 1;
  }

  std::error_code ec;
  std::filesystem::create_directories(std::string(artifact_dir), ec);
  if (ec) {
    std::cerr << "error: cannot create artifact directory: " << artifact_dir << "\n";
    return 2;
  }

  const std::string artifact_base =
      (std::filesystem::path(std::string(artifact_dir)) / ArtifactBaseName(output_path)).string();
  const std::string ll_path = artifact_base + ".ll";
  const bool write_ir_ok =
      RunTimedPhase(phase_timings, "write-llvm-ir", [&]() { return WriteFile(ll_path, ir.output); });
  if (!write_ir_ok) {
    std::cerr << "error: cannot write LLVM IR file: " << ll_path << "\n";
    return 2;
  }

  const holyc::llvm_backend::Result build_result = RunTimedPhase(
      phase_timings, "aot-codegen-link",
      [&]() {
        return holyc::llvm_backend::BuildExecutableFromIr(ir.output, output_path, artifact_dir,
                                                          target_triple, opt_level);
      });
  if (!build_result.ok) {
    std::cerr << "error: " << build_result.output << "\n";
    return 1;
  }
  if (!keep_temps) {
    std::filesystem::remove(ll_path, ec);
    const std::string obj_path = artifact_base + ".o";
    std::filesystem::remove(obj_path, ec);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc <= 1) {
    print_usage();
    return 0;
  }

  constexpr bool kStrictModeDefault = true;
  constexpr holyc::llvm_backend::OptLevel kBuildOptDefault = holyc::llvm_backend::OptLevel::kO2;
  constexpr holyc::llvm_backend::OptLevel kJitOptDefault = holyc::llvm_backend::OptLevel::kO2;
  constexpr holyc::llvm_backend::OptLevel kReplOptDefault = holyc::llvm_backend::OptLevel::kO1;

  const std::string_view arg1 = argv[1];
  if (arg1 == "--version") {
    std::cout << "holyc " << HOLYC_VERSION;
#ifdef HOLYC_HAS_LLVM
    std::cout << " (llvm-enabled)";
#endif
    std::cout << "\n";
    return 0;
  }

  if (arg1 == "--print-strict-mode") {
    std::cout << (kStrictModeDefault ? "strict" : "permissive") << "\n";
    return 0;
  }

  if (arg1 == "check") {
    if (argc < 3) {
      std::cerr << "error: check requires an input file\n";
      return 2;
    }

    holyc::frontend::ExecutionMode mode = holyc::frontend::ExecutionMode::kJit;
    bool strict_mode = kStrictModeDefault;
    bool time_phases = false;
    std::string time_phases_json;
    for (int i = 3; i < argc; ++i) {
      const std::string_view arg = argv[i];
      std::string mode_err;
      if (TryParseModeArg(arg, &mode, &mode_err)) {
        if (!mode_err.empty()) {
          std::cerr << mode_err << "\n";
          return 2;
        }
        continue;
      }
      if (TryParseStrictArg(arg, &strict_mode)) {
        continue;
      }
      std::string phase_err;
      if (TryParseTimePhasesArg(arg, &time_phases, &time_phases_json, &phase_err)) {
        if (!phase_err.empty()) {
          std::cerr << phase_err << "\n";
          return 2;
        }
        continue;
      }
      std::cerr << "error: unknown check argument: " << arg << "\n";
      return 2;
    }

    const std::string_view input_path = argv[2];
    std::string input_text;
    std::vector<holyc::frontend::PhaseTiming> phase_timings;
    std::vector<holyc::frontend::PhaseTiming>* phase_out =
        time_phases ? &phase_timings : nullptr;
    const bool read_ok =
        RunTimedPhase(phase_out, "read-source", [&]() { return ReadFile(input_path, &input_text); });
    if (!read_ok) {
      std::cerr << "error: cannot read file: " << input_path << "\n";
      return 2;
    }

    const holyc::frontend::ParseResult result =
        holyc::frontend::CheckSource(input_text, input_path, mode, strict_mode, phase_out);
    MaybeReportPhaseTimings("check", time_phases, time_phases_json, phase_timings);
    if (!result.ok) {
      std::cerr << result.output << "\n";
      return 1;
    }

    std::cout << result.output;
    return 0;
  }

  if (arg1 == "ast-dump") {
    if (argc < 3) {
      std::cerr << "error: ast-dump requires an input file\n";
      return 2;
    }

    holyc::frontend::ExecutionMode mode = holyc::frontend::ExecutionMode::kJit;
    bool strict_mode = kStrictModeDefault;
    bool time_phases = false;
    std::string time_phases_json;
    for (int i = 3; i < argc; ++i) {
      const std::string_view arg = argv[i];
      std::string mode_err;
      if (TryParseModeArg(arg, &mode, &mode_err)) {
        if (!mode_err.empty()) {
          std::cerr << mode_err << "\n";
          return 2;
        }
        continue;
      }
      if (TryParseStrictArg(arg, &strict_mode)) {
        continue;
      }
      std::string phase_err;
      if (TryParseTimePhasesArg(arg, &time_phases, &time_phases_json, &phase_err)) {
        if (!phase_err.empty()) {
          std::cerr << phase_err << "\n";
          return 2;
        }
        continue;
      }
      std::cerr << "error: unknown ast-dump argument: " << arg << "\n";
      return 2;
    }

    const std::string_view input_path = argv[2];
    std::string input_text;
    std::vector<holyc::frontend::PhaseTiming> phase_timings;
    std::vector<holyc::frontend::PhaseTiming>* phase_out =
        time_phases ? &phase_timings : nullptr;
    const bool read_ok =
        RunTimedPhase(phase_out, "read-source", [&]() { return ReadFile(input_path, &input_text); });
    if (!read_ok) {
      std::cerr << "error: cannot read file: " << input_path << "\n";
      return 2;
    }

    const holyc::frontend::ParseResult result =
        holyc::frontend::ParseAndDumpAst(input_text, input_path, mode, strict_mode, phase_out);
    MaybeReportPhaseTimings("ast-dump", time_phases, time_phases_json, phase_timings);
    if (!result.ok) {
      std::cerr << result.output << "\n";
      return 1;
    }

    std::cout << result.output;
    return 0;
  }

  if (arg1 == "preprocess") {
    if (argc < 3) {
      std::cerr << "error: preprocess requires an input file\n";
      return 2;
    }

    holyc::frontend::ExecutionMode mode = holyc::frontend::ExecutionMode::kJit;
    bool strict_mode = kStrictModeDefault;
    bool time_phases = false;
    std::string time_phases_json;
    for (int i = 3; i < argc; ++i) {
      const std::string_view arg = argv[i];
      std::string mode_err;
      if (TryParseModeArg(arg, &mode, &mode_err)) {
        if (!mode_err.empty()) {
          std::cerr << mode_err << "\n";
          return 2;
        }
        continue;
      }
      if (TryParseStrictArg(arg, &strict_mode)) {
        continue;
      }
      std::string phase_err;
      if (TryParseTimePhasesArg(arg, &time_phases, &time_phases_json, &phase_err)) {
        if (!phase_err.empty()) {
          std::cerr << phase_err << "\n";
          return 2;
        }
        continue;
      }
      std::cerr << "error: unknown preprocess argument: " << arg << "\n";
      return 2;
    }

    const std::string_view input_path = argv[2];
    std::string input_text;
    std::vector<holyc::frontend::PhaseTiming> phase_timings;
    std::vector<holyc::frontend::PhaseTiming>* phase_out =
        time_phases ? &phase_timings : nullptr;
    const bool read_ok =
        RunTimedPhase(phase_out, "read-source", [&]() { return ReadFile(input_path, &input_text); });
    if (!read_ok) {
      std::cerr << "error: cannot read file: " << input_path << "\n";
      return 2;
    }

    const holyc::frontend::ParseResult result = holyc::frontend::PreprocessSource(
        input_text, input_path, mode, strict_mode, phase_out);
    MaybeReportPhaseTimings("preprocess", time_phases, time_phases_json, phase_timings);
    if (!result.ok) {
      std::cerr << result.output << "\n";
      return 1;
    }

    std::cout << result.output;
    return 0;
  }

  if (arg1 == "emit-hir") {
    if (argc < 3) {
      std::cerr << "error: emit-hir requires an input file\n";
      return 2;
    }

    holyc::frontend::ExecutionMode mode = holyc::frontend::ExecutionMode::kJit;
    bool strict_mode = kStrictModeDefault;
    bool time_phases = false;
    std::string time_phases_json;
    for (int i = 3; i < argc; ++i) {
      const std::string_view arg = argv[i];
      std::string mode_err;
      if (TryParseModeArg(arg, &mode, &mode_err)) {
        if (!mode_err.empty()) {
          std::cerr << mode_err << "\n";
          return 2;
        }
        continue;
      }
      if (TryParseStrictArg(arg, &strict_mode)) {
        continue;
      }
      std::string phase_err;
      if (TryParseTimePhasesArg(arg, &time_phases, &time_phases_json, &phase_err)) {
        if (!phase_err.empty()) {
          std::cerr << phase_err << "\n";
          return 2;
        }
        continue;
      }
      std::cerr << "error: unknown emit-hir argument: " << arg << "\n";
      return 2;
    }

    const std::string_view input_path = argv[2];
    std::string input_text;
    std::vector<holyc::frontend::PhaseTiming> phase_timings;
    std::vector<holyc::frontend::PhaseTiming>* phase_out =
        time_phases ? &phase_timings : nullptr;
    const bool read_ok =
        RunTimedPhase(phase_out, "read-source", [&]() { return ReadFile(input_path, &input_text); });
    if (!read_ok) {
      std::cerr << "error: cannot read file: " << input_path << "\n";
      return 2;
    }

    const holyc::frontend::ParseResult result =
        holyc::frontend::EmitHir(input_text, input_path, mode, strict_mode, phase_out);
    MaybeReportPhaseTimings("emit-hir", time_phases, time_phases_json, phase_timings);
    if (!result.ok) {
      std::cerr << result.output << "\n";
      return 1;
    }

    std::cout << result.output;
    return 0;
  }

  if (arg1 == "emit-llvm") {
    if (argc < 3) {
      std::cerr << "error: emit-llvm requires an input file\n";
      return 2;
    }

    holyc::frontend::ExecutionMode mode = holyc::frontend::ExecutionMode::kAot;
    bool strict_mode = kStrictModeDefault;
    bool time_phases = false;
    std::string time_phases_json;
    for (int i = 3; i < argc; ++i) {
      const std::string_view arg = argv[i];
      std::string mode_err;
      if (TryParseModeArg(arg, &mode, &mode_err)) {
        if (!mode_err.empty()) {
          std::cerr << mode_err << "\n";
          return 2;
        }
        continue;
      }
      if (TryParseStrictArg(arg, &strict_mode)) {
        continue;
      }
      std::string phase_err;
      if (TryParseTimePhasesArg(arg, &time_phases, &time_phases_json, &phase_err)) {
        if (!phase_err.empty()) {
          std::cerr << phase_err << "\n";
          return 2;
        }
        continue;
      }
      std::cerr << "error: unknown emit-llvm argument: " << arg << "\n";
      return 2;
    }

    const std::string_view input_path = argv[2];
    std::string input_text;
    std::vector<holyc::frontend::PhaseTiming> phase_timings;
    std::vector<holyc::frontend::PhaseTiming>* phase_out =
        time_phases ? &phase_timings : nullptr;
    const bool read_ok =
        RunTimedPhase(phase_out, "read-source", [&]() { return ReadFile(input_path, &input_text); });
    if (!read_ok) {
      std::cerr << "error: cannot read file: " << input_path << "\n";
      return 2;
    }

    const holyc::frontend::ParseResult result = holyc::frontend::EmitLlvmIr(
        input_text, input_path, mode, strict_mode, phase_out);
    if (!result.ok) {
      MaybeReportPhaseTimings("emit-llvm", time_phases, time_phases_json, phase_timings);
      std::cerr << result.output << "\n";
      return 1;
    }

    const holyc::llvm_backend::Result normalized = RunTimedPhase(
        phase_out, "llvm-normalize",
        [&]() { return holyc::llvm_backend::NormalizeIr(result.output); });
    MaybeReportPhaseTimings("emit-llvm", time_phases, time_phases_json, phase_timings);
    if (!normalized.ok) {
      std::cerr << normalized.output << "\n";
      return 1;
    }
    std::cout << normalized.output;
    return 0;
  }

  if (arg1 == "jit") {
    if (argc < 3) {
      std::cerr << "error: jit requires an input file\n";
      return 2;
    }

    bool strict_mode = kStrictModeDefault;
    std::string jit_session;
    bool jit_reset = false;
    bool reset_after_run = true;
    JitBackendKind jit_backend = JitBackendKind::kLlvm;
    holyc::llvm_backend::OptLevel opt_level = kJitOptDefault;
    bool time_phases = false;
    std::string time_phases_json;
    for (int i = 3; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (TryParseStrictArg(arg, &strict_mode)) {
        continue;
      }
      std::string opt_level_error;
      if (TryParseOptLevelArg(arg, &opt_level, &opt_level_error)) {
        if (!opt_level_error.empty()) {
          std::cerr << opt_level_error << "\n";
          return 2;
        }
        continue;
      }
      std::string phase_err;
      if (TryParseTimePhasesArg(arg, &time_phases, &time_phases_json, &phase_err)) {
        if (!phase_err.empty()) {
          std::cerr << phase_err << "\n";
          return 2;
        }
        continue;
      }
      std::string backend_error;
      if (TryParseJitBackendArg(arg, &jit_backend, &backend_error)) {
        if (!backend_error.empty()) {
          std::cerr << backend_error << "\n";
          return 2;
        }
        continue;
      }
      if (TryParseJitSessionArg(arg, &jit_session)) {
        reset_after_run = false;
        continue;
      }
      if (arg == "--jit-reset") {
        jit_reset = true;
        continue;
      }
      std::cerr << "error: unknown jit argument: " << arg << "\n";
      return 2;
    }
    if (jit_backend != JitBackendKind::kLlvm) {
      std::cerr << "error: unsupported jit backend\n";
      return 2;
    }

    const std::string_view input_path = argv[2];
    std::string input_text;
    std::vector<holyc::frontend::PhaseTiming> phase_timings;
    std::vector<holyc::frontend::PhaseTiming>* phase_out =
        time_phases ? &phase_timings : nullptr;
    const bool read_ok =
        RunTimedPhase(phase_out, "read-source", [&]() { return ReadFile(input_path, &input_text); });
    if (!read_ok) {
      std::cerr << "error: cannot read file: " << input_path << "\n";
      return 2;
    }

    if (jit_reset) {
      const holyc::llvm_backend::Result reset_result = RunTimedPhase(
          phase_out, "jit-session-reset",
          [&]() { return holyc::llvm_backend::ResetJitSession(jit_session); });
      if (!reset_result.ok) {
        MaybeReportPhaseTimings("jit", time_phases, time_phases_json, phase_timings);
        std::cerr << reset_result.output << "\n";
        return 1;
      }
    }

    const holyc::frontend::ParseResult ir_result = holyc::frontend::EmitLlvmIr(
        input_text, input_path, holyc::frontend::ExecutionMode::kJit, strict_mode, phase_out);
    if (!ir_result.ok) {
      MaybeReportPhaseTimings("jit", time_phases, time_phases_json, phase_timings);
      std::cerr << ir_result.output << "\n";
      return 1;
    }

    const holyc::llvm_backend::Result result = RunTimedPhase(
        phase_out, "jit-exec",
        [&]() {
          return holyc::llvm_backend::ExecuteIrJit(ir_result.output, jit_session, reset_after_run,
                                                   "main", opt_level);
        });
    MaybeReportPhaseTimings("jit", time_phases, time_phases_json, phase_timings);
    if (!result.ok) {
      std::cerr << result.output << "\n";
      return 1;
    }
    std::cout << result.output;
    return 0;
  }

  if (arg1 == "repl") {
    bool strict_mode = kStrictModeDefault;
    std::string jit_session = "__repl__";
    bool jit_reset = false;
    holyc::llvm_backend::OptLevel opt_level = kReplOptDefault;
    for (int i = 2; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (TryParseStrictArg(arg, &strict_mode)) {
        continue;
      }
      std::string opt_level_error;
      if (TryParseOptLevelArg(arg, &opt_level, &opt_level_error)) {
        if (!opt_level_error.empty()) {
          std::cerr << opt_level_error << "\n";
          return 2;
        }
        continue;
      }
      if (TryParseJitSessionArg(arg, &jit_session)) {
        continue;
      }
      if (arg == "--jit-reset") {
        jit_reset = true;
        continue;
      }
      std::cerr << "error: unknown repl argument: " << arg << "\n";
      return 2;
    }
    return holyc::repl::RunRepl(strict_mode, jit_session, jit_reset, opt_level);
  }

  if (arg1 == "build") {
    if (argc < 3) {
      std::cerr << "error: build requires an input file\n";
      return 2;
    }

    const std::string_view input_path = argv[2];
    std::string output_path = BasenameNoExt(input_path);
    std::string artifact_dir = ".holyc-artifacts";
    std::string target_triple;
    bool strict_mode = kStrictModeDefault;
    bool keep_temps = false;
    holyc::llvm_backend::OptLevel opt_level = kBuildOptDefault;
    bool time_phases = false;
    std::string time_phases_json;

    for (int i = 3; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (arg == "-o") {
        if (i + 1 >= argc) {
          std::cerr << "error: -o requires an output path\n";
          return 2;
        }
        output_path = argv[i + 1];
        ++i;
        continue;
      }
      if (TryParseTargetArg(arg, &target_triple)) {
        continue;
      }
      if (TryParseArtifactDirArg(arg, &artifact_dir)) {
        continue;
      }
      if (TryParseStrictArg(arg, &strict_mode)) {
        continue;
      }
      std::string opt_level_error;
      if (TryParseOptLevelArg(arg, &opt_level, &opt_level_error)) {
        if (!opt_level_error.empty()) {
          std::cerr << opt_level_error << "\n";
          return 2;
        }
        continue;
      }
      std::string phase_err;
      if (TryParseTimePhasesArg(arg, &time_phases, &time_phases_json, &phase_err)) {
        if (!phase_err.empty()) {
          std::cerr << phase_err << "\n";
          return 2;
        }
        continue;
      }
      if (arg == "--keep-temps") {
        keep_temps = true;
        continue;
      }

      std::cerr << "error: unknown build argument: " << arg << "\n";
      return 2;
    }

    std::vector<holyc::frontend::PhaseTiming> phase_timings;
    const int rc = BuildExecutable(input_path, output_path, artifact_dir, target_triple,
                                   strict_mode, keep_temps, opt_level,
                                   time_phases ? &phase_timings : nullptr);
    MaybeReportPhaseTimings("build", time_phases, time_phases_json, phase_timings);
    if (rc == 0) {
      std::cout << "built " << output_path << "\n";
    }
    return rc;
  }

  if (arg1 == "run") {
    if (argc < 3) {
      std::cerr << "error: run requires an input file\n";
      return 2;
    }

    const std::string_view input_path = argv[2];
    std::string artifact_dir = ".holyc-artifacts";
    std::string target_triple;
    bool strict_mode = kStrictModeDefault;
    bool keep_temps = false;
    holyc::llvm_backend::OptLevel opt_level = kBuildOptDefault;
    bool time_phases = false;
    std::string time_phases_json;
    std::string output_path =
        (std::filesystem::path(artifact_dir) / (BasenameNoExt(input_path) + ".run")).string();

    for (int i = 3; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (arg == "-o") {
        if (i + 1 >= argc) {
          std::cerr << "error: -o requires an output path\n";
          return 2;
        }
        output_path = argv[i + 1];
        ++i;
        continue;
      }
      if (TryParseTargetArg(arg, &target_triple)) {
        continue;
      }
      if (TryParseArtifactDirArg(arg, &artifact_dir)) {
        output_path =
            (std::filesystem::path(artifact_dir) / (BasenameNoExt(input_path) + ".run")).string();
        continue;
      }
      if (TryParseStrictArg(arg, &strict_mode)) {
        continue;
      }
      std::string opt_level_error;
      if (TryParseOptLevelArg(arg, &opt_level, &opt_level_error)) {
        if (!opt_level_error.empty()) {
          std::cerr << opt_level_error << "\n";
          return 2;
        }
        continue;
      }
      std::string phase_err;
      if (TryParseTimePhasesArg(arg, &time_phases, &time_phases_json, &phase_err)) {
        if (!phase_err.empty()) {
          std::cerr << phase_err << "\n";
          return 2;
        }
        continue;
      }
      if (arg == "--keep-temps") {
        keep_temps = true;
        continue;
      }

      std::cerr << "error: unknown run argument: " << arg << "\n";
      return 2;
    }

    std::vector<holyc::frontend::PhaseTiming> phase_timings;
    std::vector<holyc::frontend::PhaseTiming>* phase_out =
        time_phases ? &phase_timings : nullptr;
    const int rc = BuildExecutable(input_path, output_path, artifact_dir, target_triple,
                                   strict_mode, keep_temps, opt_level, phase_out);
    if (rc != 0) {
      MaybeReportPhaseTimings("run", time_phases, time_phases_json, phase_timings);
      return rc;
    }

    std::string run_error;
    const int run_rc = RunTimedPhase(
        phase_out, "run-program",
        [&]() {
          return RunProgram({output_path}, &run_error);
        });
    MaybeReportPhaseTimings("run", time_phases, time_phases_json, phase_timings);
    if (run_rc != 0) {
      std::cerr << "error: executed program failed with status " << run_rc << " ("
                << run_error << ")\n";
      return 1;
    }

    if (!keep_temps) {
      std::error_code ec;
      std::filesystem::remove(output_path, ec);
    }
    return 0;
  }

  std::cerr << "error: unknown argument: " << arg1 << "\n";
  print_usage();
  return 2;
}
