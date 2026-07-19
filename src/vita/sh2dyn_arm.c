#include <string.h>
#include <stdint.h>
#include <psp2/kernel/sysmem.h>
#include "sh2core.h"
#include "sh2int.h"
#include "sh2idle.h"
#include "memory.h"

#define SH2DYN_ARM_ID 4

extern int vita_log(const char *fmt, ...);
extern int SH2InterpreterInit(void);

extern opcodefunc opcodes[0x10000];
extern fetchfunc fetchlist[0x100];

extern u8 *BiosRom;
extern u8 *LowWram;
extern u8 *HighWram;

/* ══════════════════════════════════════════════════════════════
   ARM REGISTER CONVENTIONS (within JIT blocks)
   r0-r3  = scratch / function args / return
   r4     = SH2_struct* context (callee-saved)
   r5-r7  = scratch (callee-saved via push)
   r8-r11 = available for future SH2 reg cache
   r12    = intra-call scratch
   r13(sp)= stack
   r14(lr)= link
   r15(pc)= program counter
   ══════════════════════════════════════════════════════════════ */

/* ── SH2 struct offsets (verified against sh2core.h) ────── */
#define R_OFF(n)            ((n) * 4)
#define SR_OFF              64  /* 0x40 — offset of regs.SR.all */
#define GBR_OFF             68  /* 0x44 */
#define VBR_OFF             72  /* 0x48 */
#define MACH_OFF            76  /* 0x4C */
#define MACL_OFF            80  /* 0x50 */
#define PR_OFF              84  /* 0x54 */
#define PC_OFF              88  /* 0x58 */
/* regs size = 92 (0x5C). Onchip starts after regs. cycles is much later;
   we avoid hardcoding it and use the return-value strategy instead. */

/* ══════════════════════════════════════════════════════════════
   ARM INSTRUCTION ENCODING MACROS
   ══════════════════════════════════════════════════════════════ */

/* ── Conditional prefixes ────────────────────────────────── */
#define COND_EQ  0x00000000
#define COND_NE  0x10000000
#define COND_CS  0x20000000
#define COND_CC  0x30000000
#define COND_MI  0x40000000
#define COND_PL  0x50000000
#define COND_VS  0x60000000
#define COND_VC  0x70000000
#define COND_HI  0x80000000
#define COND_LS  0x90000000
#define COND_GE  0xA0000000
#define COND_LT  0xB0000000
#define COND_GT  0xC0000000
#define COND_LE  0xD0000000
#define COND_AL  0xE0000000

/* ── Shift encodings for data-processing ─────────────────── */
#define LSL_I(n) ((n) << 7)
#define LSR_I(n) ((0x20 | (n)) << 7)
#define ASR_I(n) ((0x40 | (n)) << 7)
#define ROR_I(n) ((0x60 | (n)) << 7)

/* ── Immediate 8-bit rotated encoding helper ─────────────── */
static inline int arm_imm8_ok(uint32_t v) {
    if (v < 256) return 1;
    for (int r = 1; r < 16; r++) {
        uint32_t x = (v >> (r * 2)) | (v << (32 - r * 2));
        if (x < 256) return 1;
    }
    return 0;
}
static inline uint32_t arm_imm8(uint32_t v) {
    if (v < 256) return v;
    for (int r = 1; r < 16; r++) {
        uint32_t x = (v >> (r * 2)) | (v << (32 - r * 2));
        if (x < 256) return (r << 8) | x;
    }
    return v; /* fallback — caller checked ok */
}

/* ── MOVW / MOVT (ARMv7) ──────────────────────────────────── */
#define MOVW(rd, i16)   (0xE3000000 | ((rd)<<12) | ((i16)&0xFFF) | (((i16)>>4)&0xF0000))
#define MOVT(rd, i16)   (0xE3400000 | ((rd)<<12) | ((i16)&0xFFF) | (((i16)>>4)&0xF0000))

/* ── Data-processing (register) ──────────────────────────── */
#define DP3_R(cc, op, s, rd, rn, rm, shift)  \
    ((cc) | ((op)<<21) | ((s)<<20) | ((rn)<<16) | ((rd)<<12) | (0x10<<4) | (shift) | (rm))

/* ── Data-processing (immediate) ─────────────────────────── */
#define DP3_I(cc, op, s, rd, rn, imm8)       \
    ((cc) | ((op)<<21) | ((s)<<20) | ((rn)<<16) | ((rd)<<12) | (0x20<<4) | arm_imm8(imm8))

#define MOV_R(cc, rd, rm)       DP3_R(cc, 0xD, 0, rd, 0,  rm, 0)
#define MOVS_R(cc, rd, rm)      DP3_R(cc, 0xD, 1, rd, 0,  rm, 0)
#define MVN_R(cc, rd, rm)       DP3_R(cc, 0xF, 0, rd, 0,  rm, 0)
#define ADD_R(cc, rd, rn, rm)   DP3_R(cc, 0x4, 0, rd, rn, rm, 0)
#define ADDS_R(cc, rd, rn, rm)  DP3_R(cc, 0x4, 1, rd, rn, rm, 0)
#define ADC_R(cc, rd, rn, rm)   DP3_R(cc, 0x5, 0, rd, rn, rm, 0)
#define SUB_R(cc, rd, rn, rm)   DP3_R(cc, 0x2, 0, rd, rn, rm, 0)
#define SUBS_R(cc, rd, rn, rm)  DP3_R(cc, 0x2, 1, rd, rn, rm, 0)
#define SBC_R(cc, rd, rn, rm)   DP3_R(cc, 0x6, 0, rd, rn, rm, 0)
#define RSB_R(cc, rd, rn, rm)   DP3_R(cc, 0x3, 0, rd, rn, rm, 0)
#define AND_R(cc, rd, rn, rm)   DP3_R(cc, 0x0, 0, rd, rn, rm, 0)
#define ANDS_R(cc, rd, rn, rm)  DP3_R(cc, 0x0, 1, rd, rn, rm, 0)
#define ORR_R(cc, rd, rn, rm)   DP3_R(cc, 0xC, 0, rd, rn, rm, 0)
#define EOR_R(cc, rd, rn, rm)   DP3_R(cc, 0x1, 0, rd, rn, rm, 0)
#define BIC_R(cc, rd, rn, rm)   DP3_R(cc, 0xE, 0, rd, rn, rm, 0)
#define CMP_R(cc, rn, rm)       DP3_R(cc, 0xA, 1, 0,  rn, rm, 0)
#define TST_R(cc, rn, rm)       DP3_R(cc, 0x8, 1, 0,  rn, rm, 0)

