/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// This file defines the operations used in the LMHLO dialect.

#include "mlir-hlo/Dialect/lhlo/IR/lhlo_ops.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <unordered_set>

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"
#include "mlir-hlo/Dialect/lhlo/IR/lhlo_ops.h.inc"
#include "mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"
#include "mlir-hlo/Dialect/mhlo/IR/hlo_ops_common.h"
#include "mlir-hlo/utils/lhlo_utils.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/Value.h"

namespace mlir {
namespace lmhlo {

LmhloDialect::LmhloDialect(MLIRContext* context)
    : Dialect(getDialectNamespace(), context, TypeID::get<LmhloDialect>()) {
  context->loadDialect<mhlo::MhloDialect>();
  addOperations<
#define GET_OP_LIST
#include "mlir-hlo/Dialect/lhlo/IR/lhlo_ops.cc.inc"
      >();
}

//===----------------------------------------------------------------------===//
// AbsOp
//===----------------------------------------------------------------------===//

LogicalResult AbsOp::verify() {
  AbsOp op = *this;
  auto operandType = getElementTypeOrSelf(op.input().getType());
  auto outputType = getElementTypeOrSelf(op.output().getType());
  if (auto complexType = operandType.dyn_cast<ComplexType>()) {
    if (complexType.getElementType() != outputType) {
      return op.emitOpError(
          "requires output type to be the same as the element type of the "
          "input");
    }
    return success();
  }
  if (operandType != outputType)
    return op.emitOpError("requires all operands to have the same type");
  return success();
}

//===----------------------------------------------------------------------===//
// AllToAllOp
//===----------------------------------------------------------------------===//

// TODO(jurahul): Add verification for output shape.
LogicalResult AllGatherOp::verify() {
  AllGatherOp op = *this;
  return mlir::hlo::VerifyReplicaGroups(op, /*is_uniform_sized=*/true);
}

// TODO(jurahul): Add verification for output shape.
LogicalResult AllToAllOp::verify() {
  AllToAllOp op = *this;
  return mlir::hlo::VerifyReplicaGroups(op, /*is_uniform_sized=*/true);
}

//===----------------------------------------------------------------------===//
// AllReduceOp
//===----------------------------------------------------------------------===//

LogicalResult AllReduceOp::verify() {
  AllReduceOp op = *this;
  return VerifyAllReduce(op);
}

//===----------------------------------------------------------------------===//
// ReduceScatterOp
//===----------------------------------------------------------------------===//

LogicalResult ReduceScatterOp::verify() {
  ReduceScatterOp op = *this;
  if (failed(mlir::hlo::VerifyReplicaGroups(op, /*is_uniform_sized=*/true)))
    return failure();
  if (failed(mlir::hlo::VerifyReduceScatter(
          op, /*operand_types=*/op.operands().getTypes(),
          /*result_types=*/op.results().getTypes(),
          /*scatter_dimension=*/op.scatter_dimension())))
    return failure();
  return success();
}

//===----------------------------------------------------------------------===//
// CaseOp
//===----------------------------------------------------------------------===//

void CaseOp::getSuccessorRegions(Optional<unsigned> index,
                                 ArrayRef<Attribute> /*operands*/,
                                 SmallVectorImpl<RegionSuccessor>& regions) {
  // If the predecessor is the CaseOp, branch to all other branches.
  if (!index.hasValue()) {
    for (auto& branch : branches())
      regions.push_back(RegionSuccessor(&branch, branch.getArguments()));
  }
  // If the predecessor is one of the branches, branch back to the parent
  // operation.
  regions.push_back(RegionSuccessor());
}

//===----------------------------------------------------------------------===//
// CollectivePermuteOp
//===----------------------------------------------------------------------===//

LogicalResult CollectivePermuteOp::verify() {
  CollectivePermuteOp op = *this;
  return mlir::hlo::VerifyCollectivePermuteSourceTargetPairs(
      op, op.source_target_pairs());
}

//===----------------------------------------------------------------------===//
// ConstantOp.
//===----------------------------------------------------------------------===//

/// An lho.constant on an memref that is locally allocated and with no other
/// users (other than dealloc's) can be erased.
// TODO: This can be generalized to an arbitrary op by making use of memory
// effects (write memory effect).
struct EraseConstantOp : public OpRewritePattern<ConstantOp> {
  using OpRewritePattern<ConstantOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ConstantOp op,
                                PatternRewriter& rewriter) const override {
    Value memref = op.output();
    if (!memref.getDefiningOp<memref::AllocOp>()) {
      return failure();
    }

    // Check that all uses of the memref are either DeallocOps or this op.
    for (Operation* user : memref.getUsers())
      if (user != op && !isa<memref::DeallocOp>(user)) return failure();

    rewriter.eraseOp(op);
    return success();
  }
};

void ConstantOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                             MLIRContext* context) {
  results.add<EraseConstantOp>(context);
}

