#ifndef __YAOYAOJIT_H_
#define __YAOYAOJIT_H_

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/IRTransformLayer.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/OrcRemoteTargetClient.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/RuntimeDyld.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Mangler.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "ast.hpp"

std::unique_ptr<llvm::Module>
irgenAndTakeOwnership(FunctionAST &FnAST, const std::string &Suffix);

namespace llvm {
namespace orc {

using RemoteCli = remote::OrcRemoteTargetClient;

class YaoyaoJIT {
 private:
  ExecutionSession ES;
  std::shared_ptr<SymbolResolver> Resolver;
  std::unique_ptr<TargetMachine> TM;
  const DataLayout DL;
  LegacyRTDyldObjectLinkingLayer ObjectLayer;
  LegacyIRCompileLayer<decltype(ObjectLayer), SimpleCompiler> CompileLayer;

  using OptimizeFunction =
      std::function<std::unique_ptr<Module>(std::unique_ptr<Module>)>;

  LegacyIRTransformLayer<decltype(CompileLayer), OptimizeFunction> OptimizeLayer;

  std::unique_ptr<JITCompileCallbackManager> CompileCallbackMgr;
  std::unique_ptr<IndirectStubsManager> IndirectStubsMgr;

 public:
  YaoyaoJIT()
      : Resolver(createLegacyLookupResolver(
            ES,
            [this](StringRef Name) -> JITSymbol {
              if (auto Sym = IndirectStubsMgr->findStub(Name, false))
                return Sym;
              if (auto Sym = OptimizeLayer.findSymbol(std::string(Name), false))
                return Sym;
              else if (auto Err = Sym.takeError())
                return std::move(Err);
              if (auto SymAddr = RTDyldMemoryManager::getSymbolAddressInProcess(
                      std::string(Name)))
                return JITSymbol(SymAddr, JITSymbolFlags::Exported);
              return nullptr;
            },
            [](Error Err) { cantFail(std::move(Err), "lookupFlags failed"); })),
        TM(EngineBuilder().selectTarget()), DL(TM->createDataLayout()),
        ObjectLayer(AcknowledgeORCv1Deprecation, ES,
                    [this](VModuleKey K) {
                      return LegacyRTDyldObjectLinkingLayer::Resources{
                          std::make_shared<SectionMemoryManager>(), Resolver};
                    }),
        CompileLayer(AcknowledgeORCv1Deprecation, ObjectLayer,
                     SimpleCompiler(*TM)),
        OptimizeLayer(AcknowledgeORCv1Deprecation, CompileLayer,
                      [this](std::unique_ptr<Module> M) {
                        return optimizeModule(std::move(M));
                      }),
        CompileCallbackMgr(cantFail(orc::createLocalCompileCallbackManager(
            TM->getTargetTriple(), ES, 0))) {
    auto IndirectStubsMgrBuilder =
      orc::createLocalIndirectStubsManagerBuilder(TM->getTargetTriple());
    IndirectStubsMgr = IndirectStubsMgrBuilder();
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
  }


  TargetMachine &getTargetMachine() { return *TM; }

  VModuleKey addModule(std::unique_ptr<Module> M) {
    VModuleKey K = ES.allocateVModule();
    cantFail(OptimizeLayer.addModule(K, std::move(M)));
    return K;
  }

  Error addFunctionAST(std::unique_ptr<FunctionAST> FnAST) {
    auto SharedFnAST = std::shared_ptr<FunctionAST>(std::move(FnAST));

    auto CompileAction = [this, SharedFnAST]() {
      auto M = irgenAndTakeOwnership(*SharedFnAST, "$impl");
      addModule(std::move(M));
      auto Sym = findSymbol(SharedFnAST->getName() + "$impl");
      assert(Sym && "Couldn't find compiled function?");
      JITTargetAddress SymAddr = cantFail(Sym.getAddress());
      if (auto Err = IndirectStubsMgr->updatePointer(
              mangle(SharedFnAST->getName()), SymAddr)) {
        logAllUnhandledErrors(std::move(Err), errs(),
                              "Error updating function pointer:");
        exit(1);
      }
      return SymAddr;
    };
    auto CCAddr = cantFail(
        CompileCallbackMgr->getCompileCallback(std::move(CompileAction)));

    if (auto Err = IndirectStubsMgr->createStub(
            mangle(SharedFnAST->getName()), CCAddr, JITSymbolFlags::Exported))
      return Err;

    return Error::success();
  }

  JITSymbol findSymbol(const std::string Name) {
    return OptimizeLayer.findSymbol(mangle(Name), true);
  }

  void removeModule(VModuleKey K) { cantFail(OptimizeLayer.removeModule(K)); }

 private:
  std::string mangle(const std::string &Name) {
    std::string MangledName;
    raw_string_ostream MangledNameStream(MangledName);
    Mangler::getNameWithPrefix(MangledNameStream, Name, DL);
    return MangledNameStream.str();
  }

  std::unique_ptr<Module> optimizeModule(std::unique_ptr<Module> M) {
    auto FPM = std::make_unique<legacy::FunctionPassManager>(M.get());

    FPM->add(createInstructionCombiningPass());
    FPM->add(createReassociatePass());
    FPM->add(createGVNPass());
    FPM->add(createCFGSimplificationPass());
    FPM->doInitialization();

    for (auto &F : *M) FPM->run(F);

    return M;
  }
};

};  // namespace orc
};  // namespace llvm

#endif  // __YAOYAOJIT_H_