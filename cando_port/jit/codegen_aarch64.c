/*
 * jit/codegen_aarch64.c -- IR -> AArch64 (A64) native code emitter.
 *
 * The CanBoot port of the CanDo tracing JIT.  This is the aarch64
 * counterpart of vendor/cando/source/jit/codegen.c (the x86_64
 * emitter); it implements the single seam
 *
 *     bool cando_jit_codegen_trace(CandoVM *vm, CandoTrace *t);
 *
 * compiling a recorded loop trace into A64 machine code that honours
 * the exact same contract as the IR-interpreter (cando_trace_run):
 * run one loop iteration, return TRACE_LOOP_DONE (0) on a clean close
 * at IR_LOOP, or TRACE_GUARD_FAILED (1) after a guard miss (having
 * first replayed the failing guard's snapshot so the bytecode
 * interpreter can resume with coherent VM state).  The dispatch loop
 * in vm.c (OP_LOOP) drives the per-iteration model: it calls once,
 * and on LOOP_DONE re-enters in a tight drain loop until a guard
 * fires.  See jit.h and codegen.c for the contract.
 *
 * Design choice -- mirror the IR-interpreter's memory model rather
 * than the x86 emitter's register allocator.  Every IR result lives
 * in vals[i] (the 8-byte TraceVal scratch the caller hands us) and is
 * reloaded from there on use; frame slots are read/written through
 * frame_slots[].  No register pinning, no LICM, no sunk allocations.
 * This is the simplest *correct* native trace: it removes the
 * per-opcode interpreter dispatch + the per-op C-call overhead while
 * keeping the storage identical to the interpreter, so a side-exit's
 * snapshot replay (which reads vals[]/frame_slots[]) stays valid.  It
 * is slower than the heavily-optimised x86 backend but still a clear
 * win over the interpreter, and correctness ranks above speed.
 *
 * Op coverage (natively compiled):
 *   IR_NOP, IR_LOOP                          prologue/epilogue/close
 *   IR_KNUM, IR_KBOOL                        constants
 *   IR_SLOAD (IRT_NUM), IR_SSTORE            frame slot load/store
 *   IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD   f64 arithmetic (MOD->fmod)
 *   IR_NEG                                   f64 negate
 *   IR_EQ, IR_NEQ, IR_LT, IR_LE, IR_GT, IR_GE  f64 compare -> 1.0/0.0
 *   IR_GLOAD (IRT_NUM), IR_GSTORE            numeric globals (helper)
 *   IR_CALL_F1                               fast-native f64(f64) call
 *   IR_GUARD_NUM                             runtime no-op
 *   IR_GUARD_TRUE, IR_GUARD_FALSE            guard + snapshot side-exit
 * Any other op (IRT_OBJ slots/globals, arrays/objects, ranges, handle
 * loads, function traces) bails: cando_jit_codegen_trace returns false
 * and the trace runs on the IR-interpreter unchanged -- correct, slow.
 *
 * AArch64 specifics handled here:
 *   - I-cache coherency: after writing code we clean D-cache to PoU
 *     (dc cvau), dsb ish, invalidate I-cache to PoU (ic ivau), dsb
 *     ish, isb -- mandatory before executing freshly-written code.
 *   - AAPCS64 ABI: args x0..x4, callee-saved x19..x23 hold the live
 *     pointers (preserved across the side-exit helper call for free),
 *     16-byte aligned SP, x30/LR saved.
 *   - 64-bit constants via movz/movk sequences.
 *   - Forward branches backpatched; A64 b.cond (imm19, +-1MB) and b
 *     (imm26, +-128MB) both span the whole code buffer, so no veneers.
 *   - f64 throughout: fadd/fsub/fmul/fdiv/fneg, fcmp + cset + scvtf.
 */

#include "codegen.h"
#include "../vm/vm.h"
#include "../core/value.h"

#include <string.h>
#include <stddef.h>
#include <stdint.h>

/* Side-exit snapshot replay helper (jit.c). */
void cando_jit_replay_snapshot_for_mcode(struct CandoVM *vm,
                                          CandoTrace *t,
                                          TraceVal *vals,
                                          CandoValue *frame_slots,
                                          u32 snap_idx);

/* Numeric global load/store helpers (jit.c): return 0 on success
 * (gload writes *out), nonzero on missing / non-numeric / const-
 * protected so the caller side-exits to the interpreter. */
extern int cando_jit_gload_for_mcode(struct CandoVM *vm,
                                      struct CandoString *name, double *out);
extern int cando_jit_gstore_for_mcode(struct CandoVM *vm,
                                       struct CandoString *name, double value);

extern double fmod(double, double);
extern int printf(const char *, ...);

/* NaN-box bit patterns (mirrors core/value.h). */
#define NB_MASK            0xFFF8000000000000ULL
#define NB_CANON_NAN       0x7FF8000000000000ULL
#define D_ONE_BITS         0x3FF0000000000000ULL

/* Persistent register assignments inside the emitted body (AAPCS64
 * callee-saved x19..x23 so they survive the side-exit helper call). */
