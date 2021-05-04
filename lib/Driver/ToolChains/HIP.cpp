//===--- HIP.cpp - HIP Tool and ToolChain Implementations -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "HIP.h"
#include "CommonArgs.h"
#include "InputInfo.h"
#include "clang/Basic/Cuda.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Options.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

#if _WIN32 || _WIN64
#define NULL_FILE "nul"
#else
#define NULL_FILE "/dev/null"
#endif

namespace {

static void addBCLib(Compilation &C, const ArgList &Args,
                     ArgStringList &CmdArgs, ArgStringList LibraryPaths,
                     StringRef BCName) {
  StringRef FullName;
  for (std::string LibraryPath : LibraryPaths) {
    SmallString<128> Path(LibraryPath);
    llvm::sys::path::append(Path, BCName);
    FullName = Path;
    if (llvm::sys::fs::exists(FullName)) {
      CmdArgs.push_back(Args.MakeArgString(FullName));
      return;
    }
  }
  C.getDriver().Diag(diag::err_drv_no_such_file) << BCName;
}

} // namespace

const char *AMDGCN::Linker::constructLLVMLinkCommand(
    Compilation &C, const JobAction &JA, const InputInfoList &Inputs,
    const ArgList &Args, StringRef SubArchName,
    StringRef OutputFilePrefix) const {
  ArgStringList CmdArgs;
  SmallString<128> ExecPath(C.getDriver().Dir);
  llvm::sys::path::append(ExecPath, "llvm-link");

  // If there are more than one, users bitcode need to be
  // linked separately, without "-only-needed".
  if (Inputs.size() > 1) {
    // Add the input bc's created by compile step.
    for (const auto &II : Inputs)
      CmdArgs.push_back(II.getFilename());

    CmdArgs.push_back("-o");
    // Add an interemediate output file
    std::string TmpNamePre = C.getDriver().isSaveTempsEnabled()
            ? OutputFilePrefix.str() + "-prelinked.bc"
            : C.getDriver().GetTemporaryPath(
                  OutputFilePrefix.str() + "-prelinked", "bc");
    const char *OutputFileNamePre =
        C.addTempFile(C.getArgs().MakeArgString(TmpNamePre));
    CmdArgs.push_back(OutputFileNamePre);

    const char *ExecPre = Args.MakeArgString(ExecPath);
    C.addCommand(llvm::make_unique<Command>(JA, *this, ExecPre, CmdArgs, Inputs));

    CmdArgs.clear();
    CmdArgs.push_back("-only-needed");
    CmdArgs.push_back(OutputFileNamePre);
  } else {
    CmdArgs.push_back("-only-needed");
    CmdArgs.push_back(Inputs.front().getFilename());
  }

  // Link device bitcode using "-only-needed"
  ArgStringList LibraryPaths;

  // Find in --hip-device-lib-path and HIP_LIBRARY_PATH.
  for (auto Path : Args.getAllArgValues(options::OPT_hip_device_lib_path_EQ))
    LibraryPaths.push_back(Args.MakeArgString(Path));

  addDirectoryList(Args, LibraryPaths, "-L", "HIP_DEVICE_LIB_PATH");

  llvm::SmallVector<std::string, 10> BCLibs;

  // Add bitcode library in --hip-device-lib.
  for (auto Lib : Args.getAllArgValues(options::OPT_hip_device_lib_EQ)) {
    BCLibs.push_back(Args.MakeArgString(Lib));
  }

  for (auto Lib : BCLibs)
    addBCLib(C, Args, CmdArgs, LibraryPaths, Lib);

  // Add an intermediate output file.
  CmdArgs.push_back("-o");
  std::string TmpName = C.getDriver().isSaveTempsEnabled()
                            ? OutputFilePrefix.str() + "-linked.bc"
                            : C.getDriver().GetTemporaryPath(
                                  OutputFilePrefix.str() + "-linked", "bc");
  const char *OutputFileName =
      C.addTempFile(C.getArgs().MakeArgString(TmpName));
  CmdArgs.push_back(OutputFileName);
  const char *Exec = Args.MakeArgString(ExecPath);
  C.addCommand(llvm::make_unique<Command>(JA, *this, Exec, CmdArgs, Inputs));
  return OutputFileName;
}