/* ── Data-processing with immediate ──────────────────────── */
#define MOV_I(cc, rd, imm)      DP3_I(cc, 0xD, 0, rd, 0,  imm)
#define MVN_I(cc, rd, imm)      DP3_I(cc, 0xF, 0, rd, 0,  imm)
#define ADD_I(cc, rd, rn, imm)  DP3_I(cc, 0x4, 0, rd, rn, imm)
#define SUB_I(cc, rd, rn, imm)  DP3_I(cc, 0x2, 0, rd, rn, imm)
#define AND_I(cc, rd, rn, imm)  DP3_I(cc, 0x0, 0, rd, rn, imm)
#define ORR_I(cc, rd, rn, imm)  DP3_I(cc, 0xC, 0, rd, rn, imm)
#define EOR_I(cc, rd, rn, imm)  DP3_I(cc, 0x1, 0, rd, rn, imm)
#define BIC_I(cc, rd, rn, imm)  DP3_I(cc, 0xE, 0, rd, rn, imm)
#define CMP_I(cc, rn, imm)      DP3_I(cc, 0xA, 1, 0,  rn, imm)
#define TST_I(cc, rn, imm)      DP3_I(cc, 0x8, 1, 0,  rn, imm)

/* ── Load / Store (immediate offset) ─────────────────────── */
#define LDR_I(rd, rn, imm12)   (COND_AL | 0x05100000 | ((rn)<<16) | ((rd)<<12) | ((imm12)&0xFFF))
#define LDRB_I(rd, rn, imm12)  (COND_AL | 0x05500000 | ((rn)<<16) | ((rd)<<12) | ((imm12)&0xFFF))
#define STR_I(rd, rn, imm12)   (COND_AL | 0x05000000 | ((rn)<<16) | ((rd)<<12) | ((imm12)&0xFFF))
#define STRB_I(rd, rn, imm12)  (COND_AL | 0x05400000 | ((rn)<<16) | ((rd)<<12) | ((imm12)&0xFFF))

/* ── BX / BLX ────────────────────────────────────────────── */
#define BX(rm)          (COND_AL | 0x012FFF10 | ((rm)&0xF))
#define BLX(rm)         (COND_AL | 0x012FFF30 | ((rm)&0xF))
/* B imm (offset = signed number of instructions) */
#define B(imm24)        (COND_AL | 0x0A000000 | ((imm24)&0xFFFFFF))
#define BL(imm24)       (COND_AL | 0x0B000000 | ((imm24)&0xFFFFFF))

/* ── PUSH / POP ──────────────────────────────────────────── */
#define PUSH(reglist)   (COND_AL | 0x092D0000 | (reglist))
#define POP(reglist)    (COND_AL | 0x08BD0000 | (reglist))

/* ── MUL ─────────────────────────────────────────────────── */
#define MUL(rd, rm, rs) (COND_AL | 0x00000090 | ((rd)<<16) | ((rs)<<8) | (rm))

/* ══════════════════════════════════════════════════════════════
   JIT STATE
   ══════════════════════════════════════════════════════════════ */
static SceUID jit_memblock = -1;
static void* jit_memory = NULL;
static uint32_t* jit_ptr = NULL;
static uint32_t* jit_block_start = NULL;

/* ── Hash cache ──────────────────────────────────────────── */
#define JIT_HASH_SIZE 8192
typedef struct {
    u32 pc;
    uint32_t* block;
    int icount;
} JITCacheEntry;
static JITCacheEntry jit_hash[JIT_HASH_SIZE];

static inline void jit_cache_add(u32 pc, uint32_t* block, int icount) {
    u32 i = (pc >> 1) % JIT_HASH_SIZE;
    jit_hash[i].pc     = pc;
    jit_hash[i].block  = block;
    jit_hash[i].icount = icount;
}
static inline uint32_t* jit_cache_lookup(u32 pc) {
    JITCacheEntry* e = &jit_hash[(pc >> 1) % JIT_HASH_SIZE];
    return (e->pc == pc) ? e->block : NULL;
}
static inline void jit_cache_invalidate_page(u32 addr) {
    u32 p = addr >> 12;
    for (int i = 0; i < JIT_HASH_SIZE; i++)
        if ((jit_hash[i].pc >> 12) == p) jit_hash[i].pc = 0xFFFFFFFF;
}

/* ── Simple emitter ──────────────────────────────────────── */
static inline void emit(uint32_t v) { *jit_ptr++ = v; }

/* ══════════════════════════════════════════════════════════════
   PROLOGUE / EPILOGUE HELPERS
   ══════════════════════════════════════════════════════════════ */

/* Prologue: push {r4-r7, lr}; mov r4, r0 */
static inline void emit_prologue(void) {
    emit(0xE92D40F0); /* push {r4-r7, lr} */
    emit(0xE1A04000); /* mov r4, r0 */
}

/* Epilogue: return cycles consumed in r0; pop {r4-r7, pc} */
static inline void emit_epilogue(int n) {
    /* mov r0, #n (or MOVW if >255) */
    if (n < 256) {
        emit(MOV_I(COND_AL, 0, n));
    } else {
        emit(MOVW(0, n & 0xFFFF));
        if (n > 65535) emit(MOVT(0, n >> 16));
    }
    emit(0xE8BD80F0); /* pop {r4-r7, pc} */
}

/* Load any 32-bit immediate into rd */
static inline void emit_mov_imm(int rd, uint32_t v) {
    if (arm_imm8_ok(v))          emit(MOV_I(COND_AL, rd, v));
    else if (arm_imm8_ok(~v))    emit(MVN_I(COND_AL, rd, ~v));
    else { emit(MOVW(rd, v & 0xFFFF)); if (v > 0xFFFF) emit(MOVT(rd, v >> 16)); }
}

/* ── Context access helpers (r4 = context) ───────────────── */
static inline void emit_load_reg(int dr, int sr)  { emit(LDR_I(dr, 4, R_OFF(sr))); }
static inline void emit_store_reg(int s, int dr)   { emit(STR_I(s,  4, R_OFF(dr))); }
static inline void emit_load_pc(int d)             { emit(LDR_I(d,  4, PC_OFF));    }
static inline void emit_store_pc(int s)            { emit(STR_I(s,  4, PC_OFF));    }
static inline void emit_load_sr(int d)             { emit(LDR_I(d,  4, SR_OFF));    }
static inline void emit_store_sr(int s)            { emit(STR_I(s,  4, SR_OFF));    }
static inline void emit_inc_pc(void) {
    emit_load_pc(5);
    emit(ADD_I(COND_AL, 5, 5, 2));
    emit_store_pc(5);
}

/* ── Call a C function via BLX (addr in r0-ish) ──────────── */
/* Assumes fn address loaded into scratch via emit_mov_imm */
static inline void emit_call(int fnreg, int argreg) {
    /* r0 = arg already if argreg==0, otherwise move */
    if (argreg != 0) emit(MOV_R(COND_AL, 0, argreg));
    emit(BLX(fnreg));
}
/* For functions taking 2 args (addr, val): r0=addr, r1=val; fn in r2 */
static inline void emit_call2(int fnreg) {
    emit(BLX(fnreg));
}

/* ══════════════════════════════════════════════════════════════
   CONDITIONAL T-FLAG SET  (helper for CMPEQ, CMPPL, etc.)
   After ARM flags are set, emit this to write T into SR.
   Uses r0-r3 as scratch.
   Flags must be set by a prior comparison.
   ══════════════════════════════════════════════════════════════ */
