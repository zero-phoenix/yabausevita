#include <string.h>
#include "sh2core.h"
#include "sh2int.h"
#include "sh2idle.h"
#include "memory.h"

extern opcodefunc opcodes[0x10000];
extern fetchfunc fetchlist[0x100];
extern int SH2InterpreterInit(void);

#define SH2LRU_ID 3
#define BLOCK_MAX 24
#define CACHE_BITS 7
#define CACHE_SIZE (1 << CACHE_BITS)
#define CACHE_MASK (CACHE_SIZE - 1)

typedef struct {
    u32 start_pc;
    u16 ops[BLOCK_MAX];
    u8 count;
} BlockEntry;

static BlockEntry block_cache[CACHE_SIZE];

static int sh2lru_init(void)
{
    memset(block_cache, 0, sizeof(block_cache));
    return SH2InterpreterInit();
}

static void sh2lru_deinit(void) {}

static int sh2lru_reset(void)
{
    memset(block_cache, 0, sizeof(block_cache));
    return 0;
}

static INLINE int is_branch_term(u16 inst)
{
    if ((inst & 0xF000) == 0xA000) return 1;
    if ((inst & 0xF000) == 0xB000) return 1;
    if ((inst & 0xFF00) == 0x8900) return 1;
    if ((inst & 0xFF00) == 0x8B00) return 1;
    if ((inst & 0xFF00) == 0x8D00) return 1;
    if ((inst & 0xFF00) == 0x8F00) return 1;
    if (inst == 0x402B) return 1;
    if (inst == 0x400B) return 1;
    if (inst == 0x000B) return 1;
    if (inst == 0x002B) return 1;
    if (inst == 0x0023) return 1;
    if (inst == 0x0003) return 1;
    if ((inst & 0xFF00) == 0xC300) return 1;
    return 0;
}

static void FASTCALL sh2lru_exec(SH2_struct *context, u32 cycles)
{
    if (!context) return;
    if (context->isIdle)
    { SH2idleParse(context, cycles); return; }
    SH2idleCheck(context, cycles);

    u32 end_cycles = context->cycles + cycles;

    while (context->cycles < end_cycles)
    {
        u32 pc = context->regs.PC;
        u32 hash = (pc >> 1) & CACHE_MASK;
        BlockEntry *blk = &block_cache[hash];

        if (blk->start_pc != pc)
        {
            int cnt = 0;
            while (cnt < BLOCK_MAX)
            {
                u32 addr = pc + cnt * 2;
                u32 page = (addr >> 20) & 0x0FF;
                fetchfunc fetcher = fetchlist[page];
                u16 inst = fetcher ? (u16)fetcher(addr) : 0xFFFF;
                blk->ops[cnt] = inst;
                cnt++;
                if (is_branch_term(inst)) break;
            }
            blk->start_pc = pc;
            blk->count = (u8)cnt;
        }

        u32 block_end_pc = pc + blk->count * 2;
        int i = 0;
        while (i < blk->count && context->cycles < end_cycles)
        {
            context->instruction = blk->ops[i];
            void (*handler)(SH2_struct *) = opcodes[context->instruction];
            if (handler) handler(context);
            else context->regs.PC += 2;
            context->cycles++;
            i++;
            if (context->regs.PC != pc + i * 2) break;
        }
    }
}

static void sh2lru_write_notify(u32 start, u32 length)
{
    u32 start_page = start >> 19;
    u32 end_page = (start + length) >> 19;
    for (u32 p = start_page; p <= end_page; p++)
    {
        for (int i = 0; i < CACHE_SIZE; i++)
        {
            u32 blk_start = block_cache[i].start_pc;
            if (blk_start && (blk_start >> 19) == p)
                block_cache[i].start_pc = 0;
        }
    }
}

SH2Interface_struct SH2LRU = {
    SH2LRU_ID,
    "SH2 Block Cache",
    sh2lru_init,
    sh2lru_deinit,
    sh2lru_reset,
    sh2lru_exec,
    sh2lru_write_notify
};