const char *AMDGCN::Linker::constructShMemCommand(
                                   Compilation &C, const JobAction &JA,
                                   const InputInfoList &Inputs,
                                   const ArgList &Args,
                                   llvm::StringRef SubArchName,
                                   llvm::StringRef OutputFilePrefix,
                                   const char *InputFileName) const {
  // Construct command for hip dynamic mem pass
  ArgStringList OptArgs;
  OptArgs.push_back(InputFileName);
  OptArgs.push_back("-mtriple=spir64-unknown-unknown");
  OptArgs.push_back("-o");
  std::string TmpFileName = C.getDriver().isSaveTempsEnabled()
                                ? OutputFilePrefix.str() + "-shmem.bc"
                                : C.getDriver().GetTemporaryPath(
                                      OutputFilePrefix.str() + "-shmem", "bc");
  const char *OutputFileName =
      C.addTempFile(C.getArgs().MakeArgString(TmpFileName));
  OptArgs.push_back(OutputFileName);

  std::vector<std::string> Paths = Args.getAllArgValues(options::OPT_hip_llvm_pass_path_EQ);
  if (Paths.size() == 1) {
  Twine HipLLVMPassPath(Paths[0]);
  Twine HipLLVMPassLib = HipLLVMPassPath.concat(Twine("/libLLVMHipDynMem.so"));
  OptArgs.push_back("-load");
  OptArgs.push_back(Args.MakeArgString(HipLLVMPassLib));
  OptArgs.push_back("-hip-dyn-mem");
  }

  SmallString<128> OptPath(C.getDriver().Dir);
  llvm::sys::path::append(OptPath, "opt");
  const char *OptExec = Args.MakeArgString(OptPath);
  C.addCommand(llvm::make_unique<Command>(JA, *this, OptExec, OptArgs, Inputs));
  return OutputFileName;
}


const char *AMDGCN::Linker::constructOptCommand(
                                   Compilation &C, const JobAction &JA,
                                   const InputInfoList &Inputs,
                                   const ArgList &Args,
                                   llvm::StringRef SubArchName,
                                   llvm::StringRef OutputFilePrefix,
                                   const char *InputFileName) const {
  // Construct opt command.
  ArgStringList OptArgs;
  // The input to opt is the output from llvm-link.
  OptArgs.push_back(InputFileName);
  // Pass optimization arg to opt.
  if (Arg *A = Args.getLastArg(options::OPT_O_Group)) {
    StringRef OOpt = "3";
    if (A->getOption().matches(options::OPT_O4) ||
        A->getOption().matches(options::OPT_Ofast))
      OOpt = "3";
    else if (A->getOption().matches(options::OPT_O0))
      OOpt = "0";
    else if (A->getOption().matches(options::OPT_O)) {
      // -Os, -Oz, and -O(anything else) map to -O2
      OOpt = llvm::StringSwitch<const char *>(A->getValue())
                 .Case("1", "1")
                 .Case("2", "2")
                 .Case("3", "3")
                 .Case("s", "2")
                 .Case("z", "2")
                 .Default("2");
    }
    OptArgs.push_back(Args.MakeArgString("-O" + OOpt));
  }
  OptArgs.push_back("-mtriple=spir64-unknown-unknown");
  OptArgs.push_back("-o");
  std::string TmpFileName =
      C.getDriver().isSaveTempsEnabled()
          ? OutputFilePrefix.str() + "-optimized.bc"
          : C.getDriver().GetTemporaryPath(
                OutputFilePrefix.str() + "-optimized", "bc");
  const char *OutputFileName =
      C.addTempFile(C.getArgs().MakeArgString(TmpFileName));
  OptArgs.push_back(OutputFileName);

  SmallString<128> OptPath(C.getDriver().Dir);
  llvm::sys::path::append(OptPath, "opt");
  const char *OptExec = Args.MakeArgString(OptPath);
  C.addCommand(llvm::make_unique<Command>(JA, *this, OptExec, OptArgs, Inputs));
  return OutputFileName;
}

const char *AMDGCN::Linker::constructLLVMSpirCommand(
    Compilation &C, const JobAction &JA,
    const InputInfo &Output,
    const InputInfoList &Inputs,
    const llvm::opt::ArgList &Args,
    const char *InputFileName) const {
  // Construct llc command.
  ArgStringList LlcArgs{InputFileName,
                        "-o",
                        Output.getFilename() };

  SmallString<128> LlvmSpirvPath(C.getDriver().Dir);
  llvm::sys::path::append(LlvmSpirvPath, "llvm-spirv");
  const char *Llc = Args.MakeArgString(LlvmSpirvPath);
  C.addCommand(llvm::make_unique<Command>(JA, *this, Llc, LlcArgs, Inputs));
  return nullptr;
}