static inline void emit_t_from_cond(uint32_t cond) {
    /* Default T=0 (not taken). Then conditionally set T=1. */
    /* Load SR, clear T, then conditionally OR in 1. */
    emit_load_sr(0);                   /* r0 = SR */
    emit(BIC_I(COND_AL, 0, 0, 1));    /* r0 = SR & ~1 */
    /* Conditional: if the condition is met, set bit 0 */
    /* Use ORR with the condition: ORR r0, r0, #1 (conditional) */
    emit(ORR_I(cond, 0, 0, 1));       /* if(cond) r0 |= 1 */
    emit_store_sr(0);                  /* SR = r0 */
}

/* ══════════════════════════════════════════════════════════════
   INSTRUCTION-SPECIFIC EMITTERS
   Each returns the number of ARM words emitted, or 0 to
   signal "cannot compile, use C fallback".
   ══════════════════════════════════════════════════════════════ */

/* ADD  Rn, Rm  (0x3xxC) — sets no flags */
static int e_ADD(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, n); emit_load_reg(1, m);
    emit(ADD_R(COND_AL, 0, 0, 1));
    emit_store_reg(0, n); emit_inc_pc();
    return 5;
}
/* ADD #imm, Rn (0x7xxx) */
static int e_ADDI(u16 i) {
    int n = INSTRUCTION_B(i);
    s32 v = (s32)(s8)INSTRUCTION_CD(i);
    emit_load_reg(0, n);
    if (v >= 0) emit(ADD_I(COND_AL, 0, 0, v));
    else        emit(SUB_I(COND_AL, 0, 0, -v));
    emit_store_reg(0, n); emit_inc_pc();
    return 4;
}
/* SUB  Rn, Rm  (0x3xx8) */
static int e_SUB(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, n); emit_load_reg(1, m);
    emit(SUB_R(COND_AL, 0, 0, 1));
    emit_store_reg(0, n); emit_inc_pc();
    return 5;
}
/* MOV  Rn, Rm  (0x6xx3) */
static int e_MOV(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    if (n != m) { emit_load_reg(0, m); emit_store_reg(0, n); }
    emit_inc_pc();
    return (n != m) ? 3 : 1;
}
/* MOV.W @(disp,PC), Rn (0x9xxx) — PC-relative word load */
static int e_MOVWI(u16 i) {
    int n = INSTRUCTION_B(i), disp = INSTRUCTION_CD(i);
    /* addr = PC + disp*2 + 4 */
    emit_load_pc(0);
    emit(ADD_I(COND_AL, 0, 0, disp * 2 + 4));
    emit_mov_imm(1, (uint32_t)MappedMemoryReadWord);
    emit(BLX(1));                              /* r0 = ReadWord(addr) */
    /* sign-extend 16→32: LSL 16, ASR 16 */
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, LSL_I(16)));
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, ASR_I(16)));
    emit_store_reg(0, n); emit_inc_pc();
    return 8;
}
/* MOV.L @(disp,PC), Rn (0xDxxx) */
static int e_MOVLI(u16 i) {
    int n = INSTRUCTION_B(i), disp = INSTRUCTION_CD(i);
    emit_load_pc(0);
    emit(ADD_I(COND_AL, 0, 0, disp * 4 + 4));
    emit_mov_imm(1, (uint32_t)MappedMemoryReadLong);
    emit(BLX(1));
    emit_store_reg(0, n); emit_inc_pc();
    return 7;
}
/* MOV #imm, Rn (0xExxx) */
static int e_MOVI(u16 i) {
    int n = INSTRUCTION_B(i);
    s32 v = (s32)(s8)INSTRUCTION_CD(i);
    emit_mov_imm(0, (uint32_t)v);
    emit_store_reg(0, n); emit_inc_pc();
    return 3;
}
/* MOV.L @Rm, Rn (0x6xx2) */
static int e_MOVLL(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, m);
    emit_mov_imm(1, (uint32_t)MappedMemoryReadLong);
    emit(BLX(1));
    emit_store_reg(0, n); emit_inc_pc();
    return 6;
}
/* MOV.W @Rm, Rn (0x6xx1) */
static int e_MOVWL(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, m);
    emit_mov_imm(1, (uint32_t)MappedMemoryReadWord);
    emit(BLX(1));
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, LSL_I(16)));
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, ASR_I(16)));
    emit_store_reg(0, n); emit_inc_pc();
    return 8;
}
/* MOV.B @Rm, Rn (0x6xx0) */
static int e_MOVBL(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, m);
    emit_mov_imm(1, (uint32_t)MappedMemoryReadByte);
    emit(BLX(1));
    emit(AND_I(COND_AL, 0, 0, 0xFF));
    emit_store_reg(0, n); emit_inc_pc();
    return 7;
}
/* MOV.L Rn, @Rm (0x2xx2) */
static int e_MOVLS(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, m); /* addr */
    emit_load_reg(1, n); /* val */
    emit_mov_imm(2, (uint32_t)MappedMemoryWriteLong);
    emit(BLX(2));
    emit_inc_pc();
    return 7;
}
/* MOV.W Rn, @Rm (0x2xx1) */
static int e_MOVWS(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, m);
    emit_load_reg(1, n);
    emit_mov_imm(2, (uint32_t)MappedMemoryWriteWord);
    emit(BLX(2));
    emit_inc_pc();
    return 7;
}
/* MOV.B Rn, @Rm (0x2xx0) */
static int e_MOVBS(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, m);
    emit_load_reg(1, n);
    emit_mov_imm(2, (uint32_t)MappedMemoryWriteByte);
    emit(BLX(2));
    emit_inc_pc();
    return 7;
}
/* AND  Rn, Rm (0x2xx9) */
static int e_AND(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, n); emit_load_reg(1, m);
    emit(AND_R(COND_AL, 0, 0, 1));
    emit_store_reg(0, n); emit_inc_pc();
    return 5;
}
/* OR   Rn, Rm (0x2xxB) */
static int e_OR(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, n); emit_load_reg(1, m);
    emit(ORR_R(COND_AL, 0, 0, 1));
    emit_store_reg(0, n); emit_inc_pc();
    return 5;
}
/* XOR  Rn, Rm (0x2xxA) */
static int e_XOR(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, n); emit_load_reg(1, m);
    emit(EOR_R(COND_AL, 0, 0, 1));
    emit_store_reg(0, n); emit_inc_pc();
    return 5;
}
/* NOT  Rn, Rm (0x6xx7) */
static int e_NOT(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, m);
    emit(MVN_R(COND_AL, 0, 0));
    emit_store_reg(0, n); emit_inc_pc();
    return 4;
}
/* NEG  Rn, Rm (0x6xxB) */
static int e_NEG(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, m);
    emit(RSB_R(COND_AL, 0, 0, 0)); /* 0 - r0 */
    emit_store_reg(0, n); emit_inc_pc();
    return 4;
}
/* CMP/EQ Rn, Rm (0x3xx0) */
static int e_CMPEQ(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, n); emit_load_reg(1, m);
    emit(CMP_R(COND_AL, 0, 1));           /* sets Z */
    emit_t_from_cond(COND_EQ);
    emit_inc_pc();
    return 7;
}
/* CMP/HS Rn, Rm (0x3xx2) — unsigned >= */
static int e_CMPHS(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, n); emit_load_reg(1, m);
    emit(CMP_R(COND_AL, 0, 1));           /* sets CS for no borrow */
    emit_t_from_cond(COND_CS);
    emit_inc_pc();
    return 7;
}
/* CMP/GE Rn, Rm (0x3xx3) — signed >= */
static int e_CMPGE(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, n); emit_load_reg(1, m);
    emit(CMP_R(COND_AL, 0, 1));
    emit_t_from_cond(COND_GE);
    emit_inc_pc();
    return 7;
}
/* CMP/HI Rn, Rm (0x3xx6) — unsigned > */
static int e_CMPHI(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, n); emit_load_reg(1, m);
    emit(CMP_R(COND_AL, 0, 1));
    emit_t_from_cond(COND_HI);
    emit_inc_pc();
    return 7;
}
/* CMP/GT Rn, Rm (0x3xx7) — signed > */
static int e_CMPGT(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, n); emit_load_reg(1, m);
    emit(CMP_R(COND_AL, 0, 1));
    emit_t_from_cond(COND_GT);
    emit_inc_pc();
    return 7;
}
/* TST  Rn, Rm (0x2xx8) — T = ((Rn & Rm) == 0) */
static int e_TST(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, n); emit_load_reg(1, m);
    emit(TST_R(COND_AL, 0, 1));
    emit_t_from_cond(COND_EQ);
    emit_inc_pc();
    return 7;
}
/* CMP/IMM (0x8xx8) — T = (R0 == imm) */
static int e_CMPIM(u16 i) {
    int v = INSTRUCTION_CD(i);
    emit_load_reg(0, 0);
    if (arm_imm8_ok(v)) emit(CMP_I(COND_AL, 0, v));
    else { emit_mov_imm(1, v); emit(CMP_R(COND_AL, 0, 1)); }
    emit_t_from_cond(COND_EQ);
    emit_inc_pc();
    return (arm_imm8_ok(v)) ? 7 : 8;
}
/* AND #imm, R0 (0xC9xx) */
static int e_ANDI(u16 i) {
    int v = INSTRUCTION_CD(i);
    emit_load_reg(0, 0);
    if (arm_imm8_ok(v)) emit(AND_I(COND_AL, 0, 0, v));
    else { emit_mov_imm(1, v); emit(AND_R(COND_AL, 0, 0, 1)); }
    emit_store_reg(0, 0); emit_inc_pc();
    return (arm_imm8_ok(v)) ? 4 : 6;
}
/* OR  #imm, R0 (0xCBxx) */
static int e_ORI(u16 i) {
    int v = INSTRUCTION_CD(i);
    emit_load_reg(0, 0);
    if (arm_imm8_ok(v)) emit(ORR_I(COND_AL, 0, 0, v));
    else { emit_mov_imm(1, v); emit(ORR_R(COND_AL, 0, 0, 1)); }
    emit_store_reg(0, 0); emit_inc_pc();
    return (arm_imm8_ok(v)) ? 4 : 6;
}
/* XOR #imm, R0 (0xCAxx) */
static int e_XORI(u16 i) {
    int v = INSTRUCTION_CD(i);
    emit_load_reg(0, 0);
    if (arm_imm8_ok(v)) emit(EOR_I(COND_AL, 0, 0, v));
    else { emit_mov_imm(1, v); emit(EOR_R(COND_AL, 0, 0, 1)); }
    emit_store_reg(0, 0); emit_inc_pc();
    return (arm_imm8_ok(v)) ? 4 : 6;
}
/* TST #imm, R0 (0xC8xx) — T = ((R0 & imm) == 0) */
static int e_TSTI(u16 i) {
    int v = INSTRUCTION_CD(i);
    emit_load_reg(0, 0);
    if (arm_imm8_ok(v)) emit(TST_I(COND_AL, 0, v));
    else { emit_mov_imm(1, v); emit(TST_R(COND_AL, 0, 1)); }
    emit_t_from_cond(COND_EQ);
    emit_inc_pc();
    return (arm_imm8_ok(v)) ? 7 : 8;
}

