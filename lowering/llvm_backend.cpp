#include "llvm_backend.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hc_runtime.h"

#if defined(__unix__) || defined(__APPLE__)
#include <spawn.h>
#include <sys/wait.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
extern char** environ;
#endif

#if defined(HOLYC_HAS_LLVM) && __has_include(<llvm/ExecutionEngine/Orc/LLJIT.h>)
#define HOLYC_LLVM_HEADERS_AVAILABLE 1
#endif

#ifdef HOLYC_LLVM_HEADERS_AVAILABLE
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#endif

namespace holyc::llvm_backend {

namespace {

#ifdef HOLYC_LLVM_HEADERS_AVAILABLE

void EnsureLlvmInitialized() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();
  initialized = true;
}

Result ErrorFromDiagnostic(const llvm::SMDiagnostic& diag) {
  std::string msg;
  llvm::raw_string_ostream os(msg);
  diag.print("holyc", os);
  os.flush();
  return Result{false, msg};
}

std::unique_ptr<llvm::Module> ParseModule(std::string_view ir_text,
                                          llvm::LLVMContext& context,
                                          llvm::SMDiagnostic* out_diag) {
  auto buffer = llvm::MemoryBuffer::getMemBufferCopy(ir_text, "holyc-input.ll");
  return llvm::parseIR(buffer->getMemBufferRef(), *out_diag, context);
}

Result VerifyModule(const llvm::Module& module) {
  std::string err;
  llvm::raw_string_ostream err_os(err);
  if (llvm::verifyModule(module, &err_os)) {
    err_os.flush();
    return Result{false, err};
  }
  return Result{true, ""};
}

llvm::OptimizationLevel ToLlvmOptLevel(OptLevel opt_level) {
  switch (opt_level) {
    case OptLevel::kO0:
      return llvm::OptimizationLevel::O0;
    case OptLevel::kO1:
      return llvm::OptimizationLevel::O1;
    case OptLevel::kO2:
      return llvm::OptimizationLevel::O2;
    case OptLevel::kO3:
      return llvm::OptimizationLevel::O3;
    case OptLevel::kOs:
      return llvm::OptimizationLevel::Os;
    case OptLevel::kOz:
      return llvm::OptimizationLevel::Oz;
  }
  return llvm::OptimizationLevel::O2;
}

llvm::CodeGenOptLevel ToCodeGenOptLevel(OptLevel opt_level) {
  switch (opt_level) {
    case OptLevel::kO0:
      return llvm::CodeGenOptLevel::None;
    case OptLevel::kO1:
      return llvm::CodeGenOptLevel::Less;
    case OptLevel::kO2:
      return llvm::CodeGenOptLevel::Default;
    case OptLevel::kO3:
      return llvm::CodeGenOptLevel::Aggressive;
    case OptLevel::kOs:
    case OptLevel::kOz:
      return llvm::CodeGenOptLevel::Default;
  }
  return llvm::CodeGenOptLevel::Default;
}

Result OptimizeModule(llvm::Module& module, OptLevel opt_level) {
  if (opt_level == OptLevel::kO0) {
    return Result{true, ""};
  }

  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::PassBuilder pb;

  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);

  llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(ToLlvmOptLevel(opt_level));
  mpm.run(module, mam);
  return Result{true, ""};
}

llvm::Value* CastJitEntryValue(llvm::IRBuilder<>& builder, llvm::Value* value,
                               llvm::Type* target_type) {
  if (value == nullptr || target_type == nullptr) {
    return nullptr;
  }
  if (value->getType() == target_type) {
    return value;
  }
  if (value->getType()->isIntegerTy() && target_type->isIntegerTy()) {
    const unsigned from_bits = value->getType()->getIntegerBitWidth();
    const unsigned to_bits = target_type->getIntegerBitWidth();
    if (from_bits == to_bits) {
      return value;
    }
    if (from_bits < to_bits) {
      return builder.CreateSExt(value, target_type);
    }
    return builder.CreateTrunc(value, target_type);
  }
  if (value->getType()->isPointerTy() && target_type->isPointerTy()) {
    return builder.CreateBitCast(value, target_type);
  }
  if (value->getType()->isPointerTy() && target_type->isIntegerTy()) {
    return builder.CreatePtrToInt(value, target_type);
  }
  if (value->getType()->isIntegerTy() && target_type->isPointerTy()) {
    return builder.CreateIntToPtr(value, target_type);
  }
  return nullptr;
}