#define R_VM    19u
#define R_T     20u
#define R_FS    21u   /* frame_slots */
#define R_VALS  22u
#define R_SKIP  23u   /* skip_invariant (saved; unused -- see header) */
#define SP_REG  31u
#define XZR     31u
#define LR_REG  30u
#define FP_REG  29u

/* A64 condition codes. */
#define CC_EQ 0u
#define CC_NE 1u
#define CC_MI 4u
#define CC_VC 7u
#define CC_LS 9u
#define CC_GE 10u
#define CC_GT 12u

#define CG_MAX_GUARDS  1024u

typedef struct {
    u8  *base;
    u8  *cur;
    u8  *end;
    bool failed;
    struct CandoVM *vm;
    u16  cur_snap;          /* most recent guard's snapshot index */

    /* Forward branches to the loop-done epilogue (IR_LOOP). */
    u32  loopdone_off[CG_MAX_GUARDS];
    u32  loopdone_count;

    /* Per-guard side-exit branches: each records the offset of the
     * b.cond placeholder and the snapshot index to replay. */
    struct {
        u32 br_off;
        u16 snap_idx;
    } guards[CG_MAX_GUARDS];
    u32  guard_count;
} CG;

/* ============================================================ */
/* Byte / instruction emission                                  */
/* ============================================================ */

static void cg_w32(CG *cg, u32 insn) {
    if (cg->failed) return;
    if (cg->cur + 4 > cg->end) { cg->failed = true; return; }
    cg->cur[0] = (u8)insn;
    cg->cur[1] = (u8)(insn >> 8);
    cg->cur[2] = (u8)(insn >> 16);
    cg->cur[3] = (u8)(insn >> 24);
    cg->cur += 4;
}

static u32 cg_off(const CG *cg) { return (u32)(cg->cur - cg->base); }

static void cg_patch32(CG *cg, u32 off, u32 insn) {
    if (off + 4 > (u32)(cg->end - cg->base)) return;
    u8 *p = cg->base + off;
    p[0] = (u8)insn;        p[1] = (u8)(insn >> 8);
    p[2] = (u8)(insn >> 16); p[3] = (u8)(insn >> 24);
}