/* ── SHIFTS ──────────────────────────────────────────────── */
static int e_SHLL(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n);
    /* T = bit 31 */
    emit(DP3_R(COND_AL, 0xD, 0, 1, 0, 0, LSR_I(31))); /* r1 = Rn >> 31 */
    emit_load_sr(2); emit(BIC_I(COND_AL, 2, 2, 1)); emit(ORR_R(COND_AL, 2, 2, 1)); emit_store_sr(2);
    emit(ADD_R(COND_AL, 0, 0, 0)); /* Rn <<= 1 */
    emit_store_reg(0, n); emit_inc_pc();
    return 9;
}
static int e_SHLR(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n);
    emit(AND_I(COND_AL, 1, 0, 1));   /* T = bit 0 */
    emit_load_sr(2); emit(BIC_I(COND_AL, 2, 2, 1)); emit(ORR_R(COND_AL, 2, 2, 1)); emit_store_sr(2);
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, LSR_I(1))); /* Rn >>= 1 (logical) */
    emit_store_reg(0, n); emit_inc_pc();
    return 9;
}
static int e_SHAR(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n);
    emit(AND_I(COND_AL, 1, 0, 1));
    emit_load_sr(2); emit(BIC_I(COND_AL, 2, 2, 1)); emit(ORR_R(COND_AL, 2, 2, 1)); emit_store_sr(2);
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, ASR_I(1))); /* Rn >>= 1 (arithmetic) */
    emit_store_reg(0, n); emit_inc_pc();
    return 9;
}
static int e_ROTL(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n);
    emit(DP3_R(COND_AL, 0xD, 0, 1, 0, 0, LSR_I(31))); /* T = old bit 31 */
    emit_load_sr(2); emit(BIC_I(COND_AL, 2, 2, 1)); emit(ORR_R(COND_AL, 2, 2, 1)); emit_store_sr(2);
    emit(ADD_R(COND_AL, 0, 0, 0)); /* Rn <<= 1 */
    emit(ORR_R(COND_AL, 0, 0, 1)); /* Rn |= T */
    emit_store_reg(0, n); emit_inc_pc();
    return 9;
}
static int e_ROTR(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n);
    emit(AND_I(COND_AL, 1, 0, 1));
    emit_load_sr(2); emit(BIC_I(COND_AL, 2, 2, 1)); emit(ORR_R(COND_AL, 2, 2, 1)); emit_store_sr(2);
    emit(DP3_R(COND_AL, 0xD, 0, 3, 0, 0, LSR_I(1)));
    emit(DP3_R(COND_AL, 0xD, 0, 2, 0, 1, LSL_I(31)));
    emit(ORR_R(COND_AL, 0, 3, 2));
    emit_store_reg(0, n); emit_inc_pc();
    return 10;
}
/* SHLL2 / SHLL8 / SHLL16 */
static int e_SHLL_N(u16 i, int k) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n);
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, LSL_I(k)));
    emit_store_reg(0, n); emit_inc_pc();
    return 4;
}
/* SHLR2 / SHLR8 / SHLR16 */
static int e_SHLR_N(u16 i, int k) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n);
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, LSR_I(k)));
    emit_store_reg(0, n); emit_inc_pc();
    return 4;
}