Result BuildJitEntrypointWrapper(llvm::Module& module, llvm::Function* target_fn,
                                 std::string_view entry_symbol) {
  if (target_fn == nullptr) {
    return Result{false, "jit: missing entry target"};
  }

  llvm::LLVMContext& context = module.getContext();
  llvm::Type* i32_ty = llvm::Type::getInt32Ty(context);
  llvm::Type* i64_ty = llvm::Type::getInt64Ty(context);
  llvm::Type* ptr_ty = llvm::PointerType::get(context, 0);

  llvm::FunctionType* entry_ty = llvm::FunctionType::get(i32_ty, {}, false);
  llvm::Function* entry_fn = llvm::Function::Create(
      entry_ty, llvm::Function::ExternalLinkage, std::string(entry_symbol), module);
  llvm::BasicBlock* entry_bb = llvm::BasicBlock::Create(context, "entry", entry_fn);
  llvm::IRBuilder<> builder(entry_bb);

  llvm::Value* argc_value = llvm::ConstantInt::get(i32_ty, 1);
  llvm::GlobalVariable* argv0_global =
      builder.CreateGlobalString("holyc-jit", "__holyc_jit_argv0");
  llvm::Value* argv0_value = builder.CreateInBoundsGEP(
      argv0_global->getValueType(), argv0_global,
      {llvm::ConstantInt::get(i64_ty, 0), llvm::ConstantInt::get(i64_ty, 0)});
  llvm::ArrayType* argv_array_ty = llvm::ArrayType::get(ptr_ty, 2);
  llvm::Value* argv_storage = builder.CreateAlloca(argv_array_ty, nullptr, "argv_storage");
  llvm::Value* argv_slot0 = builder.CreateInBoundsGEP(
      argv_array_ty, argv_storage,
      {llvm::ConstantInt::get(i64_ty, 0), llvm::ConstantInt::get(i64_ty, 0)});
  builder.CreateStore(argv0_value, argv_slot0);
  llvm::Value* argv_slot1 = builder.CreateInBoundsGEP(
      argv_array_ty, argv_storage,
      {llvm::ConstantInt::get(i64_ty, 0), llvm::ConstantInt::get(i64_ty, 1)});
  builder.CreateStore(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr_ty)),
                      argv_slot1);
  llvm::Value* argv_value = builder.CreateInBoundsGEP(
      argv_array_ty, argv_storage,
      {llvm::ConstantInt::get(i64_ty, 0), llvm::ConstantInt::get(i64_ty, 0)});

  std::vector<llvm::Value*> call_args;
  call_args.reserve(target_fn->arg_size());
  for (unsigned i = 0; i < target_fn->arg_size(); ++i) {
    llvm::Type* param_ty = target_fn->getFunctionType()->getParamType(i);
    llvm::Value* arg_value = nullptr;
    if (i < 2) {
      llvm::Value* source =
          param_ty->isPointerTy() ? argv_value : static_cast<llvm::Value*>(argc_value);
      arg_value = CastJitEntryValue(builder, source, param_ty);
    }
    if (arg_value == nullptr) {
      if (!param_ty->isFirstClassType()) {
        return Result{false, "jit: entry parameter type is not host-call compatible"};
      }
      arg_value = llvm::Constant::getNullValue(param_ty);
    }
    call_args.push_back(arg_value);
  }

  llvm::Value* target_result = nullptr;
  if (target_fn->getReturnType()->isVoidTy()) {
    builder.CreateCall(target_fn, call_args);
    builder.CreateRet(llvm::ConstantInt::get(i32_ty, 0));
    return Result{true, ""};
  }

  target_result = builder.CreateCall(target_fn, call_args);
  if (target_result->getType()->isIntegerTy()) {
    if (target_result->getType()->getIntegerBitWidth() > i32_ty->getIntegerBitWidth()) {
      target_result = builder.CreateTrunc(target_result, i32_ty);
    } else if (target_result->getType()->getIntegerBitWidth() <
               i32_ty->getIntegerBitWidth()) {
      target_result = builder.CreateSExt(target_result, i32_ty);
    }
    builder.CreateRet(target_result);
    return Result{true, ""};
  }
  if (target_result->getType()->isPointerTy()) {
    llvm::Value* as_i64 = builder.CreatePtrToInt(target_result, i64_ty);
    builder.CreateRet(builder.CreateTrunc(as_i64, i32_ty));
    return Result{true, ""};
  }
  if (target_result->getType()->isFloatingPointTy()) {
    builder.CreateRet(builder.CreateFPToSI(target_result, i32_ty));
    return Result{true, ""};
  }

  return Result{false, "jit: entry return type is not host-call compatible"};
}

