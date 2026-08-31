#include <string.h>
#include "sh2core.h"
#include "sh2int.h"
#include "sh2idle.h"
#include "memory.h"

extern u8 *BiosRom;
extern u8 *LowWram;
extern u8 *HighWram;
extern SH2_struct *CurrentSH2;

extern u32 FASTCALL FetchBios(u32 addr);
extern u32 FASTCALL FetchLWram(u32 addr);
extern u32 FASTCALL FetchHWram(u32 addr);
extern u32 FASTCALL FetchCs0(u32 addr);
extern u32 FASTCALL FetchInvalid(u32 addr);
extern opcodefunc opcodes[0x10000];
extern fetchfunc fetchlist[0x100];
extern int SH2InterpreterInit(void);

#define SH2FAST_ID 2

static int sh2fast_init(void)
{
    return SH2InterpreterInit();
}

static void sh2fast_deinit(void) {}
static int sh2fast_reset(void) { return 0; }

/* Ronda 3: instrucciones ejecutadas por core. El campo context->cycles NO
   es un contador absoluto (se renueva por llamada), así que se acumula el
   delta DENTRO de cada exec: una comparación y una suma por llamada, no
   por instrucción — medir el coste por instrucción no puede encarecer la
   instrucción. [0]=MSH2, [1]=SSH2. */
unsigned long long sh2fast_instr_total[2] = {0, 0};

static void FASTCALL sh2fast_exec(SH2_struct *restrict context, u32 cycles)
{
    if (!context) return;
    u32 cycles_entrada = context->cycles;
    int idx = (context == MSH2) ? 0 : 1;   /* indice, no booleano */
    if (__builtin_expect(context->isIdle, 0))
    {
        SH2idleParse(context, cycles);
        sh2fast_instr_total[idx] += context->cycles - cycles_entrada;
        return;
    }
    SH2idleCheck(context, cycles);

    u32 last_page = 0xFFFFFFFF;
    fetchfunc cached_fetch = NULL;
    opcodefunc *restrict local_opcodes = opcodes;

    while (__builtin_expect(context->cycles < cycles, 1))
    {
        u32 pc = context->regs.PC;
        u32 page = (pc >> 20) & 0x0FF;

        if (__builtin_expect(page != last_page, 0))
        {
            last_page = page;
            cached_fetch = fetchlist[page];
        }

        if (__builtin_expect(cached_fetch != NULL, 1))
            context->instruction = (u16)cached_fetch(pc);
        else
            context->instruction = 0xFFFF;

        void (*handler)(SH2_struct *) = local_opcodes[context->instruction];
        if (__builtin_expect(handler != NULL, 1))
            handler(context);
        else
            context->regs.PC += 2;

        context->cycles++;
    }
    sh2fast_instr_total[idx] += context->cycles - cycles_entrada;
}

static void sh2fast_write_notify(u32 start, u32 length)
{
    (void)start; (void)length;
}

SH2Interface_struct SH2Fast = {
    SH2FAST_ID,
    "SH2 Fast Interpreter",
    sh2fast_init,
    sh2fast_deinit,
    sh2fast_reset,
    sh2fast_exec,
    sh2fast_write_notify
};
