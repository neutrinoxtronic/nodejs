// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/bits.h"
#include "src/base/logging.h"
#include "src/codegen/assembler-inl.h"
#include "src/codegen/machine-type.h"
#include "src/compiler/backend/instruction-selector-impl.h"
#include "src/compiler/node-matchers.h"
#include "src/compiler/node-properties.h"
#include "src/compiler/turboshaft/operations.h"

namespace v8 {
namespace internal {
namespace compiler {

#define TRACE_UNIMPL() \
  PrintF("UNIMPLEMENTED instr_sel: %s at line %d\n", __FUNCTION__, __LINE__)

#define TRACE() PrintF("instr_sel: %s at line %d\n", __FUNCTION__, __LINE__)

// Adds loong64-specific methods for generating InstructionOperands.
template <typename Adapter>
class Loong64OperandGeneratorT final : public OperandGeneratorT<Adapter> {
 public:
  OPERAND_GENERATOR_T_BOILERPLATE(Adapter)

  explicit Loong64OperandGeneratorT(InstructionSelectorT<Adapter>* selector)
      : super(selector) {}

  InstructionOperand UseOperand(Node* node, InstructionCode opcode) {
    if (CanBeImmediate(node, opcode)) {
      return UseImmediate(node);
    }
    return UseRegister(node);
  }

  // Use the zero register if the node has the immediate value zero, otherwise
  // assign a register.
  InstructionOperand UseRegisterOrImmediateZero(Node* node) {
    if ((IsIntegerConstant(node) && (GetIntegerConstantValue(node) == 0)) ||
        (IsFloatConstant(node) &&
         (base::bit_cast<int64_t>(GetFloatConstantValue(node)) == 0))) {
      return UseImmediate(node);
    }
    return UseRegister(node);
  }

  MachineRepresentation GetRepresentation(Node* node) {
    return this->sequence()->GetRepresentation(
        selector()->GetVirtualRegister(node));
  }

  bool IsIntegerConstant(Node* node) {
    return (node->opcode() == IrOpcode::kInt32Constant) ||
           (node->opcode() == IrOpcode::kInt64Constant);
  }

  int64_t GetIntegerConstantValue(Node* node) {
    if (node->opcode() == IrOpcode::kInt32Constant) {
      return OpParameter<int32_t>(node->op());
    }
    DCHECK_EQ(IrOpcode::kInt64Constant, node->opcode());
    return OpParameter<int64_t>(node->op());
  }

  bool IsFloatConstant(Node* node) {
    return (node->opcode() == IrOpcode::kFloat32Constant) ||
           (node->opcode() == IrOpcode::kFloat64Constant);
  }

  double GetFloatConstantValue(Node* node) {
    if (node->opcode() == IrOpcode::kFloat32Constant) {
      return OpParameter<float>(node->op());
    }
    DCHECK_EQ(IrOpcode::kFloat64Constant, node->opcode());
    return OpParameter<double>(node->op());
  }

  bool CanBeImmediate(Node* node, InstructionCode mode) {
    if (node->opcode() == IrOpcode::kCompressedHeapConstant) {
      if (!COMPRESS_POINTERS_BOOL) return false;
      // For builtin code we need static roots
      if (selector()->isolate()->bootstrapper() && !V8_STATIC_ROOTS_BOOL) {
        return false;
      }
      const RootsTable& roots_table = selector()->isolate()->roots_table();
      RootIndex root_index;
      CompressedHeapObjectMatcher m(node);
      if (m.HasResolvedValue() &&
          roots_table.IsRootHandle(m.ResolvedValue(), &root_index)) {
        if (!RootsTable::IsReadOnly(root_index)) return false;
        return CanBeImmediate(MacroAssemblerBase::ReadOnlyRootPtr(
                                  root_index, selector()->isolate()),
                              mode);
      }
      return false;
    }

    return IsIntegerConstant(node) &&
           CanBeImmediate(GetIntegerConstantValue(node), mode);
  }

  bool CanBeImmediate(int64_t value, InstructionCode opcode) {
    switch (ArchOpcodeField::decode(opcode)) {
      case kLoong64Cmp32:
      case kLoong64Cmp64:
        return true;
      case kLoong64Sll_w:
      case kLoong64Srl_w:
      case kLoong64Sra_w:
        return is_uint5(value);
      case kLoong64Sll_d:
      case kLoong64Srl_d:
      case kLoong64Sra_d:
        return is_uint6(value);
      case kLoong64And:
      case kLoong64And32:
      case kLoong64Or:
      case kLoong64Or32:
      case kLoong64Xor:
      case kLoong64Xor32:
      case kLoong64Tst:
        return is_uint12(value);
      case kLoong64Ld_b:
      case kLoong64Ld_bu:
      case kLoong64St_b:
      case kLoong64Ld_h:
      case kLoong64Ld_hu:
      case kLoong64St_h:
      case kLoong64Ld_w:
      case kLoong64Ld_wu:
      case kLoong64St_w:
      case kLoong64Ld_d:
      case kLoong64St_d:
      case kLoong64Fld_s:
      case kLoong64Fst_s:
      case kLoong64Fld_d:
      case kLoong64Fst_d:
        return is_int16(value);
      default:
        return is_int12(value);
    }
  }

 private:
  bool ImmediateFitsAddrMode1Instruction(int32_t imm) const {
    TRACE_UNIMPL();
    return false;
  }
};

static void VisitRR(InstructionSelectorT<TurbofanAdapter>* selector,
                    ArchOpcode opcode, Node* node) {
  Loong64OperandGeneratorT<TurbofanAdapter> g(selector);
  selector->Emit(opcode, g.DefineAsRegister(node),
                 g.UseRegister(node->InputAt(0)));
}

template <typename T>
void VisitRR(InstructionSelectorT<TurboshaftAdapter>*, ArchOpcode, T) {
  UNIMPLEMENTED();
}

template <typename Adapter>
static void VisitRRI(InstructionSelectorT<Adapter>* selector, ArchOpcode opcode,
                     Node* node) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  int32_t imm = OpParameter<int32_t>(node->op());
  selector->Emit(opcode, g.DefineAsRegister(node),
                 g.UseRegister(node->InputAt(0)), g.UseImmediate(imm));
}

template <typename Adapter>
static void VisitSimdShift(InstructionSelectorT<Adapter>* selector,
                           ArchOpcode opcode, Node* node) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  if (g.IsIntegerConstant(node->InputAt(1))) {
    selector->Emit(opcode, g.DefineAsRegister(node),
                   g.UseRegister(node->InputAt(0)),
                   g.UseImmediate(node->InputAt(1)));
  } else {
    selector->Emit(opcode, g.DefineAsRegister(node),
                   g.UseRegister(node->InputAt(0)),
                   g.UseRegister(node->InputAt(1)));
  }
}

template <typename Adapter>
static void VisitRRIR(InstructionSelectorT<Adapter>* selector,
                      ArchOpcode opcode, Node* node) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  int32_t imm = OpParameter<int32_t>(node->op());
  selector->Emit(opcode, g.DefineAsRegister(node),
                 g.UseRegister(node->InputAt(0)), g.UseImmediate(imm),
                 g.UseRegister(node->InputAt(1)));
}

static void VisitRRR(InstructionSelectorT<TurbofanAdapter>* selector,
                     ArchOpcode opcode, Node* node) {
  Loong64OperandGeneratorT<TurbofanAdapter> g(selector);
  selector->Emit(opcode, g.DefineAsRegister(node),
                 g.UseRegister(node->InputAt(0)),
                 g.UseRegister(node->InputAt(1)));
}

template <typename T>
void VisitRRR(InstructionSelectorT<TurboshaftAdapter>*, ArchOpcode, T) {
  UNIMPLEMENTED();
}

template <typename Adapter>
static void VisitUniqueRRR(InstructionSelectorT<Adapter>* selector,
                           ArchOpcode opcode, Node* node) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  selector->Emit(opcode, g.DefineAsRegister(node),
                 g.UseUniqueRegister(node->InputAt(0)),
                 g.UseUniqueRegister(node->InputAt(1)));
}

template <typename Adapter>
void VisitRRRR(InstructionSelectorT<Adapter>* selector, ArchOpcode opcode,
               Node* node) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  selector->Emit(
      opcode, g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)),
      g.UseRegister(node->InputAt(1)), g.UseRegister(node->InputAt(2)));
}

template <typename Adapter>
static void VisitRRO(InstructionSelectorT<Adapter>* selector, ArchOpcode opcode,
                     Node* node) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  selector->Emit(opcode, g.DefineAsRegister(node),
                 g.UseRegister(node->InputAt(0)),
                 g.UseOperand(node->InputAt(1), opcode));
}

template <typename Adapter>
struct ExtendingLoadMatcher {
  ExtendingLoadMatcher(Node* node, InstructionSelectorT<Adapter>* selector)
      : matches_(false), selector_(selector), base_(nullptr), immediate_(0) {
    Initialize(node);
  }

  bool Matches() const { return matches_; }

  Node* base() const {
    DCHECK(Matches());
    return base_;
  }
  int64_t immediate() const {
    DCHECK(Matches());
    return immediate_;
  }
  ArchOpcode opcode() const {
    DCHECK(Matches());
    return opcode_;
  }

 private:
  bool matches_;
  InstructionSelectorT<Adapter>* selector_;
  Node* base_;
  int64_t immediate_;
  ArchOpcode opcode_;

  void Initialize(Node* node) {
    Int64BinopMatcher m(node);
    // When loading a 64-bit value and shifting by 32, we should
    // just load and sign-extend the interesting 4 bytes instead.
    // This happens, for example, when we're loading and untagging SMIs.
    DCHECK(m.IsWord64Sar());
    if (m.left().IsLoad() && m.right().Is(32) &&
        selector_->CanCover(m.node(), m.left().node())) {
      DCHECK_EQ(selector_->GetEffectLevel(node),
                selector_->GetEffectLevel(m.left().node()));
      MachineRepresentation rep =
          LoadRepresentationOf(m.left().node()->op()).representation();
      DCHECK_EQ(3, ElementSizeLog2Of(rep));
      if (rep != MachineRepresentation::kTaggedSigned &&
          rep != MachineRepresentation::kTaggedPointer &&
          rep != MachineRepresentation::kTagged &&
          rep != MachineRepresentation::kWord64) {
        return;
      }

      Loong64OperandGeneratorT<Adapter> g(selector_);
      Node* load = m.left().node();
      Node* offset = load->InputAt(1);
      base_ = load->InputAt(0);
      opcode_ = kLoong64Ld_w;
      if (g.CanBeImmediate(offset, opcode_)) {
        immediate_ = g.GetIntegerConstantValue(offset) + 4;
        matches_ = g.CanBeImmediate(immediate_, kLoong64Ld_w);
      }
    }
  }
};

template <typename Adapter>
bool TryEmitExtendingLoad(InstructionSelectorT<Adapter>* selector, Node* node,
                          Node* output_node) {
  ExtendingLoadMatcher<Adapter> m(node, selector);
  Loong64OperandGeneratorT<Adapter> g(selector);
  if (m.Matches()) {
    InstructionOperand inputs[2];
    inputs[0] = g.UseRegister(m.base());
    InstructionCode opcode =
        m.opcode() | AddressingModeField::encode(kMode_MRI);
    DCHECK(is_int32(m.immediate()));
    inputs[1] = g.TempImmediate(static_cast<int32_t>(m.immediate()));
    InstructionOperand outputs[] = {g.DefineAsRegister(output_node)};
    selector->Emit(opcode, arraysize(outputs), outputs, arraysize(inputs),
                   inputs);
    return true;
  }
  return false;
}

template <typename Adapter>
bool TryMatchImmediate(InstructionSelectorT<Adapter>* selector,
                       InstructionCode* opcode_return, Node* node,
                       size_t* input_count_return, InstructionOperand* inputs) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  if (g.CanBeImmediate(node, *opcode_return)) {
    *opcode_return |= AddressingModeField::encode(kMode_MRI);
    inputs[0] = g.UseImmediate(node);
    *input_count_return = 1;
    return true;
  }
  return false;
}

template <typename Adapter>
static void VisitBinop(InstructionSelectorT<Adapter>* selector, Node* node,
                       InstructionCode opcode, bool has_reverse_opcode,
                       InstructionCode reverse_opcode,
                       FlagsContinuationT<Adapter>* cont) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  Int32BinopMatcher m(node);
  InstructionOperand inputs[2];
  size_t input_count = 0;
  InstructionOperand outputs[1];
  size_t output_count = 0;

  if (TryMatchImmediate(selector, &opcode, m.right().node(), &input_count,
                        &inputs[1])) {
    inputs[0] = g.UseRegister(m.left().node());
    input_count++;
  } else if (has_reverse_opcode &&
             TryMatchImmediate(selector, &reverse_opcode, m.left().node(),
                               &input_count, &inputs[1])) {
    inputs[0] = g.UseRegister(m.right().node());
    opcode = reverse_opcode;
    input_count++;
  } else {
    inputs[input_count++] = g.UseRegister(m.left().node());
    inputs[input_count++] = g.UseOperand(m.right().node(), opcode);
  }

  outputs[output_count++] = g.DefineAsRegister(node);

  DCHECK_NE(0u, input_count);
  DCHECK_EQ(1u, output_count);
  DCHECK_GE(arraysize(inputs), input_count);
  DCHECK_GE(arraysize(outputs), output_count);

  selector->EmitWithContinuation(opcode, output_count, outputs, input_count,
                                 inputs, cont);
}

template <typename Adapter>
static void VisitBinop(InstructionSelectorT<Adapter>* selector, Node* node,
                       InstructionCode opcode, bool has_reverse_opcode,
                       InstructionCode reverse_opcode) {
  FlagsContinuationT<Adapter> cont;
  VisitBinop(selector, node, opcode, has_reverse_opcode, reverse_opcode, &cont);
}

template <typename Adapter>
static void VisitBinop(InstructionSelectorT<Adapter>* selector, Node* node,
                       InstructionCode opcode,
                       FlagsContinuationT<Adapter>* cont) {
  VisitBinop(selector, node, opcode, false, kArchNop, cont);
}