// Construct a clang-offload-bundler command to bundle code objects for
// different GPU's into a HIP fat binary.
void AMDGCN::constructHIPFatbinCommand(Compilation &C, const JobAction &JA,
                  StringRef OutputFileName, const InputInfoList &Inputs,
                  const llvm::opt::ArgList &Args, const Tool& T) {
  // Construct clang-offload-bundler command to bundle object files for
  // for different GPU archs.
  ArgStringList BundlerArgs;
  BundlerArgs.push_back(Args.MakeArgString("-type=o"));

  // ToDo: Remove the dummy host binary entry which is required by
  // clang-offload-bundler.
  std::string BundlerTargetArg = "-targets=host-x86_64-unknown-linux";
  std::string BundlerInputArg = "-inputs=" NULL_FILE;

  for (const auto &II : Inputs) {
    BundlerTargetArg = BundlerTargetArg + ",hip-spir64-unknown-unknown-generic";
    BundlerInputArg = BundlerInputArg + "," + II.getFilename();
  }
  BundlerArgs.push_back(Args.MakeArgString(BundlerTargetArg));
  BundlerArgs.push_back(Args.MakeArgString(BundlerInputArg));

  auto BundlerOutputArg =
      Args.MakeArgString(std::string("-outputs=").append(OutputFileName));
  BundlerArgs.push_back(BundlerOutputArg);

  SmallString<128> BundlerPath(C.getDriver().Dir);
  llvm::sys::path::append(BundlerPath, "clang-offload-bundler");
  const char *Bundler = Args.MakeArgString(BundlerPath);
  C.addCommand(llvm::make_unique<Command>(JA, T, Bundler, BundlerArgs, Inputs));
}


// For amdgcn the inputs of the linker job are device bitcode and output is
// object file. It calls llvm-link, opt, llc, then lld steps.
void AMDGCN::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                   const InputInfo &Output,
                                   const InputInfoList &Inputs,
                                   const ArgList &Args,
                                   const char *LinkingOutput) const {

  if (JA.getType() == types::TY_HIP_FATBIN)
    return constructHIPFatbinCommand(C, JA, Output.getFilename(), Inputs, Args, *this);

  assert(getToolChain().getTriple().getArch() == llvm::Triple::spir64 &&
         "Unsupported target");

  std::string SubArchName = "generic";

  // Prefix for temporary file name.
  std::string Prefix =
      llvm::sys::path::stem(Inputs[0].getFilename()).str() + "-" + SubArchName;

  // Each command outputs different files.
  const char *LLVMLinkCommand =
      constructLLVMLinkCommand(C, JA, Inputs, Args, SubArchName, Prefix);

  const char *ShMemCommand =
      constructShMemCommand(C, JA, Inputs, Args, SubArchName, Prefix, LLVMLinkCommand);

  const char *OptCommand =
      constructOptCommand(C, JA, Inputs, Args, SubArchName, Prefix, ShMemCommand);

  constructLLVMSpirCommand(C, JA, Output, Inputs, Args, OptCommand);
}

HIPToolChain::HIPToolChain(const Driver &D, const llvm::Triple &Triple,
                             const ToolChain &HostTC, const ArgList &Args)
    : ToolChain(D, Triple, Args), HostTC(HostTC) {
  // Lookup binaries into the driver directory, this is used to
  // discover the clang-offload-bundler executable.
  getProgramPaths().push_back(getDriver().Dir);
}

void HIPToolChain::addClangTargetOptions(
    const llvm::opt::ArgList &DriverArgs,
    llvm::opt::ArgStringList &CC1Args,
    Action::OffloadKind DeviceOffloadingKind) const {

  bool err = false;
  HostTC.addClangTargetOptions(DriverArgs, CC1Args, DeviceOffloadingKind);

  if(!DriverArgs.hasArg(options::OPT_hip_llvm_pass_path_EQ)) {
    getDriver().Diag(diag::err_hip_llvm_pass_missing);
    err = true;
  }

  if(!DriverArgs.hasArg(options::OPT_hip_device_lib_path_EQ)) {
    getDriver().Diag(diag::err_hip_device_lib_path_missing);
    err = true;
  }

  if(!DriverArgs.hasArg(options::OPT_hip_device_lib_EQ)) {
    getDriver().Diag(diag::err_hip_device_lib_missing);
    err = true;
  }

  if (err) return;

  CC1Args.push_back("-fcuda-is-device");

  if (DriverArgs.hasFlag(options::OPT_fcuda_flush_denormals_to_zero,
                         options::OPT_fno_cuda_flush_denormals_to_zero, false))
    CC1Args.push_back("-fcuda-flush-denormals-to-zero");

  if (DriverArgs.hasFlag(options::OPT_fcuda_approx_transcendentals,
                         options::OPT_fno_cuda_approx_transcendentals, false))
    CC1Args.push_back("-fcuda-approx-transcendentals");

  if (DriverArgs.hasFlag(options::OPT_fgpu_rdc, options::OPT_fno_gpu_rdc,
                         false))
    CC1Args.push_back("-fgpu-rdc");

  // Default to "hidden" visibility, as object level linking will not be
  // supported for the foreseeable future.
  if (!DriverArgs.hasArg(options::OPT_fvisibility_EQ,
                         options::OPT_fvisibility_ms_compat))
    CC1Args.append({"-fvisibility", "hidden"});
}

