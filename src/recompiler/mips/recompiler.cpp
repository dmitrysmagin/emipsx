/*
 * Mips-to-mips recompiler for pcsx4all
 *
 * Copyright (c) 2009 Ulrich Hecht
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

//#define NO_ZERO_REGISTER_OPTIMISATION	1

#include "psxcommon.h"
#include "psxhle.h"
#include "psxmem.h"
#include "r3000a.h"
#include "gte.h"

#undef INLINE
#define INLINE		inline

//#define WITH_DISASM
//#define DEBUGG printf
#define DEBUGF(aa)

#include "mips_codegen.h"
#include "disasm.h"

static u32 iRegs[32]; /* used for imm caching and back up of regs in dynarec */

static u32 psxRecLUT[0x010000];

#undef PC_REC
#undef PC_REC8
#undef PC_REC16
#undef PC_REC32
#define PC_REC(x)	(psxRecLUT[(x) >> 16] + ((x) & 0xffff))
#define PC_REC8(x)	(*(u8 *)PC_REC(x))
#define PC_REC16(x)	(*(u16*)PC_REC(x))
#define PC_REC32(x)	(*(u32*)PC_REC(x))

#define RECMEM_SIZE		(12 * 1024 * 1024)
#define RECMEM_SIZE_MAX 	(RECMEM_SIZE-(512*1024))
#define REC_MAX_OPCODES		80

static u8 recMemBase[RECMEM_SIZE + (REC_MAX_OPCODES*2) + 0x4000] __attribute__ ((__aligned__ (32)));
static u32 *recMem;				/* the recompiled blocks will be here */
static s8 recRAM[0x200000];			/* and the ptr to the blocks here */
static s8 recROM[0x080000];			/* and here */
static u32 pc;					/* recompiler pc */
static u32 oldpc;
static u32 branch = 0;

#ifdef WITH_DISASM
FILE *translation_log_fp = NULL;
char disasm_buffer[512];
#endif

#include "regcache.h"

static void recReset();
static u32 recRecompile();
static void recClear(u32 Addr, u32 Size);

extern void (*recBSC[64])();
extern void (*recSPC[64])();
extern void (*recREG[32])();
extern void (*recCP0[32])();
extern void (*recCP2[64])();
extern void (*recCP2BSC[32])();

u8	*current_translation_ptr;
u32 	opcode;
u32	*recMemStart;
u32	isInBios = 0;
u32 	loadedpermregs = 0;
u32	end_block = 0;
int ibranch;
u32 blockcycles = 0;

#ifdef WITH_DISASM

