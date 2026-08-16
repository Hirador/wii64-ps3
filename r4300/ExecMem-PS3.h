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

/* Arm the allocator. It is disabled until this is called with a non-zero
   argument, and ExecMem_Alloc just returns NULL. page_allocate is not a call
   that fails politely -- a request lv2 dislikes hangs the console hard enough
   to need a power cycle -- so nothing may reach it on an ordinary boot. */
void  ExecMem_SetEnabled(int enabled);

/* Allocate `size` bytes of readable, writable and executable memory.
   Returns NULL if the allocator is disabled, PS3MAPI is missing, or the
   allocation fails. */
void* ExecMem_Alloc(unsigned int size);

/* Release a block obtained from ExecMem_Alloc. */
void  ExecMem_Free(void* ptr);

/* Whether an executable allocation can be expected to succeed, i.e. whether a
   PS3MAPI core of at least the required version is present. Safe to call
   before ExecMem_Alloc to decide whether to offer the dynarec at all. */
int   ExecMem_Available(void);

/* Probe whether PS3MAPI will hand back memory that can actually be executed,
   without involving the recompiler at all.

   A bad page_allocate does not report an error, it hangs lv2, so the parameter
   combinations cannot be swept inside one run. Each call tries exactly one and
   appends the result to `logPath`, flushing before the dangerous step; the
   next call reads back how many were already attempted and resumes past them.
   So the sweep costs one boot per combination, and a hang still leaves a log
   naming the combination that caused it. Delete the log to start over.

   Returns 0 if this run's combination worked. */
int   ExecMem_SelfTest(const char* logPath);

#ifdef __cplusplus
}
#endif

#endif
