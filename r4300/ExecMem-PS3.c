/**
 * wii64-ps3 - ExecMem-PS3.c
 *
 * See ExecMem-PS3.h for why this exists.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ppu-lv2.h>
#include <sys/process.h>

#include "ExecMem-PS3.h"

/* PS3MAPI, reached through syscall 8. Opcodes and the minimum core version are
   taken from the PS3HEN payload (ps3mapi_core.c / main.c). */
#define SYSCALL8_OPCODE_PS3MAPI             0x7777
#define PS3MAPI_OPCODE_GET_CORE_VERSION     0x0011
#define PS3MAPI_OPCODE_PROC_PAGE_ALLOCATE   0x0033
#define PS3MAPI_OPCODE_PROC_PAGE_FREE       0x0034
#define PS3MAPI_CORE_MINVERSION             0x0120

/* Page size selector understood by lv2's page_allocate. */
#define PS3MAPI_PAGE_SIZE_1M                0x400

/* Allocation flags. 0x2F is what the payload itself passes for every
   page_allocate it performs. */
#define PS3MAPI_PAGE_FLAGS                  0x2F

/* page_table[0] is the address mapped into this process, page_table[1] the
   kernel-side buffer; freeing needs both, so the pair is kept per block. */
#define MAX_EXEC_BLOCKS 4

typedef struct {
	void*    addr;
	uint64_t table[2];
} ExecBlock;

static ExecBlock execBlocks[MAX_EXEC_BLOCKS];

static int ps3mapi_get_core_version(void)
{
	lv2syscall2(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_GET_CORE_VERSION);
	return_to_user_prog(int);
}

static int ps3mapi_page_allocate(uint32_t pid, uint64_t size, uint64_t page_size,
                                 uint64_t flags, uint64_t is_executable,
                                 uint64_t* page_table)
{
	lv2syscall8(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_PROC_PAGE_ALLOCATE,
	            (uint64_t)pid, size, page_size, flags, is_executable,
	            (uint64_t)(uint32_t)page_table);
	return_to_user_prog(int);
}

static int ps3mapi_page_free(uint32_t pid, uint64_t flags, uint64_t* page_table)
{
	lv2syscall5(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_PROC_PAGE_FREE,
	            (uint64_t)pid, flags, (uint64_t)(uint32_t)page_table);
	return_to_user_prog(int);
}

int ExecMem_Available(void)
{
	/* On firmware without PS3MAPI syscall 8 is either absent or returns
	   something nonsensical; the version check covers both. */
	int version = ps3mapi_get_core_version();
	return (version >= PS3MAPI_CORE_MINVERSION);
}

void* ExecMem_Alloc(unsigned int size)
{
	int i, slot = -1;

	for(i = 0; i < MAX_EXEC_BLOCKS; ++i){
		if(!execBlocks[i].addr){ slot = i; break; }
	}
	if(slot < 0) return NULL;

	if(!ExecMem_Available()) return NULL;

	execBlocks[slot].table[0] = 0;
	execBlocks[slot].table[1] = 0;

	if(ps3mapi_page_allocate(sysProcessGetPid(), (uint64_t)size,
	                         PS3MAPI_PAGE_SIZE_1M, PS3MAPI_PAGE_FLAGS,
	                         1 /* is_executable */,
	                         execBlocks[slot].table) != 0){
		return NULL;
	}

	/* page_table[0] carries the address visible to this process. A zero here
	   means lv2 reported success without mapping anything, which must not be
	   handed back as a code buffer. */
	if(execBlocks[slot].table[0] == 0) return NULL;

	execBlocks[slot].addr = (void*)(uint32_t)execBlocks[slot].table[0];
	return execBlocks[slot].addr;
}

/* Cache maintenance. Code written through the data cache is invisible to the
   instruction fetcher until the data line is flushed to memory and the stale
   instruction line is thrown away, so both are required before calling into
   freshly written memory. */
static void execmem_flush(void* addr, unsigned int len)
{
	unsigned char* p   = (unsigned char*)addr;
	unsigned char* end = p + len;

	for(; p < end; p += 32)
		__asm__ __volatile__ ("dcbf 0,%0" : : "r" (p) : "memory");
	__asm__ __volatile__ ("sync");

	for(p = (unsigned char*)addr; p < end; p += 32)
		__asm__ __volatile__ ("icbi 0,%0" : : "r" (p) : "memory");
	__asm__ __volatile__ ("sync");
	__asm__ __volatile__ ("isync");
}

int ExecMem_SelfTest(const char* logPath)
{
	FILE* lf = fopen(logPath, "w");
	void* mem;
	int   version;

	if(!lf) return -1;

	version = ps3mapi_get_core_version();
	fprintf(lf, "ps3mapi core version: 0x%04x (need >= 0x%04x)\n",
	        version, PS3MAPI_CORE_MINVERSION);
	fflush(lf);

	if(version < PS3MAPI_CORE_MINVERSION){
		fprintf(lf, "RESULT: ps3mapi unavailable, dynarec cannot work here\n");
		fclose(lf);
		return -1;
	}

	mem = ExecMem_Alloc(4096);
	fprintf(lf, "ExecMem_Alloc(4096) -> %p\n", mem);
	fflush(lf);

	if(!mem){
		fprintf(lf, "RESULT: allocation failed\n");
		fclose(lf);
		return -1;
	}

	/* A single blr: return immediately to the caller. If executable memory
	   works this is the most trivial possible call. */
	*(unsigned int*)mem = 0x4E800020;
	execmem_flush(mem, 4);
	fprintf(lf, "wrote blr, caches flushed; about to call\n");
	fprintf(lf, "  (if the log stops here, the memory is NOT executable)\n");
	fflush(lf);

	((void (*)(void))mem)();

	fprintf(lf, "call returned normally\n");
	fprintf(lf, "RESULT: executable memory WORKS\n");
	fflush(lf);

	ExecMem_Free(mem);
	fprintf(lf, "freed ok\n");
	fclose(lf);
	return 0;
}

void ExecMem_Free(void* ptr)
{
	int i;

	if(!ptr) return;

	for(i = 0; i < MAX_EXEC_BLOCKS; ++i){
		if(execBlocks[i].addr == ptr){
			ps3mapi_page_free(sysProcessGetPid(), PS3MAPI_PAGE_FLAGS,
			                  execBlocks[i].table);
			execBlocks[i].addr = NULL;
			execBlocks[i].table[0] = 0;
			execBlocks[i].table[1] = 0;
			return;
		}
	}
}