#if !defined(__unix__) && !defined(__APPLE__)
std::string Quote(std::string_view value) {
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

std::string ArtifactBaseName(std::string_view output_path) {
  const std::filesystem::path output{std::string(output_path)};
  const std::string filename = output.filename().string();
  if (!filename.empty()) {
    return filename;
  }
  return "holyc-output";
}

std::string SessionKey(std::string_view session_name) {
  return session_name.empty() ? "__default__" : std::string(session_name);
}

struct JitSessionState {
  std::unique_ptr<llvm::orc::LLJIT> jit;
  llvm::orc::JITDylib* runtime_dylib = nullptr;
  std::vector<llvm::orc::JITDylib*> module_dylibs;
  std::uint64_t next_module_id = 0;
  std::uint64_t next_entry_id = 0;
};

std::unordered_map<std::string, JitSessionState>& JitSessions() {
  static std::unordered_map<std::string, JitSessionState> sessions;
  return sessions;
}

std::string LinkerMangledName(std::string_view symbol_name, char global_prefix) {
  if (global_prefix == '\0') {
    return std::string(symbol_name);
  }
  std::string mangled;
  mangled.reserve(symbol_name.size() + 1);
  mangled.push_back(global_prefix);
  mangled.append(symbol_name);
  return mangled;
}

llvm_backend::Result RegisterRuntimeSymbols(llvm::orc::LLJIT& jit, llvm::orc::JITDylib& runtime_jd) {
  llvm::orc::MangleAndInterner mangle(jit.getExecutionSession(), jit.getDataLayout());
  llvm::orc::SymbolMap symbols;
  const llvm::JITSymbolFlags exported = llvm::JITSymbolFlags::Exported;
  symbols[mangle("hc_runtime_abi_version")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_runtime_abi_version, exported);
  symbols[mangle("hc_print_str")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_print_str, exported);
  symbols[mangle("hc_put_char")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_put_char, exported);
  symbols[mangle("hc_print_fmt")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_print_fmt, exported);
  symbols[mangle("hc_try_push")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_try_push, exported);
  symbols[mangle("hc_try_pop")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_try_pop, exported);
  symbols[mangle("hc_throw_i64")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_throw_i64, exported);
  symbols[mangle("hc_exception_payload")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_exception_payload, exported);
  symbols[mangle("hc_exception_active")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_exception_active, exported);
  symbols[mangle("hc_try_depth")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_try_depth, exported);
  symbols[mangle("hc_register_reflection_table")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_register_reflection_table, exported);
  symbols[mangle("hc_reflection_field_count")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_reflection_field_count, exported);
  symbols[mangle("hc_reflection_fields")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_reflection_fields, exported);
  symbols[mangle("hc_malloc")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_malloc, exported);
  symbols[mangle("hc_free")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_free, exported);
  symbols[mangle("hc_memcpy")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_memcpy, exported);
  symbols[mangle("hc_memset")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_memset, exported);
  symbols[mangle("CallStkGrow")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&CallStkGrow, exported);
  symbols[mangle("Spawn")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&Spawn, exported);
  symbols[mangle("JobQue")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&JobQue, exported);
  symbols[mangle("JobResGet")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&JobResGet, exported);
  symbols[mangle("HashFind")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&HashFind, exported);
  symbols[mangle("MemberMetaData")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&MemberMetaData, exported);
  symbols[mangle("MemberMetaFind")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&MemberMetaFind, exported);
  symbols[mangle("hc_task_spawn")] =
      llvm::orc::ExecutorSymbolDef::fromPtr(&hc_task_spawn, exported);

  if (auto err = runtime_jd.define(llvm::orc::absoluteSymbols(std::move(symbols)))) {
    return {false, llvm::toString(std::move(err))};
  }
  return {true, ""};
}

std::unordered_set<std::string> AllowedHostResolverSymbols(char global_prefix) {
  static constexpr std::string_view kHostSymbolAllowlist[] = {
      "_setjmp",
      "setjmp",
      "__sigsetjmp",
  };

  std::unordered_set<std::string> out;
  for (std::string_view sym : kHostSymbolAllowlist) {
    out.insert(LinkerMangledName(sym, global_prefix));
  }
  return out;
}

llvm_backend::Result AttachHostSymbolResolver(llvm::orc::LLJIT& jit,
                                              llvm::orc::JITDylib& runtime_jd) {
  const char global_prefix = jit.getDataLayout().getGlobalPrefix();
  auto allowed_symbols =
      std::make_shared<std::unordered_set<std::string>>(AllowedHostResolverSymbols(global_prefix));
  auto generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
      global_prefix,
      [allowed_symbols](const llvm::orc::SymbolStringPtr& symbol) {
        return allowed_symbols->contains(std::string(*symbol));
      });
  if (!generator) {
    return {false, llvm::toString(generator.takeError())};
  }
  runtime_jd.addGenerator(std::move(*generator));
  return {true, ""};
}