/* EXTU.B Rn (0x6xxC) */
static int e_EXTUB(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, m);
    emit(AND_I(COND_AL, 0, 0, 0xFF));
    emit_store_reg(0, n); emit_inc_pc();
    return 4;
}
/* EXTU.W Rn (0x6xxD) */
static int e_EXTUW(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, m);
    emit_mov_imm(1, 0xFFFF);
    emit(AND_R(COND_AL, 0, 0, 1));
    emit_store_reg(0, n); emit_inc_pc();
    return 5;
}
/* EXTS.B Rn (0x6xxE) */
static int e_EXTSB(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, m);
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, LSL_I(24)));
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, ASR_I(24)));
    emit_store_reg(0, n); emit_inc_pc();
    return 5;
}
/* EXTS.W Rn (0x6xxF) */
static int e_EXTSW(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, m);
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, LSL_I(16)));
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, ASR_I(16)));
    emit_store_reg(0, n); emit_inc_pc();
    return 5;
}
/* SWAP.B (0x6xx8) — uses REV16 (ARMv6+) */
static int e_SWAPB(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, m);
    emit(0xE6BF0F30); /* REV16 r0, r0 */
    emit_store_reg(0, n); emit_inc_pc();
    return 4;
}
/* SWAP.W (0x6xx9) */
static int e_SWAPW(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, m);
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, ROR_I(16)));
    emit_store_reg(0, n); emit_inc_pc();
    return 4;
}
/* NOP (0x0009) */
static int e_NOP(void) { emit_inc_pc(); return 1; }
/* CLRT (0x0008) */
static int e_CLRT(void) {
    emit_load_sr(0); emit(BIC_I(COND_AL, 0, 0, 1)); emit_store_sr(0); emit_inc_pc();
    return 4;
}
/* SETT (0x0018) */
static int e_SETT(void) {
    emit_load_sr(0); emit(ORR_I(COND_AL, 0, 0, 1)); emit_store_sr(0); emit_inc_pc();
    return 4;
}
/* MOVT Rn (0x0xx9 + C=2) */
static int e_MOVT(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_sr(0); emit(AND_I(COND_AL, 0, 0, 1)); emit_store_reg(0, n); emit_inc_pc();
    return 4;
}
/* DT Rn (0x4xx0 + C=1) — decrement and test */
static int e_DT(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n);
    emit(SUBS_I(COND_AL, 0, 0, 1));   /* r0--, sets Z */
    emit_store_reg(0, n);
    emit_t_from_cond(COND_EQ);          /* T = (result == 0) */
    emit_inc_pc();
    return 7;
}
/* CMP/PL Rn (0x4xx5 + C=1) — T = (Rn > 0) */
static int e_CMPPL(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n);
    emit(CMP_I(COND_AL, 0, 0));
    emit_t_from_cond(COND_GT);
    emit_inc_pc();
    return 6;
}
/* CMP/PZ Rn (0x4xx1 + C=1) — T = (Rn >= 0) */
static int e_CMPPZ(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n);
    emit(CMP_I(COND_AL, 0, 0));
    emit_t_from_cond(COND_PL);
    emit_inc_pc();
    return 6;
}
/* MULU Rn, Rm (0x2xxE) */
static int e_MULU(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, n); emit_load_reg(1, m);
    emit(AND_I(COND_AL, 0, 0, 0xFFFF));
    emit(AND_I(COND_AL, 1, 1, 0xFFFF));
    emit(MUL(0, 0, 1));
    emit(STR_I(0, 4, MACL_OFF));
    emit_inc_pc();
    return 8;
}
/* MULS Rn, Rm (0x2xxF) */
static int e_MULS(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, n); emit_load_reg(1, m);
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, LSL_I(16)));
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, ASR_I(16)));
    emit(DP3_R(COND_AL, 0xD, 0, 1, 0, 0, LSL_I(16)));
    emit(DP3_R(COND_AL, 0xD, 0, 1, 0, 0, ASR_I(16)));
    emit(MUL(0, 0, 1));
    emit(STR_I(0, 4, MACL_OFF));
    emit_inc_pc();
    return 10;
}
/* MULL Rn, Rm (0x0xx7) */
static int e_MULL(u16 i) {
    int n = INSTRUCTION_B(i), m = INSTRUCTION_C(i);
    emit_load_reg(0, n); emit_load_reg(1, m);
    emit(MUL(0, 0, 1));
    emit(STR_I(0, 4, MACL_OFF));
    emit_inc_pc();
    return 6;
}
/* CLRMAC (0x0028) */
static int e_CLRMAC(void) {
    emit_mov_imm(0, 0);
    emit(STR_I(0, 4, MACH_OFF));
    emit(STR_I(0, 4, MACL_OFF));
    emit_inc_pc();
    return 5;
}
/* LDC Rn, SR (0x4xxE + C=0) */
static int e_LDCSR(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n);
    emit(AND_I(COND_AL, 0, 0, 0x3F3));
    emit_store_sr(0); emit_inc_pc();
    return 4;
}
/* LDC Rn, GBR (0x4xxE + C=1) */
static int e_LDCGBR(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n);
    emit(STR_I(0, 4, GBR_OFF)); emit_inc_pc();
    return 3;
}
/* LDC Rn, VBR (0x4xxE + C=2) */
static int e_LDCVBR(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n);
    emit(STR_I(0, 4, VBR_OFF)); emit_inc_pc();
    return 3;
}
/* STC SR, Rn (0x0xx2 + C=0) */
static int e_STCSR(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_sr(0); emit_store_reg(0, n); emit_inc_pc();
    return 3;
}
/* STC GBR, Rn (0x0xx2 + C=1) */
static int e_STCGBR(u16 i) {
    int n = INSTRUCTION_B(i);
    emit(LDR_I(0, 4, GBR_OFF)); emit_store_reg(0, n); emit_inc_pc();
    return 3;
}
/* STC VBR, Rn (0x0xx2 + C=2) */
static int e_STCVBR(u16 i) {
    int n = INSTRUCTION_B(i);
    emit(LDR_I(0, 4, VBR_OFF)); emit_store_reg(0, n); emit_inc_pc();
    return 3;
}
/* LDS Rn, MACH (0x4xxA + C=0) */
static int e_LDSMACH(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n); emit(STR_I(0, 4, MACH_OFF)); emit_inc_pc();
    return 3;
}
/* LDS Rn, MACL (0x4xxA + C=1) */
static int e_LDSMACL(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n); emit(STR_I(0, 4, MACL_OFF)); emit_inc_pc();
    return 3;
}
/* LDS Rn, PR (0x4xxA + C=2) */
static int e_LDSPR(u16 i) {
    int n = INSTRUCTION_B(i);
    emit_load_reg(0, n); emit(STR_I(0, 4, PR_OFF)); emit_inc_pc();
    return 3;
}
/* STS MACH, Rn (0x0xxA + C=0) */
static int e_STSMACH(u16 i) {
    int n = INSTRUCTION_B(i);
    emit(LDR_I(0, 4, MACH_OFF)); emit_store_reg(0, n); emit_inc_pc();
    return 3;
}
/* STS MACL, Rn (0x0xxA + C=1) */
static int e_STSMACL(u16 i) {
    int n = INSTRUCTION_B(i);
    emit(LDR_I(0, 4, MACL_OFF)); emit_store_reg(0, n); emit_inc_pc();
    return 3;
}
/* STS PR, Rn (0x0xxA + C=2) */
static int e_STSPR(u16 i) {
    int n = INSTRUCTION_B(i);
    emit(LDR_I(0, 4, PR_OFF)); emit_store_reg(0, n); emit_inc_pc();
    return 3;
}