/* ldr Dt, [Xn, #idx*8]  (64-bit FP, unsigned scaled offset). */
static void emit_ldr_d(CG *cg, u32 dt, u32 xn, u32 idx) {
    if (idx > 4095u) { cg->failed = true; return; }
    cg_w32(cg, 0xFD400000u | (idx << 10) | (xn << 5) | dt);
}
/* str Dt, [Xn, #idx*8]. */
static void emit_str_d(CG *cg, u32 dt, u32 xn, u32 idx) {
    if (idx > 4095u) { cg->failed = true; return; }
    cg_w32(cg, 0xFD000000u | (idx << 10) | (xn << 5) | dt);
}
/* ldr Xt, [Xn, #idx*8]. */
static void emit_ldr_x(CG *cg, u32 xt, u32 xn, u32 idx) {
    if (idx > 4095u) { cg->failed = true; return; }
    cg_w32(cg, 0xF9400000u | (idx << 10) | (xn << 5) | xt);
}
/* str Xt, [Xn, #idx*8]. */
static void emit_str_x(CG *cg, u32 xt, u32 xn, u32 idx) {
    if (idx > 4095u) { cg->failed = true; return; }
    cg_w32(cg, 0xF9000000u | (idx << 10) | (xn << 5) | xt);
}
/* stp Xt1, Xt2, [Xn, #imm]  (signed offset, imm multiple of 8). */
static void emit_stp(CG *cg, u32 t1, u32 t2, u32 xn, i32 imm) {
    u32 imm7 = (u32)((imm / 8) & 0x7F);
    cg_w32(cg, 0xA9000000u | (imm7 << 15) | (t2 << 10) | (xn << 5) | t1);
}
static void emit_ldp(CG *cg, u32 t1, u32 t2, u32 xn, i32 imm) {
    u32 imm7 = (u32)((imm / 8) & 0x7F);
    cg_w32(cg, 0xA9400000u | (imm7 << 15) | (t2 << 10) | (xn << 5) | t1);
}
/* sub sp, sp, #imm12 / add sp, sp, #imm12. */
static void emit_sub_sp(CG *cg, u32 imm) {
    cg_w32(cg, 0xD1000000u | ((imm & 0xFFF) << 10) | (SP_REG << 5) | SP_REG);
}
static void emit_add_sp(CG *cg, u32 imm) {
    cg_w32(cg, 0x91000000u | ((imm & 0xFFF) << 10) | (SP_REG << 5) | SP_REG);
}
/* mov Xd, sp  (add Xd, sp, #0). */
static void emit_mov_from_sp(CG *cg, u32 xd) {
    cg_w32(cg, 0x91000000u | (SP_REG << 5) | xd);
}
/* mov Xd, Xn  (orr Xd, xzr, Xn). */
static void emit_mov_reg(CG *cg, u32 xd, u32 xn) {
    cg_w32(cg, 0xAA0003E0u | (xn << 16) | xd);
}
/* movz/movk to materialise a 64-bit immediate into Xd. */
static void emit_mov_imm64(CG *cg, u32 xd, u64 v) {
    cg_w32(cg, 0xD2800000u | (((v >> 0)  & 0xFFFF) << 5) | xd);          /* movz lsl 0  */
    if ((v >> 16) & 0xFFFF)
        cg_w32(cg, 0xF2800000u | (1u << 21) | (((v >> 16) & 0xFFFF) << 5) | xd);
    if ((v >> 32) & 0xFFFF)
        cg_w32(cg, 0xF2800000u | (2u << 21) | (((v >> 32) & 0xFFFF) << 5) | xd);
    if ((v >> 48) & 0xFFFF)
        cg_w32(cg, 0xF2800000u | (3u << 21) | (((v >> 48) & 0xFFFF) << 5) | xd);
}
/* movz Wd, #imm16 (32-bit). */
static void emit_movz_w(CG *cg, u32 wd, u32 imm16) {
    cg_w32(cg, 0x52800000u | ((imm16 & 0xFFFF) << 5) | wd);
}
/* and Xd, Xn, Xm. */
static void emit_and_reg(CG *cg, u32 xd, u32 xn, u32 xm) {
    cg_w32(cg, 0x8A000000u | (xm << 16) | (xn << 5) | xd);
}
/* cmp Xn, Xm  (subs xzr, Xn, Xm). */
static void emit_cmp_reg(CG *cg, u32 xn, u32 xm) {
    cg_w32(cg, 0xEB000000u | (xm << 16) | (xn << 5) | XZR);
}
/* add Xd, Xn, Xm  (64-bit, shifted register, LSL 0). */
static void emit_add_reg(CG *cg, u32 xd, u32 xn, u32 xm) {
    cg_w32(cg, 0x8B000000u | (xm << 16) | (xn << 5) | xd);
}
/* cmp Wn, #0  (subs wzr, Wn, #0). */
static void emit_cmp_w_zero(CG *cg, u32 wn) {
    cg_w32(cg, 0x7100001Fu | (wn << 5));
}
/* fadd/fsub/fmul/fdiv Dd, Dn, Dm. */
static void emit_fadd(CG *cg, u32 dd, u32 dn, u32 dm) {
    cg_w32(cg, 0x1E602800u | (dm << 16) | (dn << 5) | dd);
}
static void emit_fsub(CG *cg, u32 dd, u32 dn, u32 dm) {
    cg_w32(cg, 0x1E603800u | (dm << 16) | (dn << 5) | dd);
}
static void emit_fmul(CG *cg, u32 dd, u32 dn, u32 dm) {
    cg_w32(cg, 0x1E600800u | (dm << 16) | (dn << 5) | dd);
}
static void emit_fdiv(CG *cg, u32 dd, u32 dn, u32 dm) {
    cg_w32(cg, 0x1E601800u | (dm << 16) | (dn << 5) | dd);
}
/* fneg Dd, Dn. */
static void emit_fneg(CG *cg, u32 dd, u32 dn) {
    cg_w32(cg, 0x1E614000u | (dn << 5) | dd);
}
/* fcmp Dn, Dm. */
static void emit_fcmp(CG *cg, u32 dn, u32 dm) {
    cg_w32(cg, 0x1E602000u | (dm << 16) | (dn << 5));
}
/* fcmp Dn, #0.0. */
static void emit_fcmp_zero(CG *cg, u32 dn) {
    cg_w32(cg, 0x1E602008u | (dn << 5));
}
/* cset Wd, cond. */
static void emit_cset(CG *cg, u32 wd, u32 cond) {
    cg_w32(cg, 0x1A9F07E0u | (((cond ^ 1u) & 0xF) << 12) | wd);
}
/* scvtf Dd, Wn  (signed 32-bit int -> double). */
static void emit_scvtf_d_w(CG *cg, u32 dd, u32 wn) {
    cg_w32(cg, 0x1E620000u | (wn << 5) | dd);
}
/* b <target>  (imm26).  Emits a placeholder if target==0xFFFFFFFF. */
static u32 emit_b_placeholder(CG *cg) {
    u32 off = cg_off(cg);
    cg_w32(cg, 0x14000000u);
    return off;
}
static void patch_b(CG *cg, u32 br_off, u32 target_off) {
    i32 rel = ((i32)target_off - (i32)br_off) / 4;
    cg_patch32(cg, br_off, 0x14000000u | ((u32)rel & 0x03FFFFFFu));
}
/* b.cond placeholder; patched later via patch_bcond. */
static u32 emit_bcond_placeholder(CG *cg, u32 cond) {
    u32 off = cg_off(cg);
    cg_w32(cg, 0x54000000u | (cond & 0xF));
    return off;
}
static void patch_bcond(CG *cg, u32 br_off, u32 target_off) {
    i32 rel = ((i32)target_off - (i32)br_off) / 4;
    u32 cond = cg->base[br_off] & 0xF;   /* low nibble holds cond */
    cg_patch32(cg, br_off,
               0x54000000u | (((u32)rel & 0x7FFFFu) << 5) | cond);
}
/* ret. */
static void emit_ret(CG *cg) { cg_w32(cg, 0xD65F03C0u); }
/* blr Xn. */
static void emit_blr(CG *cg, u32 xn) { cg_w32(cg, 0xD63F0000u | (xn << 5)); }