llvm_backend::Result InitializeJitSessionState(JitSessionState* state) {
  auto jit_or_err = llvm::orc::LLJITBuilder().create();
  if (!jit_or_err) {
    return {false, llvm::toString(jit_or_err.takeError())};
  }
  state->jit = std::move(*jit_or_err);

  auto runtime_jd_or_err = state->jit->createJITDylib("__holyc_runtime");
  if (!runtime_jd_or_err) {
    return {false, llvm::toString(runtime_jd_or_err.takeError())};
  }
  state->runtime_dylib = &*runtime_jd_or_err;

  const llvm_backend::Result runtime_symbols =
      RegisterRuntimeSymbols(*state->jit, *state->runtime_dylib);
  if (!runtime_symbols.ok) {
    return runtime_symbols;
  }

  const llvm_backend::Result resolver =
      AttachHostSymbolResolver(*state->jit, *state->runtime_dylib);
  if (!resolver.ok) {
    return resolver;
  }

  return {true, ""};
}

llvm::orc::JITDylibSearchOrder BuildModuleLinkOrder(const JitSessionState& state) {
  llvm::orc::JITDylibSearchOrder order;
  order.reserve(state.module_dylibs.size() + (state.runtime_dylib == nullptr ? 0 : 1));
  for (auto it = state.module_dylibs.rbegin(); it != state.module_dylibs.rend(); ++it) {
    order.push_back({*it, llvm::orc::JITDylibLookupFlags::MatchAllSymbols});
  }
  if (state.runtime_dylib != nullptr) {
    order.push_back({state.runtime_dylib, llvm::orc::JITDylibLookupFlags::MatchAllSymbols});
  }
  return order;
}

Result GetOrCreateJitSession(std::string_view session_key,
                             std::unordered_map<std::string, JitSessionState>* sessions,
                             JitSessionState** state_out) {
  auto it = sessions->find(std::string(session_key));
  if (it == sessions->end()) {
    JitSessionState state;
    const Result initialized = InitializeJitSessionState(&state);
    if (!initialized.ok) {
      return initialized;
    }
    it = sessions->emplace(std::string(session_key), std::move(state)).first;
  }
  *state_out = &it->second;
  return Result{true, ""};
}