//===----------------------------------------------------------------------===//
// CustomCallOp.
//===----------------------------------------------------------------------===//

LogicalResult CustomCallOp::verify() {
  CustomCallOp op = *this;
  if (op.target_arg_mapping()) {
    CustomCallTargetArgMapping mapping = *op.target_arg_mapping();
    auto verifyMapping = [&](int64_t targetNum, size_t opNum, ArrayAttr mapping,
                             StringRef kind) -> LogicalResult {
      if (targetNum < opNum)
        return op.emitOpError("number of target " + kind + " (")
               << targetNum << ") cannot be less than the number of " << kind
               << "(" << opNum << ") for the operation";

      if (mapping.size() != opNum)
        return op.emitOpError("number of entries in the mapping for " + kind +
                              " (")
               << mapping.size() << ") should match the number of " << kind
               << " for the operation (" << opNum << ")";

      std::unordered_set<int64_t> entries;
      // Each entry in the mapping should be < target_num and an entry cannot
      // appear more than once.
      for (Attribute entry : mapping) {
        int64_t intEntry = entry.cast<IntegerAttr>().getInt();
        // ODS verification will ensure that these entries are integers.
        if (!entries.insert(intEntry).second)
          return op.emitOpError("entry ")
                 << intEntry
                 << " cannot appear more than once in the mapping for " << kind;
        if (intEntry < 0 || intEntry >= targetNum)
          return op.emitOpError(
                     "entries in mapping for " + kind +
                     " must be >= 0 and less than target's number of " + kind +
                     " (")
                 << targetNum << ")";
      }
      return success();
    };
    if (failed(verifyMapping(mapping.num_args().getInt(), op.args().size(),
                             mapping.args_to_target_args(), "args")) ||
        failed(verifyMapping(mapping.num_results().getInt(), op.output().size(),
                             mapping.results_to_target_results(), "results")))
      return failure();
  }
  return success();
}

//===----------------------------------------------------------------------===//
// PadOp
//===----------------------------------------------------------------------===//

// PadOp's verifier checks if :-
//  1. Operand and output ranks match.
//  2. Padding configurations are specified for each dimension.
//  3. Output shape matches the expected output shape when the padding
//     configurations are applied to the operand.
LogicalResult PadOp::verify() {
  PadOp op = *this;
  auto operandType = op.operand().getType().dyn_cast<ShapedType>();
  auto outputType = op.output().getType().dyn_cast<ShapedType>();
  if (!(operandType && outputType && operandType.hasRank() &&
        outputType.hasRank())) {
    return success();
  }

  unsigned rank = operandType.getRank();
  // Checks if operand and output ranks match.
  if (outputType.getRank() != rank) {
    return op.emitOpError()
           << "output's rank(" << outputType.getRank()
           << ") is not same as operand's rank(" << rank << ")";
  }

  auto edgePadLowRanges = op.edge_padding_low().getValues<int64_t>();
  auto edgePadHighRanges = op.edge_padding_high().getValues<int64_t>();
  auto interiorPadRanges = op.interior_padding().getValues<int64_t>();
  // Checks if padding configurations are specified for each dimension.
  if (edgePadLowRanges.size() != rank || edgePadHighRanges.size() != rank ||
      interiorPadRanges.size() != rank) {
    return op.emitOpError() << "pad configurations to be specified for all "
                            << rank << " dimensions";
  }

  // Checks if output shape matches the expected output shape when the padding
  // configurations are applied to the operand. Expected output shape for each
  // dimension is calculated as :-
  //     low_padding + operand_dim_size + total_interior_padding + high_padding
  //  where, total_interior_padding = (operand_dim_size - 1) * interior_padding.
  for (const auto& paddings : llvm::enumerate(
           llvm::zip(edgePadLowRanges, edgePadHighRanges, interiorPadRanges,
                     operandType.getShape(), outputType.getShape()))) {
    auto index = static_cast<unsigned>(paddings.index());
    int64_t lowPad, highPad, interiorPad, operandDimSize, outputDimSize;
    std::tie(lowPad, highPad, interiorPad, operandDimSize, outputDimSize) =
        paddings.value();
    int64_t expectedDimSize =
        lowPad + operandDimSize + (operandDimSize - 1) * interiorPad + highPad;
    if (expectedDimSize != outputDimSize) {
      return op.emitOpError()
             << "expected " << index << "-th dimension size after padding is "
             << expectedDimSize << " but found " << outputDimSize;
    }
  }

  return success();
}