/* ============================================================ */
/* Guards                                                        */
/* ============================================================ */

/* Record a conditional branch to the (later-emitted) side-exit stub
 * for the given snapshot. */
static void cg_emit_guard(CG *cg, u32 cond, u16 snap_idx) {
    if (cg->guard_count >= CG_MAX_GUARDS) { cg->failed = true; return; }
    u32 br = emit_bcond_placeholder(cg, cond);
    cg->guards[cg->guard_count].br_off   = br;
    cg->guards[cg->guard_count].snap_idx = snap_idx;
    cg->guard_count++;
}

/* ============================================================ */
/* Prologue / epilogue                                           */
/* ============================================================ */

/* Frame: 64 bytes (16-aligned).  Saves x29/x30 + x19..x23.
 *   [sp+0]  x29, x30
 *   [sp+16] x19, x20
 *   [sp+32] x21, x22
 *   [sp+48] x23
 * AAPCS64 entry: x0=vm x1=t w2=skip_invariant x3=frame_slots x4=vals. */
#define FRAME_BYTES 64u

static void emit_prologue(CG *cg) {
    emit_sub_sp(cg, FRAME_BYTES);
    emit_stp(cg, FP_REG, LR_REG, SP_REG, 0);
    emit_stp(cg, R_VM, R_T, SP_REG, 16);
    emit_stp(cg, R_FS, R_VALS, SP_REG, 32);
    emit_str_x(cg, R_SKIP, SP_REG, 48 / 8);
    emit_mov_from_sp(cg, FP_REG);
    /* Capture incoming args into callee-saved holders. */
    emit_mov_reg(cg, R_VM, 0);
    emit_mov_reg(cg, R_T, 1);
    emit_mov_reg(cg, R_SKIP, 2);
    emit_mov_reg(cg, R_FS, 3);
    emit_mov_reg(cg, R_VALS, 4);
}

/* Restore + ret.  w0 must already hold the CandoTraceStatus. */
static void emit_epilogue(CG *cg) {
    emit_ldp(cg, FP_REG, LR_REG, SP_REG, 0);
    emit_ldp(cg, R_VM, R_T, SP_REG, 16);
    emit_ldp(cg, R_FS, R_VALS, SP_REG, 32);
    emit_ldr_x(cg, R_SKIP, SP_REG, 48 / 8);
    emit_add_sp(cg, FRAME_BYTES);
    emit_ret(cg);
}

/* ============================================================ */
/* Per-IR-op emitters                                            */
/* ============================================================ */

/* IR_KNUM: vals[i] = constant double bits. */
static void emit_knum(CG *cg, const CandoTraceIR *ir, IRRef k_ref, u32 i) {
    if (!IRREF_IS_K(k_ref)) { cg->failed = true; return; }
    CandoValue cv = cando_ir_get_const(ir, k_ref);
    if (!cando_is_number(cv)) { cg->failed = true; return; }
    emit_mov_imm64(cg, 9, cv.u);
    emit_str_x(cg, 9, R_VALS, i);
}

/* IR_KBOOL: vals[i] = 1.0 / 0.0. */
static void emit_kbool(CG *cg, u32 imm, u32 i) {
    emit_mov_imm64(cg, 9, imm ? D_ONE_BITS : 0ULL);
    emit_str_x(cg, 9, R_VALS, i);
}

/* IR_SLOAD (IRT_NUM): load frame_slots[slot], type-check numeric,
 * mirror raw bits into vals[i].  Side-exit (replay cur_snap) on a
 * non-numeric value, matching the IR-interp.  Check before store so
 * a bad type leaves vals[i] untouched (same as the interpreter). */
static void emit_sload_num(CG *cg, u32 slot, u32 i) {
    emit_ldr_x(cg, 9, R_FS, slot);              /* x9 = raw bits      */
    emit_mov_imm64(cg, 10, NB_MASK);            /* x10 = NB_MASK      */
    emit_and_reg(cg, 11, 9, 10);                /* x11 = bits & MASK  */
    emit_cmp_reg(cg, 11, 10);                   /* == MASK => !number */
    cg_emit_guard(cg, CC_EQ, cg->cur_snap);     /* b.eq -> side-exit  */
    emit_str_x(cg, 9, R_VALS, i);               /* vals[i] = raw bits */
}

/* IR_SSTORE: frame_slots[slot] = cando_number(vals[op2].d).  All
 * accepted traces produce numeric sources, so we always take the
 * numeric (NaN-canonicalising) path that the interpreter uses. */
