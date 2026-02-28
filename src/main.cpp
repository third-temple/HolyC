#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
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
            << "            [--jit-session=<name>] [--jit-reset]\n"
            << "                       Execute supported subset in-process\n"
            << "  repl [--strict|--permissive] [--jit-session=<name>] [--jit-reset]\n"
            << "                       Start interactive JIT-backed HolyC REPL\n"
            << "  build <file> [-o out] [--target=<triple>] [--artifact-dir=<dir>]\n"
            << "               [--keep-temps] [--strict|--permissive]\n"
            << "                       Build executable via host toolchain/LLVM\n"
            << "  run <file> [--target=<triple>] [--artifact-dir=<dir>] [--keep-temps]\n"
            << "            [--strict|--permissive]\n"
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

int BuildExecutable(std::string_view input_path, std::string_view output_path,
                    std::string_view artifact_dir, std::string_view target_triple,
                    bool strict_mode, bool keep_temps) {
  std::string input_text;
  if (!ReadFile(input_path, &input_text)) {
    std::cerr << "error: cannot read file: " << input_path << "\n";
    return 2;
  }

  const holyc::frontend::ParseResult ir = holyc::frontend::EmitLlvmIr(
      input_text, input_path, holyc::frontend::ExecutionMode::kAot, strict_mode);
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
  if (!WriteFile(ll_path, ir.output)) {
    std::cerr << "error: cannot write LLVM IR file: " << ll_path << "\n";
    return 2;
  }

  const holyc::llvm_backend::Result build_result =
      holyc::llvm_backend::BuildExecutableFromIr(ir.output, output_path, artifact_dir,
                                                 target_triple);
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
      std::cerr << "error: unknown check argument: " << arg << "\n";
      return 2;
    }

    const std::string_view input_path = argv[2];
    std::string input_text;
    if (!ReadFile(input_path, &input_text)) {
      std::cerr << "error: cannot read file: " << input_path << "\n";
      return 2;
    }

    const holyc::frontend::ParseResult result =
        holyc::frontend::CheckSource(input_text, input_path, mode, strict_mode);
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
      std::cerr << "error: unknown ast-dump argument: " << arg << "\n";
      return 2;
    }

    const std::string_view input_path = argv[2];
    std::string input_text;
    if (!ReadFile(input_path, &input_text)) {
      std::cerr << "error: cannot read file: " << input_path << "\n";
      return 2;
    }

    const holyc::frontend::ParseResult result =
        holyc::frontend::ParseAndDumpAst(input_text, input_path, mode, strict_mode);
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
      std::cerr << "error: unknown preprocess argument: " << arg << "\n";
      return 2;
    }

    const std::string_view input_path = argv[2];
    std::string input_text;
    if (!ReadFile(input_path, &input_text)) {
      std::cerr << "error: cannot read file: " << input_path << "\n";
      return 2;
    }

    const holyc::frontend::ParseResult result = holyc::frontend::PreprocessSource(
        input_text, input_path, mode, strict_mode);
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
      std::cerr << "error: unknown emit-hir argument: " << arg << "\n";
      return 2;
    }

    const std::string_view input_path = argv[2];
    std::string input_text;
    if (!ReadFile(input_path, &input_text)) {
      std::cerr << "error: cannot read file: " << input_path << "\n";
      return 2;
    }

    const holyc::frontend::ParseResult result =
        holyc::frontend::EmitHir(input_text, input_path, mode, strict_mode);
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
      std::cerr << "error: unknown emit-llvm argument: " << arg << "\n";
      return 2;
    }

    const std::string_view input_path = argv[2];
    std::string input_text;
    if (!ReadFile(input_path, &input_text)) {
      std::cerr << "error: cannot read file: " << input_path << "\n";
      return 2;
    }

    const holyc::frontend::ParseResult result = holyc::frontend::EmitLlvmIr(
        input_text, input_path, mode, strict_mode);
    if (!result.ok) {
      std::cerr << result.output << "\n";
      return 1;
    }

    const holyc::llvm_backend::Result normalized = holyc::llvm_backend::NormalizeIr(result.output);
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
    for (int i = 3; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (TryParseStrictArg(arg, &strict_mode)) {
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
    if (!ReadFile(input_path, &input_text)) {
      std::cerr << "error: cannot read file: " << input_path << "\n";
      return 2;
    }

    if (jit_reset) {
      const holyc::llvm_backend::Result reset_result =
          holyc::llvm_backend::ResetJitSession(jit_session);
      if (!reset_result.ok) {
        std::cerr << reset_result.output << "\n";
        return 1;
      }
    }

    const holyc::frontend::ParseResult ir_result = holyc::frontend::EmitLlvmIr(
        input_text, input_path, holyc::frontend::ExecutionMode::kJit, strict_mode);
    if (!ir_result.ok) {
      std::cerr << ir_result.output << "\n";
      return 1;
    }

    const holyc::llvm_backend::Result result =
        holyc::llvm_backend::ExecuteIrJit(ir_result.output, jit_session, reset_after_run);
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
    for (int i = 2; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (TryParseStrictArg(arg, &strict_mode)) {
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
    return holyc::repl::RunRepl(strict_mode, jit_session, jit_reset);
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
      if (arg == "--keep-temps") {
        keep_temps = true;
        continue;
      }

      std::cerr << "error: unknown build argument: " << arg << "\n";
      return 2;
    }

    const int rc = BuildExecutable(input_path, output_path, artifact_dir, target_triple,
                                   strict_mode, keep_temps);
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
      if (arg == "--keep-temps") {
        keep_temps = true;
        continue;
      }

      std::cerr << "error: unknown run argument: " << arg << "\n";
      return 2;
    }

    const int rc = BuildExecutable(input_path, output_path, artifact_dir, target_triple,
                                   strict_mode, keep_temps);
    if (rc != 0) {
      return rc;
    }

    std::string run_error;
    const int run_rc = RunProgram({output_path}, &run_error);
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