Result ParseAndVerifyIrModule(std::string_view ir_text, std::unique_ptr<llvm::LLVMContext>* context_out,
                              std::unique_ptr<llvm::Module>* module_out) {
  auto context = std::make_unique<llvm::LLVMContext>();
  llvm::SMDiagnostic diag;
  std::unique_ptr<llvm::Module> module = ParseModule(ir_text, *context, &diag);
  if (!module) {
    return ErrorFromDiagnostic(diag);
  }

  const Result verified = VerifyModule(*module);
  if (!verified.ok) {
    return verified;
  }

  *context_out = std::move(context);
  *module_out = std::move(module);
  return Result{true, ""};
}

Result AddModuleToJitSession(JitSessionState* state, std::unique_ptr<llvm::LLVMContext> context,
                             std::unique_ptr<llvm::Module> module,
                             llvm::orc::JITDylib** module_jd_out = nullptr) {
  llvm::orc::LLJIT* jit = state->jit.get();

  const std::string module_dylib_name =
      "__holyc_module_" + std::to_string(++state->next_module_id);
  auto module_jd_or_err = jit->createJITDylib(module_dylib_name);
  if (!module_jd_or_err) {
    return Result{false, llvm::toString(module_jd_or_err.takeError())};
  }
  llvm::orc::JITDylib* module_jd = &*module_jd_or_err;
  module_jd->setLinkOrder(BuildModuleLinkOrder(*state));

  llvm::orc::ThreadSafeModule tsm(std::move(module), std::move(context));
  if (auto err = jit->addIRModule(*module_jd, std::move(tsm))) {
    return Result{false, llvm::toString(std::move(err))};
  }

  state->module_dylibs.push_back(module_jd);
  if (module_jd_out != nullptr) {
    *module_jd_out = module_jd;
  }
  return Result{true, ""};
}

Result RunTool(const std::vector<std::string>& args) {
  if (args.empty()) {
    return Result{false, "tool invocation failed: empty command"};
  }

#if defined(__unix__) || defined(__APPLE__)
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  pid_t pid = 0;
  const int spawn_rc = posix_spawnp(&pid, args[0].c_str(), nullptr, nullptr, argv.data(), environ);
  if (spawn_rc != 0) {
    return Result{false, "failed to spawn " + args[0] + ": " + std::strerror(spawn_rc)};
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return Result{false, "failed to wait for " + args[0] + ": " + std::strerror(errno)};
  }

  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    return Result{true, ""};
  }
  if (WIFEXITED(status)) {
    return Result{false, args[0] + " exited with status " +
                             std::to_string(WEXITSTATUS(status))};
  }
  if (WIFSIGNALED(status)) {
    return Result{false, args[0] + " terminated by signal " +
                             std::to_string(WTERMSIG(status))};
  }
  return Result{false, args[0] + " failed unexpectedly"};
#else
  std::string cmd;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i > 0) {
      cmd.push_back(' ');
    }
    cmd.append(Quote(args[i]));
  }
  const int rc = std::system(cmd.c_str());
  if (rc != 0) {
    return Result{false, args[0] + " exited with non-zero status"};
  }
  return Result{true, ""};
#endif
}

#endif

}  // namespace

Result NormalizeIr(std::string_view ir_text) {
#ifdef HOLYC_LLVM_HEADERS_AVAILABLE
  EnsureLlvmInitialized();

  llvm::LLVMContext context;
  llvm::SMDiagnostic diag;
  std::unique_ptr<llvm::Module> module = ParseModule(ir_text, context, &diag);
  if (!module) {
    return ErrorFromDiagnostic(diag);
  }

  Result verified = VerifyModule(*module);
  if (!verified.ok) {
    return verified;
  }

  std::string out;
  llvm::raw_string_ostream os(out);
  module->print(os, nullptr);
  os.flush();
  return Result{true, out};
#else
  (void)ir_text;
  return Result{false, "LLVM backend not enabled at build time"};
#endif
}