static void emit_sstore(CG *cg, u32 slot, u32 op2) {
    emit_ldr_d(cg, 0, R_VALS, op2);             /* d0 = value         */
    /* cando_number: any NaN -> canonical positive qNaN (so a negative
     * NaN can't alias the boxed-null/tag space).  fcmp d0,d0 is
     * unordered iff d0 is NaN; b.vc skips to the raw store. */
    emit_fcmp(cg, 0, 0);
    u32 br = emit_bcond_placeholder(cg, CC_VC); /* ordered -> .normal */
    emit_mov_imm64(cg, 9, NB_CANON_NAN);
    emit_str_x(cg, 9, R_FS, slot);
    u32 br2 = emit_b_placeholder(cg);           /* -> .done           */
    u32 normal_off = cg_off(cg);
    patch_bcond(cg, br, normal_off);
    emit_str_d(cg, 0, R_FS, slot);              /* .normal: raw store */
    u32 done_off = cg_off(cg);
    patch_b(cg, br2, done_off);                 /* .done:             */
}

/* IR_ADD/SUB/MUL/DIV: vals[i] = vals[a] OP vals[b]. */
static void emit_arith(CG *cg, IROp op, u32 a, u32 b, u32 i) {
    emit_ldr_d(cg, 0, R_VALS, a);
    emit_ldr_d(cg, 1, R_VALS, b);
    switch (op) {
        case IR_ADD: emit_fadd(cg, 0, 0, 1); break;
        case IR_SUB: emit_fsub(cg, 0, 0, 1); break;
        case IR_MUL: emit_fmul(cg, 0, 0, 1); break;
        case IR_DIV: emit_fdiv(cg, 0, 0, 1); break;
        default:     cg->failed = true; return;
    }
    emit_str_d(cg, 0, R_VALS, i);
}

/* IR_NEG: vals[i] = -vals[op1]. */
static void emit_neg(CG *cg, u32 op1, u32 i) {
    emit_ldr_d(cg, 0, R_VALS, op1);
    emit_fneg(cg, 0, 0);
    emit_str_d(cg, 0, R_VALS, i);
}

/* IR_EQ/NEQ/LT/LE/GT/GE: vals[i] = (cmp) ? 1.0 : 0.0.  Condition
 * codes follow the IEEE FP semantics (NaN -> false except NEQ). */
static void emit_compare(CG *cg, IROp op, u32 a, u32 b, u32 i) {
    u32 cond;
    switch (op) {
        case IR_EQ:  cond = CC_EQ; break;
        case IR_NEQ: cond = CC_NE; break;
        case IR_LT:  cond = CC_MI; break;
        case IR_LE:  cond = CC_LS; break;
        case IR_GT:  cond = CC_GT; break;
        case IR_GE:  cond = CC_GE; break;
        default:     cg->failed = true; return;
    }
    emit_ldr_d(cg, 0, R_VALS, a);
    emit_ldr_d(cg, 1, R_VALS, b);
    emit_fcmp(cg, 0, 1);
    emit_cset(cg, 9, cond);
    emit_scvtf_d_w(cg, 0, 9);
    emit_str_d(cg, 0, R_VALS, i);
}

/* IR_MOD: vals[i] = fmod(vals[a], vals[b]).  Mirrors the interpreter,
 * which calls fmod for OP_MOD semantics. */
static void emit_mod(CG *cg, u32 a, u32 b, u32 i) {
    emit_ldr_d(cg, 0, R_VALS, a);
    emit_ldr_d(cg, 1, R_VALS, b);
    emit_mov_imm64(cg, 9, (u64)(uintptr_t)&fmod);
    emit_blr(cg, 9);
    emit_str_d(cg, 0, R_VALS, i);
}

/* IR_GLOAD (IRT_NUM): vals[i] = numeric global by name.  Calls the
 * jit.c helper with &vals[i] as the out-pointer; a nonzero return
 * (missing / non-numeric) side-exits via cur_snap, matching the
 * IR-interp.  `name` is the trace's interned CandoString*, embedded as
 * an immediate. */
static void emit_gload(CG *cg, const CandoTraceIR *ir, IRRef name_ref, u32 i) {
    if (!IRREF_IS_K(name_ref)) { cg->failed = true; return; }
    CandoValue cv = cando_ir_get_const(ir, name_ref);
    if (!cando_is_string(cv)) { cg->failed = true; return; }
    struct CandoString *name = (struct CandoString *)cando_as_string(cv);
    emit_mov_reg(cg, 0, R_VM);                       /* x0 = vm        */
    emit_mov_imm64(cg, 1, (u64)(uintptr_t)name);     /* x1 = name      */
    emit_mov_imm64(cg, 9, (u64)i * 8u);
    emit_add_reg(cg, 2, R_VALS, 9);                  /* x2 = &vals[i]  */
    emit_mov_imm64(cg, 9, (u64)(uintptr_t)&cando_jit_gload_for_mcode);
    emit_blr(cg, 9);
    emit_cmp_w_zero(cg, 0);                           /* w0 != 0 -> bail */
    cg_emit_guard(cg, CC_NE, cg->cur_snap);
}

/* IR_GSTORE (IRT_NUM): numeric global = vals[op2].  Nonzero return
 * (const-protected) side-exits via cur_snap. */
