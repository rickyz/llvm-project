//===-- ARMTargetTransformInfo.cpp - ARM specific TTI pass ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
/// This file implements a TargetTransformInfo analysis pass specific to the
/// ARM target machine. It uses the target's detailed information to provide
/// more precise answers to certain TTI queries, while letting the target
/// independent and default TTI implementations handle the rest.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "armtti"
#include "ARM.h"
#include "ARMTargetMachine.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/CostTable.h"
using namespace llvm;

// Declare the pass initialization routine locally as target-specific passes
// don't havve a target-wide initialization entry point, and so we rely on the
// pass constructor initialization.
namespace llvm {
void initializeARMTTIPass(PassRegistry &);
}

namespace {

class ARMTTI : public ImmutablePass, public TargetTransformInfo {
  const ARMBaseTargetMachine *TM;
  const ARMSubtarget *ST;
  const ARMTargetLowering *TLI;

  /// Estimate the overhead of scalarizing an instruction. Insert and Extract
  /// are set if the result needs to be inserted and/or extracted from vectors.
  unsigned getScalarizationOverhead(Type *Ty, bool Insert, bool Extract) const;

public:
  ARMTTI() : ImmutablePass(ID), TM(0), ST(0), TLI(0) {
    llvm_unreachable("This pass cannot be directly constructed");
  }

  ARMTTI(const ARMBaseTargetMachine *TM)
      : ImmutablePass(ID), TM(TM), ST(TM->getSubtargetImpl()),
        TLI(TM->getTargetLowering()) {
    initializeARMTTIPass(*PassRegistry::getPassRegistry());
  }

  virtual void initializePass() {
    pushTTIStack(this);
  }

  virtual void finalizePass() {
    popTTIStack();
  }

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    TargetTransformInfo::getAnalysisUsage(AU);
  }

  /// Pass identification.
  static char ID;

  /// Provide necessary pointer adjustments for the two base classes.
  virtual void *getAdjustedAnalysisPointer(const void *ID) {
    if (ID == &TargetTransformInfo::ID)
      return (TargetTransformInfo*)this;
    return this;
  }

  /// \name Scalar TTI Implementations
  /// @{

  virtual unsigned getIntImmCost(const APInt &Imm, Type *Ty) const;

  /// @}


  /// \name Vector TTI Implementations
  /// @{

  unsigned getNumberOfRegisters(bool Vector) const {
    if (Vector) {
      if (ST->hasNEON())
        return 16;
      return 0;
    }

    if (ST->isThumb1Only())
      return 8;
    return 16;
  }

  unsigned getRegisterBitWidth(bool Vector) const {
    if (Vector) {
      if (ST->hasNEON())
        return 128;
      return 0;
    }

    return 32;
  }

  unsigned getMaximumUnrollFactor() const {
    // These are out of order CPUs:
    if (ST->isCortexA15() || ST->isSwift())
      return 2;
    return 1;
  }

  unsigned getCastInstrCost(unsigned Opcode, Type *Dst,
                                      Type *Src) const;

  unsigned getVectorInstrCost(unsigned Opcode, Type *Val, unsigned Index) const;
  /// @}
};

} // end anonymous namespace

INITIALIZE_AG_PASS(ARMTTI, TargetTransformInfo, "armtti",
                   "ARM Target Transform Info", true, true, false)
char ARMTTI::ID = 0;

ImmutablePass *
llvm::createARMTargetTransformInfoPass(const ARMBaseTargetMachine *TM) {
  return new ARMTTI(TM);
}


unsigned ARMTTI::getIntImmCost(const APInt &Imm, Type *Ty) const {
  assert(Ty->isIntegerTy());

  unsigned Bits = Ty->getPrimitiveSizeInBits();
  if (Bits == 0 || Bits > 32)
    return 4;

  int32_t SImmVal = Imm.getSExtValue();
  uint32_t ZImmVal = Imm.getZExtValue();
  if (!ST->isThumb()) {
    if ((SImmVal >= 0 && SImmVal < 65536) ||
        (ARM_AM::getSOImmVal(ZImmVal) != -1) ||
        (ARM_AM::getSOImmVal(~ZImmVal) != -1))
      return 1;
    return ST->hasV6T2Ops() ? 2 : 3;
  } else if (ST->isThumb2()) {
    if ((SImmVal >= 0 && SImmVal < 65536) ||
        (ARM_AM::getT2SOImmVal(ZImmVal) != -1) ||
        (ARM_AM::getT2SOImmVal(~ZImmVal) != -1))
      return 1;
    return ST->hasV6T2Ops() ? 2 : 3;
  } else /*Thumb1*/ {
    if (SImmVal >= 0 && SImmVal < 256)
      return 1;
    if ((~ZImmVal < 256) || ARM_AM::isThumbImmShiftedVal(ZImmVal))
      return 2;
    // Load from constantpool.
    return 3;
  }
  return 2;
}

unsigned ARMTTI::getCastInstrCost(unsigned Opcode, Type *Dst,
                                    Type *Src) const {
  int ISD = TLI->InstructionOpcodeToISD(Opcode);
  assert(ISD && "Invalid opcode");

  EVT SrcTy = TLI->getValueType(Src);
  EVT DstTy = TLI->getValueType(Dst);

  if (!SrcTy.isSimple() || !DstTy.isSimple())
    return TargetTransformInfo::getCastInstrCost(Opcode, Dst, Src);

  // Some arithmetic, load and store operations have specific instructions
  // to cast up/down their types automatically at no extra cost
  // TODO: Get these tables to know at least what the related operations are
  static const TypeConversionCostTblEntry<MVT> NEONConversionTbl[] = {
    { ISD::SIGN_EXTEND, MVT::v4i32, MVT::v4i16, 0 },
    { ISD::ZERO_EXTEND, MVT::v4i32, MVT::v4i16, 0 },
    { ISD::SIGN_EXTEND, MVT::v2i64, MVT::v2i32, 1 },
    { ISD::ZERO_EXTEND, MVT::v2i64, MVT::v2i32, 1 },
    { ISD::TRUNCATE,    MVT::v4i32, MVT::v4i64, 0 },
    { ISD::TRUNCATE,    MVT::v4i16, MVT::v4i32, 1 },
  };

  if (ST->hasNEON()) {
    int Idx = ConvertCostTableLookup<MVT>(NEONConversionTbl,
                                array_lengthof(NEONConversionTbl),
                                ISD, DstTy.getSimpleVT(), SrcTy.getSimpleVT());
    if (Idx != -1)
      return NEONConversionTbl[Idx].Cost;
  }

  return TargetTransformInfo::getCastInstrCost(Opcode, Dst, Src);
}

unsigned ARMTTI::getVectorInstrCost(unsigned Opcode, Type *ValTy,
                                    unsigned Index) const {
  // Penalize inserting into an D-subregister.
  if (ST->isSwift() &&
      Opcode == Instruction::InsertElement &&
      ValTy->isVectorTy() &&
      ValTy->getScalarSizeInBits() <= 32)
    return 2;

  return TargetTransformInfo::getVectorInstrCost(Opcode, ValTy, Index);
}
