//===-- llvm-rtdyld.cpp - MCJIT Testing Tool ------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This is a testing tool for use with the MC-JIT LLVM components.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringMap.h"
#include "llvm/DebugInfo/DIContext.h"
#include "llvm/ExecutionEngine/ObjectBuffer.h"
#include "llvm/ExecutionEngine/ObjectImage.h"
#include "llvm/ExecutionEngine/RuntimeDyld.h"
#include "llvm/ExecutionEngine/RuntimeDyldChecker.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Memory.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include <system_error>

using namespace llvm;
using namespace llvm::object;

static cl::list<std::string>
InputFileList(cl::Positional, cl::ZeroOrMore,
              cl::desc("<input file>"));

enum ActionType {
  AC_Execute,
  AC_PrintLineInfo,
  AC_Verify
};

static cl::opt<ActionType>
Action(cl::desc("Action to perform:"),
       cl::init(AC_Execute),
       cl::values(clEnumValN(AC_Execute, "execute",
                             "Load, link, and execute the inputs."),
                  clEnumValN(AC_PrintLineInfo, "printline",
                             "Load, link, and print line information for each function."),
                  clEnumValN(AC_Verify, "verify",
                             "Load, link and verify the resulting memory image."),
                  clEnumValEnd));

static cl::opt<std::string>
EntryPoint("entry",
           cl::desc("Function to call as entry point."),
           cl::init("_main"));

static cl::list<std::string>
Dylibs("dylib",
       cl::desc("Add library."),
       cl::ZeroOrMore);

static cl::opt<std::string>
TripleName("triple", cl::desc("Target triple for disassembler"));

static cl::list<std::string>
CheckFiles("check",
           cl::desc("File containing RuntimeDyld verifier checks."),
           cl::ZeroOrMore);

/* *** */

// A trivial memory manager that doesn't do anything fancy, just uses the
// support library allocation routines directly.
class TrivialMemoryManager : public RTDyldMemoryManager {
public:
  SmallVector<sys::MemoryBlock, 16> FunctionMemory;
  SmallVector<sys::MemoryBlock, 16> DataMemory;

  uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID,
                               StringRef SectionName) override;
  uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID, StringRef SectionName,
                               bool IsReadOnly) override;

  void *getPointerToNamedFunction(const std::string &Name,
                                  bool AbortOnFailure = true) override {
    return nullptr;
  }

  bool finalizeMemory(std::string *ErrMsg) override { return false; }

  // Invalidate instruction cache for sections with execute permissions.
  // Some platforms with separate data cache and instruction cache require
  // explicit cache flush, otherwise JIT code manipulations (like resolved
  // relocations) will get to the data cache but not to the instruction cache.
  virtual void invalidateInstructionCache();
};

uint8_t *TrivialMemoryManager::allocateCodeSection(uintptr_t Size,
                                                   unsigned Alignment,
                                                   unsigned SectionID,
                                                   StringRef SectionName) {
  sys::MemoryBlock MB = sys::Memory::AllocateRWX(Size, nullptr, nullptr);
  FunctionMemory.push_back(MB);
  return (uint8_t*)MB.base();
}

uint8_t *TrivialMemoryManager::allocateDataSection(uintptr_t Size,
                                                   unsigned Alignment,
                                                   unsigned SectionID,
                                                   StringRef SectionName,
                                                   bool IsReadOnly) {
  sys::MemoryBlock MB = sys::Memory::AllocateRWX(Size, nullptr, nullptr);
  DataMemory.push_back(MB);
  return (uint8_t*)MB.base();
}

void TrivialMemoryManager::invalidateInstructionCache() {
  for (int i = 0, e = FunctionMemory.size(); i != e; ++i)
    sys::Memory::InvalidateInstructionCache(FunctionMemory[i].base(),
                                            FunctionMemory[i].size());

  for (int i = 0, e = DataMemory.size(); i != e; ++i)
    sys::Memory::InvalidateInstructionCache(DataMemory[i].base(),
                                            DataMemory[i].size());
}

static const char *ProgramName;

static void Message(const char *Type, const Twine &Msg) {
  errs() << ProgramName << ": " << Type << ": " << Msg << "\n";
}

static int Error(const Twine &Msg) {
  Message("error", Msg);
  return 1;
}