static void emit_gstore(CG *cg, const CandoTraceIR *ir, IRRef name_ref, u32 op2) {
    if (!IRREF_IS_K(name_ref)) { cg->failed = true; return; }
    CandoValue cv = cando_ir_get_const(ir, name_ref);
    if (!cando_is_string(cv)) { cg->failed = true; return; }
    struct CandoString *name = (struct CandoString *)cando_as_string(cv);
    emit_mov_reg(cg, 0, R_VM);                       /* x0 = vm        */
    emit_mov_imm64(cg, 1, (u64)(uintptr_t)name);     /* x1 = name      */
    emit_ldr_d(cg, 0, R_VALS, op2);                  /* d0 = value     */
    emit_mov_imm64(cg, 9, (u64)(uintptr_t)&cando_jit_gstore_for_mcode);
    emit_blr(cg, 9);
    emit_cmp_w_zero(cg, 0);
    cg_emit_guard(cg, CC_NE, cg->cur_snap);
}

/* IR_CALL_F1: vals[i] = fast_native(vals[op2]).  op1 is the index into
 * vm->fast_natives_f1[]; resolve the f64(*)(f64) pointer at codegen
 * time (registrations are write-once at startup) and embed it.  AAPCS64
 * passes the single f64 arg in d0 and returns it in d0. */
static void emit_call_f1(CG *cg, u32 native_idx, u32 op2, u32 i) {
    if (!cg->vm || native_idx >= cg->vm->fast_natives_f1_cap ||
        cg->vm->fast_natives_f1[native_idx] == NULL) {
        cg->failed = true; return;
    }
    CandoFastFn1 fn = cg->vm->fast_natives_f1[native_idx];
    emit_ldr_d(cg, 0, R_VALS, op2);                  /* d0 = arg       */
    emit_mov_imm64(cg, 9, (u64)(uintptr_t)fn);
    emit_blr(cg, 9);
    emit_str_d(cg, 0, R_VALS, i);                    /* vals[i] = d0   */
}

/* IR_GUARD_TRUE / IR_GUARD_FALSE: vals[op1] is a 0.0/1.0 bool lane.
 *   GUARD_TRUE  fails when vals[op1] == 0.0  -> b.eq (NaN: not equal,
 *                                                     guard passes)
 *   GUARD_FALSE fails when vals[op1] != 0.0  -> b.ne (NaN: unordered
 *                                                     != 0, guard fires)
 * Matches the IR-interp's `== 0.0` / `!= 0.0` exactly. */
static void emit_guard_bool(CG *cg, IROp op, u32 op1, u16 snap_idx) {
    emit_ldr_d(cg, 0, R_VALS, op1);
    emit_fcmp_zero(cg, 0);
    cg_emit_guard(cg, (op == IR_GUARD_TRUE) ? CC_EQ : CC_NE, snap_idx);
}

/* ============================================================ */
/* AArch64 instruction-cache coherency                           */
/* ============================================================ */

/* Clean D-cache to PoU + invalidate I-cache to PoU over the emitted
 * range, then dsb/isb.  Mandatory before executing freshly-written
 * A64 code -- without it the CPU may fetch stale / partially-written
 * instructions from the I-cache.  Cache line sizes come from CTR_EL0
 * (DminLine / IminLine: log2 number of 4-byte words per line). */
static void aarch64_sync_icache(const void *start, u32 len) {
    if (len == 0) return;
    uintptr_t base = (uintptr_t)start;
    uintptr_t end  = base + len;
    u64 ctr;
    __asm__ volatile ("mrs %0, ctr_el0" : "=r"(ctr));
    uintptr_t dline = (uintptr_t)4u << ((ctr >> 16) & 0xF);
    uintptr_t iline = (uintptr_t)4u << (ctr & 0xF);

    for (uintptr_t p = base & ~(dline - 1); p < end; p += dline)
        __asm__ volatile ("dc cvau, %0" :: "r"(p) : "memory");
    __asm__ volatile ("dsb ish" ::: "memory");

    for (uintptr_t p = base & ~(iline - 1); p < end; p += iline)
        __asm__ volatile ("ic ivau, %0" :: "r"(p) : "memory");
    __asm__ volatile ("dsb ish" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");
}

/* ============================================================ */
/* AArch64 executable-memory bring-up                            */
/* ============================================================ */

/* On the UEFI/AAVMF boot path the kernel runs at EL1 on the
 * firmware's page tables.  EDK2 applies image + DXE memory protection:
 * the loaded image's data sections (where the static JIT arena lives)
 * are mapped writable + execute-never, so jumping into freshly-written
 * trace code takes an instruction abort (ESR EC=0x21, IFSC permission
 * fault).  The direct-kernel path runs MMU-off (flat RWX) and never
 * reaches this code (it doesn't build the VM).
 *
 * We make the trace's pages executable by walking the active stage-1
 * translation tables (TTBR0_EL1, 4KB granule -- the QEMU virt config)
 * and clearing the PXN/UXN execute-never bits on the descriptor that
 * maps each page.  We keep the pages writable (RWX); AAVMF does not
 * set SCTLR_EL1.WXN, so clearing PXN grants EL1 execute permission.
 * Each trace buffer is page-aligned and page-sized (the mcode mmap
 * shim rounds every allocation up to a page), so this never disturbs
 * neighbouring allocations' attributes beyond the block granularity. */

static u64 cg_rd_sysreg_tcr(void) {
    u64 v; __asm__ volatile ("mrs %0, tcr_el1" : "=r"(v)); return v;
}
static u64 cg_rd_sysreg_ttbr0(void) {
    u64 v; __asm__ volatile ("mrs %0, ttbr0_el1" : "=r"(v)); return v;
}
static u64 cg_rd_sysreg_sctlr(void) {
    u64 v; __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(v)); return v;
}