Result BuildExecutableFromIr(std::string_view ir_text, std::string_view output_path,
                             std::string_view artifact_dir,
                             std::string_view target_triple, OptLevel opt_level) {
#ifdef HOLYC_LLVM_HEADERS_AVAILABLE
  EnsureLlvmInitialized();

  llvm::LLVMContext context;
  llvm::SMDiagnostic diag;
  std::unique_ptr<llvm::Module> module = ParseModule(ir_text, context, &diag);
  if (!module) {
    return ErrorFromDiagnostic(diag);
  }

  Result verified = VerifyModule(*module);
  if (!verified.ok) {
    return verified;
  }

  llvm::Triple triple(target_triple.empty() ? llvm::sys::getDefaultTargetTriple()
                                            : std::string(target_triple));
  module->setTargetTriple(triple);

  std::string target_error;
  const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, target_error);
  if (!target) {
    return Result{false, target_error};
  }

  llvm::TargetOptions options;
  std::unique_ptr<llvm::TargetMachine> tm(
      target->createTargetMachine(triple, "", "", options, llvm::Reloc::PIC_, std::nullopt,
                                  ToCodeGenOptLevel(opt_level)));
  if (!tm) {
    return Result{false, "failed to create LLVM target machine"};
  }
  module->setDataLayout(tm->createDataLayout());

  const Result optimized = OptimizeModule(*module, opt_level);
  if (!optimized.ok) {
    return optimized;
  }

  std::error_code ec_dir;
  const std::filesystem::path obj_dir = artifact_dir.empty()
                                            ? std::filesystem::path(".")
                                            : std::filesystem::path(std::string(artifact_dir));
  std::filesystem::create_directories(obj_dir, ec_dir);
  if (ec_dir) {
    return Result{false, "failed to create artifact directory: " + obj_dir.string()};
  }
  const std::string obj_path =
      (obj_dir / (ArtifactBaseName(output_path) + ".o")).string();
  std::error_code ec;
  llvm::raw_fd_ostream obj_out(obj_path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    return Result{false, "failed to open object output: " + obj_path};
  }

  llvm::legacy::PassManager pm;
  if (tm->addPassesToEmitFile(pm, obj_out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
    return Result{false, "target does not support object emission"};
  }
  pm.run(*module);
  obj_out.flush();

  std::vector<std::string> link_args;
  link_args.emplace_back("clang++");
  if (!target_triple.empty()) {
    link_args.emplace_back("--target=" + std::string(target_triple));
  }
  link_args.emplace_back("-std=c++17");
#if defined(__linux__)
  link_args.emplace_back("-Wl,--build-id=none");
#endif
  const std::string runtime_cpp = std::string(HOLYC_SOURCE_DIR) + "/runtime/hc_runtime.cpp";
  link_args.emplace_back(obj_path);
  link_args.emplace_back(runtime_cpp);
  link_args.emplace_back("-o");
  link_args.emplace_back(std::string(output_path));
  const Result link_result = RunTool(link_args);
  if (!link_result.ok) {
    return Result{false, "clang link step failed: " + link_result.output};
  }

  return Result{true, ""};
#else
  (void)ir_text;
  (void)output_path;
  (void)artifact_dir;
  (void)target_triple;
  (void)opt_level;
  return Result{false, "LLVM backend not enabled at build time"};
#endif
}

Result LoadIrJit(std::string_view ir_text, std::string_view session_name, OptLevel opt_level) {
#ifdef HOLYC_LLVM_HEADERS_AVAILABLE
  EnsureLlvmInitialized();

  const std::string key = SessionKey(session_name);
  auto& sessions = JitSessions();
  JitSessionState* state = nullptr;
  const Result session = GetOrCreateJitSession(key, &sessions, &state);
  if (!session.ok) {
    return session;
  }

  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> module;
  const Result parsed = ParseAndVerifyIrModule(ir_text, &context, &module);
  if (!parsed.ok) {
    return parsed;
  }

  module->setDataLayout(state->jit->getDataLayout());
  const Result optimized = OptimizeModule(*module, opt_level);
  if (!optimized.ok) {
    return optimized;
  }

  return AddModuleToJitSession(state, std::move(context), std::move(module));
#else
  (void)ir_text;
  (void)session_name;
  (void)opt_level;
  return Result{false, "LLVM backend not enabled at build time"};
#endif
}