static void loadDylibs() {
  for (const std::string &Dylib : Dylibs) {
    if (sys::fs::is_regular_file(Dylib)) {
      std::string ErrMsg;
      if (sys::DynamicLibrary::LoadLibraryPermanently(Dylib.c_str(), &ErrMsg))
        llvm::errs() << "Error loading '" << Dylib << "': "
                     << ErrMsg << "\n";
    } else
      llvm::errs() << "Dylib not found: '" << Dylib << "'.\n";
  }
}

/* *** */

static int printLineInfoForInput() {
  // Load any dylibs requested on the command line.
  loadDylibs();

  // If we don't have any input files, read from stdin.
  if (!InputFileList.size())
    InputFileList.push_back("-");
  for(unsigned i = 0, e = InputFileList.size(); i != e; ++i) {
    // Instantiate a dynamic linker.
    TrivialMemoryManager MemMgr;
    RuntimeDyld Dyld(&MemMgr);

    // Load the input memory buffer.
    std::unique_ptr<MemoryBuffer> InputBuffer;
    std::unique_ptr<ObjectImage> LoadedObject;
    if (std::error_code ec =
            MemoryBuffer::getFileOrSTDIN(InputFileList[i], InputBuffer))
      return Error("unable to read input: '" + ec.message() + "'");

    // Load the object file
    LoadedObject.reset(Dyld.loadObject(new ObjectBuffer(InputBuffer.release())));
    if (!LoadedObject) {
      return Error(Dyld.getErrorString());
    }

    // Resolve all the relocations we can.
    Dyld.resolveRelocations();

    std::unique_ptr<DIContext> Context(
        DIContext::getDWARFContext(LoadedObject->getObjectFile()));

    // Use symbol info to iterate functions in the object.
    for (object::symbol_iterator I = LoadedObject->begin_symbols(),
                                 E = LoadedObject->end_symbols();
         I != E; ++I) {
      object::SymbolRef::Type SymType;
      if (I->getType(SymType)) continue;
      if (SymType == object::SymbolRef::ST_Function) {
        StringRef  Name;
        uint64_t   Addr;
        uint64_t   Size;
        if (I->getName(Name)) continue;
        if (I->getAddress(Addr)) continue;
        if (I->getSize(Size)) continue;

        outs() << "Function: " << Name << ", Size = " << Size << "\n";

        DILineInfoTable Lines = Context->getLineInfoForAddressRange(Addr, Size);
        DILineInfoTable::iterator  Begin = Lines.begin();
        DILineInfoTable::iterator  End = Lines.end();
        for (DILineInfoTable::iterator It = Begin; It != End; ++It) {
          outs() << "  Line info @ " << It->first - Addr << ": "
                 << It->second.FileName << ", line:" << It->second.Line << "\n";
        }
      }
    }
  }

  return 0;
}

static int executeInput() {
  // Load any dylibs requested on the command line.
  loadDylibs();

  // Instantiate a dynamic linker.
  TrivialMemoryManager MemMgr;
  RuntimeDyld Dyld(&MemMgr);

  // If we don't have any input files, read from stdin.
  if (!InputFileList.size())
    InputFileList.push_back("-");
  for(unsigned i = 0, e = InputFileList.size(); i != e; ++i) {
    // Load the input memory buffer.
    std::unique_ptr<MemoryBuffer> InputBuffer;
    std::unique_ptr<ObjectImage> LoadedObject;
    if (std::error_code ec =
            MemoryBuffer::getFileOrSTDIN(InputFileList[i], InputBuffer))
      return Error("unable to read input: '" + ec.message() + "'");

    // Load the object file
    LoadedObject.reset(Dyld.loadObject(new ObjectBuffer(InputBuffer.release())));
    if (!LoadedObject) {
      return Error(Dyld.getErrorString());
    }
  }

  // Resolve all the relocations we can.
  Dyld.resolveRelocations();
  // Clear instruction cache before code will be executed.
  MemMgr.invalidateInstructionCache();

  // FIXME: Error out if there are unresolved relocations.

  // Get the address of the entry point (_main by default).
  void *MainAddress = Dyld.getSymbolAddress(EntryPoint);
  if (!MainAddress)
    return Error("no definition for '" + EntryPoint + "'");

  // Invalidate the instruction cache for each loaded function.
  for (unsigned i = 0, e = MemMgr.FunctionMemory.size(); i != e; ++i) {
    sys::MemoryBlock &Data = MemMgr.FunctionMemory[i];
    // Make sure the memory is executable.
    std::string ErrorStr;
    sys::Memory::InvalidateInstructionCache(Data.base(), Data.size());
    if (!sys::Memory::setExecutable(Data, &ErrorStr))
      return Error("unable to mark function executable: '" + ErrorStr + "'");
  }

  // Dispatch to _main().
  errs() << "loaded '" << EntryPoint << "' at: " << (void*)MainAddress << "\n";

  int (*Main)(int, const char**) =
    (int(*)(int,const char**)) uintptr_t(MainAddress);
  const char **Argv = new const char*[2];
  // Use the name of the first input object module as argv[0] for the target.
  Argv[0] = InputFileList[0].c_str();
  Argv[1] = nullptr;
  return Main(1, Argv);
}

