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

/* Page size selectors understood by lv2's page_allocate. Zero is not a size at
   all -- ps3mapi branches on it and calls page_allocate_auto instead, which is
   what the payload itself uses everywhere. Asking for a size smaller than the
   page size is not a combination anything in the payload ever performs. */
#define PS3MAPI_PAGE_SIZE_AUTO              0x000
#define PS3MAPI_PAGE_SIZE_4K                0x100
#define PS3MAPI_PAGE_SIZE_64K               0x200
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

/* Off unless something deliberately turns it on. page_allocate is not a call
   that fails politely: a bad request takes lv2 down with it and the console
   has to be power-cycled, so it must never be reached on an ordinary boot. */
static int execMemEnabled = 0;

void ExecMem_SetEnabled(int enabled)
{
	execMemEnabled = enabled;
}

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

	if(!execMemEnabled) return NULL;

	for(i = 0; i < MAX_EXEC_BLOCKS; ++i){
		if(!execBlocks[i].addr){ slot = i; break; }
	}
	if(slot < 0) return NULL;

	if(!ExecMem_Available()) return NULL;

	execBlocks[slot].table[0] = 0;
	execBlocks[slot].table[1] = 0;

	if(ps3mapi_page_allocate(sysProcessGetPid(), (uint64_t)size,
	                         PS3MAPI_PAGE_SIZE_AUTO, PS3MAPI_PAGE_FLAGS,
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

/* The probe sweep. A bad page_allocate does not return an error, it takes lv2
   down and costs a power cycle, so the parameters cannot be explored within a
   single run. Each entry is tried on its own boot instead, and the log records
   which ones have already been attempted so the next boot resumes past them.

   Ordered so the cheapest diagnosis comes first. Case 0 asks for ordinary
   non-executable memory: if even that hangs, page_allocate itself is unusable
   here and the executable flag is irrelevant. */
static const struct {
	const char*  what;
	unsigned int size;
	uint64_t     pageSize;
	uint64_t     executable;
} probes[] = {
	{ "1M, page_size=auto, non-executable (control)", 0x100000, PS3MAPI_PAGE_SIZE_AUTO, 0 },
	{ "1M, page_size=auto, executable",               0x100000, PS3MAPI_PAGE_SIZE_AUTO, 1 },
	{ "1M, page_size=1M, executable",                 0x100000, PS3MAPI_PAGE_SIZE_1M,   1 },
	{ "64K, page_size=auto, executable",              0x010000, PS3MAPI_PAGE_SIZE_AUTO, 1 },
	{ "64K, page_size=64K, executable",               0x010000, PS3MAPI_PAGE_SIZE_64K,  1 },
};
#define NUM_PROBES ((int)(sizeof(probes)/sizeof(probes[0])))

/* How many probes previous boots already started, counted from the log. */
static int probes_already_attempted(const char* logPath)
{
	FILE* lf = fopen(logPath, "r");
	char  line[256];
	int   n = 0;

	if(!lf) return 0;
	while(fgets(line, sizeof(line), lf))
		if(!strncmp(line, "ATTEMPT ", 8)) ++n;
	fclose(lf);
	return n;
}

int ExecMem_SelfTest(const char* logPath)
{
	FILE*    lf;
	uint64_t table[2];
	void*    mem;
	int      version, idx, ret;

	idx = probes_already_attempted(logPath);

	lf = fopen(logPath, "a");
	if(!lf) return -1;

	if(idx == 0){
		version = ps3mapi_get_core_version();
		fprintf(lf, "ps3mapi core version: 0x%04x (need >= 0x%04x)\n",
		        version, PS3MAPI_CORE_MINVERSION);
		fflush(lf);

		if(version < PS3MAPI_CORE_MINVERSION){
			fprintf(lf, "RESULT: ps3mapi unavailable, dynarec cannot work here\n");
			fclose(lf);
			return -1;
		}
	}

	if(idx >= NUM_PROBES){
		fprintf(lf, "all %d probes attempted; delete this log to run them again\n",
		        NUM_PROBES);
		fclose(lf);
		return -1;
	}

	/* Written before the call, and flushed, so that a hang still says which
	   combination caused it. */
	fprintf(lf, "ATTEMPT %d: %s\n", idx, probes[idx].what);
	fprintf(lf, "  (if the log stops here, this combination hung lv2)\n");
	fflush(lf);

	table[0] = table[1] = 0;
	ret = ps3mapi_page_allocate(sysProcessGetPid(), (uint64_t)probes[idx].size,
	                            probes[idx].pageSize, PS3MAPI_PAGE_FLAGS,
	                            probes[idx].executable, table);

	fprintf(lf, "  returned %d, page_table = { 0x%llx, 0x%llx }\n",
	        ret, (unsigned long long)table[0], (unsigned long long)table[1]);
	fflush(lf);

	if(ret != 0 || table[0] == 0){
		fprintf(lf, "  allocation failed, but survived -- next boot tries probe %d\n",
		        idx + 1);
		fclose(lf);
		return -1;
	}

	mem = (void*)(uint32_t)table[0];

	if(!probes[idx].executable){
		/* Control case. Only prove the mapping is writable; calling into
		   memory that was never asked to be executable proves nothing. */
		*(volatile unsigned int*)mem = 0x12345678;
		fprintf(lf, "  wrote and read back 0x%08x\n",
		        *(volatile unsigned int*)mem);
		fprintf(lf, "  RESULT: page_allocate itself works\n");
	} else {
		/* A single blr: return immediately to the caller. If executable
		   memory works this is the most trivial possible call. */
		*(unsigned int*)mem = 0x4E800020;
		execmem_flush(mem, 4);
		fprintf(lf, "  wrote blr, caches flushed; about to call\n");
		fprintf(lf, "  (if the log stops here, the memory is NOT executable)\n");
		fflush(lf);

		((void (*)(void))mem)();

		fprintf(lf, "  RESULT: executable memory WORKS with this combination\n");
	}
	fflush(lf);

	ps3mapi_page_free(sysProcessGetPid(), PS3MAPI_PAGE_FLAGS, table);
	fprintf(lf, "  freed ok\n");
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