Result ExecuteIrJit(std::string_view ir_text, std::string_view session_name,
                    bool reset_after_run, std::string_view entry_symbol_name, OptLevel opt_level) {
#ifdef HOLYC_LLVM_HEADERS_AVAILABLE
  EnsureLlvmInitialized();

  const std::string key = SessionKey(session_name);
  auto& sessions = JitSessions();
  if (reset_after_run) {
    sessions.erase(key);
  }

  JitSessionState* state = nullptr;
  const Result session = GetOrCreateJitSession(key, &sessions, &state);
  if (!session.ok) {
    return session;
  }

  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> module;
  const Result parsed = ParseAndVerifyIrModule(ir_text, &context, &module);
  if (!parsed.ok) {
    if (reset_after_run) {
      sessions.erase(key);
    }
    return parsed;
  }

  module->setDataLayout(state->jit->getDataLayout());
  const Result optimized = OptimizeModule(*module, opt_level);
  if (!optimized.ok) {
    if (reset_after_run) {
      sessions.erase(key);
    }
    return optimized;
  }

  if (entry_symbol_name.empty()) {
    if (reset_after_run) {
      sessions.erase(key);
    }
    return Result{false, "jit: missing entry target"};
  }

  llvm::Function* entry_fn = module->getFunction(std::string(entry_symbol_name));
  if (entry_fn == nullptr) {
    if (reset_after_run) {
      sessions.erase(key);
    }
    return Result{false, "jit: missing entry symbol '" + std::string(entry_symbol_name) + "'"};
  }
  const std::string entry_symbol = "__holyc_entry_" + std::to_string(++state->next_entry_id);
  const std::string entry_target_symbol =
      "__holyc_entry_target_" + std::to_string(state->next_entry_id);
  entry_fn->setName(entry_target_symbol);
  const Result wrapper_result = BuildJitEntrypointWrapper(*module, entry_fn, entry_symbol);
  if (!wrapper_result.ok) {
    if (reset_after_run) {
      sessions.erase(key);
    }
    return wrapper_result;
  }

  llvm::orc::JITDylib* module_jd = nullptr;
  const Result add_module = AddModuleToJitSession(state, std::move(context), std::move(module),
                                                   &module_jd);
  if (!add_module.ok) {
    if (reset_after_run) {
      sessions.erase(key);
    }
    return add_module;
  }

  llvm::orc::LLJIT* jit = state->jit.get();
  auto sym = jit->lookup(*module_jd, entry_symbol);
  if (!sym) {
    if (reset_after_run) {
      sessions.erase(key);
    }
    return Result{false, llvm::toString(sym.takeError())};
  }

  using MainFn = int (*)();
  MainFn main_fn = sym->toPtr<MainFn>();
  const int rc = main_fn();
  // Spawn() launches detached tasks; wait for completion before unloading JIT state.
  hc_spawn_wait_all();
  if (reset_after_run) {
    sessions.erase(key);
  }
  return Result{true, std::to_string(rc) + "\n"};
#else
  (void)ir_text;
  (void)session_name;
  (void)reset_after_run;
  (void)entry_symbol_name;
  (void)opt_level;
  return Result{false, "LLVM backend not enabled at build time"};
#endif
}

Result ResetJitSession(std::string_view session_name) {
#ifdef HOLYC_LLVM_HEADERS_AVAILABLE
  hc_spawn_wait_all();
  JitSessions().erase(SessionKey(session_name));
  return Result{true, ""};
#else
  (void)session_name;
  return Result{false, "LLVM backend not enabled at build time"};
#endif
}

}  // namespace holyc::llvm_backend