//===----------------------------------------------------------------------===//
// ReduceOp
//===----------------------------------------------------------------------===//

// Removes `lmhlo.copy` inside ReduceOp body.
//
// TODO(b/183920887): Remove this pattern as soon as bufferization is fixed.
struct RemoveCopyInReduceBody : public OpRewritePattern<ReduceOp> {
  using OpRewritePattern<ReduceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ReduceOp reduce,
                                PatternRewriter& rewriter) const override {
    // Find the only `lmhlo.copy` in the body of `reduce`.
    CopyOp theOnlyCopy;
    for (auto& op : reduce.body().front()) {
      if (auto copy = dyn_cast<lmhlo::CopyOp>(op)) {
        if (theOnlyCopy == nullptr) {
          theOnlyCopy = copy;
        } else {
          theOnlyCopy = nullptr;
          break;
        }
      }
    }
    if (!theOnlyCopy) return failure();

    auto newReduce = rewriter.cloneWithoutRegions(reduce);
    auto& oldReduceBody = reduce.body().front();
    Block* newBlock = rewriter.createBlock(
        &newReduce.body(), newReduce.body().end(),
        oldReduceBody.getArgumentTypes(),
        SmallVector<Location>(oldReduceBody.getNumArguments(),
                              reduce.getLoc()));

    mlir::BlockAndValueMapping bvm;
    for (auto item : llvm::zip(reduce.body().front().getArguments(),
                               newBlock->getArguments())) {
      bvm.map(std::get<0>(item), std::get<1>(item));
    }
    bvm.map(theOnlyCopy.operand(), bvm.lookup(theOnlyCopy.output()));

    rewriter.setInsertionPointToStart(newBlock);
    for (auto& op : reduce.body().front()) {
      if (llvm::isa<lmhlo::CopyOp>(op) || llvm::isa<memref::DeallocOp>(op) ||
          llvm::isa<memref::AllocOp>(op))
        continue;
      rewriter.clone(op, bvm);
    }
    rewriter.eraseOp(reduce);
    return success();
  }
};

void ReduceOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                           MLIRContext* context) {
  results.add<RemoveCopyInReduceBody>(context);
}

//===----------------------------------------------------------------------===//
// ReduceWindowOp.
//===----------------------------------------------------------------------===//

// For reduce-window, all `inputs` need to have compatible shapes.
LogicalResult ReduceWindowOp::verify() {
  ReduceWindowOp op = *this;
  if (failed(verifyCompatibleShapes(op.inputs().getTypes())))
    return op.emitOpError() << "requires same shape for all operands";
  return success();
}

//===----------------------------------------------------------------------===//
// WhileOp
//===----------------------------------------------------------------------===//

void WhileOp::getSuccessorRegions(Optional<unsigned> index,
                                  ArrayRef<Attribute> /*operands*/,
                                  SmallVectorImpl<RegionSuccessor>& regions) {
  // If the predecessor is the WhileOp or the body region, branch into the
  // cond region.
  if (!index.hasValue() || index.getValue() == 1) {
    regions.push_back(RegionSuccessor(&cond(), cond().getArguments()));
    return;
  }
  // If the predecessor is the cond region, we can branch to the body region
  // or back to the parent operation.
  regions.push_back(RegionSuccessor(&body(), body().getArguments()));
  regions.push_back(RegionSuccessor());
}

Region& WhileOp::getLoopBody() { return body(); }

// suppress warning.

using mlir::hlo::parseWindowAttributes;
using mlir::hlo::printWindowAttributes;

}  // namespace lmhlo
}  // namespace mlir

#define GET_OP_CLASSES
#include "mlir-hlo/Dialect/lhlo/IR/lhlo_ops.cc.inc"

namespace mlir {
namespace lmhlo {

// TODO(cheshire): Support folding, reuse code from hlo_ops.cc.

void FusionOp::build(OpBuilder& builder, OperationState& result,
                     ArrayRef<NamedAttribute> attributes) {
  result.addAttributes(attributes);
  Region* bodyRegion = result.addRegion();
  FusionOp::ensureTerminator(*bodyRegion, builder, result.location);
}

void FusionOp::getSuccessorRegions(Optional<unsigned> index,
                                   ArrayRef<Attribute> /*operands*/,
                                   SmallVectorImpl<RegionSuccessor>& regions) {
  // If the predecessor is the fusion region, jump back to the parent op.
  if (index.hasValue()) {
    assert(index.getValue() == 0 && "expected fusion region");
    regions.push_back(RegionSuccessor());
  } else {
    // If the predecessor is the FusionOp, branch into the region.
    regions.push_back(RegionSuccessor(&region(), region().getArguments()));
  }
}

}  // namespace lmhlo
}  // namespace mlir