llvm::opt::DerivedArgList *
HIPToolChain::TranslateArgs(const llvm::opt::DerivedArgList &Args,
                             StringRef BoundArch,
                             Action::OffloadKind DeviceOffloadKind) const {
  DerivedArgList *DAL =
      HostTC.TranslateArgs(Args, BoundArch, DeviceOffloadKind);
  if (!DAL)
    DAL = new DerivedArgList(Args.getBaseArgs());

  const OptTable &Opts = getDriver().getOpts();

  for (Arg *A : Args) {
    if (A->getOption().matches(options::OPT_Xarch__)) {
      // Skip this argument unless the architecture matches BoundArch.
      if (BoundArch.empty() || A->getValue(0) != BoundArch)
        continue;

      unsigned Index = Args.getBaseArgs().MakeIndex(A->getValue(1));
      unsigned Prev = Index;
      std::unique_ptr<Arg> XarchArg(Opts.ParseOneArg(Args, Index));

      // If the argument parsing failed or more than one argument was
      // consumed, the -Xarch_ argument's parameter tried to consume
      // extra arguments. Emit an error and ignore.
      //
      // We also want to disallow any options which would alter the
      // driver behavior; that isn't going to work in our model. We
      // use isDriverOption() as an approximation, although things
      // like -O4 are going to slip through.
      if (!XarchArg || Index > Prev + 1) {
        getDriver().Diag(diag::err_drv_invalid_Xarch_argument_with_args)
            << A->getAsString(Args);
        continue;
      } else if (XarchArg->getOption().hasFlag(options::DriverOption)) {
        getDriver().Diag(diag::err_drv_invalid_Xarch_argument_isdriver)
            << A->getAsString(Args);
        continue;
      }
      XarchArg->setBaseArg(A);
      A = XarchArg.release();
      DAL->AddSynthesizedArg(A);
    }
    DAL->append(A);
  }

  if (!BoundArch.empty()) {
    DAL->eraseArg(options::OPT_march_EQ);
    DAL->AddJoinedArg(nullptr, Opts.getOption(options::OPT_march_EQ), BoundArch);
  }

  return DAL;
}

Tool *HIPToolChain::buildLinker() const {
  assert(getTriple().getArch() == llvm::Triple::spir64);
  return new tools::AMDGCN::Linker(*this);
}

void HIPToolChain::addClangWarningOptions(ArgStringList &CC1Args) const {
  HostTC.addClangWarningOptions(CC1Args);
}

ToolChain::CXXStdlibType
HIPToolChain::GetCXXStdlibType(const ArgList &Args) const {
  return HostTC.GetCXXStdlibType(Args);
}

void HIPToolChain::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                              ArgStringList &CC1Args) const {
  HostTC.AddClangSystemIncludeArgs(DriverArgs, CC1Args);
}

void HIPToolChain::AddClangCXXStdlibIncludeArgs(const ArgList &Args,
                                                 ArgStringList &CC1Args) const {
  HostTC.AddClangCXXStdlibIncludeArgs(Args, CC1Args);
}

void HIPToolChain::AddIAMCUIncludeArgs(const ArgList &Args,
                                        ArgStringList &CC1Args) const {
  HostTC.AddIAMCUIncludeArgs(Args, CC1Args);
}

SanitizerMask HIPToolChain::getSupportedSanitizers() const {
  // The HIPToolChain only supports sanitizers in the sense that it allows
  // sanitizer arguments on the command line if they are supported by the host
  // toolchain. The HIPToolChain will actually ignore any command line
  // arguments for any of these "supported" sanitizers. That means that no
  // sanitization of device code is actually supported at this time.
  //
  // This behavior is necessary because the host and device toolchains
  // invocations often share the command line, so the device toolchain must
  // tolerate flags meant only for the host toolchain.
  return HostTC.getSupportedSanitizers();
}

VersionTuple HIPToolChain::computeMSVCVersion(const Driver *D,
                                               const ArgList &Args) const {
  return HostTC.computeMSVCVersion(D, Args);
}
