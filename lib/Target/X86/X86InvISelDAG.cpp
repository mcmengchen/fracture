//===- X86InvISelDAG.cpp - Interface for X86 InvISelDAG  =========-*- C++ -*-=//
//
//              Fracture: The Draper Decompiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// This file takes code found in LLVM and modifies it.
//
//===----------------------------------------------------------------------===//
//
// Provides inverse instruction selector functionality for the ARM targets.
//
//===----------------------------------------------------------------------===//

#include "Target/X86/X86InvISelDAG.h"
#include "X86BaseInfo.h"

using namespace llvm;

namespace fracture {

#include "X86GenInvISel.inc"

/*! \brief Transmogrify converts Arch specific OpCodes to LLVM IR.
 *
 *  Transmogrify is the handles Arch specific OpCodes that are not automatically
 *      supported.  This method will either emit LLVM IR or in more complicated
 *      cases call into the IR Emitter.
 */
SDNode* X86InvISelDAG::Transmogrify(SDNode *N) {
  // Insert fixups here
  if (!N->isMachineOpcode()) {
    // Drop noreg registers
    if (N->getOpcode() == ISD::CopyFromReg) {
      const RegisterSDNode *R = dyn_cast<RegisterSDNode>(N->getOperand(1));
      if (R != NULL && R->getReg() == 0) {
        SDValue Tmp = CurDAG->getUNDEF(R->getValueType(0));
        CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), Tmp);
        CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 1), N->getOperand(0));
      }
    }

    // FIXME: This is wrong, originally it returns NULL.
    // I think we need to add it to the CurDAG (only if it hasn't
    // already been added to the CurDAG...)
    return N;                // already selected
  }

  uint16_t TargetOpc = N->getMachineOpcode();
  switch(TargetOpc) {
    default:
      outs() << "TargetOpc: " << TargetOpc << "\n";
      break;
    case X86::RETQ:
    case X86::RETL:{
      /**<
       * RET calls into the IREmitter which passes the default IR return.
       */
      SDValue Chain = N->getOperand(0);
      SDLoc SL(N);
      SDValue RetNode = CurDAG->getNode(X86ISD::RET_FLAG, SL, MVT::Other, Chain);
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), RetNode);

      return NULL;
      break;
    }
    case X86::POP32r:{
      /**<
       * POP32r Pseudo code
       * DEST ← SS:ESP; (* Copy a doubleword *)
       * ESP ← ESP + 4;
       */

      //Get the arguments
      SDValue Chain = N->getOperand(0);
      EVT LdType = N->getValueType(0);
      SDValue ESP = N->getOperand(1);

      const MachineSDNode *MN = dyn_cast<MachineSDNode>(N);
      MachineMemOperand *MMO = NULL;        //Basically a NOP
      if (MN->memoperands_empty()) {
        errs() << "NO MACHINE OPS for POP32r!\n";
      } else {
        MMO = *(MN->memoperands_begin());
      }

      SDLoc SL(N);
      SDValue Load = CurDAG->getLoad(LdType, SL, Chain, ESP, MMO);  //Load from ESP
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), Load);       //Store EBP
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 2), SDValue(Load.getNode(), 1));

      SDValue Width = CurDAG->getConstant(4, LdType);                       //Update ESP
      SDValue NewESP = CurDAG->getNode(ISD::ADD, SL, LdType, ESP, Width);   //ESP += 4;

      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 1), NewESP);             //Store ESP

      FixChainOp(Load.getNode());

      return NULL;
      break;
    }

    case X86::PUSH32r:{
      /**<
       * PUSH32r Pseudo code
       * ESP ← ESP – 4;
       * Memory[SS:ESP] ← SRC; (* push dword *)
       */

      //Get the arguments
      SDValue Chain = N->getOperand(0);
      SDValue EBP = N->getOperand(1);
      SDValue ESP = N->getOperand(2);

      const MachineSDNode *MN = dyn_cast<MachineSDNode>(N);
      MachineMemOperand *MMO = NULL;
      if (MN->memoperands_empty()) {
        errs() << "NO MACHINE OPS for PUSH32r!\n";
      } else {
        MMO = *(MN->memoperands_begin());
      }

      SDLoc SL(N);
      SDVTList SubVTList = CurDAG->getVTList(MVT::i32);
      SDValue Width = CurDAG->getConstant(4, MVT::i32);
      SDValue NewESP = CurDAG->getNode(ISD::SUB, SL, SubVTList, ESP, Width); //ESP -= 4;

      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), NewESP);


      //Store
      SDValue Store = CurDAG->getStore(Chain, SL, EBP, NewESP, MMO);
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 1), Store);

      FixChainOp(Store.getNode());

      return NULL;
      break;
    }

    case X86::MOV32rr:{
      /**<
       * MOV32rr notes
       * Cannot use dummy arithmetic because it will get folded
       * (removed) from the DAG. Instead we search for a CopyToReg, if it
       * exists we set it's debug location to the Mov, and if it doesn't we
       * print an error and do nothing.
       */
      SDNode *C2R = NULL;
      for (SDNode::use_iterator I = N->use_begin(), E = N->use_end(); I != E; ++I) {
        if (I->getOpcode() == ISD::CopyToReg) {
          C2R = *I;
          break;
        }
      }

      assert(C2R && "Move instruction without CopytoReg!");
      C2R->setDebugLoc(N->getDebugLoc());
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N,0), N->getOperand(0));

      return NULL;
      break;
    }
    case X86::CALLpcrel32:{
      /**<
       * CALLpcrel32 notes
       * Calls into X86ISD, which is between the OpCode and IR.  This instruction is
       *    more easily handled later on.
       */
      SDValue Chain = N->getOperand(0);
      uint64_t TarVal = N->getConstantOperandVal(1);
      SDValue Target = CurDAG->getConstant(TarVal, MVT::i32);
      //SDValue noReg = N->getOperand(2);

      SDLoc SL(N);

      SDValue CallNode = CurDAG->getNode(X86ISD::CALL, SL, MVT::Other, Target, Chain);
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), CallNode);

      return NULL;
      break;
    }
    case X86::LEAVE:{
      /**<
       * LEAVE Pseudo code
       * ESP ← EBP;
       * EBP ← Pop();
       */
      const MachineSDNode *MN = dyn_cast<MachineSDNode>(N);
      MachineMemOperand *MMO = NULL;
      if (MN->memoperands_empty()) {
        errs() << "NO MACHINE OPS for PUSH32r!\n";
      } else {
        MMO = *(MN->memoperands_begin());
      }

      EVT LdType = N->getValueType(0);
      SDValue Chain = N->getOperand(0);
      SDValue EBP = N->getOperand(1);
      //SDValue ESP = N->getOperand(2);

      SDLoc SL(N);
      SDValue LoadEBP = CurDAG->getLoad(LdType, SL, Chain, EBP, MMO); //Load from EBP
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), LoadEBP);

      SDValue Width = CurDAG->getConstant(4, MVT::i32);
      SDValue NewESP = CurDAG->getNode(ISD::ADD, SL, MVT::i32, EBP, Width); //ESP -= 4;
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 1), NewESP);

      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 2), SDValue(LoadEBP.getNode(),1));   //Chain

      //For getLoad or getStore
      FixChainOp(LoadEBP.getNode());

      return NULL;
      break;
    }
    case X86::LEA32r:{
      /**<
       * LEA32r (Load Effective Address) Pseudo code
       * DEST ← EffectiveAddress(SRC); (* 16-bit address *)
       * From: http://www.ncsa.illinois.edu/People/kindr/teaching/ece190_sp10/files/PS4.pdf
       * LEA Rx, imm_val | Rx <= PC1+imm_val
       */
      SDValue Base = ConvertNoRegToZero(N->getOperand(0));
      SDValue Scale = ConvertNoRegToZero(N->getOperand(1));
      SDValue Index = ConvertNoRegToZero(N->getOperand(2));
      SDValue Disp = ConvertNoRegToZero(N->getOperand(3));
      //SDValue Seg = ConvertNoRegToZero(N->getOperand(4)); //Ignore me for now

      SDLoc SL(N);
      SDValue Mul = CurDAG->getNode(ISD::MUL, SL, MVT::i32, Index, Scale);
      SDValue Add = CurDAG->getNode(ISD::ADD, SL, MVT::i32, Disp, Base);
      SDValue Address = CurDAG->getNode(ISD::ADD, SL, MVT::i32, Mul, Add);

      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), Address);

      return NULL;
      break;
    }
    case X86::CMP32mi8:
    case X86::CMP32mi:{
       /**<
        * 6 inputs & 2 outputs (i32, ch)
        *
        * CMP32mi Manual Description
        * Compares the first source operand with the second source operand and sets the status flags in the EFLAGS register
        * according to the results. The comparison is performed by subtracting the second operand from the first operand
        * and then setting the status flags in the same manner as the SUB instruction. When an immediate value is used as
        * an operand, it is sign-extended to the length of the first operand.
        *
        */

      EVT LdType = N->getValueType(0);
      SDValue Chain = N->getOperand(0);
      SDValue EBP = N->getOperand(1);
      SDValue C1 = N->getOperand(2);
      /*
      SDValue NoReg1 = N->getOperand(3);
      SDValue C8 = N->getOperand(4);
      SDValue NoReg2 = N->getOperand(5);
      SDValue C10 = N->getOperand(6);
      */

      const MachineSDNode *MN = dyn_cast<MachineSDNode>(N);
      MachineMemOperand *MMO = NULL;        //Basically a NOP
      if (MN->memoperands_empty()) {
        errs() << "NO MACHINE OPS for POP32r!\n";
      } else {
        MMO = *(MN->memoperands_begin());
      }

      SDLoc SL(N);
      SDValue LoadEBP = CurDAG->getLoad(LdType, SL, Chain, EBP, MMO);  //Load from EBP

      SDVTList VTList = CurDAG->getVTList(MVT::i32, MVT::Other);
      SDValue NewESP = CurDAG->getNode(X86ISD::CMP , SL, VTList, SDValue(LoadEBP.getNode(),1), LoadEBP, C1); //EBP == C1;
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 1), NewESP);

      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), SDValue(NewESP.getNode(),1));   //Chain

      FixChainOp(LoadEBP.getNode());

      return NULL;
      break;
    }
    case X86::JNE_1:{
      /**<
       * JNE_1 (Jump short if Not Equal) Pseudo code
       * IF condition
       *    THEN
       *        tempEIP ← EIP + SignExtend(DEST);
       *        IF OperandSize = 16
       *            THEN tempEIP ← tempEIP AND 0000FFFFH;
       *        FI;
       *    IF tempEIP is not within code segment limit
       *        THEN #GP(0);
       *        ELSE EIP ← tempEIP
       *    FI;
       * FI;
       */
      JumpOnCondition(N, ISD::SETNE);

      return NULL;
      break;
    }
    case X86::JE_1:{
      /**<
       * JE_1 - Jump if Equal/zero
       */

      JumpOnCondition(N, ISD::SETEQ);

      return NULL;
      break;
    }
    case X86::JL_1:{
      /**<
       * JL_1 - Jump if less than
       */

      JumpOnCondition(N, ISD::SETLT);

      return NULL;
      break;
    }
    case X86::JLE_1:{
      /**<
       * JLE_1 - Jump if less or equal
       */

      JumpOnCondition(N, ISD::SETLE);

      return NULL;
      break;
    }
    case X86::JG_1:{
      /**<
       * JG_1 - Jump if Greater/not less or equal
       */

      JumpOnCondition(N, ISD::SETGT);

      return NULL;
      break;
    }
    case X86::JGE_1:{
      /**<
       * JGE_1 - Jump if Greater or equal
       */

      JumpOnCondition(N, ISD::SETGE);

      return NULL;
      break;
    }
    case X86::JBE_1:{
      /**<
       * JBE_1 - Jump if Below or Equal
       */

      JumpOnCondition(N, ISD::SETLE);

      return NULL;
      break;
    }
    case X86::JAE_1:{
      /**<
       * JAE_1 - Jump if Above or Equal
       */

      JumpOnCondition(N, ISD::SETGE);

      return NULL;
      break;
    }
    case X86::JA_1:{
      /**<
       * JA_1 - Jump if Above
       */

      JumpOnCondition(N, ISD::SETGT);

      return NULL;
      break;
    }
    case X86::JMP_1:{
      /*
       *    IF OperandSize = 32
       *        THEN
       *            EIP ← tempEIP;
       *        ELSE
       *            IF OperandSize = 16
       *                THEN (* OperandSize = 16 *)
       *                    EIP ← tempEIP AND 0000FFFFH;
       *                ELSE (* OperandSize = 64)
       *                    RIP ← tempRIP;
       *            FI;
       *    FI;
       */

      //Calling Jump On Condition with the always true variable.
      //EFLAGS isn't used in JumpOnCondition, so this should work, however there may be
      //    subtle differences if compiled back...
      JumpOnCondition(N, ISD::SETTRUE2);

      //Original logic if differences are a concern.
      /*
      SDValue Chain = N->getOperand(0);
      uint64_t TarVal = N->getConstantOperandVal(1);
      SDValue tempEIP = CurDAG->getConstant(TarVal, MVT::i32);

      SDLoc SL(N);
      // Calculate the Branch Target
      SDValue TempEIPval = CurDAG->getConstant(cast<ConstantSDNode>(tempEIP)->getZExtValue(), tempEIP.getValueType());

      SDValue BrNode = CurDAG->getNode(ISD::BR, SL, MVT::Other, TempEIPval, Chain);
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), BrNode);
      */
      return NULL;
      break;
    }
    //case X86::SHLD32rri8:{
      /*
       * The SHLD instruction is used for multi-precision shifts of 64 bits or more.
       *
       * The instruction shifts the first operand (destination operand) to the left the number of bits specified by the third
       * operand (count operand). The second operand (source operand) provides bits to shift in from the right (starting
       * with bit 0 of the destination operand).
       *
       * The destination operand can be a register or a memory location; the source operand is a register. The count
       * operand is an unsigned integer that can be stored in an immediate byte or in the CL register. If the count operand
       * is CL, the shift count is the logical AND of CL and a count mask. In non-64-bit modes and default 64-bit mode; only
       * bits 0 through 4 of the count are used. This masks the count to a value between 0 and 31. If a count is greater than
       * the operand size, the result is undefined.
       *
       * If the count is 1 or greater, the CF flag is filled with the last bit shifted out of the destination operand. For a 1-bit
       * shift, the OF flag is set if a sign change occurred; otherwise, it is cleared. If the count operand is 0, flags are not
       * affected.
       *
       *    Inputs 0 = EDX, 1 = EAX, 2 = Constant<31>
       *    Outputs i32, i32
       *
       *    C Code:
       *    int myCtr(int inp){
       *        if(inp == 10)
       *            return inp;
       *        return (inp + myCtr(inp+1));
       *    }
       *    Compiler generated IR - clang -m32 -O1 testing.c -emit-llvm -S -o testing_clang_01.ll
       *    tailrecurse.return_crit_edge:                     ; preds = %entry
       *        %0 = sub i32 9, %inp
       *        %1 = sub i32 8, %inp
       *        %2 = zext i32 %1 to i33
       *        %3 = zext i32 %0 to i33
       *        %4 = mul i33 %3, %2
       *        %5 = add i32 %inp, 1
       *        %6 = lshr i33 %4, 1
       *        %7 = mul i32 %0, %5
       *        %8 = trunc i33 %6 to i32
       *        %9 = add i32 %7, %inp
       *        %10 = add i32 %9, %8
       *        %11 = add i32 %10, 10
       *        br label %return
       */
      /*
      EVT LdType = N->getValueType(0);
      SDValue EDX = N->getOperand(0);
      SDValue EAX = N->getOperand(1);
      SDValue Constant = N->getOperand(2);

      SDLoc SL(N);


      SDValue EDXShl = CurDAG->getNode(ISD::SHL , SL, MVT::i32, EDX, Constant ); //Shift EDX by Constant;
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), EDXShl);

      //Note: This is obviously incorrect, I'm not sure what the correct approach is to get this working...
      SDValue NotCorrect = CurDAG->getNode(ISD::MUL , SL, MVT::i32, EAX, Constant );
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 1), NotCorrect);

      return NULL;
      break;
    } */
    //case X86::MUL32r:{
      /*
       * 2 Inputs (EDX, EAX) and 3 i32 outputs. 3 outputs seem odd...
       *    Each goto CopyToReg arg 2.  Each CopyToReg is in the chain.
       *    Therefore assuming each i32 is the same.
       */
    /*
      SDValue EDX = N->getOperand(0);
      SDValue EAX = N->getOperand(1);

      SDLoc SL(N);
      SDValue MulOut = CurDAG->getNode(ISD::MUL , SL, MVT::i32, EDX, EAX); //EBP * EAX;
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), MulOut);
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 1), MulOut);
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 2), MulOut);

      return NULL;
      break;
    }*/
    /*


    case X86::CMP32rm:
    case X86::CMP32mr:{
      To Do...
    }
    //case X86::MOV32mi:
    //case X86::MOV32rr_REV:
    //case X86::MOV64rr:

    case X86::JA_1:{    //JMP Short if Above
      SDValue Chain = N->getOperand(0);
      uint64_t TarVal = N->getConstantOperandVal(1);
      SDValue Target = CurDAG->getConstant(TarVal, MVT::i32);
      //SDValue EFLAGSCFR = N->getZExtValuegetOperand(2);
      SDLoc SL(N);

      // Calculate the Branch Target
      SDValue BT = CurDAG->getConstant(
          cast<ConstantSDNode>(Target)->getZExtValue(), Target.getValueType());

      // Condition Code is "Below or Equal" <=
      SDValue CC = CurDAG->getCondCode(ISD::SETGT);
      SDValue BrNode =ConvertNoRegToUNDEF
          CurDAG->getNode(ISD::BRCOND, SL, MVT::Other, CC, BT, Chain);
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), BrNode);
LdType
      return NULL;
      break;
    }

    case X86::ADD32mi8:{
      SDValue Chain = N->getOperand(0);
      SDValue EBP = N->getOperand(1);
      uint64_t C2val = N->getConstantOperandVal(2);
      SDValue C2 = CurDAG->getConstant(C2val, MVT::i32);
      //3 is undef
      uint64_t Cn4val = N->getConstantOperandVal(4);
      SDValue Cn4 = CurDAG->getConstant(C      //return CurDAG->getUNDEF(R->getValueType(0));n4val, MVT::i32);
      //4 is undef
      //6 is Constant 1

      SDLoc SL(N);
      SDVTList VTList = CurDAG->getVTList(MVT::i32, MVT::Other);
      // AJG: Not sure how fill in semantics yet.
      //SDValue v2 = N->getOperand(2);


      //SDValue Node = CurDAG->getNode(ISD::ADD, SL, VTList, v1, v2, Chain);
      //CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), Node);


      return NULL;
      break;
    }
    case X86::ADD32mr:{
      SDValue Chain = N->getOperand(0);
      SDValue EBP = N->getOperand(1);
      uint64_t C1val = N->getConstantOperandVal(2);== When to use getStore ==
      SDValue C1 = CurDAG->getConstant(C1val, MVT::i32);
      //3 is noReg
      uint64_t Cn4val = N->getConstantOperandVal(4);
      SDValue Cn4 = CurDAG->getConstant(Cn4val, MVT::i32);
      //5 is noReg

      SDLoc SL(N);
      SDVTList VTList = CurDAG->getVTList(MVT::i32, MVT::Other);

      return NULL;
      break;
    }
    case X86::ADD32rr_REV:{ //Note: no chain, two input, two output
      SDValue ESI = N->getOperand(0);
      SDValue EAX = N->getOperand(1);

      SDLoc SL(N);
      SDVTList VTList = CurDAG->getVTList(MVT::i32, MVT::i32);

      SDValue Node = CurDAG->getNode(ISD::ADD, SL, VTList, EAX, ESI);
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), Node);

      return NULL;
      break;
    }

    case X86::ADD32i32:{
      uint64_t C1val = N->getConstantOperandVal(0);
      SDValue C1 = CurDAG->getConstant(C1val, MVT::i32);
      SDValue C2 = N->getOperand(1);

      SDLoc SL(N);
      SDVTList VTList = CurDAG->getVTList(MVT::i32, MVT::i32);

      SDValue Node = CurDAG->getNode(ISD::ADD, SL, VTList, C2, C1);
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), Node);

      return NULL;
      break;
    }
    case X86::SUB32ri8:
    case X86::SUB32i32:{
      uint64_t C1val = N->getConstantOperandVal(0);
      SDValue C1 = CurDAG->getConstant(C1val, MVT::i32);
      SDValue C2 = N->getOperand(1);

      SDLoc SL(N);
      SDVTList VTList = CurDAG->getVTList(MVT::i32, MVT::i32);

      SDValue Node = CurDAG->getNode(ISD::SUB, SL, VTList, C2, C1);
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), Node);

      return NULL;
      break;
    }
    */

  }

  SDNode* TheRes = InvertCode(N);
  return TheRes;
}

