#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"

#include <iostream>

using namespace llvm;

class IRTestbed : public PassInfoMixin<IRTestbed> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};

PreservedAnalyses IRTestbed::run(Module &M, ModuleAnalysisManager &MAM) {
  // TODO: your pass code.

  return PreservedAnalyses::all();
}

extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "IRTestbed", "v0.0",
    [](PassBuilder &PB) {
      /* Register this pass to a pipeline point when you want it to be called.
       * See the relevant doxygen for other callbacks.
       * (https://llvm.org/doxygen/classllvm_1_1PassBuilder.html) */
      PB.registerOptimizerLastEPCallback(
        [](ModulePassManager &MPM, OptimizationLevel OL) {
          MPM.addPass(IRTestbed());
        });
    }};
}
