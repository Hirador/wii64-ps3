/**
 * wii64-ps3 - ExecMem-PS3.h
 *
 * Executable memory for the dynamic recompiler on PS3.
 *
 * The recompiler emits PowerPC into a buffer and branches into it. On the Wii
 * that buffer was ordinary heap memory, but PS3 GameOS maps the process heap
 * non-executable, so the same code locks the console the moment it jumps into
 * a recompiled block. That is the "we were unable to execute from heap (no
 * dynarec)" limitation the original port stopped at.
 *
 * PS3MAPI gained a way around this in late 2022. Syscall 8 opcode 0x0033
 * (PROC_PAGE_ALLOCATE) takes an is_executable flag; when set, lv2 temporarily
 * patches the mmapper page-attribute constant from 0x4000 to 0x4004 before
 * exporting the page into the calling process, handing back a genuinely
 * executable mapping. See ps3mapi_core.c in the PS3HEN payload.
 *
 * This needs Cobra/Mamba 8.4+ or PS3HEN, which is not a real restriction --
 * you already need one of them to run homebrew at all. When PS3MAPI is
 * unavailable the allocation fails cleanly so the caller can fall back to the
 * interpreter rather than locking up.
 */

#ifndef EXECMEM_PS3_H
#define EXECMEM_PS3_H

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate `size` bytes of readable, writable and executable memory.
   Returns NULL if PS3MAPI is missing or the allocation fails. */
void* ExecMem_Alloc(unsigned int size);

/* Release a block obtained from ExecMem_Alloc. */
void  ExecMem_Free(void* ptr);

/* Whether an executable allocation can be expected to succeed, i.e. whether a
   PS3MAPI core of at least the required version is present. Safe to call
   before ExecMem_Alloc to decide whether to offer the dynarec at all. */
int   ExecMem_Available(void);

/* Self-test: allocate a page, write a single `blr` into it, flush the caches
   and call it. Answers the one question the dynarec cannot -- whether memory
   from ExecMem_Alloc is genuinely executable -- without involving the
   recompiler at all.

   Writes its progress to `logPath` line by line, flushing after each step, so
   that if the call locks the console the log still shows exactly how far it
   reached. Returns 0 if the call returned normally. */
int   ExecMem_SelfTest(const char* logPath);

#ifdef __cplusplus
}
#endif

#endif