template <typename Adapter>
static void VisitBinop(InstructionSelectorT<Adapter>* selector, Node* node,
                       InstructionCode opcode) {
  VisitBinop(selector, node, opcode, false, kArchNop);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitStackSlot(Node* node) {
  StackSlotRepresentation rep = StackSlotRepresentationOf(node->op());
  int alignment = rep.alignment();
  int slot = frame_->AllocateSpillSlot(rep.size(), alignment);
  OperandGenerator g(this);

  Emit(kArchStackSlot, g.DefineAsRegister(node),
       sequence()->AddImmediate(Constant(slot)), 0, nullptr);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitAbortCSADcheck(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kArchAbortCSADcheck, g.NoOutput(), g.UseFixed(node->InputAt(0), a0));
}

template <typename Adapter>
void EmitLoad(InstructionSelectorT<Adapter>* selector, Node* node,
              InstructionCode opcode, Node* output = nullptr) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);

  ExternalReferenceMatcher m(base);
  if (m.HasResolvedValue() && g.IsIntegerConstant(index) &&
      selector->CanAddressRelativeToRootsRegister(m.ResolvedValue())) {
    ptrdiff_t const delta =
        g.GetIntegerConstantValue(index) +
        MacroAssemblerBase::RootRegisterOffsetForExternalReference(
            selector->isolate(), m.ResolvedValue());
    // Check that the delta is a 32-bit integer due to the limitations of
    // immediate operands.
    if (is_int32(delta)) {
      opcode |= AddressingModeField::encode(kMode_Root);
      selector->Emit(opcode,
                     g.DefineAsRegister(output == nullptr ? node : output),
                     g.UseImmediate(static_cast<int32_t>(delta)));
      return;
    }
  }

  if (base != nullptr && base->opcode() == IrOpcode::kLoadRootRegister) {
    selector->Emit(opcode | AddressingModeField::encode(kMode_Root),
                   g.DefineAsRegister(output == nullptr ? node : output),
                   g.UseImmediate(index));
    return;
  }

  if (g.CanBeImmediate(index, opcode)) {
    selector->Emit(opcode | AddressingModeField::encode(kMode_MRI),
                   g.DefineAsRegister(output == nullptr ? node : output),
                   g.UseRegister(base), g.UseImmediate(index));
  } else {
    selector->Emit(opcode | AddressingModeField::encode(kMode_MRR),
                   g.DefineAsRegister(output == nullptr ? node : output),
                   g.UseRegister(base), g.UseRegister(index));
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitStoreLane(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitLoadLane(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitLoadTransform(Node* node) {
  LoadTransformParameters params = LoadTransformParametersOf(node->op());

  InstructionCode opcode = kArchNop;
  switch (params.transformation) {
      // TODO(LOONG_dev): LOONG64 S128 LoadSplat
    case LoadTransformation::kS128Load8Splat:
      opcode = kLoong64S128LoadSplat;
      break;
    case LoadTransformation::kS128Load16Splat:
      opcode = kLoong64S128LoadSplat;
      break;
    case LoadTransformation::kS128Load32Splat:
      opcode = kLoong64S128LoadSplat;
      break;
    case LoadTransformation::kS128Load64Splat:
      opcode = kLoong64S128LoadSplat;
      break;
    case LoadTransformation::kS128Load8x8S:
      opcode = kLoong64S128Load8x8S;
      break;
    case LoadTransformation::kS128Load8x8U:
      opcode = kLoong64S128Load8x8U;
      break;
    case LoadTransformation::kS128Load16x4S:
      opcode = kLoong64S128Load16x4S;
      break;
    case LoadTransformation::kS128Load16x4U:
      opcode = kLoong64S128Load16x4U;
      break;
    case LoadTransformation::kS128Load32x2S:
      opcode = kLoong64S128Load32x2S;
      break;
    case LoadTransformation::kS128Load32x2U:
      opcode = kLoong64S128Load32x2U;
      break;
    case LoadTransformation::kS128Load32Zero:
      opcode = kLoong64S128Load32Zero;
      break;
    case LoadTransformation::kS128Load64Zero:
      opcode = kLoong64S128Load64Zero;
      break;
    default:
      UNIMPLEMENTED();
  }

  EmitLoad(this, node, opcode);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitLoad(node_t node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    LoadRepresentation load_rep = LoadRepresentationOf(node->op());

    InstructionCode opcode = kArchNop;
    switch (load_rep.representation()) {
      case MachineRepresentation::kFloat32:
        opcode = kLoong64Fld_s;
        break;
      case MachineRepresentation::kFloat64:
        opcode = kLoong64Fld_d;
        break;
      case MachineRepresentation::kBit:  // Fall through.
      case MachineRepresentation::kWord8:
        opcode = load_rep.IsUnsigned() ? kLoong64Ld_bu : kLoong64Ld_b;
        break;
      case MachineRepresentation::kWord16:
        opcode = load_rep.IsUnsigned() ? kLoong64Ld_hu : kLoong64Ld_h;
        break;
      case MachineRepresentation::kWord32:
        opcode = kLoong64Ld_w;
        break;
#ifdef V8_COMPRESS_POINTERS
      case MachineRepresentation::kTaggedSigned:
        opcode = kLoong64LoadDecompressTaggedSigned;
        break;
      case MachineRepresentation::kTaggedPointer:
      case MachineRepresentation::kTagged:
        opcode = kLoong64LoadDecompressTagged;
        break;
#else
      case MachineRepresentation::kTaggedSigned:   // Fall through.
      case MachineRepresentation::kTaggedPointer:  // Fall through.
      case MachineRepresentation::kTagged:         // Fall through.
#endif
      case MachineRepresentation::kWord64:
        opcode = kLoong64Ld_d;
        break;
      case MachineRepresentation::kCompressedPointer:  // Fall through.
      case MachineRepresentation::kCompressed:
#ifdef V8_COMPRESS_POINTERS
        opcode = kLoong64Ld_wu;
        break;
#endif
      case MachineRepresentation::kSandboxedPointer:
        opcode = kLoong64LoadDecodeSandboxedPointer;
        break;
      case MachineRepresentation::kMapWord:  // Fall through.
      case MachineRepresentation::kNone:     // Fall through.
      case MachineRepresentation::kSimd128:  // Fall through.
      case MachineRepresentation::kSimd256:
        UNREACHABLE();
    }

    EmitLoad(this, node, opcode);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitProtectedLoad(Node* node) {
  // TODO(eholk)
  UNIMPLEMENTED();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitStorePair(Node* node) {
  UNREACHABLE();
}

template <>
void InstructionSelectorT<TurboshaftAdapter>::VisitStore(turboshaft::OpIndex) {
  UNREACHABLE();
}

template <>
void InstructionSelectorT<TurbofanAdapter>::VisitStore(Node* node) {
  Loong64OperandGeneratorT<TurbofanAdapter> g(this);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* value = node->InputAt(2);

  StoreRepresentation store_rep = StoreRepresentationOf(node->op());
  WriteBarrierKind write_barrier_kind = store_rep.write_barrier_kind();
  MachineRepresentation rep = store_rep.representation();

  if (v8_flags.enable_unconditional_write_barriers &&
      CanBeTaggedOrCompressedPointer(rep)) {
    write_barrier_kind = kFullWriteBarrier;
  }

  // TODO(loong64): I guess this could be done in a better way.
  if (write_barrier_kind != kNoWriteBarrier &&
      !v8_flags.disable_write_barriers) {
    DCHECK(CanBeTaggedOrCompressedPointer(rep));
    AddressingMode addressing_mode;
    InstructionOperand inputs[3];
    size_t input_count = 0;
    inputs[input_count++] = g.UseUniqueRegister(base);
    // OutOfLineRecordWrite uses the index in an arithmetic instruction, so we
    // must check kArithmeticImm as well as kLoadStoreImm64.
    if (g.CanBeImmediate(index, kLoong64Add_d)) {
      inputs[input_count++] = g.UseImmediate(index);
      addressing_mode = kMode_MRI;
    } else {
      inputs[input_count++] = g.UseUniqueRegister(index);
      addressing_mode = kMode_MRR;
    }
    inputs[input_count++] = g.UseUniqueRegister(value);
    RecordWriteMode record_write_mode =
        WriteBarrierKindToRecordWriteMode(write_barrier_kind);
    InstructionCode code = kArchStoreWithWriteBarrier;
    code |= AddressingModeField::encode(addressing_mode);
    code |= RecordWriteModeField::encode(record_write_mode);
    Emit(code, 0, nullptr, input_count, inputs);
  } else {
    ArchOpcode opcode;
    switch (rep) {
      case MachineRepresentation::kFloat32:
        opcode = kLoong64Fst_s;
        break;
      case MachineRepresentation::kFloat64:
        opcode = kLoong64Fst_d;
        break;
      case MachineRepresentation::kBit:  // Fall through.
      case MachineRepresentation::kWord8:
        opcode = kLoong64St_b;
        break;
      case MachineRepresentation::kWord16:
        opcode = kLoong64St_h;
        break;
      case MachineRepresentation::kWord32:
        opcode = kLoong64St_w;
        break;
      case MachineRepresentation::kWord64:
        opcode = kLoong64St_d;
        break;
      case MachineRepresentation::kTaggedSigned:   // Fall through.
      case MachineRepresentation::kTaggedPointer:  // Fall through.
      case MachineRepresentation::kTagged:
        opcode = kLoong64StoreCompressTagged;
        break;
      case MachineRepresentation::kCompressedPointer:  // Fall through.
      case MachineRepresentation::kCompressed:
#ifdef V8_COMPRESS_POINTERS
        opcode = kLoong64StoreCompressTagged;
        break;
#endif
      case MachineRepresentation::kSandboxedPointer:
        opcode = kLoong64StoreEncodeSandboxedPointer;
        break;
      case MachineRepresentation::kMapWord:  // Fall through.
      case MachineRepresentation::kNone:     // Fall through.
      case MachineRepresentation::kSimd128:  // Fall through.
      case MachineRepresentation::kSimd256:
        UNREACHABLE();
    }

    ExternalReferenceMatcher m(base);
    if (m.HasResolvedValue() && g.IsIntegerConstant(index) &&
        CanAddressRelativeToRootsRegister(m.ResolvedValue())) {
      ptrdiff_t const delta =
          g.GetIntegerConstantValue(index) +
          MacroAssemblerBase::RootRegisterOffsetForExternalReference(
              isolate(), m.ResolvedValue());
      // Check that the delta is a 32-bit integer due to the limitations of
      // immediate operands.
      if (is_int32(delta)) {
        Emit(opcode | AddressingModeField::encode(kMode_Root), g.NoOutput(),
             g.UseImmediate(static_cast<int32_t>(delta)),
             g.UseRegisterOrImmediateZero(value));
        return;
      }
    }

    if (base != nullptr && base->opcode() == IrOpcode::kLoadRootRegister) {
      // This will only work if {index} is a constant.
      Emit(opcode | AddressingModeField::encode(kMode_Root), g.NoOutput(),
           g.UseImmediate(index), g.UseRegisterOrImmediateZero(value));
      return;
    }

    if (g.CanBeImmediate(index, opcode)) {
      Emit(opcode | AddressingModeField::encode(kMode_MRI), g.NoOutput(),
           g.UseRegister(base), g.UseImmediate(index),
           g.UseRegisterOrImmediateZero(value));
    } else {
      Emit(opcode | AddressingModeField::encode(kMode_MRR), g.NoOutput(),
           g.UseRegister(base), g.UseRegister(index),
           g.UseRegisterOrImmediateZero(value));
    }
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitProtectedStore(node_t node) {
  // TODO(eholk)
  UNIMPLEMENTED();
}

template <>
void InstructionSelectorT<TurboshaftAdapter>::VisitWord32And(
    turboshaft::OpIndex) {
  UNIMPLEMENTED();
}

template <>
void InstructionSelectorT<TurbofanAdapter>::VisitWord32And(Node* node) {
  Loong64OperandGeneratorT<TurbofanAdapter> g(this);
  Int32BinopMatcher m(node);
  if (m.left().IsWord32Shr() && CanCover(node, m.left().node()) &&
      m.right().HasResolvedValue()) {
    uint32_t mask = m.right().ResolvedValue();
    uint32_t mask_width = base::bits::CountPopulation(mask);
    uint32_t mask_msb = base::bits::CountLeadingZeros32(mask);
    if ((mask_width != 0) && (mask_msb + mask_width == 32)) {
      // The mask must be contiguous, and occupy the least-significant bits.
      DCHECK_EQ(0u, base::bits::CountTrailingZeros32(mask));

      // Select Bstrpick_w for And(Shr(x, imm), mask) where the mask is in the
      // least significant bits.
      Int32BinopMatcher mleft(m.left().node());
      if (mleft.right().HasResolvedValue()) {
        // Any shift value can match; int32 shifts use `value % 32`.
        uint32_t lsb = mleft.right().ResolvedValue() & 0x1F;

        // Bstrpick_w cannot extract bits past the register size, however since
        // shifting the original value would have introduced some zeros we can
        // still use Bstrpick_w with a smaller mask and the remaining bits will
        // be zeros.
        if (lsb + mask_width > 32) mask_width = 32 - lsb;

        Emit(kLoong64Bstrpick_w, g.DefineAsRegister(node),
             g.UseRegister(mleft.left().node()), g.TempImmediate(lsb),
             g.TempImmediate(mask_width));
        return;
      }
      // Other cases fall through to the normal And operation.
    }
  }
  if (m.right().HasResolvedValue()) {
    uint32_t mask = m.right().ResolvedValue();
    uint32_t shift = base::bits::CountPopulation(~mask);
    uint32_t msb = base::bits::CountLeadingZeros32(~mask);
    if (shift != 0 && shift != 32 && msb + shift == 32) {
      // Insert zeros for (x >> K) << K => x & ~(2^K - 1) expression reduction
      // and remove constant loading of inverted mask.
      Emit(kLoong64Bstrins_w, g.DefineSameAsFirst(node),
           g.UseRegister(m.left().node()), g.TempImmediate(0),
           g.TempImmediate(shift));
      return;
    }
  }
  VisitBinop(this, node, kLoong64And32, true, kLoong64And32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64And(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Int64BinopMatcher m(node);
  if (m.left().IsWord64Shr() && CanCover(node, m.left().node()) &&
      m.right().HasResolvedValue()) {
    uint64_t mask = m.right().ResolvedValue();
    uint32_t mask_width = base::bits::CountPopulation(mask);
    uint32_t mask_msb = base::bits::CountLeadingZeros64(mask);
    if ((mask_width != 0) && (mask_msb + mask_width == 64)) {
      // The mask must be contiguous, and occupy the least-significant bits.
      DCHECK_EQ(0u, base::bits::CountTrailingZeros64(mask));

      // Select Bstrpick_d for And(Shr(x, imm), mask) where the mask is in the
      // least significant bits.
      Int64BinopMatcher mleft(m.left().node());
      if (mleft.right().HasResolvedValue()) {
        // Any shift value can match; int64 shifts use `value % 64`.
        uint32_t lsb =
            static_cast<uint32_t>(mleft.right().ResolvedValue() & 0x3F);

        // Bstrpick_d cannot extract bits past the register size, however since
        // shifting the original value would have introduced some zeros we can
        // still use Bstrpick_d with a smaller mask and the remaining bits will
        // be zeros.
        if (lsb + mask_width > 64) mask_width = 64 - lsb;

        if (lsb == 0 && mask_width == 64) {
          Emit(kArchNop, g.DefineSameAsFirst(node), g.Use(mleft.left().node()));
        } else {
          Emit(kLoong64Bstrpick_d, g.DefineAsRegister(node),
               g.UseRegister(mleft.left().node()), g.TempImmediate(lsb),
               g.TempImmediate(static_cast<int32_t>(mask_width)));
        }
        return;
      }
      // Other cases fall through to the normal And operation.
    }
  }
  if (m.right().HasResolvedValue()) {
    uint64_t mask = m.right().ResolvedValue();
    uint32_t shift = base::bits::CountPopulation(~mask);
    uint32_t msb = base::bits::CountLeadingZeros64(~mask);
    if (shift != 0 && shift < 32 && msb + shift == 64) {
      // Insert zeros for (x >> K) << K => x & ~(2^K - 1) expression reduction
      // and remove constant loading of inverted mask. Dins cannot insert bits
      // past word size, so shifts smaller than 32 are covered.
      Emit(kLoong64Bstrins_d, g.DefineSameAsFirst(node),
           g.UseRegister(m.left().node()), g.TempImmediate(0),
           g.TempImmediate(shift));
      return;
    }
  }
  VisitBinop(this, node, kLoong64And, true, kLoong64And);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Or(node_t node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    VisitBinop(this, node, kLoong64Or32, true, kLoong64Or32);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Or(Node* node) {
  VisitBinop(this, node, kLoong64Or, true, kLoong64Or);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Xor(Node* node) {
  Int32BinopMatcher m(node);
  if (m.left().IsWord32Or() && CanCover(node, m.left().node()) &&
      m.right().Is(-1)) {
    Int32BinopMatcher mleft(m.left().node());
    if (!mleft.right().HasResolvedValue()) {
      Loong64OperandGeneratorT<Adapter> g(this);
      Emit(kLoong64Nor32, g.DefineAsRegister(node),
           g.UseRegister(mleft.left().node()),
           g.UseRegister(mleft.right().node()));
      return;
    }
  }
  if (m.right().Is(-1)) {
    // Use Nor for bit negation and eliminate constant loading for xori.
    Loong64OperandGeneratorT<Adapter> g(this);
    Emit(kLoong64Nor32, g.DefineAsRegister(node),
         g.UseRegister(m.left().node()), g.TempImmediate(0));
    return;
  }
  VisitBinop(this, node, kLoong64Xor32, true, kLoong64Xor32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Xor(Node* node) {
  Int64BinopMatcher m(node);
  if (m.left().IsWord64Or() && CanCover(node, m.left().node()) &&
      m.right().Is(-1)) {
    Int64BinopMatcher mleft(m.left().node());
    if (!mleft.right().HasResolvedValue()) {
      Loong64OperandGeneratorT<Adapter> g(this);
      Emit(kLoong64Nor, g.DefineAsRegister(node),
           g.UseRegister(mleft.left().node()),
           g.UseRegister(mleft.right().node()));
      return;
    }
  }
  if (m.right().Is(-1)) {
    // Use Nor for bit negation and eliminate constant loading for xori.
    Loong64OperandGeneratorT<Adapter> g(this);
    Emit(kLoong64Nor, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
         g.TempImmediate(0));
    return;
  }
  VisitBinop(this, node, kLoong64Xor, true, kLoong64Xor);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Shl(Node* node) {
  Int32BinopMatcher m(node);
  if (m.left().IsWord32And() && CanCover(node, m.left().node()) &&
      m.right().IsInRange(1, 31)) {
    Loong64OperandGeneratorT<Adapter> g(this);
    Int32BinopMatcher mleft(m.left().node());
    // Match Word32Shl(Word32And(x, mask), imm) to Sll_w where the mask is
    // contiguous, and the shift immediate non-zero.
    if (mleft.right().HasResolvedValue()) {
      uint32_t mask = mleft.right().ResolvedValue();
      uint32_t mask_width = base::bits::CountPopulation(mask);
      uint32_t mask_msb = base::bits::CountLeadingZeros32(mask);
      if ((mask_width != 0) && (mask_msb + mask_width == 32)) {
        uint32_t shift = m.right().ResolvedValue();
        DCHECK_EQ(0u, base::bits::CountTrailingZeros32(mask));
        DCHECK_NE(0u, shift);
        if ((shift + mask_width) >= 32) {
          // If the mask is contiguous and reaches or extends beyond the top
          // bit, only the shift is needed.
          Emit(kLoong64Sll_w, g.DefineAsRegister(node),
               g.UseRegister(mleft.left().node()),
               g.UseImmediate(m.right().node()));
          return;
        }
      }
    }
  }
  VisitRRO(this, kLoong64Sll_w, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Shr(Node* node) {
  Int32BinopMatcher m(node);
  if (m.left().IsWord32And() && m.right().HasResolvedValue()) {
    uint32_t lsb = m.right().ResolvedValue() & 0x1F;
    Int32BinopMatcher mleft(m.left().node());
    if (mleft.right().HasResolvedValue() &&
        mleft.right().ResolvedValue() != 0) {
      // Select Bstrpick_w for Shr(And(x, mask), imm) where the result of the
      // mask is shifted into the least-significant bits.
      uint32_t mask = (mleft.right().ResolvedValue() >> lsb) << lsb;
      unsigned mask_width = base::bits::CountPopulation(mask);
      unsigned mask_msb = base::bits::CountLeadingZeros32(mask);
      if ((mask_msb + mask_width + lsb) == 32) {
        Loong64OperandGeneratorT<Adapter> g(this);
        DCHECK_EQ(lsb, base::bits::CountTrailingZeros32(mask));
        Emit(kLoong64Bstrpick_w, g.DefineAsRegister(node),
             g.UseRegister(mleft.left().node()), g.TempImmediate(lsb),
             g.TempImmediate(mask_width));
        return;
      }
    }
  }
  VisitRRO(this, kLoong64Srl_w, node);
}

template <>
void InstructionSelectorT<TurboshaftAdapter>::VisitWord32Sar(
    turboshaft::OpIndex) {
  UNIMPLEMENTED();
}

template <>
void InstructionSelectorT<TurbofanAdapter>::VisitWord32Sar(Node* node) {
  Int32BinopMatcher m(node);
  if (CanCover(node, m.left().node())) {
    Loong64OperandGeneratorT<TurbofanAdapter> g(this);
    if (m.left().IsWord32Shl()) {
      Int32BinopMatcher mleft(m.left().node());
      if (m.right().HasResolvedValue() && mleft.right().HasResolvedValue()) {
        uint32_t sar = m.right().ResolvedValue();
        uint32_t shl = mleft.right().ResolvedValue();
        if ((sar == shl) && (sar == 16)) {
          Emit(kLoong64Ext_w_h, g.DefineAsRegister(node),
               g.UseRegister(mleft.left().node()));
          return;
        } else if ((sar == shl) && (sar == 24)) {
          Emit(kLoong64Ext_w_b, g.DefineAsRegister(node),
               g.UseRegister(mleft.left().node()));
          return;
        } else if ((sar == shl) && (sar == 32)) {
          Emit(kLoong64Sll_w, g.DefineAsRegister(node),
               g.UseRegister(mleft.left().node()), g.TempImmediate(0));
          return;
        }
      }
    } else if (m.left().IsTruncateInt64ToInt32()) {
      Emit(kLoong64Sra_w, g.DefineAsRegister(node),
           g.UseRegister(m.left().InputAt(0)),
           g.UseOperand(node->InputAt(1), kLoong64Sra_w));
      return;
    }
  }
  VisitRRO(this, kLoong64Sra_w, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Shl(node_t node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    Loong64OperandGeneratorT<Adapter> g(this);
    Int64BinopMatcher m(node);
    if ((m.left().IsChangeInt32ToInt64() ||
         m.left().IsChangeUint32ToUint64()) &&
        m.right().IsInRange(32, 63) && CanCover(node, m.left().node())) {
      // There's no need to sign/zero-extend to 64-bit if we shift out the upper
      // 32 bits anyway.
      Emit(kLoong64Sll_d, g.DefineAsRegister(node),
           g.UseRegister(m.left().node()->InputAt(0)),
           g.UseImmediate(m.right().node()));
      return;
    }
    if (m.left().IsWord64And() && CanCover(node, m.left().node()) &&
        m.right().IsInRange(1, 63)) {
      // Match Word64Shl(Word64And(x, mask), imm) to Sll_d where the mask is
      // contiguous, and the shift immediate non-zero.
      Int64BinopMatcher mleft(m.left().node());
      if (mleft.right().HasResolvedValue()) {
        uint64_t mask = mleft.right().ResolvedValue();
        uint32_t mask_width = base::bits::CountPopulation(mask);
        uint32_t mask_msb = base::bits::CountLeadingZeros64(mask);
        if ((mask_width != 0) && (mask_msb + mask_width == 64)) {
          uint64_t shift = m.right().ResolvedValue();
          DCHECK_EQ(0u, base::bits::CountTrailingZeros64(mask));
          DCHECK_NE(0u, shift);

          if ((shift + mask_width) >= 64) {
            // If the mask is contiguous and reaches or extends beyond the top
            // bit, only the shift is needed.
            Emit(kLoong64Sll_d, g.DefineAsRegister(node),
                 g.UseRegister(mleft.left().node()),
                 g.UseImmediate(m.right().node()));
            return;
          }
        }
      }
    }
    VisitRRO(this, kLoong64Sll_d, node);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Shr(Node* node) {
  Int64BinopMatcher m(node);
  if (m.left().IsWord64And() && m.right().HasResolvedValue()) {
    uint32_t lsb = m.right().ResolvedValue() & 0x3F;
    Int64BinopMatcher mleft(m.left().node());
    if (mleft.right().HasResolvedValue() &&
        mleft.right().ResolvedValue() != 0) {
      // Select Bstrpick_d for Shr(And(x, mask), imm) where the result of the
      // mask is shifted into the least-significant bits.
      uint64_t mask = (mleft.right().ResolvedValue() >> lsb) << lsb;
      unsigned mask_width = base::bits::CountPopulation(mask);
      unsigned mask_msb = base::bits::CountLeadingZeros64(mask);
      if ((mask_msb + mask_width + lsb) == 64) {
        Loong64OperandGeneratorT<Adapter> g(this);
        DCHECK_EQ(lsb, base::bits::CountTrailingZeros64(mask));
        Emit(kLoong64Bstrpick_d, g.DefineAsRegister(node),
             g.UseRegister(mleft.left().node()), g.TempImmediate(lsb),
             g.TempImmediate(mask_width));
        return;
      }
    }
  }
  VisitRRO(this, kLoong64Srl_d, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Sar(node_t node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    if (TryEmitExtendingLoad(this, node, node)) return;

    Int64BinopMatcher m(node);
    if (m.left().IsChangeInt32ToInt64() && m.right().HasResolvedValue() &&
        is_uint5(m.right().ResolvedValue()) &&
        CanCover(node, m.left().node())) {
      if ((m.left().InputAt(0)->opcode() != IrOpcode::kLoad &&
           m.left().InputAt(0)->opcode() != IrOpcode::kLoadImmutable) ||
          !CanCover(m.left().node(), m.left().InputAt(0))) {
        Loong64OperandGeneratorT<Adapter> g(this);
        Emit(kLoong64Sra_w, g.DefineAsRegister(node),
             g.UseRegister(m.left().node()->InputAt(0)),
             g.UseImmediate(m.right().node()));
        return;
      }
    }

    VisitRRO(this, kLoong64Sra_d, node);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Rol(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Rol(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Ror(Node* node) {
  VisitRRO(this, kLoong64Rotr_w, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Ror(Node* node) {
  VisitRRO(this, kLoong64Rotr_d, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32ReverseBits(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64ReverseBits(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32ReverseBytes(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64ByteSwap32, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64ReverseBytes(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64ByteSwap64, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitSimd128ReverseBytes(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Clz(Node* node) {
  VisitRR(this, kLoong64Clz_w, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Clz(Node* node) {
  VisitRR(this, kLoong64Clz_d, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Ctz(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Ctz(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Popcnt(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Popcnt(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32Add(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Int32BinopMatcher m(node);

  // Select Alsl_w for (left + (left_of_right << imm)).
  if (m.right().opcode() == IrOpcode::kWord32Shl &&
      CanCover(node, m.left().node()) && CanCover(node, m.right().node())) {
    Int32BinopMatcher mright(m.right().node());
    if (mright.right().HasResolvedValue() && !m.left().HasResolvedValue()) {
      int32_t shift_value =
          static_cast<int32_t>(mright.right().ResolvedValue());
      if (shift_value > 0 && shift_value <= 31) {
        Emit(kLoong64Alsl_w, g.DefineAsRegister(node),
             g.UseRegister(mright.left().node()),
             g.UseRegister(m.left().node()), g.TempImmediate(shift_value));
        return;
      }
    }
  }

  // Select Alsl_w for ((left_of_left << imm) + right).
  if (m.left().opcode() == IrOpcode::kWord32Shl &&
      CanCover(node, m.right().node()) && CanCover(node, m.left().node())) {
    Int32BinopMatcher mleft(m.left().node());
    if (mleft.right().HasResolvedValue() && !m.right().HasResolvedValue()) {
      int32_t shift_value = static_cast<int32_t>(mleft.right().ResolvedValue());
      if (shift_value > 0 && shift_value <= 31) {
        Emit(kLoong64Alsl_w, g.DefineAsRegister(node),
             g.UseRegister(mleft.left().node()),
             g.UseRegister(m.right().node()), g.TempImmediate(shift_value));
        return;
      }
    }
  }

  VisitBinop(this, node, kLoong64Add_w, true, kLoong64Add_w);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64Add(node_t node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    Loong64OperandGeneratorT<Adapter> g(this);
    Int64BinopMatcher m(node);

    // Select Alsl_d for (left + (left_of_right << imm)).
    if (m.right().opcode() == IrOpcode::kWord64Shl &&
        CanCover(node, m.left().node()) && CanCover(node, m.right().node())) {
      Int64BinopMatcher mright(m.right().node());
      if (mright.right().HasResolvedValue() && !m.left().HasResolvedValue()) {
        int32_t shift_value =
            static_cast<int32_t>(mright.right().ResolvedValue());
        if (shift_value > 0 && shift_value <= 31) {
          Emit(kLoong64Alsl_d, g.DefineAsRegister(node),
               g.UseRegister(mright.left().node()),
               g.UseRegister(m.left().node()), g.TempImmediate(shift_value));
          return;
        }
      }
    }

    // Select Alsl_d for ((left_of_left << imm) + right).
    if (m.left().opcode() == IrOpcode::kWord64Shl &&
        CanCover(node, m.right().node()) && CanCover(node, m.left().node())) {
      Int64BinopMatcher mleft(m.left().node());
      if (mleft.right().HasResolvedValue() && !m.right().HasResolvedValue()) {
        int32_t shift_value =
            static_cast<int32_t>(mleft.right().ResolvedValue());
        if (shift_value > 0 && shift_value <= 31) {
          Emit(kLoong64Alsl_d, g.DefineAsRegister(node),
               g.UseRegister(mleft.left().node()),
               g.UseRegister(m.right().node()), g.TempImmediate(shift_value));
          return;
        }
      }
    }

    VisitBinop(this, node, kLoong64Add_d, true, kLoong64Add_d);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32Sub(Node* node) {
  VisitBinop(this, node, kLoong64Sub_w);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64Sub(Node* node) {
  VisitBinop(this, node, kLoong64Sub_d);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32Mul(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Int32BinopMatcher m(node);
  if (m.right().HasResolvedValue() && m.right().ResolvedValue() > 0) {
    uint32_t value = static_cast<uint32_t>(m.right().ResolvedValue());
    if (base::bits::IsPowerOfTwo(value)) {
      Emit(kLoong64Sll_w | AddressingModeField::encode(kMode_None),
           g.DefineAsRegister(node), g.UseRegister(m.left().node()),
           g.TempImmediate(base::bits::WhichPowerOfTwo(value)));
      return;
    }
    if (base::bits::IsPowerOfTwo(value - 1) && value - 1 > 0 &&
        value - 1 <= 31) {
      Emit(kLoong64Alsl_w, g.DefineAsRegister(node),
           g.UseRegister(m.left().node()), g.UseRegister(m.left().node()),
           g.TempImmediate(base::bits::WhichPowerOfTwo(value - 1)));
      return;
    }
    if (base::bits::IsPowerOfTwo(value + 1)) {
      InstructionOperand temp = g.TempRegister();
      Emit(kLoong64Sll_w | AddressingModeField::encode(kMode_None), temp,
           g.UseRegister(m.left().node()),
           g.TempImmediate(base::bits::WhichPowerOfTwo(value + 1)));
      Emit(kLoong64Sub_w | AddressingModeField::encode(kMode_None),
           g.DefineAsRegister(node), temp, g.UseRegister(m.left().node()));
      return;
    }
  }
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  if (CanCover(node, left) && CanCover(node, right)) {
    if (left->opcode() == IrOpcode::kWord64Sar &&
        right->opcode() == IrOpcode::kWord64Sar) {
      Int64BinopMatcher leftInput(left), rightInput(right);
      if (leftInput.right().Is(32) && rightInput.right().Is(32)) {
        // Combine untagging shifts with Mulh_d.
        Emit(kLoong64Mulh_d, g.DefineSameAsFirst(node),
             g.UseRegister(leftInput.left().node()),
             g.UseRegister(rightInput.left().node()));
        return;
      }
    }
  }
  VisitRRR(this, kLoong64Mul_w, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32MulHigh(Node* node) {
  VisitRRR(this, kLoong64Mulh_w, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64MulHigh(Node* node) {
  VisitRRR(this, kLoong64Mulh_d, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint32MulHigh(Node* node) {
  VisitRRR(this, kLoong64Mulh_wu, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint64MulHigh(Node* node) {
  VisitRRR(this, kLoong64Mulh_du, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64Mul(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Int64BinopMatcher m(node);
  if (m.right().HasResolvedValue() && m.right().ResolvedValue() > 0) {
    uint64_t value = static_cast<uint64_t>(m.right().ResolvedValue());
    if (base::bits::IsPowerOfTwo(value)) {
      Emit(kLoong64Sll_d | AddressingModeField::encode(kMode_None),
           g.DefineAsRegister(node), g.UseRegister(m.left().node()),
           g.TempImmediate(base::bits::WhichPowerOfTwo(value)));
      return;
    }
    if (base::bits::IsPowerOfTwo(value - 1) && value - 1 > 0) {
      // Alsl_d macro will handle the shifting value out of bound cases.
      Emit(kLoong64Alsl_d, g.DefineAsRegister(node),
           g.UseRegister(m.left().node()), g.UseRegister(m.left().node()),
           g.TempImmediate(base::bits::WhichPowerOfTwo(value - 1)));
      return;
    }
    if (base::bits::IsPowerOfTwo(value + 1)) {
      InstructionOperand temp = g.TempRegister();
      Emit(kLoong64Sll_d | AddressingModeField::encode(kMode_None), temp,
           g.UseRegister(m.left().node()),
           g.TempImmediate(base::bits::WhichPowerOfTwo(value + 1)));
      Emit(kLoong64Sub_d | AddressingModeField::encode(kMode_None),
           g.DefineAsRegister(node), temp, g.UseRegister(m.left().node()));
      return;
    }
  }
  Emit(kLoong64Mul_d, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32Div(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Int32BinopMatcher m(node);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  if (CanCover(node, left) && CanCover(node, right)) {
    if (left->opcode() == IrOpcode::kWord64Sar &&
        right->opcode() == IrOpcode::kWord64Sar) {
      Int64BinopMatcher rightInput(right), leftInput(left);
      if (rightInput.right().Is(32) && leftInput.right().Is(32)) {
        // Combine both shifted operands with Div_d.
        Emit(kLoong64Div_d, g.DefineSameAsFirst(node),
             g.UseRegister(leftInput.left().node()),
             g.UseRegister(rightInput.left().node()));
        return;
      }
    }
  }
  Emit(kLoong64Div_w, g.DefineSameAsFirst(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint32Div(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Int32BinopMatcher m(node);
  Emit(kLoong64Div_wu, g.DefineSameAsFirst(node),
       g.UseRegister(m.left().node()), g.UseRegister(m.right().node()));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32Mod(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Int32BinopMatcher m(node);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  if (CanCover(node, left) && CanCover(node, right)) {
    if (left->opcode() == IrOpcode::kWord64Sar &&
        right->opcode() == IrOpcode::kWord64Sar) {
      Int64BinopMatcher rightInput(right), leftInput(left);
      if (rightInput.right().Is(32) && leftInput.right().Is(32)) {
        // Combine both shifted operands with Mod_d.
        Emit(kLoong64Mod_d, g.DefineSameAsFirst(node),
             g.UseRegister(leftInput.left().node()),
             g.UseRegister(rightInput.left().node()));
        return;
      }
    }
  }
  Emit(kLoong64Mod_w, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint32Mod(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Int32BinopMatcher m(node);
  Emit(kLoong64Mod_wu, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64Div(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Int64BinopMatcher m(node);
  Emit(kLoong64Div_d, g.DefineSameAsFirst(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint64Div(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Int64BinopMatcher m(node);
  Emit(kLoong64Div_du, g.DefineSameAsFirst(node),
       g.UseRegister(m.left().node()), g.UseRegister(m.right().node()));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64Mod(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Int64BinopMatcher m(node);
  Emit(kLoong64Mod_d, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint64Mod(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Int64BinopMatcher m(node);
  Emit(kLoong64Mod_du, g.DefineAsRegister(node), g.UseRegister(m.left().node()),
       g.UseRegister(m.right().node()));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitChangeFloat32ToFloat64(Node* node) {
  VisitRR(this, kLoong64Float32ToFloat64, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitRoundInt32ToFloat32(Node* node) {
  VisitRR(this, kLoong64Int32ToFloat32, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitRoundUint32ToFloat32(Node* node) {
  VisitRR(this, kLoong64Uint32ToFloat32, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitChangeInt32ToFloat64(node_t node) {
  VisitRR(this, kLoong64Int32ToFloat64, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitChangeInt64ToFloat64(Node* node) {
  VisitRR(this, kLoong64Int64ToFloat64, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitChangeUint32ToFloat64(Node* node) {
  VisitRR(this, kLoong64Uint32ToFloat64, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTruncateFloat32ToInt32(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  InstructionCode opcode = kLoong64Float32ToInt32;
  TruncateKind kind = OpParameter<TruncateKind>(node->op());
  if (kind == TruncateKind::kSetOverflowToMin) {
    opcode |= MiscField::encode(true);
  }
  Emit(opcode, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTruncateFloat32ToUint32(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  InstructionCode opcode = kLoong64Float32ToUint32;
  TruncateKind kind = OpParameter<TruncateKind>(node->op());
  if (kind == TruncateKind::kSetOverflowToMin) {
    opcode |= MiscField::encode(true);
  }
  Emit(opcode, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitChangeFloat64ToInt32(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Node* value = node->InputAt(0);
  // TODO(LOONG_dev): LOONG64 Match ChangeFloat64ToInt32(Float64Round##OP) to
  // corresponding instruction which does rounding and conversion to
  // integer format.
  if (CanCover(node, value)) {
    if (value->opcode() == IrOpcode::kChangeFloat32ToFloat64) {
      Node* next = value->InputAt(0);
      if (!CanCover(value, next)) {
        // Match float32 -> float64 -> int32 representation change path.
        Emit(kLoong64Float32ToInt32, g.DefineAsRegister(node),
             g.UseRegister(value->InputAt(0)));
        return;
      }
    }
  }
  VisitRR(this, kLoong64Float64ToInt32, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitChangeFloat64ToInt64(Node* node) {
  VisitRR(this, kLoong64Float64ToInt64, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitChangeFloat64ToUint32(Node* node) {
  VisitRR(this, kLoong64Float64ToUint32, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitChangeFloat64ToUint64(Node* node) {
  VisitRR(this, kLoong64Float64ToUint64, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTruncateFloat64ToUint32(Node* node) {
  VisitRR(this, kLoong64Float64ToUint32, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTruncateFloat64ToInt64(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  InstructionCode opcode = kLoong64Float64ToInt64;
  TruncateKind kind = OpParameter<TruncateKind>(node->op());
  if (kind == TruncateKind::kSetOverflowToMin) {
    opcode |= MiscField::encode(true);
  }
  Emit(opcode, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTryTruncateFloat32ToInt64(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
  }

  this->Emit(kLoong64Float32ToInt64, output_count, outputs, 1, inputs);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTryTruncateFloat64ToInt64(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
  }

  Emit(kLoong64Float64ToInt64, output_count, outputs, 1, inputs);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTryTruncateFloat32ToUint64(
    Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
  }

  Emit(kLoong64Float32ToUint64, output_count, outputs, 1, inputs);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTryTruncateFloat64ToUint64(
    Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);

  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
  }

  Emit(kLoong64Float64ToUint64, output_count, outputs, 1, inputs);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTryTruncateFloat64ToInt32(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand temps[] = {g.TempDoubleRegister()};
  InstructionOperand outputs[2];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
  }

  Emit(kLoong64Float64ToInt32, output_count, outputs, 1, inputs, 1, temps);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTryTruncateFloat64ToUint32(
    Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand temps[] = {g.TempDoubleRegister()};
  InstructionOperand outputs[2];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
  }

  Emit(kLoong64Float64ToUint32, output_count, outputs, 1, inputs, 1, temps);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitBitcastWord32ToWord64(Node* node) {
  DCHECK(SmiValuesAre31Bits());
  DCHECK(COMPRESS_POINTERS_BOOL);
  EmitIdentity(node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitChangeInt32ToInt64(node_t node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    Loong64OperandGeneratorT<Adapter> g(this);
    Node* value = node->InputAt(0);
    if ((value->opcode() == IrOpcode::kLoad ||
         value->opcode() == IrOpcode::kLoadImmutable) &&
        CanCover(node, value)) {
      // Generate sign-extending load.
      LoadRepresentation load_rep = LoadRepresentationOf(value->op());
      InstructionCode opcode = kArchNop;
      switch (load_rep.representation()) {
        case MachineRepresentation::kBit:  // Fall through.
        case MachineRepresentation::kWord8:
          opcode = load_rep.IsUnsigned() ? kLoong64Ld_bu : kLoong64Ld_b;
          break;
        case MachineRepresentation::kWord16:
          opcode = load_rep.IsUnsigned() ? kLoong64Ld_hu : kLoong64Ld_h;
          break;
        case MachineRepresentation::kWord32:
          opcode = kLoong64Ld_w;
          break;
        default:
          UNREACHABLE();
      }
      EmitLoad(this, value, opcode, node);
      return;
    } else if (value->opcode() == IrOpcode::kTruncateInt64ToInt32) {
      EmitIdentity(node);
      return;
    }
    Emit(kLoong64Sll_w, g.DefineAsRegister(node),
         g.UseRegister(node->InputAt(0)), g.TempImmediate(0));
  }
}

template <typename Adapter>
bool InstructionSelectorT<Adapter>::ZeroExtendsWord32ToWord64NoPhis(
    Node* node) {
  DCHECK_NE(node->opcode(), IrOpcode::kPhi);
  switch (node->opcode()) {
    // Comparisons only emit 0/1, so the upper 32 bits must be zero.
    case IrOpcode::kWord32Equal:
    case IrOpcode::kInt32LessThan:
    case IrOpcode::kInt32LessThanOrEqual:
    case IrOpcode::kUint32LessThan:
    case IrOpcode::kUint32LessThanOrEqual:
      return true;
    case IrOpcode::kWord32And: {
      Int32BinopMatcher m(node);
      if (m.right().HasResolvedValue()) {
        uint32_t mask = m.right().ResolvedValue();
        return is_uint31(mask);
      }
      return false;
    }
    case IrOpcode::kWord32Shr: {
      Int32BinopMatcher m(node);
      if (m.right().HasResolvedValue()) {
        uint8_t sa = m.right().ResolvedValue() & 0x1f;
        return sa > 0;
      }
      return false;
    }
    case IrOpcode::kLoad:
    case IrOpcode::kLoadImmutable: {
      LoadRepresentation load_rep = LoadRepresentationOf(node->op());
      if (load_rep.IsUnsigned()) {
        switch (load_rep.representation()) {
          case MachineRepresentation::kBit:    // Fall through.
          case MachineRepresentation::kWord8:  // Fall through.
          case MachineRepresentation::kWord16:
            return true;
          default:
            return false;
        }
      }
      return false;
    }
    default:
      return false;
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitChangeUint32ToUint64(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Node* value = node->InputAt(0);

  if (value->opcode() == IrOpcode::kLoad) {
    LoadRepresentation load_rep = LoadRepresentationOf(value->op());
    if (load_rep.IsUnsigned() &&
        load_rep.representation() == MachineRepresentation::kWord32) {
      EmitLoad(this, value, kLoong64Ld_wu, node);
      return;
    }
  }
  if (ZeroExtendsWord32ToWord64(value)) {
    EmitIdentity(node);
    return;
  }
  Emit(kLoong64Bstrpick_d, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)), g.TempImmediate(0),
       g.TempImmediate(32));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTruncateInt64ToInt32(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Node* value = node->InputAt(0);
  if (CanCover(node, value)) {
    switch (value->opcode()) {
      case IrOpcode::kWord64Sar: {
        if (CanCover(value, value->InputAt(0)) &&
            TryEmitExtendingLoad(this, value, node)) {
          return;
        } else {
          Int64BinopMatcher m(value);
          if (m.right().IsInRange(32, 63)) {
            // After smi untagging no need for truncate. Combine sequence.
            Emit(kLoong64Sra_d, g.DefineAsRegister(node),
                 g.UseRegister(m.left().node()),
                 g.UseImmediate(m.right().node()));
            return;
          }
        }
        break;
      }
      default:
        break;
    }
  }
  Emit(kLoong64Sll_w, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),
       g.TempImmediate(0));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTruncateFloat64ToFloat32(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Node* value = node->InputAt(0);
  // Match TruncateFloat64ToFloat32(ChangeInt32ToFloat64) to corresponding
  // instruction.
  if (CanCover(node, value) &&
      value->opcode() == IrOpcode::kChangeInt32ToFloat64) {
    Emit(kLoong64Int32ToFloat32, g.DefineAsRegister(node),
         g.UseRegister(value->InputAt(0)));
    return;
  }
  VisitRR(this, kLoong64Float64ToFloat32, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTruncateFloat64ToWord32(Node* node) {
  VisitRR(this, kArchTruncateDoubleToI, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitRoundFloat64ToInt32(node_t node) {
  VisitRR(this, kLoong64Float64ToInt32, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitRoundInt64ToFloat32(Node* node) {
  VisitRR(this, kLoong64Int64ToFloat32, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitRoundInt64ToFloat64(Node* node) {
  VisitRR(this, kLoong64Int64ToFloat64, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitRoundUint64ToFloat32(Node* node) {
  VisitRR(this, kLoong64Uint64ToFloat32, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitRoundUint64ToFloat64(Node* node) {
  VisitRR(this, kLoong64Uint64ToFloat64, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitBitcastFloat32ToInt32(Node* node) {
  VisitRR(this, kLoong64Float64ExtractLowWord32, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitBitcastFloat64ToInt64(Node* node) {
  VisitRR(this, kLoong64BitcastDL, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitBitcastInt32ToFloat32(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64Float64InsertLowWord32, g.DefineAsRegister(node),
       ImmediateOperand(ImmediateOperand::INLINE_INT32, 0),
       g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitBitcastInt64ToFloat64(Node* node) {
  VisitRR(this, kLoong64BitcastLD, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Add(Node* node) {
  VisitRRR(this, kLoong64Float32Add, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Add(Node* node) {
  VisitRRR(this, kLoong64Float64Add, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Sub(Node* node) {
  VisitRRR(this, kLoong64Float32Sub, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Sub(node_t node) {
  VisitRRR(this, kLoong64Float64Sub, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Mul(Node* node) {
  VisitRRR(this, kLoong64Float32Mul, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Mul(Node* node) {
  VisitRRR(this, kLoong64Float64Mul, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Div(Node* node) {
  VisitRRR(this, kLoong64Float32Div, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Div(node_t node) {
  VisitRRR(this, kLoong64Float64Div, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Mod(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64Float64Mod, g.DefineAsFixed(node, f0),
       g.UseFixed(node->InputAt(0), f0), g.UseFixed(node->InputAt(1), f1))
      ->MarkAsCall();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Max(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64Float32Max, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Max(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64Float64Max, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Min(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64Float32Min, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Min(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64Float64Min, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Abs(Node* node) {
  VisitRR(this, kLoong64Float32Abs, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Abs(Node* node) {
  VisitRR(this, kLoong64Float64Abs, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Sqrt(Node* node) {
  VisitRR(this, kLoong64Float32Sqrt, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Sqrt(Node* node) {
  VisitRR(this, kLoong64Float64Sqrt, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32RoundDown(Node* node) {
  VisitRR(this, kLoong64Float32RoundDown, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64RoundDown(Node* node) {
  VisitRR(this, kLoong64Float64RoundDown, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32RoundUp(Node* node) {
  VisitRR(this, kLoong64Float32RoundUp, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64RoundUp(Node* node) {
  VisitRR(this, kLoong64Float64RoundUp, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32RoundTruncate(Node* node) {
  VisitRR(this, kLoong64Float32RoundTruncate, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64RoundTruncate(Node* node) {
  VisitRR(this, kLoong64Float64RoundTruncate, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64RoundTiesAway(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32RoundTiesEven(Node* node) {
  VisitRR(this, kLoong64Float32RoundTiesEven, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64RoundTiesEven(Node* node) {
  VisitRR(this, kLoong64Float64RoundTiesEven, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Neg(Node* node) {
  VisitRR(this, kLoong64Float32Neg, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Neg(Node* node) {
  VisitRR(this, kLoong64Float64Neg, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Ieee754Binop(
    Node* node, InstructionCode opcode) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(opcode, g.DefineAsFixed(node, f0), g.UseFixed(node->InputAt(0), f0),
       g.UseFixed(node->InputAt(1), f1))
      ->MarkAsCall();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Ieee754Unop(
    Node* node, InstructionCode opcode) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(opcode, g.DefineAsFixed(node, f0), g.UseFixed(node->InputAt(0), f0))
      ->MarkAsCall();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::EmitMoveParamToFPR(node_t node,
                                                       int32_t index) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    OperandGenerator g(this);
    int count = linkage()->GetParameterLocation(index).GetLocation();
    InstructionOperand out_op = g.TempRegister(-count);
    Emit(kArchNop, out_op);
    Emit(kLoong64BitcastLD, g.DefineAsRegister(node), out_op);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::EmitMoveFPRToParam(
    InstructionOperand* op, LinkageLocation location) {
  OperandGenerator g(this);
  int count = location.GetLocation();
  InstructionOperand new_op = g.TempRegister(-count);
  Emit(kLoong64BitcastDL, new_op, *op);
  *op = new_op;
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::EmitPrepareArguments(
    ZoneVector<PushParameter>* arguments, const CallDescriptor* call_descriptor,
    node_t node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    Loong64OperandGeneratorT<Adapter> g(this);

    // Prepare for C function call.
    if (call_descriptor->IsCFunctionCall()) {
      int gp_param_count =
          static_cast<int>(call_descriptor->GPParameterCount());
      int fp_param_count =
          static_cast<int>(call_descriptor->FPParameterCount());
      Emit(kArchPrepareCallCFunction | ParamField::encode(gp_param_count) |
               FPParamField::encode(fp_param_count),
           0, nullptr, 0, nullptr);

      // Poke any stack arguments.
      int slot = 0;
      for (PushParameter input : (*arguments)) {
        Emit(kLoong64Poke, g.NoOutput(), g.UseRegister(input.node),
             g.TempImmediate(slot << kSystemPointerSizeLog2));
        ++slot;
      }
    } else {
      int push_count = static_cast<int>(call_descriptor->ParameterSlotCount());
      if (push_count > 0) {
        // Calculate needed space
        int stack_size = 0;
        for (PushParameter input : (*arguments)) {
          if (input.node) {
            stack_size += input.location.GetSizeInPointers();
          }
        }
        Emit(kLoong64StackClaim, g.NoOutput(),
             g.TempImmediate(stack_size << kSystemPointerSizeLog2));
      }
      for (size_t n = 0; n < arguments->size(); ++n) {
        PushParameter input = (*arguments)[n];
        if (input.node) {
          Emit(kLoong64Poke, g.NoOutput(), g.UseRegister(input.node),
               g.TempImmediate(static_cast<int>(n << kSystemPointerSizeLog2)));
        }
      }
    }
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::EmitPrepareResults(
    ZoneVector<PushParameter>* results, const CallDescriptor* call_descriptor,
    node_t node) {
  Loong64OperandGeneratorT<Adapter> g(this);

  for (PushParameter output : *results) {
    if (!output.location.IsCallerFrameSlot()) continue;
    // Skip any alignment holes in nodes.
    if (this->valid(output.node)) {
      DCHECK(!call_descriptor->IsCFunctionCall());
      if (output.location.GetType() == MachineType::Float32()) {
        MarkAsFloat32(output.node);
      } else if (output.location.GetType() == MachineType::Float64()) {
        MarkAsFloat64(output.node);
      } else if (output.location.GetType() == MachineType::Simd128()) {
        abort();
      }
      int offset = call_descriptor->GetOffsetToReturns();
      int reverse_slot = -output.location.GetLocation() - offset;
      Emit(kLoong64Peek, g.DefineAsRegister(output.node),
           g.UseImmediate(reverse_slot));
    }
  }
}

template <typename Adapter>
bool InstructionSelectorT<Adapter>::IsTailCallAddressImmediate() {
  return false;
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUnalignedLoad(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUnalignedStore(Node* node) {
  UNREACHABLE();
}

namespace {

// Shared routine for multiple compare operations.
template <typename Adapter>
static void VisitCompare(InstructionSelectorT<Adapter>* selector,
                         InstructionCode opcode, InstructionOperand left,
                         InstructionOperand right,
                         FlagsContinuationT<Adapter>* cont) {
#ifdef V8_COMPRESS_POINTERS
  if (opcode == kLoong64Cmp32) {
    Loong64OperandGeneratorT<Adapter> g(selector);
    InstructionOperand inputs[] = {left, right};
    if (right.IsImmediate()) {
      InstructionOperand temps[1] = {g.TempRegister()};
      selector->EmitWithContinuation(opcode, 0, nullptr, arraysize(inputs),
                                     inputs, arraysize(temps), temps, cont);
    } else {
      InstructionOperand temps[2] = {g.TempRegister(), g.TempRegister()};
      selector->EmitWithContinuation(opcode, 0, nullptr, arraysize(inputs),
                                     inputs, arraysize(temps), temps, cont);
    }
    return;
  }
#endif
  selector->EmitWithContinuation(opcode, left, right, cont);
}

// Shared routine for multiple float32 compare operations.
template <typename Adapter>
void VisitFloat32Compare(InstructionSelectorT<Adapter>* selector,
                         typename Adapter::node_t node,
                         FlagsContinuationT<Adapter>* cont) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    Loong64OperandGeneratorT<Adapter> g(selector);
    Float32BinopMatcher m(node);
    InstructionOperand lhs, rhs;

    lhs = m.left().IsZero() ? g.UseImmediate(m.left().node())
                            : g.UseRegister(m.left().node());
    rhs = m.right().IsZero() ? g.UseImmediate(m.right().node())
                             : g.UseRegister(m.right().node());
    VisitCompare(selector, kLoong64Float32Cmp, lhs, rhs, cont);
  }
}

// Shared routine for multiple float64 compare operations.
template <typename Adapter>
void VisitFloat64Compare(InstructionSelectorT<Adapter>* selector,
                         typename Adapter::node_t node,
                         FlagsContinuationT<Adapter>* cont) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    Loong64OperandGeneratorT<Adapter> g(selector);
    Float64BinopMatcher m(node);
    InstructionOperand lhs, rhs;

    lhs = m.left().IsZero() ? g.UseImmediate(m.left().node())
                            : g.UseRegister(m.left().node());
    rhs = m.right().IsZero() ? g.UseImmediate(m.right().node())
                             : g.UseRegister(m.right().node());
    VisitCompare(selector, kLoong64Float64Cmp, lhs, rhs, cont);
  }
}

// Shared routine for multiple word compare operations.
template <typename Adapter>
void VisitWordCompare(InstructionSelectorT<Adapter>* selector, Node* node,
                      InstructionCode opcode, FlagsContinuationT<Adapter>* cont,
                      bool commutative) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);

  // Match immediates on left or right side of comparison.
  if (g.CanBeImmediate(right, opcode)) {
    if (opcode == kLoong64Tst) {
      if (left->opcode() == IrOpcode::kTruncateInt64ToInt32) {
        VisitCompare(selector, opcode, g.UseRegister(left->InputAt(0)),
                     g.UseImmediate(right), cont);
      } else {
        VisitCompare(selector, opcode, g.UseRegister(left),
                     g.UseImmediate(right), cont);
      }
    } else {
      switch (cont->condition()) {
        case kEqual:
        case kNotEqual:
          if (cont->IsSet()) {
            VisitCompare(selector, opcode, g.UseUniqueRegister(left),
                         g.UseImmediate(right), cont);
          } else {
            VisitCompare(selector, opcode, g.UseUniqueRegister(left),
                         g.UseImmediate(right), cont);
          }
          break;
        case kSignedLessThan:
        case kSignedGreaterThanOrEqual:
        case kSignedLessThanOrEqual:
        case kSignedGreaterThan:
        case kUnsignedLessThan:
        case kUnsignedGreaterThanOrEqual:
        case kUnsignedLessThanOrEqual:
        case kUnsignedGreaterThan:
          VisitCompare(selector, opcode, g.UseUniqueRegister(left),
                       g.UseImmediate(right), cont);
          break;
        default:
          UNREACHABLE();
      }
    }
  } else if (g.CanBeImmediate(left, opcode)) {
    if (!commutative) cont->Commute();
    if (opcode == kLoong64Tst) {
      VisitCompare(selector, opcode, g.UseRegister(right), g.UseImmediate(left),
                   cont);
    } else {
      switch (cont->condition()) {
        case kEqual:
        case kNotEqual:
          if (cont->IsSet()) {
            VisitCompare(selector, opcode, g.UseUniqueRegister(right),
                         g.UseImmediate(left), cont);
          } else {
            VisitCompare(selector, opcode, g.UseUniqueRegister(right),
                         g.UseImmediate(left), cont);
          }
          break;
        case kSignedLessThan:
        case kSignedGreaterThanOrEqual:
        case kSignedLessThanOrEqual:
        case kSignedGreaterThan:
        case kUnsignedLessThan:
        case kUnsignedGreaterThanOrEqual:
        case kUnsignedLessThanOrEqual:
        case kUnsignedGreaterThan:
          VisitCompare(selector, opcode, g.UseUniqueRegister(right),
                       g.UseImmediate(left), cont);
          break;
        default:
          UNREACHABLE();
      }
    }
  } else {
    VisitCompare(selector, opcode, g.UseUniqueRegister(left),
                 g.UseUniqueRegister(right), cont);
  }
}

template <typename Adapter>
void VisitOptimizedWord32Compare(InstructionSelectorT<Adapter>* selector,
                                 Node* node, InstructionCode opcode,
                                 FlagsContinuationT<Adapter>* cont) {
  // TODO(LOONG_dev): LOONG64 Add check for debug mode
  VisitWordCompare(selector, node, opcode, cont, false);
}

#ifdef USE_SIMULATOR
// Shared routine for multiple word compare operations.
template <typename Adapter>
void VisitFullWord32Compare(InstructionSelectorT<Adapter>* selector, Node* node,
                            InstructionCode opcode,
                            FlagsContinuationT<Adapter>* cont) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  InstructionOperand leftOp = g.TempRegister();
  InstructionOperand rightOp = g.TempRegister();

  selector->Emit(kLoong64Sll_d, leftOp, g.UseRegister(node->InputAt(0)),
                 g.TempImmediate(32));
  selector->Emit(kLoong64Sll_d, rightOp, g.UseRegister(node->InputAt(1)),
                 g.TempImmediate(32));

  VisitCompare(selector, opcode, leftOp, rightOp, cont);
}
#endif

template <typename Adapter>
void VisitWord32Compare(InstructionSelectorT<Adapter>* selector,
                        typename Adapter::node_t node,
                        FlagsContinuationT<Adapter>* cont) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    // LOONG64 doesn't support Word32 compare instructions. Instead it relies
    // that the values in registers are correctly sign-extended and uses
    // Word64 comparison instead.
#ifdef USE_SIMULATOR
    // When call to a host function in simulator, if the function return a
    // int32 value, the simulator do not sign-extended to int64 because in
    // simulator we do not know the function whether return a int32 or int64.
    // so we need do a full word32 compare in this case.
    if (node->InputAt(0)->opcode() == IrOpcode::kCall ||
        node->InputAt(1)->opcode() == IrOpcode::kCall) {
      VisitFullWord32Compare(selector, node, kLoong64Cmp64, cont);
      return;
    }
#endif
    VisitOptimizedWord32Compare(selector, node, kLoong64Cmp32, cont);
  }
}

template <typename Adapter>
void VisitWord64Compare(InstructionSelectorT<Adapter>* selector, Node* node,
                        FlagsContinuationT<Adapter>* cont) {
  VisitWordCompare(selector, node, kLoong64Cmp64, cont, false);
}

template <typename Adapter>
void VisitAtomicLoad(InstructionSelectorT<Adapter>* selector, Node* node,
                     AtomicWidth width) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);

  // The memory order is ignored.
  AtomicLoadParameters atomic_load_params = AtomicLoadParametersOf(node->op());
  LoadRepresentation load_rep = atomic_load_params.representation();
  InstructionCode code;
  switch (load_rep.representation()) {
    case MachineRepresentation::kWord8:
      DCHECK_IMPLIES(load_rep.IsSigned(), width == AtomicWidth::kWord32);
      code = load_rep.IsSigned() ? kAtomicLoadInt8 : kAtomicLoadUint8;
      break;
    case MachineRepresentation::kWord16:
      DCHECK_IMPLIES(load_rep.IsSigned(), width == AtomicWidth::kWord32);
      code = load_rep.IsSigned() ? kAtomicLoadInt16 : kAtomicLoadUint16;
      break;
    case MachineRepresentation::kWord32:
      code = (width == AtomicWidth::kWord32) ? kAtomicLoadWord32
                                             : kLoong64Word64AtomicLoadUint32;
      break;
    case MachineRepresentation::kWord64:
      code = kLoong64Word64AtomicLoadUint64;
      break;
#ifdef V8_COMPRESS_POINTERS
    case MachineRepresentation::kTaggedSigned:
      code = kLoong64AtomicLoadDecompressTaggedSigned;
      break;
    case MachineRepresentation::kTaggedPointer:
    case MachineRepresentation::kTagged:
      code = kLoong64AtomicLoadDecompressTagged;
      break;
#else
    case MachineRepresentation::kTaggedSigned:   // Fall through.
    case MachineRepresentation::kTaggedPointer:  // Fall through.
    case MachineRepresentation::kTagged:
      code = kLoong64Word64AtomicLoadUint64;
      break;
#endif
    case MachineRepresentation::kCompressedPointer:  // Fall through.
    case MachineRepresentation::kCompressed:
      DCHECK(COMPRESS_POINTERS_BOOL);
      code = kLoong64Word64AtomicLoadUint32;
      break;
    default:
      UNREACHABLE();
  }

  if (g.CanBeImmediate(index, code)) {
    selector->Emit(code | AddressingModeField::encode(kMode_MRI) |
                       AtomicWidthField::encode(width),
                   g.DefineAsRegister(node), g.UseRegister(base),
                   g.UseImmediate(index));
  } else {
    InstructionOperand addr_reg = g.TempRegister();
    selector->Emit(kLoong64Add_d | AddressingModeField::encode(kMode_None),
                   addr_reg, g.UseRegister(index), g.UseRegister(base));
    // Emit desired load opcode, using temp addr_reg.
    selector->Emit(code | AddressingModeField::encode(kMode_MRI) |
                       AtomicWidthField::encode(width),
                   g.DefineAsRegister(node), addr_reg, g.TempImmediate(0));
  }
}

template <typename T>
void VisitAtomicStore(InstructionSelectorT<TurboshaftAdapter>*, T,
                      AtomicWidth) {
  UNIMPLEMENTED();
}

void VisitAtomicStore(InstructionSelectorT<TurbofanAdapter>* selector,
                      Node* node, AtomicWidth width) {
  Loong64OperandGeneratorT<TurbofanAdapter> g(selector);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* value = node->InputAt(2);

  // The memory order is ignored.
  AtomicStoreParameters store_params = AtomicStoreParametersOf(node->op());
  WriteBarrierKind write_barrier_kind = store_params.write_barrier_kind();
  MachineRepresentation rep = store_params.representation();

  if (v8_flags.enable_unconditional_write_barriers &&
      CanBeTaggedOrCompressedPointer(rep)) {
    write_barrier_kind = kFullWriteBarrier;
  }

  InstructionCode code;

  if (write_barrier_kind != kNoWriteBarrier &&
      !v8_flags.disable_write_barriers) {
    DCHECK(CanBeTaggedPointer(rep));
    DCHECK_EQ(kTaggedSize, 8);

    RecordWriteMode record_write_mode =
        WriteBarrierKindToRecordWriteMode(write_barrier_kind);
    code = kArchAtomicStoreWithWriteBarrier;
    code |= RecordWriteModeField::encode(record_write_mode);
  } else {
    switch (rep) {
      case MachineRepresentation::kWord8:
        code = kAtomicStoreWord8;
        break;
      case MachineRepresentation::kWord16:
        code = kAtomicStoreWord16;
        break;
      case MachineRepresentation::kWord32:
        code = kAtomicStoreWord32;
        break;
      case MachineRepresentation::kWord64:
        DCHECK_EQ(width, AtomicWidth::kWord64);
        code = kLoong64Word64AtomicStoreWord64;
        break;
      case MachineRepresentation::kTaggedSigned:   // Fall through.
      case MachineRepresentation::kTaggedPointer:  // Fall through.
      case MachineRepresentation::kTagged:
        DCHECK_EQ(AtomicWidthSize(width), kTaggedSize);
        code = kLoong64AtomicStoreCompressTagged;
        break;
      case MachineRepresentation::kCompressedPointer:  // Fall through.
      case MachineRepresentation::kCompressed:
        DCHECK(COMPRESS_POINTERS_BOOL);
        DCHECK_EQ(width, AtomicWidth::kWord32);
        code = kLoong64AtomicStoreCompressTagged;
        break;
      default:
        UNREACHABLE();
    }
  }

  if (g.CanBeImmediate(index, code)) {
    selector->Emit(code | AddressingModeField::encode(kMode_MRI) |
                       AtomicWidthField::encode(width),
                   g.NoOutput(), g.UseRegister(base), g.UseImmediate(index),
                   g.UseRegisterOrImmediateZero(value));
  } else {
    InstructionOperand addr_reg = g.TempRegister();
    selector->Emit(kLoong64Add_d | AddressingModeField::encode(kMode_None),
                   addr_reg, g.UseRegister(index), g.UseRegister(base));
    // Emit desired store opcode, using temp addr_reg.
    selector->Emit(code | AddressingModeField::encode(kMode_MRI) |
                       AtomicWidthField::encode(width),
                   g.NoOutput(), addr_reg, g.TempImmediate(0),
                   g.UseRegisterOrImmediateZero(value));
  }
}

template <typename Adapter>
void VisitAtomicExchange(InstructionSelectorT<Adapter>* selector, Node* node,
                         ArchOpcode opcode, AtomicWidth width) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* value = node->InputAt(2);

  AddressingMode addressing_mode = kMode_MRI;
  InstructionOperand inputs[3];
  size_t input_count = 0;
  inputs[input_count++] = g.UseUniqueRegister(base);
  inputs[input_count++] = g.UseUniqueRegister(index);
  inputs[input_count++] = g.UseUniqueRegister(value);
  InstructionOperand outputs[1];
  outputs[0] = g.UseUniqueRegister(node);
  InstructionOperand temp[3];
  temp[0] = g.TempRegister();
  temp[1] = g.TempRegister();
  temp[2] = g.TempRegister();
  InstructionCode code = opcode | AddressingModeField::encode(addressing_mode) |
                         AtomicWidthField::encode(width);
  selector->Emit(code, 1, outputs, input_count, inputs, 3, temp);
}

template <typename Adapter>
void VisitAtomicCompareExchange(InstructionSelectorT<Adapter>* selector,
                                Node* node, ArchOpcode opcode,
                                AtomicWidth width) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* old_value = node->InputAt(2);
  Node* new_value = node->InputAt(3);

  AddressingMode addressing_mode = kMode_MRI;
  InstructionOperand inputs[4];
  size_t input_count = 0;
  inputs[input_count++] = g.UseUniqueRegister(base);
  inputs[input_count++] = g.UseUniqueRegister(index);
  inputs[input_count++] = g.UseUniqueRegister(old_value);
  inputs[input_count++] = g.UseUniqueRegister(new_value);
  InstructionOperand outputs[1];
  outputs[0] = g.UseUniqueRegister(node);
  InstructionOperand temp[3];
  temp[0] = g.TempRegister();
  temp[1] = g.TempRegister();
  temp[2] = g.TempRegister();
  InstructionCode code = opcode | AddressingModeField::encode(addressing_mode) |
                         AtomicWidthField::encode(width);
  selector->Emit(code, 1, outputs, input_count, inputs, 3, temp);
}

template <typename Adapter>
void VisitAtomicBinop(InstructionSelectorT<Adapter>* selector, Node* node,
                      ArchOpcode opcode, AtomicWidth width) {
  Loong64OperandGeneratorT<Adapter> g(selector);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* value = node->InputAt(2);

  AddressingMode addressing_mode = kMode_MRI;
  InstructionOperand inputs[3];
  size_t input_count = 0;
  inputs[input_count++] = g.UseUniqueRegister(base);
  inputs[input_count++] = g.UseUniqueRegister(index);
  inputs[input_count++] = g.UseUniqueRegister(value);
  InstructionOperand outputs[1];
  outputs[0] = g.UseUniqueRegister(node);
  InstructionOperand temps[4];
  temps[0] = g.TempRegister();
  temps[1] = g.TempRegister();
  temps[2] = g.TempRegister();
  temps[3] = g.TempRegister();
  InstructionCode code = opcode | AddressingModeField::encode(addressing_mode) |
                         AtomicWidthField::encode(width);
  selector->Emit(code, 1, outputs, input_count, inputs, 4, temps);
}

}  // namespace

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitStackPointerGreaterThan(
    node_t node, FlagsContinuationT<Adapter>* cont) {
  StackCheckKind kind;
  node_t value;
  if constexpr (Adapter::IsTurboshaft) {
    const auto& op =
        this->turboshaft_graph()
            ->Get(node)
            .template Cast<turboshaft::StackPointerGreaterThanOp>();
    kind = op.kind;
    value = op.stack_limit();
  } else {
    kind = StackCheckKindOf(node->op());
    value = node->InputAt(0);
  }
  InstructionCode opcode =
      kArchStackPointerGreaterThan | MiscField::encode(static_cast<int>(kind));

  Loong64OperandGeneratorT<Adapter> g(this);

  // No outputs.
  InstructionOperand* const outputs = nullptr;
  const int output_count = 0;

  // TempRegister(0) is used to store the comparison result.
  // Applying an offset to this stack check requires a temp register. Offsets
  // are only applied to the first stack check. If applying an offset, we must
  // ensure the input and temp registers do not alias, thus kUniqueRegister.
  InstructionOperand temps[] = {g.TempRegister(), g.TempRegister()};
  const int temp_count = (kind == StackCheckKind::kJSFunctionEntry ? 2 : 1);
  const auto register_mode = (kind == StackCheckKind::kJSFunctionEntry)
                                 ? OperandGenerator::kUniqueRegister
                                 : OperandGenerator::kRegister;

  InstructionOperand inputs[] = {g.UseRegisterWithMode(value, register_mode)};
  static constexpr int input_count = arraysize(inputs);

  EmitWithContinuation(opcode, output_count, outputs, input_count, inputs,
                       temp_count, temps, cont);
}

// Shared routine for word comparisons against zero.
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWordCompareZero(
    node_t user, node_t value, FlagsContinuationT<Adapter>* cont) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    Loong64OperandGeneratorT<Adapter> g(this);
    // Try to combine with comparisons against 0 by simply inverting the branch.
    while (value->opcode() == IrOpcode::kWord32Equal && CanCover(user, value)) {
      Int32BinopMatcher m(value);
      if (!m.right().Is(0)) break;
      user = value;
      value = m.left().node();
      cont->Negate();
    }

    if (CanCover(user, value)) {
      switch (value->opcode()) {
        case IrOpcode::kWord32Equal:
          cont->OverwriteAndNegateIfEqual(kEqual);
          return VisitWord32Compare(this, value, cont);
        case IrOpcode::kInt32LessThan:
          cont->OverwriteAndNegateIfEqual(kSignedLessThan);
          return VisitWord32Compare(this, value, cont);
        case IrOpcode::kInt32LessThanOrEqual:
          cont->OverwriteAndNegateIfEqual(kSignedLessThanOrEqual);
          return VisitWord32Compare(this, value, cont);
        case IrOpcode::kUint32LessThan:
          cont->OverwriteAndNegateIfEqual(kUnsignedLessThan);
          return VisitWord32Compare(this, value, cont);
        case IrOpcode::kUint32LessThanOrEqual:
          cont->OverwriteAndNegateIfEqual(kUnsignedLessThanOrEqual);
          return VisitWord32Compare(this, value, cont);
        case IrOpcode::kWord64Equal:
          cont->OverwriteAndNegateIfEqual(kEqual);
          return VisitWord64Compare(this, value, cont);
        case IrOpcode::kInt64LessThan:
          cont->OverwriteAndNegateIfEqual(kSignedLessThan);
          return VisitWord64Compare(this, value, cont);
        case IrOpcode::kInt64LessThanOrEqual:
          cont->OverwriteAndNegateIfEqual(kSignedLessThanOrEqual);
          return VisitWord64Compare(this, value, cont);
        case IrOpcode::kUint64LessThan:
          cont->OverwriteAndNegateIfEqual(kUnsignedLessThan);
          return VisitWord64Compare(this, value, cont);
        case IrOpcode::kUint64LessThanOrEqual:
          cont->OverwriteAndNegateIfEqual(kUnsignedLessThanOrEqual);
          return VisitWord64Compare(this, value, cont);
        case IrOpcode::kFloat32Equal:
          cont->OverwriteAndNegateIfEqual(kEqual);
          return VisitFloat32Compare(this, value, cont);
        case IrOpcode::kFloat32LessThan:
          cont->OverwriteAndNegateIfEqual(kUnsignedLessThan);
          return VisitFloat32Compare(this, value, cont);
        case IrOpcode::kFloat32LessThanOrEqual:
          cont->OverwriteAndNegateIfEqual(kUnsignedLessThanOrEqual);
          return VisitFloat32Compare(this, value, cont);
        case IrOpcode::kFloat64Equal:
          cont->OverwriteAndNegateIfEqual(kEqual);
          return VisitFloat64Compare(this, value, cont);
        case IrOpcode::kFloat64LessThan:
          cont->OverwriteAndNegateIfEqual(kUnsignedLessThan);
          return VisitFloat64Compare(this, value, cont);
        case IrOpcode::kFloat64LessThanOrEqual:
          cont->OverwriteAndNegateIfEqual(kUnsignedLessThanOrEqual);
          return VisitFloat64Compare(this, value, cont);
        case IrOpcode::kProjection:
          // Check if this is the overflow output projection of an
          // <Operation>WithOverflow node.
          if (ProjectionIndexOf(value->op()) == 1u) {
            // We cannot combine the <Operation>WithOverflow with this branch
            // unless the 0th projection (the use of the actual value of the
            // <Operation> is either nullptr, which means there's no use of the
            // actual value, or was already defined, which means it is scheduled
            // *AFTER* this branch).
            Node* const node = value->InputAt(0);
            Node* const result = NodeProperties::FindProjection(node, 0);
            if (result == nullptr || IsDefined(result)) {
              switch (node->opcode()) {
                case IrOpcode::kInt32AddWithOverflow:
                  cont->OverwriteAndNegateIfEqual(kOverflow);
                  return VisitBinop(this, node, kLoong64Add_d, cont);
                case IrOpcode::kInt32SubWithOverflow:
                  cont->OverwriteAndNegateIfEqual(kOverflow);
                  return VisitBinop(this, node, kLoong64Sub_d, cont);
                case IrOpcode::kInt32MulWithOverflow:
                  cont->OverwriteAndNegateIfEqual(kOverflow);
                  return VisitBinop(this, node, kLoong64MulOvf_w, cont);
                case IrOpcode::kInt64MulWithOverflow:
                  cont->OverwriteAndNegateIfEqual(kOverflow);
                  return VisitBinop(this, node, kLoong64MulOvf_d, cont);
                case IrOpcode::kInt64AddWithOverflow:
                  cont->OverwriteAndNegateIfEqual(kOverflow);
                  return VisitBinop(this, node, kLoong64AddOvf_d, cont);
                case IrOpcode::kInt64SubWithOverflow:
                  cont->OverwriteAndNegateIfEqual(kOverflow);
                  return VisitBinop(this, node, kLoong64SubOvf_d, cont);
                default:
                  break;
              }
            }
          }
          break;
        case IrOpcode::kWord32And:
        case IrOpcode::kWord64And:
          return VisitWordCompare(this, value, kLoong64Tst, cont, true);
        case IrOpcode::kStackPointerGreaterThan:
          cont->OverwriteAndNegateIfEqual(kStackPointerGreaterThanCondition);
          return VisitStackPointerGreaterThan(value, cont);
        default:
          break;
      }
    }

    // Continuation could not be combined with a compare, emit compare against
    // 0.
    VisitCompare(this, kLoong64Cmp32, g.UseRegister(value), g.TempImmediate(0),
                 cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitSwitch(Node* node,
                                                const SwitchInfo& sw) {
  Loong64OperandGeneratorT<Adapter> g(this);
  InstructionOperand value_operand = g.UseRegister(node->InputAt(0));

  // Emit either ArchTableSwitch or ArchBinarySearchSwitch.
  if (enable_switch_jump_table_ ==
      InstructionSelector::kEnableSwitchJumpTable) {
    static const size_t kMaxTableSwitchValueRange = 2 << 16;
    size_t table_space_cost = 10 + 2 * sw.value_range();
    size_t table_time_cost = 3;
    size_t lookup_space_cost = 2 + 2 * sw.case_count();
    size_t lookup_time_cost = sw.case_count();
    if (sw.case_count() > 0 &&
        table_space_cost + 3 * table_time_cost <=
            lookup_space_cost + 3 * lookup_time_cost &&
        sw.min_value() > std::numeric_limits<int32_t>::min() &&
        sw.value_range() <= kMaxTableSwitchValueRange) {
      InstructionOperand index_operand = value_operand;
      if (sw.min_value()) {
        index_operand = g.TempRegister();
        Emit(kLoong64Sub_w, index_operand, value_operand,
             g.TempImmediate(sw.min_value()));
      }
      // Generate a table lookup.
      return EmitTableSwitch(sw, index_operand);
    }
  }

  // Generate a tree of conditional jumps.
  return EmitBinarySearchSwitch(sw, value_operand);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Equal(node_t node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
    Int32BinopMatcher m(node);
    if (m.right().Is(0)) {
      return VisitWordCompareZero(m.node(), m.left().node(), &cont);
    }
    if (isolate() && (V8_STATIC_ROOTS_BOOL ||
                      (COMPRESS_POINTERS_BOOL && !isolate()->bootstrapper()))) {
      Loong64OperandGeneratorT<Adapter> g(this);
      const RootsTable& roots_table = isolate()->roots_table();
      RootIndex root_index;
      Node* left = nullptr;
      Handle<HeapObject> right;
      // HeapConstants and CompressedHeapConstants can be treated the same when
      // using them as an input to a 32-bit comparison. Check whether either is
      // present.
      {
        CompressedHeapObjectBinopMatcher m(node);
        if (m.right().HasResolvedValue()) {
          left = m.left().node();
          right = m.right().ResolvedValue();
        } else {
          HeapObjectBinopMatcher m2(node);
          if (m2.right().HasResolvedValue()) {
            left = m2.left().node();
            right = m2.right().ResolvedValue();
          }
        }
      }
      if (!right.is_null() && roots_table.IsRootHandle(right, &root_index)) {
        DCHECK_NE(left, nullptr);
        if (RootsTable::IsReadOnly(root_index)) {
          Tagged_t ptr =
              MacroAssemblerBase::ReadOnlyRootPtr(root_index, isolate());
          if (g.CanBeImmediate(ptr, kLoong64Cmp32)) {
            return VisitCompare(this, kLoong64Cmp32, g.UseRegister(left),
                                g.TempImmediate(ptr), &cont);
          }
        }
      }
    }
    VisitWord32Compare(this, node, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32LessThan(node_t node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kSignedLessThan, node);
  VisitWord32Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32LessThanOrEqual(node_t node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kSignedLessThanOrEqual, node);
  VisitWord32Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint32LessThan(node_t node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitWord32Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint32LessThanOrEqual(node_t node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitWord32Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32AddWithOverflow(node_t node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
      FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
      return VisitBinop(this, node, kLoong64Add_d, &cont);
    }
    FlagsContinuation cont;
    VisitBinop(this, node, kLoong64Add_d, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32SubWithOverflow(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
      FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
      return VisitBinop(this, node, kLoong64Sub_d, &cont);
    }
    FlagsContinuation cont;
    VisitBinop(this, node, kLoong64Sub_d, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32MulWithOverflow(node_t node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
      FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
      return VisitBinop(this, node, kLoong64MulOvf_w, &cont);
    }
    FlagsContinuation cont;
    VisitBinop(this, node, kLoong64MulOvf_w, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64MulWithOverflow(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
      FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
      return VisitBinop(this, node, kLoong64MulOvf_d, &cont);
    }
    FlagsContinuation cont;
    VisitBinop(this, node, kLoong64MulOvf_d, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64AddWithOverflow(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
      FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
      return VisitBinop(this, node, kLoong64AddOvf_d, &cont);
    }
    FlagsContinuation cont;
    VisitBinop(this, node, kLoong64AddOvf_d, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64SubWithOverflow(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
      FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
      return VisitBinop(this, node, kLoong64SubOvf_d, &cont);
    }
    FlagsContinuation cont;
    VisitBinop(this, node, kLoong64SubOvf_d, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Equal(Node* const node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
    VisitWord64Compare(this, node, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64LessThan(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    FlagsContinuation cont = FlagsContinuation::ForSet(kSignedLessThan, node);
    VisitWord64Compare(this, node, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64LessThanOrEqual(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    FlagsContinuation cont =
        FlagsContinuation::ForSet(kSignedLessThanOrEqual, node);
    VisitWord64Compare(this, node, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint64LessThan(node_t node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
    VisitWord64Compare(this, node, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint64LessThanOrEqual(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    FlagsContinuation cont =
        FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
    VisitWord64Compare(this, node, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Equal(node_t node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
  VisitFloat32Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32LessThan(node_t node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitFloat32Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32LessThanOrEqual(node_t node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitFloat32Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Equal(node_t node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
  VisitFloat64Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64LessThan(node_t node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitFloat64Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64LessThanOrEqual(node_t node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitFloat64Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64ExtractLowWord32(Node* node) {
  VisitRR(this, kLoong64Float64ExtractLowWord32, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64ExtractHighWord32(Node* node) {
  VisitRR(this, kLoong64Float64ExtractHighWord32, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64SilenceNaN(Node* node) {
  VisitRR(this, kLoong64Float64SilenceNaN, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64InsertLowWord32(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  Emit(kLoong64Float64InsertLowWord32, g.DefineSameAsFirst(node),
       g.UseRegister(left), g.UseRegister(right));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64InsertHighWord32(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  Emit(kLoong64Float64InsertHighWord32, g.DefineSameAsFirst(node),
       g.UseRegister(left), g.UseRegister(right));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitMemoryBarrier(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64Dbar, g.NoOutput());
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32AtomicLoad(Node* node) {
  VisitAtomicLoad(this, node, AtomicWidth::kWord32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32AtomicStore(node_t node) {
  VisitAtomicStore(this, node, AtomicWidth::kWord32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64AtomicLoad(Node* node) {
  VisitAtomicLoad(this, node, AtomicWidth::kWord64);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64AtomicStore(node_t node) {
  VisitAtomicStore(this, node, AtomicWidth::kWord64);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32AtomicExchange(Node* node) {
  ArchOpcode opcode;
  MachineType type = AtomicOpType(node->op());
  if (type == MachineType::Int8()) {
    opcode = kAtomicExchangeInt8;
  } else if (type == MachineType::Uint8()) {
    opcode = kAtomicExchangeUint8;
  } else if (type == MachineType::Int16()) {
    opcode = kAtomicExchangeInt16;
  } else if (type == MachineType::Uint16()) {
    opcode = kAtomicExchangeUint16;
  } else if (type == MachineType::Int32() || type == MachineType::Uint32()) {
    opcode = kAtomicExchangeWord32;
  } else {
    UNREACHABLE();
  }

  VisitAtomicExchange(this, node, opcode, AtomicWidth::kWord32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64AtomicExchange(Node* node) {
  ArchOpcode opcode;
  MachineType type = AtomicOpType(node->op());
  if (type == MachineType::Uint8()) {
    opcode = kAtomicExchangeUint8;
  } else if (type == MachineType::Uint16()) {
    opcode = kAtomicExchangeUint16;
  } else if (type == MachineType::Uint32()) {
    opcode = kAtomicExchangeWord32;
  } else if (type == MachineType::Uint64()) {
    opcode = kLoong64Word64AtomicExchangeUint64;
  } else {
    UNREACHABLE();
  }
  VisitAtomicExchange(this, node, opcode, AtomicWidth::kWord64);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32AtomicCompareExchange(
    Node* node) {
  ArchOpcode opcode;
  MachineType type = AtomicOpType(node->op());
  if (type == MachineType::Int8()) {
    opcode = kAtomicCompareExchangeInt8;
  } else if (type == MachineType::Uint8()) {
    opcode = kAtomicCompareExchangeUint8;
  } else if (type == MachineType::Int16()) {
    opcode = kAtomicCompareExchangeInt16;
  } else if (type == MachineType::Uint16()) {
    opcode = kAtomicCompareExchangeUint16;
  } else if (type == MachineType::Int32() || type == MachineType::Uint32()) {
    opcode = kAtomicCompareExchangeWord32;
  } else {
    UNREACHABLE();
  }

  VisitAtomicCompareExchange(this, node, opcode, AtomicWidth::kWord32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64AtomicCompareExchange(
    Node* node) {
  ArchOpcode opcode;
  MachineType type = AtomicOpType(node->op());
  if (type == MachineType::Uint8()) {
    opcode = kAtomicCompareExchangeUint8;
  } else if (type == MachineType::Uint16()) {
    opcode = kAtomicCompareExchangeUint16;
  } else if (type == MachineType::Uint32()) {
    opcode = kAtomicCompareExchangeWord32;
  } else if (type == MachineType::Uint64()) {
    opcode = kLoong64Word64AtomicCompareExchangeUint64;
  } else {
    UNREACHABLE();
  }
  VisitAtomicCompareExchange(this, node, opcode, AtomicWidth::kWord64);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32AtomicBinaryOperation(
    node_t node, ArchOpcode int8_op, ArchOpcode uint8_op, ArchOpcode int16_op,
    ArchOpcode uint16_op, ArchOpcode word32_op) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    ArchOpcode opcode;
    MachineType type = AtomicOpType(node->op());
    if (type == MachineType::Int8()) {
      opcode = int8_op;
    } else if (type == MachineType::Uint8()) {
      opcode = uint8_op;
    } else if (type == MachineType::Int16()) {
      opcode = int16_op;
    } else if (type == MachineType::Uint16()) {
      opcode = uint16_op;
    } else if (type == MachineType::Int32() || type == MachineType::Uint32()) {
      opcode = word32_op;
    } else {
      UNREACHABLE();
    }

    VisitAtomicBinop(this, node, opcode, AtomicWidth::kWord32);
  }
}

#define VISIT_ATOMIC_BINOP(op)                                            \
  template <typename Adapter>                                             \
  void InstructionSelectorT<Adapter>::VisitWord32Atomic##op(Node* node) { \
    VisitWord32AtomicBinaryOperation(                                     \
        node, kAtomic##op##Int8, kAtomic##op##Uint8, kAtomic##op##Int16,  \
        kAtomic##op##Uint16, kAtomic##op##Word32);                        \
  }
VISIT_ATOMIC_BINOP(Add)
VISIT_ATOMIC_BINOP(Sub)
VISIT_ATOMIC_BINOP(And)
VISIT_ATOMIC_BINOP(Or)
VISIT_ATOMIC_BINOP(Xor)
#undef VISIT_ATOMIC_BINOP

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64AtomicBinaryOperation(
    node_t node, ArchOpcode uint8_op, ArchOpcode uint16_op,
    ArchOpcode uint32_op, ArchOpcode uint64_op) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    ArchOpcode opcode;
    MachineType type = AtomicOpType(node->op());
    if (type == MachineType::Uint8()) {
      opcode = uint8_op;
    } else if (type == MachineType::Uint16()) {
      opcode = uint16_op;
    } else if (type == MachineType::Uint32()) {
      opcode = uint32_op;
    } else if (type == MachineType::Uint64()) {
      opcode = uint64_op;
    } else {
      UNREACHABLE();
    }
    VisitAtomicBinop(this, node, opcode, AtomicWidth::kWord64);
  }
}

#define VISIT_ATOMIC_BINOP(op)                                                 \
  template <typename Adapter>                                                  \
  void InstructionSelectorT<Adapter>::VisitWord64Atomic##op(Node* node) {      \
    VisitWord64AtomicBinaryOperation(node, kAtomic##op##Uint8,                 \
                                     kAtomic##op##Uint16, kAtomic##op##Word32, \
                                     kLoong64Word64Atomic##op##Uint64);        \
  }
VISIT_ATOMIC_BINOP(Add)
VISIT_ATOMIC_BINOP(Sub)
VISIT_ATOMIC_BINOP(And)
VISIT_ATOMIC_BINOP(Or)
VISIT_ATOMIC_BINOP(Xor)
#undef VISIT_ATOMIC_BINOP

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32AbsWithOverflow(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64AbsWithOverflow(Node* node) {
  UNREACHABLE();
}

#define SIMD_TYPE_LIST(V) \
  V(F64x2)                \
  V(F32x4)                \
  V(I64x2)                \
  V(I32x4)                \
  V(I16x8)                \
  V(I8x16)

#define SIMD_UNOP_LIST(V)                                             \
  V(F64x2Abs, kLoong64F64x2Abs)                                       \
  V(F64x2Neg, kLoong64F64x2Neg)                                       \
  V(F64x2Sqrt, kLoong64F64x2Sqrt)                                     \
  V(F64x2Ceil, kLoong64F64x2Ceil)                                     \
  V(F64x2Floor, kLoong64F64x2Floor)                                   \
  V(F64x2Trunc, kLoong64F64x2Trunc)                                   \
  V(F64x2NearestInt, kLoong64F64x2NearestInt)                         \
  V(I64x2Neg, kLoong64I64x2Neg)                                       \
  V(I64x2BitMask, kLoong64I64x2BitMask)                               \
  V(F64x2ConvertLowI32x4S, kLoong64F64x2ConvertLowI32x4S)             \
  V(F64x2ConvertLowI32x4U, kLoong64F64x2ConvertLowI32x4U)             \
  V(F64x2PromoteLowF32x4, kLoong64F64x2PromoteLowF32x4)               \
  V(F32x4SConvertI32x4, kLoong64F32x4SConvertI32x4)                   \
  V(F32x4UConvertI32x4, kLoong64F32x4UConvertI32x4)                   \
  V(F32x4Abs, kLoong64F32x4Abs)                                       \
  V(F32x4Neg, kLoong64F32x4Neg)                                       \
  V(F32x4Sqrt, kLoong64F32x4Sqrt)                                     \
  V(F32x4Ceil, kLoong64F32x4Ceil)                                     \
  V(F32x4Floor, kLoong64F32x4Floor)                                   \
  V(F32x4Trunc, kLoong64F32x4Trunc)                                   \
  V(F32x4NearestInt, kLoong64F32x4NearestInt)                         \
  V(F32x4DemoteF64x2Zero, kLoong64F32x4DemoteF64x2Zero)               \
  V(I64x2Abs, kLoong64I64x2Abs)                                       \
  V(I64x2SConvertI32x4Low, kLoong64I64x2SConvertI32x4Low)             \
  V(I64x2SConvertI32x4High, kLoong64I64x2SConvertI32x4High)           \
  V(I64x2UConvertI32x4Low, kLoong64I64x2UConvertI32x4Low)             \
  V(I64x2UConvertI32x4High, kLoong64I64x2UConvertI32x4High)           \
  V(I32x4SConvertF32x4, kLoong64I32x4SConvertF32x4)                   \
  V(I32x4UConvertF32x4, kLoong64I32x4UConvertF32x4)                   \
  V(I32x4Neg, kLoong64I32x4Neg)                                       \
  V(I32x4SConvertI16x8Low, kLoong64I32x4SConvertI16x8Low)             \
  V(I32x4SConvertI16x8High, kLoong64I32x4SConvertI16x8High)           \
  V(I32x4UConvertI16x8Low, kLoong64I32x4UConvertI16x8Low)             \
  V(I32x4UConvertI16x8High, kLoong64I32x4UConvertI16x8High)           \
  V(I32x4Abs, kLoong64I32x4Abs)                                       \
  V(I32x4BitMask, kLoong64I32x4BitMask)                               \
  V(I32x4TruncSatF64x2SZero, kLoong64I32x4TruncSatF64x2SZero)         \
  V(I32x4TruncSatF64x2UZero, kLoong64I32x4TruncSatF64x2UZero)         \
  V(I32x4RelaxedTruncF32x4S, kLoong64I32x4RelaxedTruncF32x4S)         \
  V(I32x4RelaxedTruncF32x4U, kLoong64I32x4RelaxedTruncF32x4U)         \
  V(I32x4RelaxedTruncF64x2SZero, kLoong64I32x4RelaxedTruncF64x2SZero) \
  V(I32x4RelaxedTruncF64x2UZero, kLoong64I32x4RelaxedTruncF64x2UZero) \
  V(I16x8Neg, kLoong64I16x8Neg)                                       \
  V(I16x8SConvertI8x16Low, kLoong64I16x8SConvertI8x16Low)             \
  V(I16x8SConvertI8x16High, kLoong64I16x8SConvertI8x16High)           \
  V(I16x8UConvertI8x16Low, kLoong64I16x8UConvertI8x16Low)             \
  V(I16x8UConvertI8x16High, kLoong64I16x8UConvertI8x16High)           \
  V(I16x8Abs, kLoong64I16x8Abs)                                       \
  V(I16x8BitMask, kLoong64I16x8BitMask)                               \
  V(I8x16Neg, kLoong64I8x16Neg)                                       \
  V(I8x16Abs, kLoong64I8x16Abs)                                       \
  V(I8x16Popcnt, kLoong64I8x16Popcnt)                                 \
  V(I8x16BitMask, kLoong64I8x16BitMask)                               \
  V(S128Not, kLoong64S128Not)                                         \
  V(I64x2AllTrue, kLoong64I64x2AllTrue)                               \
  V(I32x4AllTrue, kLoong64I32x4AllTrue)                               \
  V(I16x8AllTrue, kLoong64I16x8AllTrue)                               \
  V(I8x16AllTrue, kLoong64I8x16AllTrue)                               \
  V(V128AnyTrue, kLoong64V128AnyTrue)

#define SIMD_SHIFT_OP_LIST(V) \
  V(I64x2Shl)                 \
  V(I64x2ShrS)                \
  V(I64x2ShrU)                \
  V(I32x4Shl)                 \
  V(I32x4ShrS)                \
  V(I32x4ShrU)                \
  V(I16x8Shl)                 \
  V(I16x8ShrS)                \
  V(I16x8ShrU)                \
  V(I8x16Shl)                 \
  V(I8x16ShrS)                \
  V(I8x16ShrU)

#define SIMD_BINOP_LIST(V)                                \
  V(F64x2Add, kLoong64F64x2Add)                           \
  V(F64x2Sub, kLoong64F64x2Sub)                           \
  V(F64x2Mul, kLoong64F64x2Mul)                           \
  V(F64x2Div, kLoong64F64x2Div)                           \
  V(F64x2Min, kLoong64F64x2Min)                           \
  V(F64x2Max, kLoong64F64x2Max)                           \
  V(F64x2Eq, kLoong64F64x2Eq)                             \
  V(F64x2Ne, kLoong64F64x2Ne)                             \
  V(F64x2Lt, kLoong64F64x2Lt)                             \
  V(F64x2Le, kLoong64F64x2Le)                             \
  V(F64x2RelaxedMin, kLoong64F64x2RelaxedMin)             \
  V(F64x2RelaxedMax, kLoong64F64x2RelaxedMax)             \
  V(I64x2Eq, kLoong64I64x2Eq)                             \
  V(I64x2Ne, kLoong64I64x2Ne)                             \
  V(I64x2Add, kLoong64I64x2Add)                           \
  V(I64x2Sub, kLoong64I64x2Sub)                           \
  V(I64x2Mul, kLoong64I64x2Mul)                           \
  V(I64x2GtS, kLoong64I64x2GtS)                           \
  V(I64x2GeS, kLoong64I64x2GeS)                           \
  V(F32x4Add, kLoong64F32x4Add)                           \
  V(F32x4Sub, kLoong64F32x4Sub)                           \
  V(F32x4Mul, kLoong64F32x4Mul)                           \
  V(F32x4Div, kLoong64F32x4Div)                           \
  V(F32x4Max, kLoong64F32x4Max)                           \
  V(F32x4Min, kLoong64F32x4Min)                           \
  V(F32x4Eq, kLoong64F32x4Eq)                             \
  V(F32x4Ne, kLoong64F32x4Ne)                             \
  V(F32x4Lt, kLoong64F32x4Lt)                             \
  V(F32x4Le, kLoong64F32x4Le)                             \
  V(F32x4RelaxedMin, kLoong64F32x4RelaxedMin)             \
  V(F32x4RelaxedMax, kLoong64F32x4RelaxedMax)             \
  V(I32x4Add, kLoong64I32x4Add)                           \
  V(I32x4Sub, kLoong64I32x4Sub)                           \
  V(I32x4Mul, kLoong64I32x4Mul)                           \
  V(I32x4MaxS, kLoong64I32x4MaxS)                         \
  V(I32x4MinS, kLoong64I32x4MinS)                         \
  V(I32x4MaxU, kLoong64I32x4MaxU)                         \
  V(I32x4MinU, kLoong64I32x4MinU)                         \
  V(I32x4Eq, kLoong64I32x4Eq)                             \
  V(I32x4Ne, kLoong64I32x4Ne)                             \
  V(I32x4GtS, kLoong64I32x4GtS)                           \
  V(I32x4GeS, kLoong64I32x4GeS)                           \
  V(I32x4GtU, kLoong64I32x4GtU)                           \
  V(I32x4GeU, kLoong64I32x4GeU)                           \
  V(I32x4DotI16x8S, kLoong64I32x4DotI16x8S)               \
  V(I16x8Add, kLoong64I16x8Add)                           \
  V(I16x8AddSatS, kLoong64I16x8AddSatS)                   \
  V(I16x8AddSatU, kLoong64I16x8AddSatU)                   \
  V(I16x8Sub, kLoong64I16x8Sub)                           \
  V(I16x8SubSatS, kLoong64I16x8SubSatS)                   \
  V(I16x8SubSatU, kLoong64I16x8SubSatU)                   \
  V(I16x8Mul, kLoong64I16x8Mul)                           \
  V(I16x8MaxS, kLoong64I16x8MaxS)                         \
  V(I16x8MinS, kLoong64I16x8MinS)                         \
  V(I16x8MaxU, kLoong64I16x8MaxU)                         \
  V(I16x8MinU, kLoong64I16x8MinU)                         \
  V(I16x8Eq, kLoong64I16x8Eq)                             \
  V(I16x8Ne, kLoong64I16x8Ne)                             \
  V(I16x8GtS, kLoong64I16x8GtS)                           \
  V(I16x8GeS, kLoong64I16x8GeS)                           \
  V(I16x8GtU, kLoong64I16x8GtU)                           \
  V(I16x8GeU, kLoong64I16x8GeU)                           \
  V(I16x8RoundingAverageU, kLoong64I16x8RoundingAverageU) \
  V(I16x8SConvertI32x4, kLoong64I16x8SConvertI32x4)       \
  V(I16x8UConvertI32x4, kLoong64I16x8UConvertI32x4)       \
  V(I16x8Q15MulRSatS, kLoong64I16x8Q15MulRSatS)           \
  V(I16x8RelaxedQ15MulRS, kLoong64I16x8RelaxedQ15MulRS)   \
  V(I8x16Add, kLoong64I8x16Add)                           \
  V(I8x16AddSatS, kLoong64I8x16AddSatS)                   \
  V(I8x16AddSatU, kLoong64I8x16AddSatU)                   \
  V(I8x16Sub, kLoong64I8x16Sub)                           \
  V(I8x16SubSatS, kLoong64I8x16SubSatS)                   \
  V(I8x16SubSatU, kLoong64I8x16SubSatU)                   \
  V(I8x16MaxS, kLoong64I8x16MaxS)                         \
  V(I8x16MinS, kLoong64I8x16MinS)                         \
  V(I8x16MaxU, kLoong64I8x16MaxU)                         \
  V(I8x16MinU, kLoong64I8x16MinU)                         \
  V(I8x16Eq, kLoong64I8x16Eq)                             \
  V(I8x16Ne, kLoong64I8x16Ne)                             \
  V(I8x16GtS, kLoong64I8x16GtS)                           \
  V(I8x16GeS, kLoong64I8x16GeS)                           \
  V(I8x16GtU, kLoong64I8x16GtU)                           \
  V(I8x16GeU, kLoong64I8x16GeU)                           \
  V(I8x16RoundingAverageU, kLoong64I8x16RoundingAverageU) \
  V(I8x16SConvertI16x8, kLoong64I8x16SConvertI16x8)       \
  V(I8x16UConvertI16x8, kLoong64I8x16UConvertI16x8)       \
  V(S128And, kLoong64S128And)                             \
  V(S128Or, kLoong64S128Or)                               \
  V(S128Xor, kLoong64S128Xor)                             \
  V(S128AndNot, kLoong64S128AndNot)

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitS128Const(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  static const int kUint32Immediates = kSimd128Size / sizeof(uint32_t);
  uint32_t val[kUint32Immediates];
  memcpy(val, S128ImmediateParameterOf(node->op()).data(), kSimd128Size);
  // If all bytes are zeros or ones, avoid emitting code for generic constants
  bool all_zeros = !(val[0] || val[1] || val[2] || val[3]);
  bool all_ones = val[0] == UINT32_MAX && val[1] == UINT32_MAX &&
                  val[2] == UINT32_MAX && val[3] == UINT32_MAX;
  InstructionOperand dst = g.DefineAsRegister(node);
  if (all_zeros) {
    Emit(kLoong64S128Zero, dst);
  } else if (all_ones) {
    Emit(kLoong64S128AllOnes, dst);
  } else {
    Emit(kLoong64S128Const, dst, g.UseImmediate(val[0]), g.UseImmediate(val[1]),
         g.UseImmediate(val[2]), g.UseImmediate(val[3]));
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitS128Zero(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64S128Zero, g.DefineAsRegister(node));
}

#define SIMD_VISIT_SPLAT(Type)                                         \
  template <typename Adapter>                                          \
  void InstructionSelectorT<Adapter>::Visit##Type##Splat(Node* node) { \
    VisitRR(this, kLoong64##Type##Splat, node);                        \
  }
SIMD_TYPE_LIST(SIMD_VISIT_SPLAT)
#undef SIMD_VISIT_SPLAT

#define SIMD_VISIT_EXTRACT_LANE(Type, Sign)                           \
  template <typename Adapter>                                         \
  void InstructionSelectorT<Adapter>::Visit##Type##ExtractLane##Sign( \
      Node* node) {                                                   \
    VisitRRI(this, kLoong64##Type##ExtractLane##Sign, node);          \
  }
SIMD_VISIT_EXTRACT_LANE(F64x2, )
SIMD_VISIT_EXTRACT_LANE(F32x4, )
SIMD_VISIT_EXTRACT_LANE(I64x2, )
SIMD_VISIT_EXTRACT_LANE(I32x4, )
SIMD_VISIT_EXTRACT_LANE(I16x8, U)
SIMD_VISIT_EXTRACT_LANE(I16x8, S)
SIMD_VISIT_EXTRACT_LANE(I8x16, U)
SIMD_VISIT_EXTRACT_LANE(I8x16, S)
#undef SIMD_VISIT_EXTRACT_LANE

#define SIMD_VISIT_REPLACE_LANE(Type)                                        \
  template <typename Adapter>                                                \
  void InstructionSelectorT<Adapter>::Visit##Type##ReplaceLane(Node* node) { \
    VisitRRIR(this, kLoong64##Type##ReplaceLane, node);                      \
  }
SIMD_TYPE_LIST(SIMD_VISIT_REPLACE_LANE)
#undef SIMD_VISIT_REPLACE_LANE

#define SIMD_VISIT_UNOP(Name, instruction)                      \
  template <typename Adapter>                                   \
  void InstructionSelectorT<Adapter>::Visit##Name(Node* node) { \
    VisitRR(this, instruction, node);                           \
  }
SIMD_UNOP_LIST(SIMD_VISIT_UNOP)
#undef SIMD_VISIT_UNOP

#define SIMD_VISIT_SHIFT_OP(Name)                               \
  template <typename Adapter>                                   \
  void InstructionSelectorT<Adapter>::Visit##Name(Node* node) { \
    VisitSimdShift(this, kLoong64##Name, node);                 \
  }
SIMD_SHIFT_OP_LIST(SIMD_VISIT_SHIFT_OP)
#undef SIMD_VISIT_SHIFT_OP

#define SIMD_VISIT_BINOP(Name, instruction)                     \
  template <typename Adapter>                                   \
  void InstructionSelectorT<Adapter>::Visit##Name(Node* node) { \
    VisitRRR(this, instruction, node);                          \
  }
SIMD_BINOP_LIST(SIMD_VISIT_BINOP)
#undef SIMD_VISIT_BINOP

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitS128Select(Node* node) {
  VisitRRRR(this, kLoong64S128Select, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI8x16RelaxedLaneSelect(Node* node) {
  VisitS128Select(node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI16x8RelaxedLaneSelect(Node* node) {
  VisitS128Select(node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x4RelaxedLaneSelect(Node* node) {
  VisitS128Select(node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI64x2RelaxedLaneSelect(Node* node) {
  VisitS128Select(node);
}

#define SIMD_UNIMP_OP_LIST(V) \
  V(F64x2Qfma)                \
  V(F64x2Qfms)                \
  V(F32x4Qfma)                \
  V(F32x4Qfms)                \
  V(I16x8DotI8x16I7x16S)      \
  V(I32x4DotI8x16I7x16AddS)

#define SIMD_VISIT_UNIMP_OP(Name)                               \
  template <typename Adapter>                                   \
  void InstructionSelectorT<Adapter>::Visit##Name(Node* node) { \
    UNIMPLEMENTED();                                            \
  }
SIMD_UNIMP_OP_LIST(SIMD_VISIT_UNIMP_OP)

#undef SIMD_VISIT_UNIMP_OP
#undef SIMD_UNIMP_OP_LIST

#if V8_ENABLE_WEBASSEMBLY
namespace {

struct ShuffleEntry {
  uint8_t shuffle[kSimd128Size];
  ArchOpcode opcode;
};

static const ShuffleEntry arch_shuffles[] = {
    {{0, 1, 2, 3, 16, 17, 18, 19, 4, 5, 6, 7, 20, 21, 22, 23},
     kLoong64S32x4InterleaveRight},
    {{8, 9, 10, 11, 24, 25, 26, 27, 12, 13, 14, 15, 28, 29, 30, 31},
     kLoong64S32x4InterleaveLeft},
    {{0, 1, 2, 3, 8, 9, 10, 11, 16, 17, 18, 19, 24, 25, 26, 27},
     kLoong64S32x4PackEven},
    {{4, 5, 6, 7, 12, 13, 14, 15, 20, 21, 22, 23, 28, 29, 30, 31},
     kLoong64S32x4PackOdd},
    {{0, 1, 2, 3, 16, 17, 18, 19, 8, 9, 10, 11, 24, 25, 26, 27},
     kLoong64S32x4InterleaveEven},
    {{4, 5, 6, 7, 20, 21, 22, 23, 12, 13, 14, 15, 28, 29, 30, 31},
     kLoong64S32x4InterleaveOdd},

    {{0, 1, 16, 17, 2, 3, 18, 19, 4, 5, 20, 21, 6, 7, 22, 23},
     kLoong64S16x8InterleaveRight},
    {{8, 9, 24, 25, 10, 11, 26, 27, 12, 13, 28, 29, 14, 15, 30, 31},
     kLoong64S16x8InterleaveLeft},
    {{0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21, 24, 25, 28, 29},
     kLoong64S16x8PackEven},
    {{2, 3, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23, 26, 27, 30, 31},
     kLoong64S16x8PackOdd},
    {{0, 1, 16, 17, 4, 5, 20, 21, 8, 9, 24, 25, 12, 13, 28, 29},
     kLoong64S16x8InterleaveEven},
    {{2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31},
     kLoong64S16x8InterleaveOdd},
    {{6, 7, 4, 5, 2, 3, 0, 1, 14, 15, 12, 13, 10, 11, 8, 9},
     kLoong64S16x4Reverse},
    {{2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13},
     kLoong64S16x2Reverse},

    {{0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23},
     kLoong64S8x16InterleaveRight},
    {{8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31},
     kLoong64S8x16InterleaveLeft},
    {{0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30},
     kLoong64S8x16PackEven},
    {{1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31},
     kLoong64S8x16PackOdd},
    {{0, 16, 2, 18, 4, 20, 6, 22, 8, 24, 10, 26, 12, 28, 14, 30},
     kLoong64S8x16InterleaveEven},
    {{1, 17, 3, 19, 5, 21, 7, 23, 9, 25, 11, 27, 13, 29, 15, 31},
     kLoong64S8x16InterleaveOdd},
    {{7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8},
     kLoong64S8x8Reverse},
    {{3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12},
     kLoong64S8x4Reverse},
    {{1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14},
     kLoong64S8x2Reverse}};

bool TryMatchArchShuffle(const uint8_t* shuffle, const ShuffleEntry* table,
                         size_t num_entries, bool is_swizzle,
                         ArchOpcode* opcode) {
  uint8_t mask = is_swizzle ? kSimd128Size - 1 : 2 * kSimd128Size - 1;
  for (size_t i = 0; i < num_entries; ++i) {
    const ShuffleEntry& entry = table[i];
    int j = 0;
    for (; j < kSimd128Size; ++j) {
      if ((entry.shuffle[j] & mask) != (shuffle[j] & mask)) {
        break;
      }
    }
    if (j == kSimd128Size) {
      *opcode = entry.opcode;
      return true;
    }
  }
  return false;
}

}  // namespace

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI8x16Shuffle(Node* node) {
  uint8_t shuffle[kSimd128Size];
  bool is_swizzle;
  CanonicalizeShuffle(node, shuffle, &is_swizzle);
  uint8_t shuffle32x4[4];
  ArchOpcode opcode;
  if (TryMatchArchShuffle(shuffle, arch_shuffles, arraysize(arch_shuffles),
                          is_swizzle, &opcode)) {
    VisitRRR(this, opcode, node);
    return;
  }
  Node* input0 = node->InputAt(0);
  Node* input1 = node->InputAt(1);
  uint8_t offset;
  Loong64OperandGeneratorT<Adapter> g(this);
  if (wasm::SimdShuffle::TryMatchConcat(shuffle, &offset)) {
    Emit(kLoong64S8x16Concat, g.DefineSameAsFirst(node), g.UseRegister(input1),
         g.UseRegister(input0), g.UseImmediate(offset));
    return;
  }
  if (wasm::SimdShuffle::TryMatch32x4Shuffle(shuffle, shuffle32x4)) {
    Emit(kLoong64S32x4Shuffle, g.DefineAsRegister(node), g.UseRegister(input0),
         g.UseRegister(input1),
         g.UseImmediate(wasm::SimdShuffle::Pack4Lanes(shuffle32x4)));
    return;
  }
  Emit(kLoong64I8x16Shuffle, g.DefineAsRegister(node), g.UseRegister(input0),
       g.UseRegister(input1),
       g.UseImmediate(wasm::SimdShuffle::Pack4Lanes(shuffle)),
       g.UseImmediate(wasm::SimdShuffle::Pack4Lanes(shuffle + 4)),
       g.UseImmediate(wasm::SimdShuffle::Pack4Lanes(shuffle + 8)),
       g.UseImmediate(wasm::SimdShuffle::Pack4Lanes(shuffle + 12)));
}
#else
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI8x16Shuffle(Node* node) {
  UNREACHABLE();
}
#endif  // V8_ENABLE_WEBASSEMBLY

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI8x16Swizzle(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  InstructionOperand temps[] = {g.TempSimd128Register()};
  // We don't want input 0 or input 1 to be the same as output, since we will
  // modify output before do the calculation.
  Emit(kLoong64I8x16Swizzle, g.DefineAsRegister(node),
       g.UseUniqueRegister(node->InputAt(0)),
       g.UseUniqueRegister(node->InputAt(1)), arraysize(temps), temps);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitSignExtendWord8ToInt32(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64Ext_w_b, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitSignExtendWord16ToInt32(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64Ext_w_h, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitSignExtendWord8ToInt64(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64Ext_w_b, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitSignExtendWord16ToInt64(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64Ext_w_h, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitSignExtendWord32ToInt64(Node* node) {
  Loong64OperandGeneratorT<Adapter> g(this);
  Emit(kLoong64Sll_w, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),
       g.TempImmediate(0));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF32x4Pmin(Node* node) {
  VisitUniqueRRR(this, kLoong64F32x4Pmin, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF32x4Pmax(Node* node) {
  VisitUniqueRRR(this, kLoong64F32x4Pmax, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x2Pmin(Node* node) {
  VisitUniqueRRR(this, kLoong64F64x2Pmin, node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x2Pmax(Node* node) {
  VisitUniqueRRR(this, kLoong64F64x2Pmax, node);
}

#define VISIT_EXT_MUL(OPCODE1, OPCODE2)                                    \
  template <typename Adapter>                                              \
  void InstructionSelectorT<Adapter>::Visit##OPCODE1##ExtMulLow##OPCODE2(  \
      Node* node) {}                                                       \
  template <typename Adapter>                                              \
  void InstructionSelectorT<Adapter>::Visit##OPCODE1##ExtMulHigh##OPCODE2( \
      Node* node) {}

VISIT_EXT_MUL(I64x2, I32x4S)
VISIT_EXT_MUL(I64x2, I32x4U)
VISIT_EXT_MUL(I32x4, I16x8S)
VISIT_EXT_MUL(I32x4, I16x8U)
VISIT_EXT_MUL(I16x8, I8x16S)
VISIT_EXT_MUL(I16x8, I8x16U)
#undef VISIT_EXT_MUL

#define VISIT_EXTADD_PAIRWISE(OPCODE)                             \
  template <typename Adapter>                                     \
  void InstructionSelectorT<Adapter>::Visit##OPCODE(Node* node) { \
    Loong64OperandGeneratorT<Adapter> g(this);                    \
    Emit(kLoong64ExtAddPairwise, g.DefineAsRegister(node),        \
         g.UseRegister(node->InputAt(0)));                        \
  }
VISIT_EXTADD_PAIRWISE(I16x8ExtAddPairwiseI8x16S)
VISIT_EXTADD_PAIRWISE(I16x8ExtAddPairwiseI8x16U)
VISIT_EXTADD_PAIRWISE(I32x4ExtAddPairwiseI16x8S)
VISIT_EXTADD_PAIRWISE(I32x4ExtAddPairwiseI16x8U)
#undef VISIT_EXTADD_PAIRWISE

template <typename Adapter>
void InstructionSelectorT<Adapter>::AddOutputToSelectContinuation(
    OperandGenerator* g, int first_input_index, node_t node) {
  UNREACHABLE();
}

// static
MachineOperatorBuilder::Flags
InstructionSelector::SupportedMachineOperatorFlags() {
  MachineOperatorBuilder::Flags flags = MachineOperatorBuilder::kNoFlags;
  return flags | MachineOperatorBuilder::kWord32ShiftIsSafe |
         MachineOperatorBuilder::kInt32DivIsSafe |
         MachineOperatorBuilder::kUint32DivIsSafe |
         MachineOperatorBuilder::kFloat64RoundDown |
         MachineOperatorBuilder::kFloat32RoundDown |
         MachineOperatorBuilder::kFloat64RoundUp |
         MachineOperatorBuilder::kFloat32RoundUp |
         MachineOperatorBuilder::kFloat64RoundTruncate |
         MachineOperatorBuilder::kFloat32RoundTruncate |
         MachineOperatorBuilder::kFloat64RoundTiesEven |
         MachineOperatorBuilder::kFloat32RoundTiesEven;
}

// static
MachineOperatorBuilder::AlignmentRequirements
InstructionSelector::AlignmentRequirements() {
  return MachineOperatorBuilder::AlignmentRequirements::
      FullUnalignedAccessSupport();
}

template class EXPORT_TEMPLATE_DEFINE(V8_EXPORT_PRIVATE)
    InstructionSelectorT<TurbofanAdapter>;
template class EXPORT_TEMPLATE_DEFINE(V8_EXPORT_PRIVATE)
    InstructionSelectorT<TurboshaftAdapter>;

#undef SIMD_BINOP_LIST
#undef SIMD_SHIFT_OP_LIST
#undef SIMD_UNOP_LIST
#undef SIMD_TYPE_LIST
#undef TRACE_UNIMPL
#undef TRACE

}  // namespace compiler
}  // namespace internal
}  // namespace v8