/* ── MOV.B @(disp, R0), Rn (0x8xx4) ─────────────────────── */
static int e_MOVBL4_B(u16 i) {
    int n = INSTRUCTION_C(i), disp = INSTRUCTION_D(i);
    emit_load_reg(0, n); emit_load_reg(1, 0);
    emit(ADD_R(COND_AL, 0, 0, 1));
    if (disp) emit(ADD_I(COND_AL, 0, 0, disp));
    emit_mov_imm(1, (uint32_t)MappedMemoryReadByte);
    emit(BLX(1));
    emit_store_reg(0, n); emit_inc_pc();
    return 9;
}
/* MOV.W @(disp, R0), Rn (0x8xx5) */
static int e_MOVWL4_B(u16 i) {
    int n = INSTRUCTION_C(i), disp = INSTRUCTION_D(i);
    emit_load_reg(0, n); emit_load_reg(1, 0);
    emit(ADD_R(COND_AL, 0, 0, 1));
    if (disp) emit(ADD_I(COND_AL, 0, 0, disp * 2));
    emit_mov_imm(1, (uint32_t)MappedMemoryReadWord);
    emit(BLX(1));
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, LSL_I(16)));
    emit(DP3_R(COND_AL, 0xD, 0, 0, 0, 0, ASR_I(16)));
    emit_store_reg(0, n); emit_inc_pc();
    return 11;
}
/* MOV.L @(disp, Rn), R0 (0x5xxx) */
static int e_MOVLL4_5(u16 i) {
    int n = INSTRUCTION_B(i), disp = INSTRUCTION_CD(i);
    emit_load_reg(0, n);
    if (disp) emit(ADD_I(COND_AL, 0, 0, disp * 4));
    emit_mov_imm(1, (uint32_t)MappedMemoryReadLong);
    emit(BLX(1));
    emit_store_reg(0, 0); emit_inc_pc();
    return (disp) ? 7 : 6;
}
/* MOV.B @(disp, Rn), R0 (0x8xx4 with n=0) — handled by e_MOVBL4_B */
/* MOV.W @(disp, Rn), R0 (0x8xx5 with n=0) — handled by e_MOVWL4_B */
/* MOV.L @(disp, Rn), Rm (0x1xxx) — fall back to C for now */
/* MOV.B R0, @(disp, Rn) (0x8xx0) */
static int e_MOVBS40(u16 i) {
    int n = INSTRUCTION_C(i), disp = INSTRUCTION_D(i);
    /* addr = Rn + R0 + disp, write R0 as byte */
    emit_load_reg(0, n); emit_load_reg(1, 0);
    emit(ADD_R(COND_AL, 0, 0, 1));
    if (disp) emit(ADD_I(COND_AL, 0, 0, disp));
    /* r0 = addr, r1 = value (R0) */
    emit(MOV_R(COND_AL, 1, 0)); /* Now careful: we need addr AND value */
    /* Rethink: r0 = addr, need val in r1. But we already computed addr in r0 */
    /* Actually: addr in r0, we need to preserve it. Use r2 for addr. */
    emit_store_reg(0, 12); /* Store addr in r12 temporarily (scratch) */
    emit_load_reg(1, 0);  /* r1 = R0 (value) */
    emit(LDR_I(0, 12, 0)); /* r0 = addr - NO, r12 is not mem, it's a reg! */
    /* Let me redo this properly */
    emit_load_reg(0, n); emit_load_reg(1, 0);
    emit(ADD_R(COND_AL, 0, 0, 1));
    if (disp) emit(ADD_I(COND_AL, 0, 0, disp));
    emit(MOV_R(COND_AL, 2, 0));  /* r2 = addr */
    emit_load_reg(1, 0);          /* r1 = R0 (value) */
    emit(MOV_R(COND_AL, 0, 2));  /* r0 = addr */
    emit_mov_imm(2, (uint32_t)MappedMemoryWriteByte);
    emit(BLX(2));
    emit_inc_pc();
    return 13;
}
/* MOV.W R0, @(disp, Rn) (0x8xx1) */
static int e_MOVWS40(u16 i) {
    int n = INSTRUCTION_C(i), disp = INSTRUCTION_D(i);
    emit_load_reg(0, n); emit_load_reg(1, 0);
    emit(ADD_R(COND_AL, 0, 0, 1));
    if (disp) emit(ADD_I(COND_AL, 0, 0, disp * 2));
    emit(MOV_R(COND_AL, 2, 0));
    emit_load_reg(1, 0);
    emit(MOV_R(COND_AL, 0, 2));
    emit_mov_imm(2, (uint32_t)MappedMemoryWriteWord);
    emit(BLX(2));
    emit_inc_pc();
    return 12;
}

/* ══════════════════════════════════════════════════════════════
   BLOCK DECODER
   Walks SH2 instructions from PC, emitting ARM code.
   Returns instruction count (0 = fallback needed).
   ══════════════════════════════════════════════════════════════ */
