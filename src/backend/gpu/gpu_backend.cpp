#include "gpu_backend.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Type.h"
#include "llvm/PassManager.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Scalar.h"

#if LLVM_MAJOR_VERSION <= 3 && LLVM_MINOR_VERSION <= 4
#include "llvm/Analysis/Verifier.h"
#else
#include "llvm/IR/Verifier.h"
#endif

#include <algorithm>
#include <fstream>
#include <utility>

#include "error.h"
#include "gpu_codegen.h"
#include "gpu_function.h"
#include "intrinsics.h"
#include "ir.h"
#include "ir_queries.h"
#include "backend/llvm/llvm_codegen.h"
#include "backend/llvm/llvm_defines.h"
#include "tensor_index.h"
#include "types.h"
#include "util/collections.h"

#ifndef NASSERT
#define ASSERT(cond) \
  do { \
    if (!(cond)) { \
      std::cerr << "Assert error: " << __FILE__ << ":" << __LINE__ << std::endl; \
      exit(1); \
    } \
  } while (0)
#else
#define ASSERT(cond) do { (void)sizeof(cond); } while (0)
#endif

namespace simit {
namespace backend {

Function* GPUBackend::compile(ir::Func irFunc, const ir::Storage& storage) {
  std::ofstream irFile("simit.sim", std::ofstream::trunc);
  irFile << irFunc;
  irFile.close();

  this->irFunc = irFunc;
  this->module = createNVVMModule("kernels-module");
  this->dataLayout.reset(new llvm::DataLayout(module));

  this->storage = storage;
  symtable.clear();
  buffers.clear();
  globals.clear();

  // This backend stores all system tensors as globals.
  // TODO: Replace hacky makeSystemTensorsGlobalIfNoStorage with
  //       makeSystemTensorsGlobal. The makeSystemTensorsGlobalIfNoStorage
  //       function was used to make the old init system that relied on storage
  //       work while transitioning to the new one based on pexprs
//  func = makeSystemTensorsGlobal(func);
  this->irFunc = makeSystemTensorsGlobalIfHasTensorIndex(this->irFunc);

  const ir::Environment& env = this->irFunc.getEnvironment();
  emitGlobals(env);

  std::vector<ir::Func> callTree = ir::getCallTree(this->irFunc);
  std::reverse(callTree.begin(), callTree.end());

  llvm::Function *func = nullptr;
  for (auto &f : callTree) {
    // If we're not compiling the top-level func, then we do regular stack
    // allocations.
    inKernel = (f.getName() != this->irFunc.getName());

    if (f.getKind() != ir::Func::Internal) continue;
    iassert(f.getBody().defined());

    this->storage.add(f.getStorage());

    // Emit function
    symtable.scope(); // put function arguments in new scope
    func = emitEmptyFunction(f.getName(), f.getArguments(), f.getResults(),
                             !inKernel, false);

    // Add constants to symbol table
    for (auto &global : f.getEnvironment().getConstants()) {
      symtable.insert(global.first, compile(global.second));
    }

    compile(f.getBody());
    builder->CreateRetVoid();

    symtable.unscope();
  }
  iassert(func);

  iassert(!llvm::verifyModule(*module))
      << "LLVM module does not pass verification";

#ifndef SIMIT_DEBUG
  // Run LLVM optimization passes on the function
  // We use the built-in PassManagerBuilder to build
  // the set of passes that are similar to clang's -O3
  llvm::FunctionPassManager fpm(module);
  llvm::PassManager mpm;
  llvm::PassManagerBuilder pmBuilder;
  
  pmBuilder.OptLevel = 3;

  pmBuilder.BBVectorize = 1;
  pmBuilder.LoopVectorize = 1;
//  pmBuilder.LoadCombine = 1;
  pmBuilder.SLPVectorize = 1;

  llvm::DataLayout dataLayout(module);
#if LLVM_MAJOR_VERSION >= 3 && LLVM_MINOR_VERSION >= 5
  fpm.add(new llvm::DataLayoutPass(dataLayout));
#else
  fpm.add(new llvm::DataLayout(dataLayout));
#endif

  pmBuilder.populateFunctionPassManager(fpm);
  pmBuilder.populateModulePassManager(mpm);

  fpm.doInitialization();
  fpm.run(*func);
  fpm.doFinalization();
  
  mpm.run(*module);
#endif

  // Add temporaries to buffers
  // for (const ir::Var& tmp : env.getTemporaries()) {
  //   buffers[tmp] = symtable.get(tmp);
  // }

  // Fake an EngineBuilder to allow interfacing with the LLVMFunction
  // superclass.
  std::shared_ptr<llvm::EngineBuilder> engineBuilder = createEngineBuilder(module);
  return new GPUFunction(this->irFunc, func, module, engineBuilder, storage);
}

void GPUBackend::compile(const ir::Literal& op) {
  const ir::TensorType *type = op.type.toTensor();
  if (type->order() == 0) {
    // Delegate scalar literals to generic LLVM backend
    LLVMBackend::compile(op);
  }
  else {
    // Put the data in global memory and generate a pointer
    ir::ScalarType ctype = type->getComponentType();
    llvm::Constant *dataConstant = nullptr;
    switch (ctype.kind) {
      case ir::ScalarType::Int: {
        iassert(ctype.bytes() == sizeof(uint32_t))
            << "Incorrect native types used for constant data array";
        iassert(op.size % sizeof(uint32_t) == 0)
            << "Literal data size not a multiple of element size";
        dataConstant = llvm::ConstantDataArray::get(
            LLVM_CTX, llvm::ArrayRef<uint32_t>(
                reinterpret_cast<uint32_t*>(op.data),
                op.size/sizeof(uint32_t)));
        break;
      }
      case ir::ScalarType::Float: {
        if (ir::ScalarType::floatBytes == sizeof(float)) {
          iassert(op.size % sizeof(float) == 0)
              << "Literal data size not a multiple of element size";
          dataConstant = llvm::ConstantDataArray::get(
              LLVM_CTX, llvm::ArrayRef<float>(
                  reinterpret_cast<float*>(op.data),
                  op.size/sizeof(float)));
        }
        else if (ir::ScalarType::floatBytes == sizeof(double)) {
          iassert(op.size % sizeof(double) == 0)
              << "Literal data size not a multiple of element size";
          dataConstant = llvm::ConstantDataArray::get(
              LLVM_CTX, llvm::ArrayRef<double>(
                  reinterpret_cast<double*>(op.data),
                  op.size/sizeof(double)));
        }
        else {
          unreachable;
        }
        break;
      }
      case ir::ScalarType::Boolean: {
        not_supported_yet;
        // This code is untested, but likely correct
        iassert(op.size % sizeof(bool) == 0)
            << "Literal data size not a multiple of element size";
        iassert(sizeof(bool) == sizeof(uint32_t))
            << "Boolean literal assumes 32-bit data format";
        dataConstant = llvm::ConstantDataArray::get(
            LLVM_CTX, llvm::ArrayRef<uint32_t>(
                reinterpret_cast<uint32_t*>(op.data),
                op.size/sizeof(uint32_t)));
        break;
      }
      default: unreachable;
    }
    iassert(dataConstant != nullptr);

    llvm::GlobalVariable *globalData =
        new llvm::GlobalVariable(*module, dataConstant->getType(), true,
                                 llvm::GlobalVariable::InternalLinkage,
                                 dataConstant, "global_const", nullptr,
                                 llvm::GlobalVariable::NotThreadLocal,
                                 CUDA_GLOBAL_ADDRSPACE);
    llvm::Type *finalType = llvmType(*type, CUDA_GLOBAL_ADDRSPACE);
    val = builder->CreateBitCast(globalData, finalType);
  }
  iassert(val);
}

void GPUBackend::compile(const ir::Call& op) {
  std::map<ir::Func, std::string> nvvmIntrinsicByName =
      {{ir::intrinsics::sin(),    std::string("__nv_sinf")},
       {ir::intrinsics::cos(),    std::string("__nv_cosf")},
       {ir::intrinsics::sqrt(),   std::string("__nv_sqrtf")},
       {ir::intrinsics::log(),    std::string("__nv_logf")},
       {ir::intrinsics::exp(),    std::string("__nv_fast_expf")},
       {ir::intrinsics::pow(),    std::string("__nv_fast_powf")},
       {ir::intrinsics::atan2(),  std::string("__nv_atan2f")},
       {ir::intrinsics::tan(),    std::string("__nv_tanf")},
       {ir::intrinsics::asin(),   std::string("__nv_asinf")},
       {ir::intrinsics::acos(),   std::string("__nv_acosf")}};
  
  std::vector<llvm::Type*> argTypes;
  std::vector<llvm::Value*> args;
  llvm::Function *fun = nullptr;

  // compile arguments first
  for (auto a: op.actuals) {
    //FIX: remove once solve() is no longer needed
    //iassert(isScalar(a.type()));
    ir::ScalarType ctype = a.type().isTensor() ? a.type().toTensor()->getComponentType()
                                               : a.type().toArray()->elementType;
    argTypes.push_back(llvmType(ctype));
    args.push_back(compile(a));
  }

  auto foundIntrinsic = nvvmIntrinsicByName.find(op.func);
  if (foundIntrinsic != nvvmIntrinsicByName.end()) {
    auto ftype = llvm::FunctionType::get(llvmFloatType(), argTypes, false);
    module->getOrInsertFunction(foundIntrinsic->second, ftype);
    fun = module->getFunction(foundIntrinsic->second);
  }
  else if (op.func == ir::intrinsics::norm()) {
    iassert(args.size() == 1);
    auto type = op.actuals[0].type().toTensor();
    std::vector<ir::IndexDomain> dimensions = type->getDimensions();

    // Dense operation
    if (!type->hasSystemDimensions()) {
      args.push_back(emitComputeLen(dimensions[0]));
      std::string funcName = ir::ScalarType::singleFloat() ?
          "norm_f32" : "norm_f64";
      val = emitCall(funcName, args, llvmFloatType());
    }
    else {
      // Fire off kernel for sparse computation
      llvm::Value *result = builder->CreateAlloca(
          llvmFloatType(), llvmInt(1));
      llvm::Value *size = emitComputeLen(dimensions[0]);
      ir::Type resultType = ir::TensorType::make(type->getComponentType());
      emitShardedDot(op.actuals[0].type(), op.actuals[0].type(),
                     resultType, args[0], args[0], size, result);
      llvm::Value *sqrt = getBuiltIn(
          nvvmIntrinsicByName.at(ir::intrinsics::sqrt()),
          llvmFloatType(), { llvmFloatType() });
      val = builder->CreateCall(sqrt, builder->CreateLoad(result));
    }
    return;
  }
  else if (op.func == ir::intrinsics::loc()) {
    val = emitCall("loc", args, LLVM_INT);
    return;
  }
  else if (op.func == ir::intrinsics::dot()) {
    iassert(args.size() == 2);
    // we need to add the vector length to the args
    auto type1 = op.actuals[0].type().toTensor();
    auto type2 = op.actuals[1].type().toTensor();
    auto type1Dimensions = type1->getDimensions();
    auto type2Dimensions = type2->getDimensions();

    uassert(type1Dimensions[0] == type2Dimensions[0]) <<
      "dimension mismatch in dot product";

    // Dense operation
    if (!type1->hasSystemDimensions() && !type2->hasSystemDimensions()) {
      std::string funcName = ir::ScalarType::singleFloat() ?
          "dot_f32" : "dot_f64";
      args.push_back(emitComputeLen(type1Dimensions[0]));
      val = emitCall(funcName, args, llvmFloatType());
      return;
    }

    // Fallthrough: fire off a kernel for sparse operation
    iassert(type1->hasSystemDimensions() && type2->hasSystemDimensions());

    llvm::Value *result = builder->CreateAlloca(llvmFloatType(), llvmInt(1));
    llvm::Value *size = emitComputeLen(type1Dimensions[0]);
    ir::Type resultType = ir::TensorType::make(type1->getComponentType());
    emitShardedDot(op.actuals[0].type(), op.actuals[1].type(),
                   resultType, args[0], args[1],
                   size, result);
    val = result;

    return;
  }
  // if not an intrinsic function, try to find it in the module
  else if (module->getFunction(op.func.getName())) {
    fun = module->getFunction(op.func.getName());
  }
  else {
    std::cerr << "GPUBackend::compile unsupported node:\n\n" << op << "\n\n";
    ASSERT(false && "No code generation for this type");
  }
  
  val = builder->CreateCall(fun, args);
}

void GPUBackend::compile(const ir::VarExpr& op) {
  LLVMBackend::compile(op);
}

void GPUBackend::compile(const ir::Load& op) {
  LLVMBackend::compile(op);
}

void GPUBackend::compile(const ir::FieldRead& op) {
  LLVMBackend::compile(op);
}

void GPUBackend::compile(const ir::Length& op) {
  LLVMBackend::compile(op);
}

void GPUBackend::compile(const ir::IndexRead& op) {
  LLVMBackend::compile(op);
}

void GPUBackend::compile(const ir::VarDecl& op) {
  tassert(op.var.getType().isTensor()) << "Only tensor decls supported";

  if (inKernel) {
    ir::Var var = op.var;
    if (isScalar(var.getType())) {
      // Allow LLVMBackend to emit a local alloca
      LLVMBackend::compile(op);
    }
    else {
      const ir::TensorType *ttype = var.getType().toTensor();
      ir::ScalarType ctype = ttype->getComponentType();
      llvm::Value *llvmVar;
      if (!ttype->hasSystemDimensions()) {
        llvmVar = builder->CreateAlloca(
            llvmType(ctype), llvmInt(ttype->size()), var.getName());
      }
      else {
        // NOTE: This could be really slow or result in OOM if the loops and
        // temporaries generated in lowering do not work well.
        llvm::Function* mallocFunc = getBuiltIn(
            "malloc", LLVM_INT8_PTR, {LLVM_INT64});
        llvm::Value *len = emitComputeLen(ttype, storage.getStorage(var));
        len = builder->CreateIntCast(len, LLVM_INT64, true);
        llvmVar = builder->CreateCall(mallocFunc, len);
        llvmVar = builder->CreatePointerCast(llvmVar, llvmType(*ttype));
      }
      symtable.insert(var, llvmVar);
    }
  }
  else { // Root scope
    // Always global, to be accessible to all kernels
    makeGlobalTensor(op.var);
  }
}

void GPUBackend::compile(const ir::AssignStmt& op) {
  // Only atomic for a compound scalar-scalar assign
  const ir::TensorType *varType = op.var.getType().toTensor();
  const ir::TensorType *valType = op.value.type().toTensor();
  if (op.cop != ir::CompoundOperator::None &&
      varType->order() == 0) {
    iassert(symtable.contains(op.var)) << op.var << " has not been declared";
    switch (op.cop) {
      case ir::CompoundOperator::Add: {
        llvm::Value *value = compile(op.value);
        llvm::Value *varPtr = symtable.get(op.var);
        // Globals are stored as pointer-pointers so we must load them
        if (util::contains(globals, op.var)) {
          varPtr = builder->CreateLoad(varPtr, op.var.getName());
        }
        // Guard against non-pointer
        iassert(varPtr->getType()->isPointerTy());
        // TODO: This check should probably look at things in env instead
        if (buffers.find(op.var) != buffers.end()) {
          // Global or argument which might be accessed in parallel
          emitAtomicLoadAdd(varPtr, value);
        }
        else {
          // Local, will not be accessed in parallel
          LLVMBackend::compile(op);
        }
        break;
      }
      default: ierror << "Unknown compound operator type: " << op.cop;
    }
  }
  else if (varType->order() > 0 && valType->order() == 0 &&
           ir::isa<ir::Literal>(op.value) &&
           (ir::to<ir::Literal>(op.value)->getFloatVal(0) == 0.0 ||
            ((int*)ir::to<ir::Literal>(op.value))[0] == 0) &&
           !inKernel) {
    llvm::Value *varPtr = compile(op.var);
    llvm::Value *len = emitComputeLen(varType, storage.getStorage(op.var));
    emitShardedMemSet(op.var.getType(), varPtr, len);
  }
  else {
    LLVMBackend::compile(op);
  }
}

void GPUBackend::compile(const ir::CallStmt& op) {
  std::map<ir::Func, std::string> nvvmIntrinsicByName =
      {{ir::intrinsics::sin(),    std::string("__nv_sinf")},
       {ir::intrinsics::cos(),    std::string("__nv_cosf")},
       {ir::intrinsics::sqrt(),   std::string("__nv_sqrtf")},
       {ir::intrinsics::log(),    std::string("__nv_logf")},
       {ir::intrinsics::exp(),    std::string("__nv_fast_expf")},
       {ir::intrinsics::pow(),    std::string("__nv_fast_powf")},
       {ir::intrinsics::atan2(),  std::string("__nv_atan2f")},
       {ir::intrinsics::tan(),    std::string("__nv_tanf")},
       {ir::intrinsics::asin(),   std::string("__nv_asinf")},
       {ir::intrinsics::acos(),   std::string("__nv_acosf")}};
  
  std::vector<llvm::Type*> argTypes;
  std::vector<llvm::Value*> args;
  llvm::Function *fun = nullptr;
  llvm::Value *call = nullptr;

  // compile arguments first
  for (auto a: op.actuals) {
    //FIX: remove once solve() is no longer needed
    //iassert(isScalar(a.type()));
    argTypes.push_back(llvmType(a.type().toTensor()->getComponentType()));
    args.push_back(compile(a));
  }

  ir::Func callee = op.callee;

  if (callee.getKind() == ir::Func::Intrinsic) {
    iassert(callee != ir::intrinsics::norm() &&
            callee != ir::intrinsics::dot())
        << "norm and dot should have been lowered";

    std::string floatTypeName = ir::ScalarType::singleFloat() ? "_f32" : "_f64";

    // first, see if this is an LLVM intrinsic
    auto foundIntrinsic = nvvmIntrinsicByName.find(callee);
    if (foundIntrinsic != nvvmIntrinsicByName.end()) {
      fun = getBuiltIn(foundIntrinsic->second, llvmFloatType(), argTypes);
      call = builder->CreateCall(fun, args);
    }
    else if (callee == ir::intrinsics::mod()) {
      iassert(op.actuals.size() == 2) << "mod takes two inputs, got"
                                       << op.actuals.size();
      call = builder->CreateSRem(compile(op.actuals[0]),
                                 compile(op.actuals[1]));
    }
    else if (callee == ir::intrinsics::det()) {
      iassert(args.size() == 1);
      std::string fname = callee.getName() + "3" + floatTypeName;
      call = emitCall(fname, args, llvmFloatType());
    }
    else if (callee == ir::intrinsics::inv()) {
      iassert(args.size() == 1);

      ir::Var result = op.results[0];
      llvm::Value *llvmResult = symtable.get(result);
      args.push_back(llvmResult);

      std::string fname = callee.getName() + "3" + floatTypeName;
      call = emitCall(fname, args);
    }
    else if (op.callee == ir::intrinsics::loc()) {
      call = emitCall("loc", args, LLVM_INT);
    }
    else {
      ierror << "intrinsic " << op.callee.getName() << " not found";
    }
  
    iassert(call);
    if (!call->getType()->isVoidTy()) {
      iassert(op.results.size() == 1);
      ir::Var var = op.results[0];
      llvm::Value *llvmVar = symtable.get(var);
      builder->CreateStore(call, llvmVar);
    }
  }
  // if not an intrinsic function, try to find it in the module
  else {
    if (module->getFunction(callee.getName())) {
      for (ir::Var r : op.results) {
        argTypes.push_back(llvmType(r.getType().toTensor()->getComponentType()));

        llvm::Value *llvmResult = symtable.get(r);
        args.push_back(llvmResult);
        symtable.insert(r, llvmResult);
      }
      fun = module->getFunction(op.callee.getName());
      call = builder->CreateCall(fun, args);
    }
    else {
      ierror << "function " << op.callee.getName() << " not found in module";
    }
  }
}

void GPUBackend::compile(const ir::Store& op) {
  if (op.cop != ir::CompoundOperator::None) {
    llvm::Value *buffer = compile(op.buffer);
    llvm::Value *index = compile(op.index);
    llvm::Value *value = compile(op.value);
    std::string locName = std::string(buffer->getName()) + PTR_SUFFIX;
    llvm::Value *bufferLoc = builder->CreateInBoundsGEP(buffer, index, locName);
    switch (op.cop) {
      case ir::CompoundOperator::Add: {
        emitAtomicLoadAdd(bufferLoc, value);
        break;
      }
      default: ierror << "Unknown compound operator type";
    }
  }
  else {
    LLVMBackend::compile(op);
  }
}

void GPUBackend::compile(const ir::FieldWrite& op) {
  // Sparse memset 0 should be emitted as a kernel
  ir::Type fieldType = getFieldType(op.elementOrSet, op.fieldName);
  ir::Type valueType = op.value.type();
  if (fieldType.toTensor()->order() > 0 &&
      valueType.toTensor()->order() == 0 &&
      ir::isa<ir::Literal>(op.value) &&
      ir::to<ir::Literal>(op.value)->getFloatVal(0) == 0.0) {
    // TODO: Currently do not support int memsets
    tassert(valueType.toTensor()->getComponentType().kind
            == ir::ScalarType::Float)
        << "Assigning int/bool tensor to zero unsupported"
        << std::endl << op.elementOrSet << "." << op.fieldName
        << " = " << op.value;
    llvm::Value *fieldPtr = emitFieldRead(op.elementOrSet, op.fieldName);
    // For now we'll assume fields are always dense row major
    emitShardedMemSet(
        fieldType, fieldPtr, emitComputeLen(
            fieldType.toTensor(), ir::TensorStorage::Kind::Dense));
  }
  else {
    LLVMBackend::compile(op);
  }
}

void GPUBackend::compile(const ir::Scope& op) {
  LLVMBackend::compile(op);
}

void GPUBackend::compile(const ir::IfThenElse& op) {
  LLVMBackend::compile(op);
}

void GPUBackend::compile(const ir::ForRange& op) {
  LLVMBackend::compile(op);
}

void GPUBackend::compile(const ir::For& op) {
  // Loop will be emitted linearly, instead of as a kernel over processors
  LLVMBackend::compile(op);
}

void GPUBackend::compile(const ir::While& op) {
  // Loop will be emitted linearly, instead of as a kernel over processors
  LLVMBackend::compile(op);
}

void GPUBackend::compile(const ir::Print& op) {
  LLVMBackend::compile(op);
}

void GPUBackend::compile(const ir::GPUKernel& op) {
  GPUSharding kernelSharding = op.sharding;

  // Stash the symtable
  symtable.scope();

  // Stash the current basic block
  llvm::BasicBlock *prevBB = builder->GetInsertBlock();

  // Pass all globals reads as arguments. Exclude them from the global list in
  // the scope of the GPUKernel so they are resolved from the symtable properly.
  std::set<ir::Var> excludeGlobals;
  std::vector<ir::Var> kernelArgs;
  for (auto var : op.reads) {
    kernelArgs.push_back(var);
    excludeGlobals.insert(var);
  }
  std::vector<ir::Var> kernelResults;
  for (auto var : op.writes) {
    // Skip repeated arguments
    if (op.reads.find(var) != op.reads.end()) continue;
    kernelResults.push_back(var);
    excludeGlobals.insert(var);
  }

  // HACK: Stash argument vars from the globals
  std::set<ir::Var> oldGlobals = globals;
  globals.clear();
  std::set_difference(oldGlobals.begin(), oldGlobals.end(),
                      excludeGlobals.begin(), excludeGlobals.end(),
                      std::inserter(globals, globals.begin()));

  // Push domain variables into kernel args
  if (kernelSharding.xSharded) {
    iassert(kernelSharding.xDomain.getKind() == ir::IndexSet::Set);
    iassert(ir::isa<ir::VarExpr>(kernelSharding.xDomain.getSet()));
    ir::Var xDomainVar = ir::to<ir::VarExpr>(kernelSharding.xDomain.getSet())->var;
    if (std::find(kernelArgs.begin(), kernelArgs.end(), xDomainVar) ==
        kernelArgs.end() &&
        std::find(kernelResults.begin(), kernelResults.end(), xDomainVar) ==
        kernelResults.end()) {
      // If not a duplicate
      kernelArgs.push_back(xDomainVar);
    }
  }
  iassert(!kernelSharding.ySharded && !kernelSharding.zSharded);
  // TODO(gkanwar): Passing const arguments to kernels does not work properly
  // at the moment. This is blocked on consts being handled correctly in
  // the general backend.

  // Create LLVM func
  llvm::Function *kernel = emitEmptyFunction(
      irFunc.getName() + "_nested_kernel", kernelArgs,
      kernelResults, true, false, false);
  builder->SetInsertPoint(&kernel->getEntryBlock());
  
  // Parameter attributes
  llvm::AttributeSet attrSet = kernel->getAttributes();
  for (unsigned slot = 0; slot < attrSet.getNumSlots(); ++slot) {
    int index = attrSet.getSlotIndex(slot);
    attrSet = attrSet.addAttribute(LLVM_CTX, index, llvm::Attribute::NoAlias);
  }
  kernel->setAttributes(attrSet);
  
  llvm::BasicBlock *bodyStart = llvm::BasicBlock::Create(
    LLVM_CTX, "bodyStart", kernel);
  llvm::BasicBlock *earlyExit = llvm::BasicBlock::Create(
    LLVM_CTX, "earlyExit", kernel);
  
  // Guard: check if we're outside the intended range of the kernel loop and 
  // early-exit if so.
  llvm::Value *cond = builder->CreateICmpULT(getTidX(),
    emitComputeLen(kernelSharding.xDomain));
  builder->CreateCondBr(cond, bodyStart, earlyExit);

  builder->SetInsertPoint(earlyExit);
  builder->CreateRetVoid();

  // Continue with kernel body
  builder->SetInsertPoint(bodyStart);

  // Kernel metadata
  addNVVMAnnotation(kernel, "kernel", llvmInt(1), module);

  // Code generate for the kernel
  if (kernelSharding.xSharded) {
    symtable.insert(kernelSharding.xVar, getTidX());
  }
  if (kernelSharding.ySharded) {
    symtable.insert(kernelSharding.yVar, getTidY());
  }
  if (kernelSharding.zSharded) {
    symtable.insert(kernelSharding.zVar, getTidZ());
  }
  
  inKernel = true;
  LLVMBackend::compile(op.body);
  inKernel = false;

  // NVVM kernel should always return void
  builder->CreateRetVoid();

  // Unstash globals
  globals = oldGlobals;

  // Unstash symtable
  symtable.unscope();

  // Emit a dynamic kernel launch
  builder->SetInsertPoint(prevBB);
  std::vector<llvm::Value*> args;
  for (auto &irArg : kernelArgs) {
    llvm::Value *arg = symtable.get(irArg);
    // TODO: Move this global vs. local distinction to function
    // and kernel symtable management
    if (util::contains(globals, irArg)) {
      arg = builder->CreateLoad(arg, irArg.getName());
    }
    args.push_back(arg);
  }
  for (auto &irRes : kernelResults) {
    // TODO(gkanwar): Figure out inouts
    llvm::Value *res = symtable.get(irRes);
    if (util::contains(globals, irRes)) {
      res = builder->CreateLoad(res, irRes.getName());
    }
    args.push_back(res);
  }
  emitKernelLaunch(kernel, args, kernelSharding);
}

namespace {

// TODO(gkanwar): Do we need to clean attrs now that we are passing in BC?
void cleanFuncAttrs(llvm::Function *func) {
  // Clean attributes off of params
  llvm::AttributeSet funcAttrs = func->getAttributes();
  llvm::AttributeSet cleanAttrs;
  for (unsigned slot = 0; slot < funcAttrs.getNumSlots(); ++slot) {
    // Never add func attributes, because attribute groups are
    // disallowed in NVVM. If left on, they trip up the parser
    if (slot == 0) continue;
    // Remove readonly from param attrs
    int index = funcAttrs.getSlotIndex(slot);
    llvm::AttributeSet cleanSlot = funcAttrs.removeAttribute(
        LLVM_CTX, index, llvm::Attribute::ReadOnly);
    cleanAttrs.addAttributes(LLVM_CTX, index, cleanSlot);
  }

  func->setAttributes(cleanAttrs);
}

}

llvm::Value *GPUBackend::emitBarrier() {
  llvm::Function *func = getBuiltIn("llvm.nvvm.barrier0", LLVM_VOID, {});
  cleanFuncAttrs(func);
  return builder->CreateCall(func);
}

llvm::Value *GPUBackend::emitCheckRoot() {
  not_supported_yet;
  assert(false && "unreachable");
  return NULL;
}

llvm::Value *GPUBackend::getTidX() {
  llvm::Function *tidFunc = getBuiltIn(
      "llvm.nvvm.read.ptx.sreg.tid.x", LLVM_INT, {});
  cleanFuncAttrs(tidFunc);
  llvm::Function *bidFunc = getBuiltIn(
      "llvm.nvvm.read.ptx.sreg.ctaid.x", LLVM_INT, {});
  cleanFuncAttrs(bidFunc);
  auto tid = builder->CreateCall(tidFunc);
  auto bid = builder->CreateCall(bidFunc);
  auto blockOffset = builder->CreateMul(bid, llvmInt(blockSize));
  return builder->CreateAdd(tid, blockOffset);
}

llvm::Value *GPUBackend::getTidY() {
  not_supported_yet; // these should never be emitted at this point
  return nullptr;
}

llvm::Value *GPUBackend::getTidZ() {
  not_supported_yet; // these should never be emitted at this point
  return nullptr;
}

llvm::Value *GPUBackend::emitCastGlobalToGen(llvm::Value *src) {
  iassert(src->getType()->isPointerTy());
  llvm::PointerType *srcPtrTy = llvm::cast<llvm::PointerType>(src->getType());
  iassert(srcPtrTy->getAddressSpace() ==
          CUDA_GLOBAL_ADDRSPACE);
  llvm::Value *srcCast = builder->CreateBitCast(src, CUDA_INT8_PTR_GLOBAL);
  llvm::Function *castFunc = getBuiltIn(
      "llvm.nvvm.ptr.global.to.gen.p0i8.p1i8",
      LLVM_INT8_PTR, { CUDA_INT8_PTR_GLOBAL });
  cleanFuncAttrs(castFunc);
  llvm::Value *out = builder->CreateCall(castFunc, srcCast);
  llvm::Type *genTy = llvm::PointerType::getUnqual(srcPtrTy->getElementType());
  return builder->CreateBitCast(out, genTy);
}

void GPUBackend::emitThreadBarrier() {
  llvm::Function *func = getBuiltIn("llvm.nvvm.barrier0", LLVM_VOID, {});
  cleanFuncAttrs(func);
  builder->CreateCall(func);
}

void GPUBackend::emitDeviceSync() {
  llvm::Function *syncFunc = getBuiltIn("cudaDeviceSynchronize", LLVM_INT, {});
  builder->CreateCall(syncFunc);
}

void GPUBackend::emitAtomicLoadAdd(llvm::Value *ptr, llvm::Value *value) {
  if (value->getType()->isIntegerTy()) {
    builder->CreateAtomicRMW(llvm::AtomicRMWInst::Add, ptr, value,
                             llvm::AtomicOrdering::Monotonic);
  }
  else if (value->getType()->isFloatTy()) {
    emitAtomicFLoadAdd(ptr, value);
  }
  else {
    ierror << "Unknown LLVM value type for atomic load add";
  }
}

void GPUBackend::emitAtomicFLoadAdd(llvm::Value *ptr, llvm::Value *value) {
  llvm::Type *ptrGenTy = ptr->getType();
  iassert(ptrGenTy->isPointerTy())
      << "Atomic float load add requires pointer type for ptr";
  llvm::PointerType *ptrTy = reinterpret_cast<llvm::PointerType*>(ptrGenTy);
  unsigned addrspace = ptrTy->getAddressSpace();
  std::vector<llvm::Type*> argTys;
  std::string funcName;
  switch (addrspace) {
    case CUDA_GENERIC_ADDRSPACE: {
      argTys.push_back(LLVM_FLOAT_PTR);
      argTys.push_back(LLVM_FLOAT);
      funcName = "llvm.nvvm.atomic.load.add.f32.p0f32";
      break;
    }
    case CUDA_GLOBAL_ADDRSPACE: {
      argTys.push_back(CUDA_FLOAT_PTR_GLOBAL);
      argTys.push_back(LLVM_FLOAT);
      funcName = "llvm.nvvm.atomic.load.add.f32.p1f32";
      break;
    }
    case CUDA_SHARED_ADDRSPACE: {
      argTys.push_back(llvm::Type::getFloatPtrTy(LLVM_CTX, addrspace));
      argTys.push_back(LLVM_FLOAT);
      funcName = "llvm.nvvm.atomic.load.add.f32.p3f32";
      break;
    }
    default:
      ierror << "Unsupported addrspace for float load/add: " << addrspace;
  }
  llvm::Function *func = getBuiltIn(funcName, LLVM_FLOAT, argTys);
  cleanFuncAttrs(func);
  builder->CreateCall2(func, ptr, value);
}

void GPUBackend::emitKernelLaunch(llvm::Function *kernel,
                                  std::vector<llvm::Value*> args,
                                  GPUSharding sharding) {
  iassert(sharding.xSharded && !sharding.ySharded && !sharding.zSharded);
  emitKernelLaunch(kernel, args,
                   emitComputeLen(sharding.xDomain), nullptr, nullptr);
}

void GPUBackend::emitKernelLaunch(llvm::Function *kernel,
                                  std::vector<llvm::Value*> args,
                                  llvm::Value *xSize,
                                  llvm::Value *ySize,
                                  llvm::Value *zSize) {
  iassert(xSize) << "x dimension must be non-null";
  iassert(!ySize && !zSize) << "y and z dimensions not currently supported";

  // LLVM types
  // struct dim3
  llvm::StructType *dim3Ty = getOrCreateDim3Ty();

  // cudaGetParamBufferV2
  std::vector<llvm::Type*> getParamArgTys = {
    LLVM_INT8_PTR, dim3Ty, dim3Ty, LLVM_INT
  };
  llvm::Function *getParamFunc = getBuiltIn(
      "cudaGetParameterBufferV2", LLVM_INT8_PTR, getParamArgTys);

  // CUstream_st
  llvm::PointerType *cuStreamPtrTy = getOrCreateCUStreamPtrTy();

  // cudaLaunchDeviceV2
  std::vector<llvm::Type*> launchDevArgTys = {
    LLVM_INT8_PTR, cuStreamPtrTy
  };
  llvm::Function *cudaLaunchFunc = getBuiltIn(
      "cudaLaunchDeviceV2", LLVM_INT, launchDevArgTys);

  // Build dimensions
  std::vector<llvm::Constant*> gridDimsVec = {
    llvmInt(1), llvmInt(1), llvmInt(1)
  };
  llvm::Value *gridDims =
      llvm::ConstantStruct::get(dim3Ty, gridDimsVec);
  
  // numBlocks = 1 + ( (len-1) / blockSize )
  llvm::Value *numBlocks =  builder->CreateAdd(
                              builder->CreateUDiv(
                                builder->CreateSub(
                                  xSize,
                                  llvmInt(1)),
                                llvmInt(blockSize)
                              ), llvmInt(1)
                            );
  gridDims = builder->CreateInsertValue(
      gridDims,
      numBlocks,
      llvm::ArrayRef<unsigned>({0}));

  std::vector<llvm::Constant*> initBlockDims = {
    llvmInt(blockSize), llvmInt(1), llvmInt(1)
  };
  llvm::Constant *blockDims =
      llvm::ConstantStruct::get(dim3Ty, initBlockDims);

  // Build param buffer
  llvm::Value *kernelBitCast = builder->CreateBitCast(kernel, LLVM_INT8_PTR);
  llvm::Value *paramBuf = builder->CreateCall4(
      getParamFunc, kernelBitCast, gridDims, blockDims, llvmInt(0));

  // Insert args into param buffer, 8-byte aligned
  emitFillBuf(paramBuf, args, 8, false);

  builder->CreateCall2(cudaLaunchFunc, paramBuf,
                       llvm::ConstantPointerNull::get(cuStreamPtrTy));

  // Synchronize memory after the call
  emitDeviceSync();
}

void GPUBackend::emitGlobals(const ir::Environment& env) {
  LLVMBackend::emitGlobals(env);

  // We must add the managed annotation to all globals
  for (const ir::Var& ext : env.getExternVars()) {
    llvm::Value *global = symtable.get(ext);
    addNVVMAnnotation(global, "managed", llvmInt(1), module);
  }
  for (const ir::Var& tmp : env.getTemporaries()) {
    llvm::Value *global = symtable.get(tmp);
    addNVVMAnnotation(global, "managed", llvmInt(1), module);
  }
  for (const ir::TensorIndex& tensorIndex : env.getTensorIndices()) {
    const ir::Var& coordArray = tensorIndex.getCoordArray();
    llvm::Value *global = symtable.get(coordArray);
    addNVVMAnnotation(global, "managed", llvmInt(1), module);
    const ir::Var& sinkArray = tensorIndex.getSinkArray();
    global = symtable.get(sinkArray);
    addNVVMAnnotation(global, "managed", llvmInt(1), module);
  }

  // We must add externs and temporaries to the list of globally
  // allocated buffers, because the GPU backend does not simply
  // map the pointer to host memory, but instead must allocate
  // and copy the values back and forth.
  // for (const ir::Var& ext : env.getExternVars()) {
  //   llvm::Value *global = symtable.get(ext);
  //   buffers.insert(std::pair<ir::Var, llvm::Value*>(ext, global));
  // }
  // for (const ir::Var& tmp : env.getTemporaries()) {
  //   llvm::Value *global = symtable.get(tmp);
  //   buffers.insert(std::pair<ir::Var, llvm::Value*>(tmp, global));
  // }
}

void GPUBackend::emitPrintf(std::string format,
                            std::vector<llvm::Value*> args) {
  format = "(%d) " + format; // add thread ID
  llvm::Value *formatPtr = emitGlobalString(format);
  
  // add thread ID to beginning:
  std::reverse(args.begin(), args.end());
  args.push_back(getTidX());
  std::reverse(args.begin(), args.end());
  
  // Convert any args that need to be extended
  for (size_t i = 0; i < args.size(); ++i) {
    auto &arg = args[i];
    if (arg->getType()->isFloatTy()) {
      args[i] = builder->CreateFPExt(arg, LLVM_DOUBLE);
    }
    else if (arg->getType()->isIntegerTy()) {
      unsigned width = arg->getType()->getIntegerBitWidth();
      if (width == 1) {
        // Zero-extend boolean values
        args[i] = builder->CreateZExt(arg, LLVM_INT);
      }
      else if (width < 32) {
        args[i] = builder->CreateSExt(arg, LLVM_INT);
      }
    }
  }

  // Alloc args buf
  size_t size = 0;
  for (auto &arg : args) {
    size_t argSize = dataLayout->getTypeAllocSize(arg->getType());
    if (argSize == 8) {
      // 8-byte args should be 8-byte aligned
      if (size % 8 != 0) {
        iassert(size % 4 == 0);
        size += 4;
      }
    }
    size += argSize;
    iassert(size % 4 == 0) << "All arguments must be 4-byte aligned";
  }

  llvm::AllocaInst *argBuf =
      builder->CreateAlloca(LLVM_INT8, llvmInt(size), "buffer");
  // Align 8 on the buffer, so vprintf will be happy
  argBuf->setAlignment(8);
  // Args should still be 4-byte aligned
  emitFillBuf(argBuf, args, 4, true);

  // Create and call vprintf syscall
  llvm::Function *vprintf = getBuiltIn(
      "vprintf", LLVM_INT, {LLVM_INT8_PTR, LLVM_INT8_PTR});

  builder->CreateCall2(vprintf, formatPtr, argBuf);
}

void GPUBackend::emitMemCpy(llvm::Value *dst, llvm::Value *src,
                            llvm::Value *size, unsigned align) {
  iassert(dst->getType()->isPointerTy());
  iassert(src->getType()->isPointerTy());


  unsigned dstAddrspace = llvm::cast<llvm::PointerType>(
      dst->getType())->getAddressSpace();
  llvm::Type *dstCastTy = nullptr;
  std::string dstTyStr;
  if (dstAddrspace == CUDA_GLOBAL_ADDRSPACE) {
    dstCastTy = CUDA_INT8_PTR_GLOBAL;
    dstTyStr = "p1i8";
  }
  else if (dstAddrspace == CUDA_GENERIC_ADDRSPACE) {
    dstCastTy = LLVM_INT8_PTR;
    dstTyStr = "p0i8";
  }
  else {
    not_supported_yet;
  }
  iassert(dstCastTy != nullptr);

  unsigned srcAddrspace = llvm::cast<llvm::PointerType>(
      src->getType())->getAddressSpace();
  llvm::Type *srcCastTy = nullptr;
  std::string srcTyStr;
  if (srcAddrspace == CUDA_GLOBAL_ADDRSPACE) {
    srcCastTy = CUDA_INT8_PTR_GLOBAL;
    srcTyStr = "p1i8";
  }
  else if (srcAddrspace == CUDA_GENERIC_ADDRSPACE) {
    srcCastTy = LLVM_INT8_PTR;
    srcTyStr = "p0i8";
  }
  else {
    not_supported_yet;
  }
  iassert(srcCastTy != nullptr);

  // Emit our own memcpy decl, since the built-in has attributes which
  // are not handled by NVVM
  std::string memcpyName = "llvm.memcpy."+dstTyStr+"."+srcTyStr+".i32";
  llvm::Function *func = getBuiltIn(
      memcpyName, LLVM_VOID,
      {dstCastTy, srcCastTy, LLVM_INT, LLVM_INT, LLVM_BOOL});
  cleanFuncAttrs(func);

  llvm::Value *llvmAlign = llvmInt(align);
  llvm::Value *castDst = builder->CreateBitCast(dst, dstCastTy);
  llvm::Value *castSrc = builder->CreateBitCast(src, srcCastTy);
  llvm::Constant *isVolatile = llvmBool(true);
  builder->CreateCall5(func, castDst, castSrc, size, llvmAlign, isVolatile);
}

void GPUBackend::emitMemSet(llvm::Value *dst, llvm::Value *val,
                            llvm::Value *size, unsigned align) {
  iassert(dst->getType()->isPointerTy());

  unsigned dstAddrspace = llvm::cast<llvm::PointerType>(
      dst->getType())->getAddressSpace();
  llvm::Type *dstCastTy = nullptr;
  std::string dstTyStr;
  if (dstAddrspace == CUDA_GLOBAL_ADDRSPACE) {
    dstCastTy = CUDA_INT8_PTR_GLOBAL;
    dstTyStr = "p1i8";
  }
  else if (dstAddrspace == CUDA_GENERIC_ADDRSPACE) {
    dstCastTy = LLVM_INT8_PTR;
    dstTyStr = "p0i8";
  }
  else {
    not_supported_yet;
  }
  iassert(dstCastTy != nullptr);

  // Emit our own memset decl, since the built-in has attributes which
  // are not handled by NVVM
  std::string memsetName = "llvm.memset."+dstTyStr+".i32";
  llvm::Function *func = getBuiltIn(
      memsetName, LLVM_VOID,
      { dstCastTy, LLVM_INT8, LLVM_INT, LLVM_INT, LLVM_BOOL });
  cleanFuncAttrs(func);

  llvm::Value *llvmAlign = llvmInt(align);
  llvm::Value *castDst = builder->CreateBitCast(dst, dstCastTy);
  llvm::Constant *isVolatile = llvmBool(true);
  builder->CreateCall5(func, castDst, val, size, llvmAlign, isVolatile);
}


void GPUBackend::emitShardedMemSet(ir::Type targetType, llvm::Value *target,
                                   llvm::Value *length) {
  iassert(!inKernel);
  iassert(targetType.isTensor());

  // Stash the symtable
  util::ScopedMap<ir::Var, llvm::Value*> oldSymtable = symtable;
  symtable = util::ScopedMap<simit::ir::Var, llvm::Value*>();
  // Stash the current basic block
  llvm::BasicBlock *prevBB = builder->GetInsertBlock();

  // Create LLVM func
  ir::Var targetArg("target", targetType);
  ir::Var lengthArg("length", ir::Int);
  llvm::Function *kernel = emitEmptyFunction(
      "memset_kernel", {targetArg, lengthArg}, {},  true, false);
  builder->SetInsertPoint(&kernel->getEntryBlock());

  // Kernel metadata
  addNVVMAnnotation(kernel, "kernel", llvmInt(1), module);
  
  llvm::BasicBlock *bodyStart = llvm::BasicBlock::Create(
    LLVM_CTX, "bodyStart", kernel);
  llvm::BasicBlock *earlyExit = llvm::BasicBlock::Create(
    LLVM_CTX, "earlyExit", kernel);
  
  // Guard: check if we're outside the intended range of the kernel loop and 
  // early-exit if so.
  llvm::Value *cond = builder->CreateICmpULT(getTidX(), symtable.get(lengthArg));
  builder->CreateCondBr(cond, bodyStart, earlyExit);

  builder->SetInsertPoint(earlyExit);
  builder->CreateRetVoid();

  // Continue with kernel body
  builder->SetInsertPoint(bodyStart);

  // Actual assign
  llvm::Value *value = nullptr;
  if (targetType.toTensor()->getComponentType().kind == ir::ScalarType::Float) {
    value = llvmFP(0);
  }
  else if (targetType.toTensor()->getComponentType().kind == ir::ScalarType::Int) {
    value = llvmInt(0);
  }
  else {
    not_supported_yet;
  }
  iassert(value != nullptr);

  llvm::Value *ptr = builder->CreateGEP(symtable.get(targetArg), getTidX());
  builder->CreateStore(value, ptr);

  // Kernel should always return void
  builder->CreateRetVoid();

  // Unstash symtable
  symtable = oldSymtable;

  // Emit kernel launch
  builder->SetInsertPoint(prevBB);
  emitKernelLaunch(kernel, {target, length}, length, nullptr, nullptr);
}

void GPUBackend::emitShardedDot(ir::Type vec1Type, ir::Type vec2Type,
                                ir::Type resType,
                                llvm::Value *vec1, llvm::Value *vec2,
                                llvm::Value *size, llvm::Value *result) {
  // Clear result first
  iassert(resType.toTensor()->getComponentType().kind == ir::ScalarType::Float);
  builder->CreateStore(llvmFP(0), result);

  // Stash the symtable
  util::ScopedMap<ir::Var, llvm::Value*> oldSymtable = symtable;
  symtable = util::ScopedMap<simit::ir::Var, llvm::Value*>();
  // Stash the current basic block
  llvm::BasicBlock *prevBB = builder->GetInsertBlock();

  // Create LLVM func
  ir::Var resVar("result", resType);
  ir::Var vec1Var("vec1", vec1Type);
  ir::Var vec2Var("vec2", vec2Type);
  ir::Var sizeVar("size", ir::Int);
  llvm::Function *kernel = emitEmptyFunction(
      "dot_kernel", {vec1Var, vec2Var, sizeVar}, {resVar}, true, false);
  builder->SetInsertPoint(&kernel->getEntryBlock());

  // Kernel metadata
  addNVVMAnnotation(kernel, "kernel", llvmInt(1), module);
  
  llvm::BasicBlock *bodyStart = llvm::BasicBlock::Create(
      LLVM_CTX, "bodyStart", kernel);
  llvm::BasicBlock *earlyExit = llvm::BasicBlock::Create(
      LLVM_CTX, "earlyExit", kernel);
  
  // Guard: check if we're outside the intended range of the kernel loop and 
  // early-exit if so.
  llvm::Value *cond = builder->CreateICmpULT(getTidX(), symtable.get(sizeVar));
  builder->CreateCondBr(cond, bodyStart, earlyExit);

  builder->SetInsertPoint(earlyExit);
  builder->CreateRetVoid();

  // Continue with kernel body
  builder->SetInsertPoint(bodyStart);

  // Perform multiply and add
  llvm::Value *val1 = builder->CreateLoad(
      builder->CreateGEP(symtable.get(vec1Var), getTidX()));
  llvm::Value *val2 = builder->CreateLoad(
      builder->CreateGEP(symtable.get(vec2Var), getTidX()));
  llvm::Value *mul;
  iassert(val1->getType()->isFloatTy());
  mul = builder->CreateFMul(val1, val2);
  emitAtomicLoadAdd(symtable.get(resVar), mul);

  // Kernel should always return void
  builder->CreateRetVoid();

  // Unstash symtable
  symtable = oldSymtable;

  // Emit kernel launch
  builder->SetInsertPoint(prevBB);
  emitKernelLaunch(kernel, {vec1, vec2, size, result}, size, nullptr, nullptr);
}

void GPUBackend::emitFillBuf(llvm::Value *buffer,
                             std::vector<llvm::Value*> vals,
                             unsigned align,
                             bool alignToArgSize) {
  iassert(align % 4 == 0) << "Align must be a multiple of 4";
  uint64_t bufIndex = 0;
  for (auto &val : vals) {
    unsigned argSize = dataLayout->getTypeAllocSize(val->getType());
    unsigned localAlign = alignToArgSize ? std::max(argSize, align) : align;
    if (bufIndex % localAlign != 0) {
      iassert(bufIndex % 4 == 0) << "Cannot accept non 4-byte aligned params";
      bufIndex += (localAlign - bufIndex%localAlign);
    }
    llvm::Value *bufPtr = builder->CreateGEP(buffer, llvmInt(bufIndex));
    llvm::Value *valPtr = builder->CreateBitCast(
        bufPtr,
        // Pointer to arg type, addrspace 0
        llvm::PointerType::get(val->getType(), 0));
    builder->CreateAlignedStore(val, valPtr, localAlign);
    bufIndex += argSize;
  }
}

llvm::Value* GPUBackend::makeGlobalTensor(ir::Var var) {
  llvm::Value *llvmGlobal = LLVMBackend::makeGlobalTensor(var);

  // Annotate the global as managed memory to allow us to write its
  // value from the CUDA setup
  llvm::Value *global = buffers[var];
  addNVVMAnnotation(global, "managed", llvmInt(1), module);

  // Replace the load in the symtable with an appropriately casted version
  llvm::Value *llvmTmp = emitCastGlobalToGen(llvmGlobal);
  symtable.insert(var, llvmTmp);

  // Add to env as a temporary so we can allocate memory appropriately
  ir::Environment& env = irFunc.getEnvironment();
  // HACK: Insert into env with the correct global name, in case of
  // global name conflicts
  if (llvmGlobal->getName() != var.getName()) {
    ir::Var newVar(llvmGlobal->getName(), var.getType());
    env.addTemporary(newVar);
  }
  else {
    env.addTemporary(var);
  }

  return llvmTmp;
}

}
}