#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL
#define PTE_PXN       (1ULL << 53)
#define PTE_UXN       (1ULL << 54)

/* Walk the stage-1 tables for `va` and return a pointer to the leaf
 * descriptor (page or block) that maps it, or NULL on a hole / an
 * unsupported (non-4KB) granule. */
static u64 *cg_walk_leaf(u64 va) {
    u64 tcr = cg_rd_sysreg_tcr();
    u32 t0sz = (u32)(tcr & 0x3F);
    u32 tg0  = (u32)((tcr >> 14) & 0x3);   /* 00 = 4KB */
    if (tg0 != 0) return NULL;
    int resolved = 64 - (int)t0sz - 12;    /* VA bits above page offset */
    if (resolved <= 0) return NULL;
    int num_levels = (resolved + 8) / 9;   /* ceil(resolved / 9) */
    if (num_levels < 1) num_levels = 1;
    if (num_levels > 4) num_levels = 4;
    int level = 4 - num_levels;
    u64 table = cg_rd_sysreg_ttbr0() & PTE_ADDR_MASK;
    for (;;) {
        int shift = 12 + 9 * (3 - level);
        u32 idx = (u32)((va >> shift) & 0x1FF);
        u64 *desc = (u64 *)(uintptr_t)(table + (u64)idx * 8);
        u64 d = *desc;
        if ((d & 1ULL) == 0) return NULL;          /* invalid */
        if (level == 3) return desc;               /* page descriptor */
        if ((d & 3ULL) == 1ULL) return desc;       /* block descriptor */
        table = d & PTE_ADDR_MASK;                 /* table -> descend */
        if (++level > 3) return NULL;
    }
}