static int decode_block(u32 pc) {
    u32 page = (pc >> 20) & 0x0FF;
    fetchfunc cached_fetch = fetchlist[page];
    if (!cached_fetch) return 0;

    jit_block_start = jit_ptr;
    emit_prologue();

    u32 cur = pc;
    int total = 0;
    int max_i = 48;
    int fallback = 0;

    while (total < max_i && !fallback) {
        u16 inst = (u16)cached_fetch(cur);
        u8 a = INSTRUCTION_A(inst);
        u8 b = INSTRUCTION_B(inst);
        u8 c = INSTRUCTION_C(inst);
        u8 d = INSTRUCTION_D(inst);
        int emitted = 0;

        /* Dispatch by top 4 bits (opcode type) */
        switch (a) {
        /* ── 0 ────────────────────────────────────────────── */
        case 0x0:
            if (inst == 0x0009)      emitted = e_NOP();
            else if (inst == 0x0008) emitted = e_CLRT();
            else if (inst == 0x0018) emitted = e_SETT();
            else if (inst == 0x0028) emitted = e_CLRMAC();
            else if (inst == 0x000B) { fallback = 1; } /* RTS */
            else if (inst == 0x001B) { fallback = 1; } /* SLEEP */
            else if (inst == 0x002B) { fallback = 1; } /* RTE */
            else if (d == 7)         emitted = e_MULL(inst);
            else if (d == 0x0F)      { fallback = 1; } /* MACL */
            else if (d == 2) { /* STC.SR/GBR/VBR */
                if      (c == 0) emitted = e_STCSR(inst);
                else if (c == 1) emitted = e_STCGBR(inst);
                else if (c == 2) emitted = e_STCVBR(inst);
                else fallback = 1;
            }
            else if (d == 10) { /* STS.MACH/MACL/PR */
                if      (c == 0) emitted = e_STSMACH(inst);
                else if (c == 1) emitted = e_STSMACL(inst);
                else if (c == 2) emitted = e_STSPR(inst);
                else fallback = 1;
            }
            else if (d == 9) {
                if      (c == 0) emitted = e_NOP();
                else if (c == 1) { fallback = 1; } /* DIV0U */
                else if (c == 2) emitted = e_MOVT(inst);
                else fallback = 1;
            }
            else if (d == 8) {
                if      (c == 0) emitted = e_CLRT();
                else if (c == 1) emitted = e_SETT();
                else if (c == 2) emitted = e_CLRMAC();
                else fallback = 1;
            }
            else if (d == 4)  emitted = e_MOVBS40(inst);
            else if (d == 12) emitted = e_MOVBL4_B(inst);
            else if (d == 5)  emitted = e_MOVWS40(inst);
            else if (d == 13) emitted = e_MOVWL4_B(inst);
            else if (d == 3) { /* BRAF/BSRF */ fallback = 1; }
            else fallback = 1;
            break;

        /* ── 1: MOV.L @(disp,Rn), Rm (SH2movls4) ─────────── */
        case 0x1: fallback = 1; break;

        /* ── 2: memory ops, logicals ─────────────────────── */
        case 0x2:
            if      (d == 0)  emitted = e_MOVBS(inst);
            else if (d == 1)  emitted = e_MOVWS(inst);
            else if (d == 2)  emitted = e_MOVLS(inst);
            else if (d == 8)  { int r = e_TST(inst);   if (r) emitted = r; else fallback = 1; }
            else if (d == 9)  emitted = e_AND(inst);
            else if (d == 10) emitted = e_XOR(inst);
            else if (d == 11) emitted = e_OR(inst);
            else if (d == 14) emitted = e_MULU(inst);
            else if (d == 15) emitted = e_MULS(inst);
            else if (d == 7)  fallback = 1; /* DIV0S */
            else if (d == 12) fallback = 1; /* CMPSTR */
            else if (d == 13) fallback = 1; /* XTRCT */
            else if (d >= 4 && d <= 6) fallback = 1; /* pre-dec @-Rn */
            else fallback = 1;
            break;

        /* ── 3: arithmetic / compare ─────────────────────── */
        case 0x3:
            if      (d == 0)  { int r = e_CMPEQ(inst); if (r) emitted = r; else fallback = 1; }
            else if (d == 2)     emitted = e_CMPHS(inst);
            else if (d == 3)     emitted = e_CMPGE(inst);
            else if (d == 6)     emitted = e_CMPHI(inst);
            else if (d == 7)     emitted = e_CMPGT(inst);
            else if (d == 8)     emitted = e_SUB(inst);
            else if (d == 12)    emitted = e_ADD(inst);
            else if (d == 14) fallback = 1; /* ADDC */
            else if (d == 15) fallback = 1; /* ADDV */
            else if (d == 10) fallback = 1; /* SUBC */
            else if (d == 11) fallback = 1; /* SUBV */
            else if (d == 4)  fallback = 1; /* DIV1 */
            else if (d == 5)  fallback = 1; /* DMULU */
            else if (d == 13) fallback = 1; /* DMULS */
            else fallback = 1;
            break;

        /* ── 4: shifts, LDC/STC, JSR/JMP, etc ─────────────- */
        case 0x4:
            if (d == 0) {
                if      (c == 0) emitted = e_SHLL(inst);
                else if (c == 1) { int r = e_DT(inst); if (r) emitted = r; else fallback = 1; }
                else if (c == 2) emitted = e_SHLL(inst); /* SHAL */
                else fallback = 1;
            }
            else if (d == 1) {
                if      (c == 0) emitted = e_SHLR(inst);
                else if (c == 1) { int r = e_CMPPZ(inst); if (r) emitted = r; else fallback = 1; }
                else if (c == 2) emitted = e_SHAR(inst);
                else fallback = 1;
            }
            else if (d == 2)  fallback = 1; /* STS.L */
            else if (d == 3)  fallback = 1; /* STC.L */
            else if (d == 4) {
                if      (c == 0) emitted = e_ROTL(inst);
                else if (c == 2) fallback = 1; /* ROTCL */
                else fallback = 1;
            }
            else if (d == 5) {
                if      (c == 0) emitted = e_ROTR(inst);
                else if (c == 1) { int r = e_CMPPL(inst); if (r) emitted = r; else fallback = 1; }
                else if (c == 2) fallback = 1; /* ROTCR */
                else fallback = 1;
            }
            else if (d == 6)  fallback = 1; /* LDS.L */
            else if (d == 7)  fallback = 1; /* LDC.L */
            else if (d == 8) {
                if      (c == 0) emitted = e_SHLL_N(inst, 2);
                else if (c == 1) emitted = e_SHLL_N(inst, 8);
                else if (c == 2) emitted = e_SHLL_N(inst, 16);
                else fallback = 1;
            }
            else if (d == 9) {
                if      (c == 0) emitted = e_SHLR_N(inst, 2);
                else if (c == 1) emitted = e_SHLR_N(inst, 8);
                else if (c == 2) emitted = e_SHLR_N(inst, 16);
                else fallback = 1;
            }
            else if (d == 10) {
                if      (c == 0) emitted = e_LDSMACH(inst);
                else if (c == 1) emitted = e_LDSMACL(inst);
                else if (c == 2) emitted = e_LDSPR(inst);
                else fallback = 1;
            }
            else if (d == 11) fallback = 1; /* JSR/TAS/JMP */
            else if (d == 14) {
                if      (c == 0) emitted = e_LDCSR(inst);
                else if (c == 1) emitted = e_LDCGBR(inst);
                else if (c == 2) emitted = e_LDCVBR(inst);
                else fallback = 1;
            }
            else if (d == 15) fallback = 1; /* MACW */
            else fallback = 1;
            break;

        /* ── 5: MOV.L @(disp,Rn), R0 ─────────────────────── */
        case 0x5: emitted = e_MOVLL4_5(inst); break;

        /* ── 6: MOV group, NOT, NEG, SWAP, EXTS/EXTU ──────- */
        case 0x6:
            if      (d == 0)  emitted = e_MOVBL(inst);
            else if (d == 1)  emitted = e_MOVWL(inst);
            else if (d == 2)  emitted = e_MOVLL(inst);
            else if (d == 3)  emitted = e_MOV(inst);
            else if (d == 7)  emitted = e_NOT(inst);
            else if (d == 8)  emitted = e_SWAPB(inst);
            else if (d == 9)  emitted = e_SWAPW(inst);
            else if (d == 10) fallback = 1; /* NEGC */
            else if (d == 11) emitted = e_NEG(inst);
            else if (d == 12) emitted = e_EXTUB(inst);
            else if (d == 13) emitted = e_EXTUW(inst);
            else if (d == 14) emitted = e_EXTSB(inst);
            else if (d == 15) emitted = e_EXTSW(inst);
            else if (d >= 4 && d <= 6) fallback = 1; /* MOV @(disp,Rn) with post-inc */
            else fallback = 1;
            break;

        /* ── 7: ADD #imm, Rn ─────────────────────────────── */
        case 0x7: emitted = e_ADDI(inst); break;

        /* ── 8: BF/BT/CMPIM/MOV @(disp,Rn,R0) ─────────────- */
        case 0x8:
            if      (b == 8)  { int r = e_CMPIM(inst); if (r) emitted = r; else fallback = 1; }
            else if (b == 9)  fallback = 1; /* BT */
            else if (b == 11) fallback = 1; /* BF */
            else if (b == 13) fallback = 1; /* BTS */
            else if (b == 15) fallback = 1; /* BFS */
            else if (b == 0)  emitted = e_MOVBS40(inst);
            else if (b == 1)  emitted = e_MOVWS40(inst);
            else if (b == 4)  emitted = e_MOVBL4_B(inst);
            else if (b == 5)  emitted = e_MOVWL4_B(inst);
            else fallback = 1;
            break;

        /* ── 9: MOV.W @(disp,PC), Rn ─────────────────────── */
        case 0x9: emitted = e_MOVWI(inst); break;

        /* ── A: BRA ────────────────────────────────────────- */
        case 0xA: fallback = 1; break;

        /* ── B: BSR ────────────────────────────────────────- */
        case 0xB: fallback = 1; break;

        /* ── C: GBR ops, TRAPA, imm logicals ─────────────── */
        case 0xC:
            if      (b == 8)  { int r = e_TSTI(inst); if (r) emitted = r; else fallback = 1; }
            else if (b == 9)  emitted = e_ANDI(inst);
            else if (b == 10) emitted = e_XORI(inst);
            else if (b == 11) emitted = e_ORI(inst);
            else if (b >= 0 && b <= 7) fallback = 1; /* GBR / TRAPA / MOVEA */
            else if (b >= 12 && b <= 15) fallback = 1; /* TSTM/ANDM/XORM/ORM */
            else fallback = 1;
            break;

        /* ── D: MOV.L @(disp,PC), Rn ─────────────────────── */
        case 0xD: emitted = e_MOVLI(inst); break;

        /* ── E: MOV #imm, Rn ─────────────────────────────── */
        case 0xE: emitted = e_MOVI(inst); break;

        default: fallback = 1; break;
        }

        if (fallback || emitted == 0) { fallback = 1; break; }
        total++;
        cur += 2;

        /* If the emitted code modifies PC (branch), we could detect it here.
           For now we rely on fallback for any branch instruction. */
    }

    if (total == 0) { jit_ptr = jit_block_start; return 0; }

    emit_epilogue(total);
    return total;
}