/*! \brief JumpOnCondition Jump on a specific condition.
 *
 *  This method applies when a Jump opcode has a condition and three inputs.
 */
bool X86InvISelDAG::JumpOnCondition(SDNode *N, ISD::CondCode cond) {
  //Only seem to have enough arguments to do tempEIP is not within code segment limit
  //Comparison should be before this instruction, result stored in EFLAGS
  SDValue Chain = N->getOperand(0);
  uint64_t TarVal = N->getConstantOperandVal(1);
  SDValue tempEIP = CurDAG->getConstant(TarVal, MVT::i32);
  //SDValue EFLAGSCFR = N->getOperand(2);


  SDLoc SL(N);
  // Calculate the Branch Target
  SDValue TempEIPval = CurDAG->getConstant(cast<ConstantSDNode>(tempEIP)->getZExtValue(), tempEIP.getValueType());

  // Condition Code is "Below or Equal" <=
  SDValue Condition = CurDAG->getCondCode(cond);
  SDValue BrNode = CurDAG->getNode(ISD::BRCOND, SL, MVT::Other, Condition, TempEIPval, Chain);
  CurDAG->ReplaceAllUsesOfValueWith(SDValue(N, 0), BrNode);

  return true;
}

/*! \brief ConvertNoRegToZero handles the NoReg input case.
 *
 *  ConvertNoRegToZero NoReg inputs were causing fracture to crash.  This
 *      method converts those cases to an i32 constant.
 */
SDValue X86InvISelDAG::ConvertNoRegToZero(const SDValue N){
  if(N.getOpcode() == ISD::CopyFromReg ){
    const RegisterSDNode *R = dyn_cast<RegisterSDNode>(N.getOperand(1));
    if (R != NULL && R->getReg() == 0)
      return CurDAG->getConstant(4, MVT::i32);
  }
  return N;
}


}
