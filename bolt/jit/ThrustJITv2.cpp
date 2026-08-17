/*
 * Copyright (c) ByteDance Ltd. and/or its affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef ENABLE_BOLT_JIT

#include "bolt/jit/ThrustJITv2.h"

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/TargetSelect.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <vector>

namespace bytedance::bolt::jit {

namespace {

class CodeSizeTrackingPlugin final
    : public llvm::orc::LinkGraphLinkingLayer::Plugin {
 public:
  explicit CodeSizeTrackingPlugin(ThrustJITv2::CodeSizeTracker& tracker)
      : tracker_(tracker) {}

  void notifyMaterializing(
      llvm::orc::MaterializationResponsibility& mr,
      llvm::jitlink::LinkGraph&,
      llvm::jitlink::JITLinkContext&,
      llvm::MemoryBufferRef inputObject) override {
    if (auto err = mr.withResourceKeyDo([&](llvm::orc::ResourceKey key) {
          tracker_.addAllocatedBytes(key, inputObject.getBufferSize());
        })) {
      llvm::consumeError(std::move(err));
    }
  }

  llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility&) override {
    return llvm::Error::success();
  }

  llvm::Error notifyRemovingResources(
      llvm::orc::JITDylib&,
      llvm::orc::ResourceKey key) override {
    tracker_.take(key);
    return llvm::Error::success();
  }

  void notifyTransferringResources(
      llvm::orc::JITDylib&,
      llvm::orc::ResourceKey dstKey,
      llvm::orc::ResourceKey srcKey) override {
    tracker_.transfer(dstKey, srcKey);
  }

 private:
  ThrustJITv2::CodeSizeTracker& tracker_;
};

} // namespace

llvm::Expected<std::unique_ptr<ThrustJITv2>> ThrustJITv2::Create() {
  static std::once_flag llvmTargetInitialized;
  std::call_once(llvmTargetInitialized, []() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
  });

  auto result = std::unique_ptr<ThrustJITv2>(new ThrustJITv2());
  auto targetMachineBuilder = llvm::orc::JITTargetMachineBuilder::detectHost();
  if (!targetMachineBuilder) {
    return targetMachineBuilder.takeError();
  }
  targetMachineBuilder->setRelocationModel(llvm::Reloc::PIC_);
  auto jit =
      llvm::orc::LLJITBuilder()
          .setJITTargetMachineBuilder(std::move(*targetMachineBuilder))
          .setNumCompileThreads(8)
          .setObjectLinkingLayerCreator(
              [tracker = &result->codeSizeTracker_](
                  llvm::orc::ExecutionSession& executionSession,
                  const llvm::Triple&)
                  -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
                auto layer = std::make_unique<llvm::orc::ObjectLinkingLayer>(
                    executionSession);
                layer->addPlugin(
                    std::make_shared<CodeSizeTrackingPlugin>(*tracker));
                return std::unique_ptr<llvm::orc::ObjectLayer>(
                    std::move(layer));
              })
          .create();
  if (!jit) {
    return jit.takeError();
  }

  result->jit_ = std::move(*jit);
  return result;
}

ThrustJITv2* ThrustJITv2::getInstance() {
  static auto res = Create();
  if (!res) {
    llvm::errs() << llvm::toString(res.takeError()) << "\n";
    return nullptr;
  }
  return (*res).get();
}

CompiledModuleSP ThrustJITv2::CompileModule(
    std::function<bool(llvm::Module&)> irGenerator,
    const std::string& funcName) {
  {
    std::unique_lock lock(mutex_);
    if (auto cached = compiledModuleCache_.get(funcName); cached != nullptr) {
      return cached;
    }

    if (compilingFunctions_.count(funcName) > 0) {
      compilingCv_.wait(
          lock, [&]() { return compilingFunctions_.count(funcName) == 0; });
      return compiledModuleCache_.get(funcName);
    }

    compilingFunctions_.insert(funcName);
  }

  auto clearCompilingFlag = [this, &funcName]() {
    std::lock_guard lock(mutex_);
    compilingFunctions_.erase(funcName);
    compilingCv_.notify_all();
  };

  auto llvmContext = std::make_unique<llvm::LLVMContext>();
  auto llvmModule = std::make_unique<llvm::Module>(funcName, *llvmContext);
  llvmModule->setDataLayout(jit_->getDataLayout());
  if (irGenerator(*llvmModule)) {
    clearCompilingFlag();
    return nullptr;
  }

  std::vector<std::string> funcNames;
  for (auto& function : *llvmModule) {
    if (!function.isDeclaration()) {
      funcNames.emplace_back(function.getName().str());
    }
  }

  auto resourceTracker = jit_->getMainJITDylib().createResourceTracker();
  llvm::orc::ResourceKey resourceKey = 0;
  if (auto keyErr = resourceTracker->withResourceKeyDo(
          [&](llvm::orc::ResourceKey key) { resourceKey = key; })) {
    llvm::handleAllErrors(std::move(keyErr), [&](llvm::ErrorInfoBase& eib) {
      llvm::errs() << "[JITv2] ResourceTracker key Error: " << eib.message()
                   << '\n';
    });
    clearCompilingFlag();
    return nullptr;
  }

  auto err = jit_->addIRModule(
      resourceTracker,
      llvm::orc::ThreadSafeModule(
          std::move(llvmModule), std::move(llvmContext)));
  if (err) {
    takeTrackedObjectSize(resourceKey);
    llvm::handleAllErrors(std::move(err), [&](llvm::ErrorInfoBase& eib) {
      llvm::errs() << "[JITv2] addIRModule Error: " << eib.message() << '\n';
    });
    clearCompilingFlag();
    return nullptr;
  }

  auto compiledModule = std::make_shared<CompiledModule>();
  compiledModule->setKey(funcName);
  for (const auto& fn : funcNames) {
    auto symbol = jit_->lookup(fn);
    if (!symbol) {
      takeTrackedObjectSize(resourceKey);
      llvm::handleAllErrors(symbol.takeError(), [&](llvm::ErrorInfoBase& eib) {
        llvm::errs() << "[JITv2] lookup Error: " << eib.message() << '\n';
      });
      auto removeErr = resourceTracker->remove();
      llvm::consumeError(std::move(removeErr));
      clearCompilingFlag();
      return nullptr;
    }
    compiledModule->setFuncPtr(
        fn, reinterpret_cast<intptr_t>(symbol->toPtr<void*>()));
  }

  auto codeSize = takeTrackedObjectSize(resourceKey);
  compiledModule->setCodeSize(codeSize);
  compiledModule->appendCleanCallback(
      [this, codeSize, resourceTracker]() mutable {
        if (resourceTracker && !resourceTracker->isDefunct()) {
          auto err = resourceTracker->remove();
          llvm::handleAllErrors(std::move(err), [&](llvm::ErrorInfoBase& eib) {
            llvm::errs() << "[JITv2] Remove ResourceTracker Error: "
                         << eib.message() << '\n';
          });
        }
        if (codeSize > 0) {
          decreaseMemoryUsage(codeSize);
        }
      });
  increaseMemoryUsage(codeSize);

  {
    std::lock_guard lock(mutex_);
    compiledModuleCache_.put(funcName, compiledModule);
    while (memoryUsage_.load(std::memory_order_acquire) >
               memoryLimit_.load(std::memory_order_acquire) &&
           compiledModuleCache_.size() > 1) {
      auto oldestKey = compiledModuleCache_.oldestKey();
      if (oldestKey.has_value() && *oldestKey != funcName) {
        compiledModuleCache_.erase(*oldestKey);
      } else {
        break;
      }
    }
    compilingFunctions_.erase(funcName);
  }
  compilingCv_.notify_all();

  return compiledModule;
}

CompiledModuleSP ThrustJITv2::LookupSymbolsInCache(
    const std::string& funcName) {
  std::lock_guard lock(mutex_);
  return compiledModuleCache_.get(funcName);
}

const llvm::DataLayout& ThrustJITv2::getDataLayout() const {
  return jit_->getDataLayout();
}

size_t ThrustJITv2::GetMemoryUsage() const noexcept {
  return memoryUsage_.load(std::memory_order_acquire);
}

void ThrustJITv2::SetMemoryLimitForTest(size_t limit) noexcept {
  memoryLimit_.store(limit, std::memory_order_release);
}

void ThrustJITv2::ClearCacheForTest() {
  std::lock_guard lock(mutex_);
  compiledModuleCache_.clear();
}

size_t ThrustJITv2::takeTrackedObjectSize(llvm::orc::ResourceKey resourceKey) {
  return codeSizeTracker_.take(resourceKey);
}

void ThrustJITv2::increaseMemoryUsage(size_t size) {
  memoryUsage_.fetch_add(size, std::memory_order_release);
}

void ThrustJITv2::decreaseMemoryUsage(size_t size) {
  memoryUsage_.fetch_sub(size, std::memory_order_release);
}

} // namespace bytedance::bolt::jit

#endif