static void cg_make_rwx(const void *addr, u32 len) {
    if (len == 0) return;
    uintptr_t base = (uintptr_t)addr & ~(uintptr_t)0xFFF;
    uintptr_t end  = ((uintptr_t)addr + len + 0xFFF) & ~(uintptr_t)0xFFF;

    static int diag_done = 0;
    if (!diag_done) {
        diag_done = 1;
        u64 *d = cg_walk_leaf((u64)(uintptr_t)addr);
        printf("canboot: jit mmu t0sz=%lu sctlr_wxn=%lu desc=0x%lx\n",
               (unsigned long)(cg_rd_sysreg_tcr() & 0x3F),
               (unsigned long)((cg_rd_sysreg_sctlr() >> 19) & 1),
               (unsigned long)(d ? *d : 0));
    }

    for (uintptr_t p = base; p < end; p += 0x1000) {
        u64 *desc = cg_walk_leaf((u64)p);
        if (!desc) continue;
        u64 d  = *desc;
        u64 nd = d & ~(PTE_PXN | PTE_UXN);
        if (nd != d) {
            *desc = nd;
            __asm__ volatile ("dsb ishst" ::: "memory");
            __asm__ volatile ("tlbi vaae1is, %0" :: "r"(p >> 12) : "memory");
        }
    }
    __asm__ volatile ("dsb ish" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");
}

/* ============================================================ */
/* Loop-trace codegen                                            */
/* ============================================================ */

static bool codegen_loop_trace(struct CandoVM *vm, CandoTrace *t) {
    if (t->ir.ir_count == 0) return false;

    /* Size the buffer to the trace.  ~16 instructions/op is generous
     * for the op set above; round up so mcode_alloc's page rounding
     * gives a clean fit.  Bail (-> interpreter) if alloc fails. */
    u32 est = 512u + t->ir.ir_count * 96u;
    if (!cando_mcode_alloc(&t->mcode, est)) return false;

    CG cg = (CG){0};
    cg.base = t->mcode.base;
    cg.cur  = t->mcode.base;
    cg.end  = t->mcode.base + t->mcode.size;
    cg.vm   = vm;

    emit_prologue(&cg);

    for (u32 i = 1; i < t->ir.ir_count && !cg.failed; i++) {
        const IRIns *in = &t->ir.ir[i];
        switch (in->op) {
        case IR_NOP:
            break;
        case IR_KNUM:
            emit_knum(&cg, &t->ir, in->op1, i);
            break;
        case IR_KBOOL:
            emit_kbool(&cg, in->op1, i);
            break;
        case IR_SLOAD:
            if (in->type != IRT_NUM) { cg.failed = true; break; }
            emit_sload_num(&cg, in->op1, i);
            break;
        case IR_SSTORE:
            /* Sunk stores are not supported (we never sink allocs). */
            if (in->flags & IRF_SUNK) { cg.failed = true; break; }
            emit_sstore(&cg, in->op1, in->op2);
            break;
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
            emit_arith(&cg, (IROp)in->op, in->op1, in->op2, i);
            break;
        case IR_MOD:
            emit_mod(&cg, in->op1, in->op2, i);
            break;
        case IR_NEG:
            emit_neg(&cg, in->op1, i);
            break;
        case IR_CALL_F1:
            emit_call_f1(&cg, in->op1, in->op2, i);
            break;
        case IR_GLOAD:
            if (in->type != IRT_NUM) { cg.failed = true; break; }
            emit_gload(&cg, &t->ir, in->op1, i);
            break;
        case IR_GSTORE:
            emit_gstore(&cg, &t->ir, in->op1, in->op2);
            break;
        case IR_EQ: case IR_NEQ: case IR_LT: case IR_LE:
        case IR_GT: case IR_GE:
            emit_compare(&cg, (IROp)in->op, in->op1, in->op2, i);
            break;
        case IR_GUARD_NUM:
            /* SLOAD already type-checked; pure ops keep the type. */
            break;
        case IR_GUARD_TRUE:
        case IR_GUARD_FALSE:
            emit_guard_bool(&cg, (IROp)in->op, in->op1, (u16)in->op2);
            cg.cur_snap = (u16)in->op2;
            break;
        case IR_LOOP:
            /* Clean iteration close -> return TRACE_LOOP_DONE.  Branch
             * to the shared loop-done epilogue emitted after the body. */
            if (cg.loopdone_count >= CG_MAX_GUARDS) { cg.failed = true; break; }
            cg.loopdone_off[cg.loopdone_count++] = emit_b_placeholder(&cg);
            break;
        default:
            cg.failed = true;
            break;
        }
    }

    if (cg.failed) { cando_mcode_free(&t->mcode); return false; }

    /* Loop-done epilogue: w0 = TRACE_LOOP_DONE (0). */
    u32 loopdone_target = cg_off(&cg);
    emit_movz_w(&cg, 0, 0);
    emit_epilogue(&cg);

    /* Per-guard stubs: load snap_idx into w9, branch to common. */
    u32 stub_off[CG_MAX_GUARDS];
    u32 stub_b[CG_MAX_GUARDS];
    for (u32 g = 0; g < cg.guard_count && !cg.failed; g++) {
        stub_off[g] = cg_off(&cg);
        emit_movz_w(&cg, 9, cg.guards[g].snap_idx);
        stub_b[g] = emit_b_placeholder(&cg);
    }

    /* Common side-exit: replay snapshot then return TRACE_GUARD_FAILED. */
    u32 common_off = cg_off(&cg);
    emit_mov_reg(&cg, 0, R_VM);          /* x0 = vm          */
    emit_mov_reg(&cg, 1, R_T);           /* x1 = t           */
    emit_mov_reg(&cg, 2, R_VALS);        /* x2 = vals        */
    emit_mov_reg(&cg, 3, R_FS);          /* x3 = frame_slots */
    emit_mov_reg(&cg, 4, 9);             /* w4 = snap_idx    */
    emit_mov_imm64(&cg, 9,
                   (u64)(uintptr_t)&cando_jit_replay_snapshot_for_mcode);
    emit_blr(&cg, 9);
    emit_movz_w(&cg, 0, 1);              /* TRACE_GUARD_FAILED */
    emit_epilogue(&cg);

    if (cg.failed) { cando_mcode_free(&t->mcode); return false; }

    /* Backpatch all forward branches. */
    for (u32 b = 0; b < cg.loopdone_count; b++)
        patch_b(&cg, cg.loopdone_off[b], loopdone_target);
    for (u32 g = 0; g < cg.guard_count; g++) {
        patch_bcond(&cg, cg.guards[g].br_off, stub_off[g]);
        patch_b(&cg, stub_b[g], common_off);
    }

    if (cg.failed) { cando_mcode_free(&t->mcode); return false; }

    t->mcode.written = (u32)(cg.cur - cg.base);
    if (!cando_mcode_finalize(&t->mcode)) {
        cando_mcode_free(&t->mcode);
        return false;
    }
    /* AArch64: (1) make the page executable -- AAVMF maps the JIT
     * arena execute-never; (2) make the freshly-written instructions
     * visible to the instruction stream before we jump into them. */
    cg_make_rwx(t->mcode.base, t->mcode.written);
    aarch64_sync_icache(t->mcode.base, t->mcode.written);

    t->mcode_fn = (CandoTraceStatus (*)(struct CandoVM *, CandoTrace *,
                                        bool, CandoValue *, TraceVal *))
                  (uintptr_t)cg.base;
    return true;
}

/* ============================================================ */
/* Entry point                                                   */
/* ============================================================ */

bool cando_jit_codegen_trace(struct CandoVM *vm, CandoTrace *t) {
    if (!t) return false;
    /* Function traces are not yet compiled on aarch64; they fall back
     * to the IR-interpreter (correct, slower). */
    if (t->is_function_trace) return false;
    if (t->mcode_fn != NULL) return true;
    return codegen_loop_trace(vm, t);
}