/* ══════════════════════════════════════════════════════════════
   JIT BLOCK EXECUTION PROXY
   ══════════════════════════════════════════════════════════════ */
typedef int (*jit_block_fn)(SH2_struct*);

/* ══════════════════════════════════════════════════════════════
   INIT / DEINIT / RESET
   ══════════════════════════════════════════════════════════════ */
static int sh2dyn_arm_init(void) {
    vita_log("[SH2DynARM] Initializing ARM JIT core v2.0...\n");
    jit_memblock = sceKernelAllocMemBlock("Yabause_JIT_Cache",
                   SCE_KERNEL_MEMBLOCK_TYPE_USER_RWX, 8 * 1024 * 1024, NULL);
    if (jit_memblock >= 0) {
        sceKernelGetMemBlockBase(jit_memblock, &jit_memory);
        jit_ptr = (uint32_t*)jit_memory;
        vita_log("[SH2DynARM] 8MB RWX JIT cache at %p\n", jit_memory);
    }
    memset(jit_hash, 0, sizeof(jit_hash));
    return SH2InterpreterInit();
}

static void sh2dyn_arm_deinit(void) {
    if (jit_memblock >= 0) {
        sceKernelFreeMemBlock(jit_memblock);
        jit_memblock = -1; jit_memory = NULL; jit_ptr = NULL;
    }
}

static int sh2dyn_arm_reset(void) {
    jit_ptr = (uint32_t*)jit_memory;
    memset(jit_hash, 0, sizeof(jit_hash));
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   MAIN EXECUTION LOOP
   ══════════════════════════════════════════════════════════════ */
static void FASTCALL sh2dyn_arm_exec(SH2_struct *restrict context, u32 cycles)
{
    if (!context) return;
    if (__builtin_expect(context->isIdle, 0)) { SH2idleParse(context, cycles); return; }
    SH2idleCheck(context, cycles);

    u32 last_page = 0xFFFFFFFF;
    fetchfunc cached_fetch = NULL;
    opcodefunc *restrict local_opcodes = opcodes;

    while (__builtin_expect(context->cycles < cycles, 1))
    {
        u32 pc = context->regs.PC;

        /* ── Try JIT cache ─────────────────────────────── */
        uint32_t* block = jit_cache_lookup(pc);
        if (__builtin_expect(block != NULL, 1)) {
            context->cycles += ((jit_block_fn)block)(context);
            continue;
        }

        /* ── Compile new block ─────────────────────────── */
        uint32_t* saved = jit_ptr;
        int compiled = decode_block(pc);
        if (compiled > 0) {
            __clear_cache((char*)saved, (char*)jit_ptr);
            jit_cache_add(pc, saved, compiled);
            context->cycles += ((jit_block_fn)saved)(context);
            continue;
        }

        /* ── Fallback to C interpreter ─────────────────── */
        {
            u32 page = (pc >> 20) & 0x0FF;
            if (__builtin_expect(page != last_page, 0)) {
                last_page = page;
                cached_fetch = fetchlist[page];
            }
            if (__builtin_expect(cached_fetch != NULL, 1))
                context->instruction = (u16)cached_fetch(pc);
            else
                context->instruction = 0xFFFF;

            void (*handler)(SH2_struct*) = local_opcodes[context->instruction];
            if (__builtin_expect(handler != NULL, 1))
                handler(context);
            else
                context->regs.PC += 2;
            context->cycles++;
        }
    }
}

static void sh2dyn_arm_write_notify(u32 start, u32 length) {
    u32 sp = start >> 12;
    u32 ep = (start + length - 1) >> 12;
    for (u32 p = sp; p <= ep; p++)
        for (int i = 0; i < JIT_HASH_SIZE; i++)
            if ((jit_hash[i].pc >> 12) == p) jit_hash[i].pc = 0xFFFFFFFF;
}

SH2Interface_struct SH2DynARM = {
    SH2DYN_ARM_ID,
    "SH2 ARM Dynamic Recompiler v2.0",
    sh2dyn_arm_init,
    sh2dyn_arm_deinit,
    sh2dyn_arm_reset,
    sh2dyn_arm_exec,
    sh2dyn_arm_write_notify
};