static int checkAllExpressions(RuntimeDyldChecker &Checker) {
  for (const auto& CheckerFileName : CheckFiles) {
    std::unique_ptr<MemoryBuffer> CheckerFileBuf;
    if (std::error_code EC =
          MemoryBuffer::getFileOrSTDIN(CheckerFileName, CheckerFileBuf))
      return Error("unable to read input '" + CheckerFileName + "': " +
                   EC.message());

    if (!Checker.checkAllRulesInBuffer("# rtdyld-check:", CheckerFileBuf.get()))
      return Error("some checks in '" + CheckerFileName + "' failed");
  }
  return 0;
}

static int linkAndVerify() {

  // Check for missing triple.
  if (TripleName == "") {
    llvm::errs() << "Error: -triple required when running in -verify mode.\n";
    return 1;
  }

  // Look up the target and build the disassembler.
  Triple TheTriple(Triple::normalize(TripleName));
  std::string ErrorStr;
  const Target *TheTarget =
    TargetRegistry::lookupTarget("", TheTriple, ErrorStr);
  if (!TheTarget) {
    llvm::errs() << "Error accessing target '" << TripleName << "': "
                 << ErrorStr << "\n";
    return 1;
  }
  TripleName = TheTriple.getTriple();

  std::unique_ptr<MCSubtargetInfo> STI(
    TheTarget->createMCSubtargetInfo(TripleName, "", ""));
  assert(STI && "Unable to create subtarget info!");

  std::unique_ptr<MCRegisterInfo> MRI(TheTarget->createMCRegInfo(TripleName));
  assert(MRI && "Unable to create target register info!");

  std::unique_ptr<MCAsmInfo> MAI(TheTarget->createMCAsmInfo(*MRI, TripleName));
  assert(MAI && "Unable to create target asm info!");

  MCContext Ctx(MAI.get(), MRI.get(), nullptr);

  std::unique_ptr<MCDisassembler> Disassembler(
    TheTarget->createMCDisassembler(*STI, Ctx));
  assert(Disassembler && "Unable to create disassembler!");

  std::unique_ptr<MCInstrInfo> MII(TheTarget->createMCInstrInfo());

  std::unique_ptr<MCInstPrinter> InstPrinter(
    TheTarget->createMCInstPrinter(0, *MAI, *MII, *MRI, *STI));

  // Load any dylibs requested on the command line.
  loadDylibs();

  // Instantiate a dynamic linker.
  TrivialMemoryManager MemMgr;
  RuntimeDyld Dyld(&MemMgr);

  // If we don't have any input files, read from stdin.
  if (!InputFileList.size())
    InputFileList.push_back("-");
  for(unsigned i = 0, e = InputFileList.size(); i != e; ++i) {
    // Load the input memory buffer.
    std::unique_ptr<MemoryBuffer> InputBuffer;
    std::unique_ptr<ObjectImage> LoadedObject;
    if (std::error_code ec =
            MemoryBuffer::getFileOrSTDIN(InputFileList[i], InputBuffer))
      return Error("unable to read input: '" + ec.message() + "'");

    // Load the object file
    LoadedObject.reset(
      Dyld.loadObject(new ObjectBuffer(InputBuffer.release())));
    if (!LoadedObject) {
      return Error(Dyld.getErrorString());
    }
  }

  // Resolve all the relocations we can.
  Dyld.resolveRelocations();

  RuntimeDyldChecker Checker(Dyld, Disassembler.get(), InstPrinter.get(),
                             llvm::dbgs());
  return checkAllExpressions(Checker);
}

int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  ProgramName = argv[0];
  llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllDisassemblers();

  cl::ParseCommandLineOptions(argc, argv, "llvm MC-JIT tool\n");

  switch (Action) {
  case AC_Execute:
    return executeInput();
  case AC_PrintLineInfo:
    return printLineInfoForInput();
  case AC_Verify:
    return linkAndVerify();
  }
}