#define make_stub_label(name)                                                 \
 { (void *)name, (char*)#name }                                                      \

disasm_label stub_labels[] =
{
  make_stub_label(gteMFC2),
  make_stub_label(gteMTC2),
  make_stub_label(gteLWC2),
  make_stub_label(gteSWC2),
  make_stub_label(gteRTPS),
  make_stub_label(gteOP),
  make_stub_label(gteNCLIP),
  make_stub_label(gteDPCS),
  make_stub_label(gteINTPL),
  make_stub_label(gteMVMVA),
  make_stub_label(gteNCDS),
  make_stub_label(gteNCDT),
  make_stub_label(gteCDP),
  make_stub_label(gteNCCS),
  make_stub_label(gteCC),
  make_stub_label(gteNCS),
  make_stub_label(gteNCT),
  make_stub_label(gteSQR),
  make_stub_label(gteDCPL),
  make_stub_label(gteDPCT),
  make_stub_label(gteAVSZ3),
  make_stub_label(gteAVSZ4),
  make_stub_label(gteRTPT),
  make_stub_label(gteGPF),
  make_stub_label(gteGPL),
  make_stub_label(gteNCCT),
  make_stub_label(psxMemRead8),
  make_stub_label(psxMemRead16),
  make_stub_label(psxMemRead32),
  make_stub_label(psxMemWrite8),
  make_stub_label(psxMemWrite16),
  make_stub_label(psxMemWrite32),
  make_stub_label(psxException),
  make_stub_label(psxBranchTest_rec),
  make_stub_label(psx_interrupt)
};

const u32 num_stub_labels = sizeof(stub_labels) / sizeof(disasm_label);

#define DISASM_INIT								\
  	/*translation_log_fp = fopen("/mnt/sd/translation_log.txt", "a+");*/     	\
  	printf(/*translation_log_fp, */"Block PC %x (MIPS) -> %p\n", pc,  \
   		recMemStart);                                          	        \

#define DISASM_PSX								\
		disasm_mips_instruction(psxRegs.code,disasm_buffer,pc, 0, 0);	\
    		DEBUGG(/*translation_log_fp, */"%08x: %08x %s\n", pc, 		\
			psxRegs.code, disasm_buffer);   			\

#define DISASM_HOST								\
	DEBUGG(/*translation_log_fp, */"\n");                                      \
  	for(	current_translation_ptr = (u8*)recMemStart;            	    	\
   		(u32)current_translation_ptr < (u32)recMem; 			\
		current_translation_ptr += 4)  					\
  	{                                                                       \
    		opcode = *(u32*)current_translation_ptr;			\
		disasm_mips_instruction(opcode, disasm_buffer,                   \
     			(u32)current_translation_ptr, stub_labels,		\
			num_stub_labels);     			        	\
    		DEBUGG(/*translation_log_fp, */"%08x: %s\t(0x%08x)\n", 			\
			current_translation_ptr, disasm_buffer, opcode);	        \
  	}                                                                       \
                                                                              	\
  	DEBUGG(/*translation_log_fp, */"\n");                                      \
 	/*fflush(translation_log_fp);*/                                             \
	/*gp2x_sync();*/								\
  	/*fclose(translation_log_fp);*/						\

#else

#define DISASM_PSX								\

#define DISASM_HOST								\

#define DISASM_INIT								\

#endif

#define REC_FUNC_TEST(f) 														\
extern void psx##f(); 															\
void rec##f() 																	\
{ 																				\
	regClearJump();																\
	LI32(TEMP_1, pc); \
	SW(TEMP_1, PERM_REG_1, offpc); 							\
	LI32(TEMP_1, psxRegs.code); \
	SW(TEMP_1, PERM_REG_1, offcode); 							\
	CALLFunc((u32)psx##f); 														\
}

#include "gen_alu.h"     // Helpers for generating ALU opcodes
#include "opcodes.h"
#include <sys/cachectl.h>

void clear_insn_cache(void *start, void *end, int flags)
{
	cacheflush(start, (char *)end - (char *)start, ICACHE);
}

static u32 recRecompile()
{
	if ( (u32)recMem - (u32)recMemBase >= RECMEM_SIZE_MAX )
		recReset();

	recMem = (u32*)(((u32)recMem + 64) & ~(63));
	recMemStart = recMem;

	regReset();

	blockcycles = 0;

	PC_REC32(psxRegs.pc) = (u32)recMem;
	oldpc = pc = psxRegs.pc;
	ibranch = 0;

	DISASM_INIT

	if( isInBios )
	{
		if( isInBios == 1 )
		{
			isInBios = 2;
			PUSH(MIPSREG_RA);
			PUSH(MIPSREG_S8);
			PUSH(MIPSREG_S7);
			PUSH(MIPSREG_S6);
			PUSH(MIPSREG_S5);
			PUSH(MIPSREG_S4);
			PUSH(MIPSREG_S3);
			PUSH(MIPSREG_S2);
			PUSH(MIPSREG_S1);
			PUSH(MIPSREG_S0);
		}
		else if( isInBios == 2 && psxRegs.pc == 0x80030000 )
		{
			PC_REC32(psxRegs.pc) = 0;
			isInBios = 0;
			POP(MIPSREG_S0);
			POP(MIPSREG_S1);
			POP(MIPSREG_S2);
			POP(MIPSREG_S3);
			POP(MIPSREG_S4);
			POP(MIPSREG_S5);
			POP(MIPSREG_S6);
			POP(MIPSREG_S7);
			POP(MIPSREG_S8);
			POP(MIPSREG_RA);
			write32(0x00000008 | (MIPSREG_RA << 21)); /* jr ra */
			write32(0);
			clear_insn_cache(recMemStart, recMem, 0);
			return (u32)recMemStart;
		}
	}

	rec_recompile_start();
	memset(iRegs, 0xff, 32*4);

	for (;;)
	{
		psxRegs.code = *(u32 *)((char *)PSXM(pc));
		DISASM_PSX
		pc+=4;
		recBSC[psxRegs.code>>26]();
		regUpdate();
		branch = 0;
		if (end_block)
		{
			end_block = 0;
			rec_recompile_end();
			DISASM_HOST
			clear_insn_cache(recMemStart, recMem, 0);
			return (u32)recMemStart;
		}
	}

	return (u32)recMemStart;
}

static int recInit()
{
	int i;

	recMem = (u32*)recMemBase;
	memset(recMem, 0, RECMEM_SIZE + (REC_MAX_OPCODES*2) + 0x4000);
	loadedpermregs = 0;
	recReset();

	//recRAM = (char*) malloc(0x200000);
	//recROM = (char*) malloc(0x080000);
	if (recRAM == NULL || recROM == NULL || recMemBase == NULL || psxRecLUT == NULL) {
		printf("Error allocating memory\n"); return -1;
	}

	for (i=0; i<0x80; i++) psxRecLUT[i + 0x0000] = (u32)&recRAM[(i & 0x1f) << 16];
	memcpy(psxRecLUT + 0x8000, psxRecLUT, 0x80 * 4);
	memcpy(psxRecLUT + 0xa000, psxRecLUT, 0x80 * 4);

	for (i=0; i<0x08; i++) psxRecLUT[i + 0xbfc0] = (u32)&recROM[i << 16];

	return 0;
}

static void recShutdown() { }

static void recExecute()
{
	loadedpermregs = 0;

	void (**recFunc)() = (void (**)()) (u32)PC_REC(psxRegs.pc);

	if (*recFunc == 0) 
		recRecompile();

	(*recFunc)();
}

static void recExecuteBlock(unsigned target_pc)
{
	isInBios = 1;
	loadedpermregs = 0;	

	void (**recFunc)() = (void (**)()) (u32)PC_REC(psxRegs.pc);

	if (*recFunc == 0) 
		recRecompile();

	(*recFunc)();
}

static void recClear(u32 Addr, u32 Size)
{
	//memset((u32*)PC_REC(Addr), 0, (Size * 4));
	//memset(recRAM, 0, 0x200000);
	//printf("addr %x\n", Addr);

	memset((u32*)PC_REC(Addr), 0, (Size * 4));
	//memset(recRAM+0x0, 0, 0x200000);

	if( Addr == 0x8003d000 )
	{
		// temp fix for Buster Bros Collection and etc.
		memset(recRAM+0x4d88, 0, 0x8);
		//recReset();
		//memset(recRAM, 0, 0x200000);
		//memset(&recRAM[0x1362<<1], 0, (0x4));
	}
}

static void recReset()
{
	memset(recRAM, 0, 0x200000);
	memset(recROM, 0, 0x080000);

	//memset((void*)recMemBase, 0, RECMEM_SIZE);

	recMem = (u32*)recMemBase;

	regReset();

	branch = 0;	
	end_block = 0;
}

R3000Acpu psxRec =
{
	recInit,
	recReset,
	recExecute,
	recExecuteBlock,
	recClear,
	recShutdown
};
