/*
 *
 * vcomp implementation
 *
 * Copyright 2011 Austin English
 * Copyright 2012 Dan Kegel
 * Copyright 2015-2016 Sebastian Lackner
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <assert.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/list.h"
#include "wine/asm.h"

WINE_DEFAULT_DEBUG_CHANNEL(vcomp);

typedef CRITICAL_SECTION *omp_lock_t;
typedef CRITICAL_SECTION *omp_nest_lock_t;

static struct list vcomp_idle_threads = LIST_INIT(vcomp_idle_threads);
static DWORD   vcomp_context_tls = TLS_OUT_OF_INDEXES;
static HMODULE vcomp_module;
static int     vcomp_max_threads;
static int     vcomp_num_threads;
static BOOL    vcomp_nested_fork = FALSE;

static RTL_CRITICAL_SECTION vcomp_section;
static RTL_CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &vcomp_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": vcomp_section") }
};
static RTL_CRITICAL_SECTION vcomp_section = { &critsect_debug, -1, 0, 0, 0, 0 };

#define VCOMP_DYNAMIC_FLAGS_STATIC      0x01
#define VCOMP_DYNAMIC_FLAGS_CHUNKED     0x02
#define VCOMP_DYNAMIC_FLAGS_GUIDED      0x03
#define VCOMP_DYNAMIC_FLAGS_INCREMENT   0x40

struct vcomp_thread_data
{
    struct vcomp_team_data  *team;
    struct vcomp_task_data  *task;
    int                     thread_num;
    BOOL                    parallel;
    int                     fork_threads;

    /* only used for concurrent tasks */
    struct list             entry;
    CONDITION_VARIABLE      cond;

    /* single */
    unsigned int            single;

    /* section */
    unsigned int            section;

    /* dynamic */
    unsigned int            dynamic;
    unsigned int            dynamic_type;
    unsigned int            dynamic_begin;
    unsigned int            dynamic_end;
};

struct vcomp_team_data
{
    CONDITION_VARIABLE      cond;
    int                     num_threads;
    int                     finished_threads;

    /* callback arguments */
    int                     nargs;
    void                    *wrapper;
    __ms_va_list            valist;

    /* barrier */
    unsigned int            barrier;
    int                     barrier_count;
};

struct vcomp_task_data
{
    /* single */
    unsigned int            single;

    /* section */
    unsigned int            section;
    int                     num_sections;
    int                     section_index;

    /* dynamic */
    unsigned int            dynamic;
    unsigned int            dynamic_first;
    unsigned int            dynamic_last;
    unsigned int            dynamic_iterations;
    int                     dynamic_step;
    unsigned int            dynamic_chunksize;
};

#if defined(__i386__)

extern void CDECL _vcomp_fork_call_wrapper(void *wrapper, int nargs, __ms_va_list args);
__ASM_GLOBAL_FUNC( _vcomp_fork_call_wrapper,
                   "pushl %ebp\n\t"
                   __ASM_CFI(".cfi_adjust_cfa_offset 4\n\t")
                   __ASM_CFI(".cfi_rel_offset %ebp,0\n\t")
                   "movl %esp,%ebp\n\t"
                   __ASM_CFI(".cfi_def_cfa_register %ebp\n\t")
                   "pushl %esi\n\t"
                   __ASM_CFI(".cfi_rel_offset %esi,-4\n\t")
                   "pushl %edi\n\t"
                   __ASM_CFI(".cfi_rel_offset %edi,-8\n\t")
                   "movl 12(%ebp),%edx\n\t"
                   "movl %esp,%edi\n\t"
                   "shll $2,%edx\n\t"
                   "jz 1f\n\t"
                   "subl %edx,%edi\n\t"
                   "andl $~15,%edi\n\t"
                   "movl %edi,%esp\n\t"
                   "movl 12(%ebp),%ecx\n\t"
                   "movl 16(%ebp),%esi\n\t"
                   "cld\n\t"
                   "rep; movsl\n"
                   "1:\tcall *8(%ebp)\n\t"
                   "leal -8(%ebp),%esp\n\t"
                   "popl %edi\n\t"
                   __ASM_CFI(".cfi_same_value %edi\n\t")
                   "popl %esi\n\t"
                   __ASM_CFI(".cfi_same_value %esi\n\t")
                   "popl %ebp\n\t"
                   __ASM_CFI(".cfi_def_cfa %esp,4\n\t")
                   __ASM_CFI(".cfi_same_value %ebp\n\t")
                   "ret" )

#elif defined(__x86_64__)

extern void CDECL _vcomp_fork_call_wrapper(void *wrapper, int nargs, __ms_va_list args);
__ASM_GLOBAL_FUNC( _vcomp_fork_call_wrapper,
                   "pushq %rbp\n\t"
                   __ASM_SEH(".seh_pushreg %rbp\n\t")
                   __ASM_CFI(".cfi_adjust_cfa_offset 8\n\t")
                   __ASM_CFI(".cfi_rel_offset %rbp,0\n\t")
                   "movq %rsp,%rbp\n\t"
                   __ASM_SEH(".seh_setframe %rbp,0\n\t")
                   __ASM_CFI(".cfi_def_cfa_register %rbp\n\t")
                   "pushq %rsi\n\t"
                   __ASM_SEH(".seh_pushreg %rsi\n\t")
                   __ASM_CFI(".cfi_rel_offset %rsi,-8\n\t")
                   "pushq %rdi\n\t"
                   __ASM_SEH(".seh_pushreg %rdi\n\t")
                   __ASM_SEH(".seh_endprologue\n\t")
                   __ASM_CFI(".cfi_rel_offset %rdi,-16\n\t")
                   "movq %rcx,%rax\n\t"
                   "movq $4,%rcx\n\t"
                   "cmp %rcx,%rdx\n\t"
                   "cmovgq %rdx,%rcx\n\t"
                   "leaq 0(,%rcx,8),%rdx\n\t"
                   "subq %rdx,%rsp\n\t"
                   "andq $~15,%rsp\n\t"
                   "movq %rsp,%rdi\n\t"
                   "movq %r8,%rsi\n\t"
                   "rep; movsq\n\t"
                   "movq 0(%rsp),%rcx\n\t"
                   "movq 8(%rsp),%rdx\n\t"
                   "movq 16(%rsp),%r8\n\t"
                   "movq 24(%rsp),%r9\n\t"
                   "callq *%rax\n\t"
                   "leaq -16(%rbp),%rsp\n\t"
                   "popq %rdi\n\t"
                   __ASM_CFI(".cfi_same_value %rdi\n\t")
                   "popq %rsi\n\t"
                   __ASM_CFI(".cfi_same_value %rsi\n\t")
                   __ASM_CFI(".cfi_def_cfa_register %rsp\n\t")
                   "popq %rbp\n\t"
                   __ASM_CFI(".cfi_adjust_cfa_offset -8\n\t")
                   __ASM_CFI(".cfi_same_value %rbp\n\t")
                   "ret")

#elif defined(__arm__)

extern void CDECL _vcomp_fork_call_wrapper(void *wrapper, int nargs, __ms_va_list args);
__ASM_GLOBAL_FUNC( _vcomp_fork_call_wrapper,
                   ".arm\n\t"
                   "push {r4, r5, LR}\n\t"
                   "mov r4, r0\n\t"
                   "mov r5, SP\n\t"
                   "lsl r3, r1, #2\n\t"
                   "cmp r3, #0\n\t"
                   "beq 5f\n\t"
                   "sub SP, SP, r3\n\t"
                   "tst r1, #1\n\t"
                   "subeq SP, SP, #4\n\t"
                   "1:\tsub r3, r3, #4\n\t"
                   "ldr r0, [r2, r3]\n\t"
                   "str r0, [SP, r3]\n\t"
                   "cmp r3, #0\n\t"
                   "bgt 1b\n\t"
                   "cmp r1, #1\n\t"
                   "bgt 2f\n\t"
                   "pop {r0}\n\t"
                   "b 5f\n\t"
                   "2:\tcmp r1, #2\n\t"
                   "bgt 3f\n\t"
                   "pop {r0-r1}\n\t"
                   "b 5f\n\t"
                   "3:\tcmp r1, #3\n\t"
                   "bgt 4f\n\t"
                   "pop {r0-r2}\n\t"
                   "b 5f\n\t"
                   "4:\tpop {r0-r3}\n\t"
                   "5:\tblx r4\n\t"
                   "mov SP, r5\n\t"
                   "pop {r4, r5, PC}" )

#elif defined(__aarch64__)

extern void CDECL _vcomp_fork_call_wrapper(void *wrapper, int nargs, __ms_va_list args);
__ASM_GLOBAL_FUNC( _vcomp_fork_call_wrapper,
                   "stp x29, x30, [SP,#-16]!\n\t"
                   "mov x29, SP\n\t"
                   "mov x9, x0\n\t"
                   "cbz w1, 2f\n\t"
                   "mov w10, w1\n\t"
                   "mov x11, x2\n\t"
                   "ldr w12, [x11, #24]\n\t"
                   "ldr x13, [x11, #8]\n\t"
                   "ldr x0, [x13, w12, sxtw]\n\t"
                   "add w12, w12, #8\n\t"
                   "ldr x1, [x13, w12, sxtw]\n\t"
                   "add w12, w12, #8\n\t"
                   "ldr x2, [x13, w12, sxtw]\n\t"
                   "add w12, w12, #8\n\t"
                   "ldr x3, [x13, w12, sxtw]\n\t"
                   "add w12, w12, #8\n\t"
                   "ldr x4, [x13, w12, sxtw]\n\t"
                   "add w12, w12, #8\n\t"
                   "ldr x5, [x13, w12, sxtw]\n\t"
                   "add w12, w12, #8\n\t"
                   "ldr x6, [x13, w12, sxtw]\n\t"
                   "add w12, w12, #8\n\t"
                   "ldr x7, [x13, w12, sxtw]\n\t"
                   "add w12, w12, #8\n\t"
                   "add x13, x13, w12, sxtw\n\t"
                   "subs w12, w10, #8\n\t"
                   "b.le 2f\n\t"
                   "ldr x11, [x11]\n\t"
                   "lsl w12, w12, #3\n\t"
                   "sub SP, SP, w12, sxtw\n\t"
                   "tbz w12, #3, 1f\n\t"
                   "sub SP, SP, #8\n\t"
                   "1: sub w12, w12, #8\n\t"
                   "ldr x14, [x13, w12, sxtw]\n\t"
                   "str x14, [SP,  w12, sxtw]\n\t"
                   "cbnz w12, 1b\n\t"
                   "2: blr x9\n\t"
                   "mov SP, x29\n\t"
                   "ldp x29, x30, [SP], #16\n\t"
                   "ret\n" )

#else

static void CDECL _vcomp_fork_call_wrapper(void *wrapper, int nargs, __ms_va_list args)
{
    ERR("Not implemented for this architecture\n");
}

#endif

#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))

static inline char interlocked_cmpxchg8(char *dest, char xchg, char compare)
{
    char ret;
    __asm__ __volatile__( "lock; cmpxchgb %2,(%1)"
                          : "=a" (ret) : "r" (dest), "q" (xchg), "0" (compare) : "memory" );
    return ret;
}

static inline short interlocked_cmpxchg16(short *dest, short xchg, short compare)
{
    short ret;
    __asm__ __volatile__( "lock; cmpxchgw %2,(%1)"
                          : "=a" (ret) : "r" (dest), "r" (xchg), "0" (compare) : "memory" );
    return ret;
}

static inline char interlocked_xchg_add8(char *dest, char incr)
{
    char ret;
    __asm__ __volatile__( "lock; xaddb %0,(%1)"
                          : "=q" (ret) : "r" (dest), "0" (incr) : "memory" );
    return ret;
}

static inline short interlocked_xchg_add16(short *dest, short incr)
{
    short ret;
    __asm__ __volatile__( "lock; xaddw %0,(%1)"
                          : "=r" (ret) : "r" (dest), "0" (incr) : "memory" );
    return ret;
}

#else  /* __GNUC__ */

#ifdef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_1
static inline char interlocked_cmpxchg8(char *dest, char xchg, char compare)
{
    return __sync_val_compare_and_swap(dest, compare, xchg);
}

static inline char interlocked_xchg_add8(char *dest, char incr)
{
    return __sync_fetch_and_add(dest, incr);
}
#else
static char interlocked_cmpxchg8(char *dest, char xchg, char compare)
{
    EnterCriticalSection(&vcomp_section);
    if (*dest == compare) *dest = xchg; else compare = *dest;
    LeaveCriticalSection(&vcomp_section);
    return compare;
}

static char interlocked_xchg_add8(char *dest, char incr)
{
    char ret;
    EnterCriticalSection(&vcomp_section);
    ret = *dest; *dest += incr;
    LeaveCriticalSection(&vcomp_section);
    return ret;
}
#endif

#ifdef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_2
static inline short interlocked_cmpxchg16(short *dest, short xchg, short compare)
{
    return __sync_val_compare_and_swap(dest, compare, xchg);
}

static inline short interlocked_xchg_add16(short *dest, short incr)
{
    return __sync_fetch_and_add(dest, incr);
}
#else
static short interlocked_cmpxchg16(short *dest, short xchg, short compare)
{
    EnterCriticalSection(&vcomp_section);
    if (*dest == compare) *dest = xchg; else compare = *dest;
    LeaveCriticalSection(&vcomp_section);
    return compare;
}

static short interlocked_xchg_add16(short *dest, short incr)
{
    short ret;
    EnterCriticalSection(&vcomp_section);
    ret = *dest; *dest += incr;
    LeaveCriticalSection(&vcomp_section);
    return ret;
}
#endif

#endif  /* __GNUC__ */

static inline struct vcomp_thread_data *vcomp_get_thread_data(void)
{
    return (struct vcomp_thread_data *)TlsGetValue(vcomp_context_tls);
}

static inline void vcomp_set_thread_data(struct vcomp_thread_data *thread_data)
{
    TlsSetValue(vcomp_context_tls, thread_data);
}

static struct vcomp_thread_data *vcomp_init_thread_data(void)
{
    struct vcomp_thread_data *thread_data = vcomp_get_thread_data();
    struct
    {
        struct vcomp_thread_data thread;
        struct vcomp_task_data   task;
    } *data;

    if (thread_data) return thread_data;
    if (!(data = HeapAlloc(GetProcessHeap(), 0, sizeof(*data))))
    {
        ERR("could not create thread data\n");
        ExitProcess(1);
    }

    data->task.single           = 0;
    data->task.section          = 0;
    data->task.dynamic          = 0;

    thread_data = &data->thread;
    thread_data->team           = NULL;
    thread_data->task           = &data->task;
    thread_data->thread_num     = 0;
    thread_data->parallel       = FALSE;
    thread_data->fork_threads   = 0;
    thread_data->single         = 1;
    thread_data->section        = 1;
    thread_data->dynamic        = 1;
    thread_data->dynamic_type   = 0;

    vcomp_set_thread_data(thread_data);
    return thread_data;
}

static void vcomp_free_thread_data(void)
{
    struct vcomp_thread_data *thread_data = vcomp_get_thread_data();
    if (!thread_data) return;

    HeapFree(GetProcessHeap(), 0, thread_data);
    vcomp_set_thread_data(NULL);
}

void CDECL _vcomp_atomic_add_i1(char *dest, char val)
{
    interlocked_xchg_add8(dest, val);
}

void CDECL _vcomp_atomic_and_i1(char *dest, char val)
{
    char old;
    do old = *dest; while (interlocked_cmpxchg8(dest, old & val, old) != old);
}

void CDECL _vcomp_atomic_div_i1(char *dest, char val)
{
    char old;
    do old = *dest; while (interlocked_cmpxchg8(dest, old / val, old) != old);
}

void CDECL _vcomp_atomic_div_ui1(unsigned char *dest, unsigned char val)
{
    unsigned char old;
    do old = *dest; while ((unsigned char)interlocked_cmpxchg8((char *)dest, old / val, old) != old);
}

void CDECL _vcomp_atomic_mul_i1(char *dest, char val)
{
    char old;
    do old = *dest; while (interlocked_cmpxchg8(dest, old * val, old) != old);
}

void CDECL _vcomp_atomic_or_i1(char *dest, char val)
{
    char old;
    do old = *dest; while (interlocked_cmpxchg8(dest, old | val, old) != old);
}

void CDECL _vcomp_atomic_shl_i1(char *dest, unsigned int val)
{
    char old;
    do old = *dest; while (interlocked_cmpxchg8(dest, old << val, old) != old);
}

void CDECL _vcomp_atomic_shr_i1(char *dest, unsigned int val)
{
    char old;
    do old = *dest; while (interlocked_cmpxchg8(dest, old >> val, old) != old);
}

void CDECL _vcomp_atomic_shr_ui1(unsigned char *dest, unsigned int val)
{
    unsigned char old;
    do old = *dest; while ((unsigned char)interlocked_cmpxchg8((char *)dest, old >> val, old) != old);
}

void CDECL _vcomp_atomic_sub_i1(char *dest, char val)
{
    interlocked_xchg_add8(dest, -val);
}

void CDECL _vcomp_atomic_xor_i1(char *dest, char val)
{
    char old;
    do old = *dest; while (interlocked_cmpxchg8(dest, old ^ val, old) != old);
}

static void CDECL _vcomp_atomic_bool_and_i1(char *dest, char val)
{
    char old;
    do old = *dest; while (interlocked_cmpxchg8(dest, old && val, old) != old);
}

static void CDECL _vcomp_atomic_bool_or_i1(char *dest, char val)
{
    char old;
    do old = *dest; while (interlocked_cmpxchg8(dest, old ? old : (val != 0), old) != old);
}

void CDECL _vcomp_reduction_i1(unsigned int flags, char *dest, char val)
{
    static void (CDECL * const funcs[])(char *, char) =
    {
        _vcomp_atomic_add_i1,
        _vcomp_atomic_add_i1,
        _vcomp_atomic_mul_i1,
        _vcomp_atomic_and_i1,
        _vcomp_atomic_or_i1,
        _vcomp_atomic_xor_i1,
        _vcomp_atomic_bool_and_i1,
        _vcomp_atomic_bool_or_i1,
    };
    unsigned int op = (flags >> 8) & 0xf;
    op = min(op, ARRAY_SIZE(funcs) - 1);
    funcs[op](dest, val);
}

void CDECL _vcomp_atomic_add_i2(short *dest, short val)
{
    interlocked_xchg_add16(dest, val);
}

void CDECL _vcomp_atomic_and_i2(short *dest, short val)
{
    short old;
    do old = *dest; while (interlocked_cmpxchg16(dest, old & val, old) != old);
}

void CDECL _vcomp_atomic_div_i2(short *dest, short val)
{
    short old;
    do old = *dest; while (interlocked_cmpxchg16(dest, old / val, old) != old);
}

void CDECL _vcomp_atomic_div_ui2(unsigned short *dest, unsigned short val)
{
    unsigned short old;
    do old = *dest; while ((unsigned short)interlocked_cmpxchg16((short *)dest, old / val, old) != old);
}

void CDECL _vcomp_atomic_mul_i2(short *dest, short val)
{
    short old;
    do old = *dest; while (interlocked_cmpxchg16(dest, old * val, old) != old);
}

void CDECL _vcomp_atomic_or_i2(short *dest, short val)
{
    short old;
    do old = *dest; while (interlocked_cmpxchg16(dest, old | val, old) != old);
}

void CDECL _vcomp_atomic_shl_i2(short *dest, unsigned int val)
{
    short old;
    do old = *dest; while (interlocked_cmpxchg16(dest, old << val, old) != old);
}

void CDECL _vcomp_atomic_shr_i2(short *dest, unsigned int val)
{
    short old;
    do old = *dest; while (interlocked_cmpxchg16(dest, old >> val, old) != old);
}

void CDECL _vcomp_atomic_shr_ui2(unsigned short *dest, unsigned int val)
{
    unsigned short old;
    do old = *dest; while ((unsigned short)interlocked_cmpxchg16((short *)dest, old >> val, old) != old);
}

void CDECL _vcomp_atomic_sub_i2(short *dest, short val)
{
    interlocked_xchg_add16(dest, -val);
}

void CDECL _vcomp_atomic_xor_i2(short *dest, short val)
{
    short old;
    do old = *dest; while (interlocked_cmpxchg16(dest, old ^ val, old) != old);
}

static void CDECL _vcomp_atomic_bool_and_i2(short *dest, short val)
{
    short old;
    do old = *dest; while (interlocked_cmpxchg16(dest, old && val, old) != old);
}

static void CDECL _vcomp_atomic_bool_or_i2(short *dest, short val)
{
    short old;
    do old = *dest; while (interlocked_cmpxchg16(dest, old ? old : (val != 0), old) != old);
}

void CDECL _vcomp_reduction_i2(unsigned int flags, short *dest, short val)
{
    static void (CDECL * const funcs[])(short *, short) =
    {
        _vcomp_atomic_add_i2,
        _vcomp_atomic_add_i2,
        _vcomp_atomic_mul_i2,
        _vcomp_atomic_and_i2,
        _vcomp_atomic_or_i2,
        _vcomp_atomic_xor_i2,
        _vcomp_atomic_bool_and_i2,
        _vcomp_atomic_bool_or_i2,
    };
    unsigned int op = (flags >> 8) & 0xf;
    op = min(op, ARRAY_SIZE(funcs) - 1);
    funcs[op](dest, val);
}

void CDECL _vcomp_atomic_add_i4(int *dest, int val)
{
    InterlockedExchangeAdd(dest, val);
}

void CDECL _vcomp_atomic_and_i4(int *dest, int val)
{
    int old;
    do old = *dest; while (InterlockedCompareExchange(dest, old & val, old) != old);
}

void CDECL _vcomp_atomic_div_i4(int *dest, int val)
{
    int old;
    do old = *dest; while (InterlockedCompareExchange(dest, old / val, old) != old);
}

void CDECL _vcomp_atomic_div_ui4(unsigned int *dest, unsigned int val)
{
    unsigned int old;
    do old = *dest; while (InterlockedCompareExchange((int *)dest, old / val, old) != old);
}

void CDECL _vcomp_atomic_mul_i4(int *dest, int val)
{
    int old;
    do old = *dest; while (InterlockedCompareExchange(dest, old * val, old) != old);
}

void CDECL _vcomp_atomic_or_i4(int *dest, int val)
{
    int old;
    do old = *dest; while (InterlockedCompareExchange(dest, old | val, old) != old);
}

void CDECL _vcomp_atomic_shl_i4(int *dest, int val)
{
    int old;
    do old = *dest; while (InterlockedCompareExchange(dest, old << val, old) != old);
}

void CDECL _vcomp_atomic_shr_i4(int *dest, int val)
{
    int old;
    do old = *dest; while (InterlockedCompareExchange(dest, old >> val, old) != old);
}

void CDECL _vcomp_atomic_shr_ui4(unsigned int *dest, unsigned int val)
{
    unsigned int old;
    do old = *dest; while (InterlockedCompareExchange((int *)dest, old >> val, old) != old);
}

void CDECL _vcomp_atomic_sub_i4(int *dest, int val)
{
    InterlockedExchangeAdd(dest, -val);
}

void CDECL _vcomp_atomic_xor_i4(int *dest, int val)
{
    int old;
    do old = *dest; while (InterlockedCompareExchange(dest, old ^ val, old) != old);
}

static void CDECL _vcomp_atomic_bool_and_i4(int *dest, int val)
{
    int old;
    do old = *dest; while (InterlockedCompareExchange(dest, old && val, old) != old);
}

static void CDECL _vcomp_atomic_bool_or_i4(int *dest, int val)
{
    int old;
    do old = *dest; while (InterlockedCompareExchange(dest, old ? old : (val != 0), old) != old);
}

void CDECL _vcomp_reduction_i4(unsigned int flags, int *dest, int val)
{
    static void (CDECL * const funcs[])(int *, int) =
    {
        _vcomp_atomic_add_i4,
        _vcomp_atomic_add_i4,
        _vcomp_atomic_mul_i4,
        _vcomp_atomic_and_i4,
        _vcomp_atomic_or_i4,
        _vcomp_atomic_xor_i4,
        _vcomp_atomic_bool_and_i4,
        _vcomp_atomic_bool_or_i4,
    };
    unsigned int op = (flags >> 8) & 0xf;
    op = min(op, ARRAY_SIZE(funcs) - 1);
    funcs[op](dest, val);
}

void CDECL _vcomp_atomic_add_i8(LONG64 *dest, LONG64 val)
{
    LONG64 old;
    do old = *dest; while (InterlockedCompareExchange64(dest, old + val, old) != old);
}

void CDECL _vcomp_atomic_and_i8(LONG64 *dest, LONG64 val)
{
    LONG64 old;
    do old = *dest; while (InterlockedCompareExchange64(dest, old & val, old) != old);
}

void CDECL _vcomp_atomic_div_i8(LONG64 *dest, LONG64 val)
{
    LONG64 old;
    do old = *dest; while (InterlockedCompareExchange64(dest, old / val, old) != old);
}

void CDECL _vcomp_atomic_div_ui8(ULONG64 *dest, ULONG64 val)
{
    ULONG64 old;
    do old = *dest; while (InterlockedCompareExchange64((LONG64 *)dest, old / val, old) != old);
}

void CDECL _vcomp_atomic_mul_i8(LONG64 *dest, LONG64 val)
{
    LONG64 old;
    do old = *dest; while (InterlockedCompareExchange64(dest, old * val, old) != old);
}

void CDECL _vcomp_atomic_or_i8(LONG64 *dest, LONG64 val)
{
    LONG64 old;
    do old = *dest; while (InterlockedCompareExchange64(dest, old | val, old) != old);
}

void CDECL _vcomp_atomic_shl_i8(LONG64 *dest, unsigned int val)
{
    LONG64 old;
    do old = *dest; while (InterlockedCompareExchange64(dest, old << val, old) != old);
}

void CDECL _vcomp_atomic_shr_i8(LONG64 *dest, unsigned int val)
{
    LONG64 old;
    do old = *dest; while (InterlockedCompareExchange64(dest, old >> val, old) != old);
}

void CDECL _vcomp_atomic_shr_ui8(ULONG64 *dest, unsigned int val)
{
    ULONG64 old;
    do old = *dest; while (InterlockedCompareExchange64((LONG64 *)dest, old >> val, old) != old);
}

void CDECL _vcomp_atomic_sub_i8(LONG64 *dest, LONG64 val)
{
    LONG64 old;
    do old = *dest; while (InterlockedCompareExchange64(dest, old - val, old) != old);
}

void CDECL _vcomp_atomic_xor_i8(LONG64 *dest, LONG64 val)
{
    LONG64 old;
    do old = *dest; while (InterlockedCompareExchange64(dest, old ^ val, old) != old);
}

static void CDECL _vcomp_atomic_bool_and_i8(LONG64 *dest, LONG64 val)
{
    LONG64 old;
    do old = *dest; while (InterlockedCompareExchange64(dest, old && val, old) != old);
}

static void CDECL _vcomp_atomic_bool_or_i8(LONG64 *dest, LONG64 val)
{
    LONG64 old;
    do old = *dest; while (InterlockedCompareExchange64(dest, old ? old : (val != 0), old) != old);
}

void CDECL _vcomp_reduction_i8(unsigned int flags, LONG64 *dest, LONG64 val)
{
    static void (CDECL * const funcs[])(LONG64 *, LONG64) =
    {
        _vcomp_atomic_add_i8,
        _vcomp_atomic_add_i8,
        _vcomp_atomic_mul_i8,
        _vcomp_atomic_and_i8,
        _vcomp_atomic_or_i8,
        _vcomp_atomic_xor_i8,
        _vcomp_atomic_bool_and_i8,
        _vcomp_atomic_bool_or_i8,
    };
    unsigned int op = (flags >> 8) & 0xf;
    op = min(op, ARRAY_SIZE(funcs) - 1);
    funcs[op](dest, val);
}

void CDECL _vcomp_atomic_add_r4(float *dest, float val)
{
    int old, new;
    do
    {
        old = *(int *)dest;
        *(float *)&new = *(float *)&old + val;
    }
    while (InterlockedCompareExchange((int *)dest, new, old) != old);
}

void CDECL _vcomp_atomic_div_r4(float *dest, float val)
{
    int old, new;
    do
    {
        old = *(int *)dest;
        *(float *)&new = *(float *)&old / val;
    }
    while (InterlockedCompareExchange((int *)dest, new, old) != old);
}

void CDECL _vcomp_atomic_mul_r4(float *dest, float val)
{
    int old, new;
    do
    {
        old = *(int *)dest;
        *(float *)&new = *(float *)&old * val;
    }
    while (InterlockedCompareExchange((int *)dest, new, old) != old);
}

void CDECL _vcomp_atomic_sub_r4(float *dest, float val)
{
    int old, new;
    do
    {
        old = *(int *)dest;
        *(float *)&new = *(float *)&old - val;
    }
    while (InterlockedCompareExchange((int *)dest, new, old) != old);
}

static void CDECL _vcomp_atomic_bool_and_r4(float *dest, float val)
{
    int old, new;
    do
    {
        old = *(int *)dest;
        *(float *)&new = (*(float *)&old != 0.0) ? (val != 0.0) : 0.0;
    }
    while (InterlockedCompareExchange((int *)dest, new, old) != old);
}

static void CDECL _vcomp_atomic_bool_or_r4(float *dest, float val)
{
    int old, new;
    do
    {
        old = *(int *)dest;
        *(float *)&new = (*(float *)&old != 0.0) ? *(float *)&old : (val != 0.0);
    }
    while (InterlockedCompareExchange((int *)dest, new, old) != old);
}

void CDECL _vcomp_reduction_r4(unsigned int flags, float *dest, float val)
{
    static void (CDECL * const funcs[])(float *, float) =
    {
        _vcomp_atomic_add_r4,
        _vcomp_atomic_add_r4,
        _vcomp_atomic_mul_r4,
        _vcomp_atomic_bool_or_r4,
        _vcomp_atomic_bool_or_r4,
        _vcomp_atomic_bool_or_r4,
        _vcomp_atomic_bool_and_r4,
        _vcomp_atomic_bool_or_r4,
    };
    unsigned int op = (flags >> 8) & 0xf;
    op = min(op, ARRAY_SIZE(funcs) - 1);
    funcs[op](dest, val);
}

void CDECL _vcomp_atomic_add_r8(double *dest, double val)
{
    LONG64 old, new;
    do
    {
        old = *(LONG64 *)dest;
        *(double *)&new = *(double *)&old + val;
    }
    while (InterlockedCompareExchange64((LONG64 *)dest, new, old) != old);
}

void CDECL _vcomp_atomic_div_r8(double *dest, double val)
{
    LONG64 old, new;
    do
    {
        old = *(LONG64 *)dest;
        *(double *)&new = *(double *)&old / val;
    }
    while (InterlockedCompareExchange64((LONG64 *)dest, new, old) != old);
}

void CDECL _vcomp_atomic_mul_r8(double *dest, double val)
{
    LONG64 old, new;
    do
    {
        old = *(LONG64 *)dest;
        *(double *)&new = *(double *)&old * val;
    }
    while (InterlockedCompareExchange64((LONG64 *)dest, new, old) != old);
}

void CDECL _vcomp_atomic_sub_r8(double *dest, double val)
{
    LONG64 old, new;
    do
    {
        old = *(LONG64 *)dest;
        *(double *)&new = *(double *)&old - val;
    }
    while (InterlockedCompareExchange64((LONG64 *)dest, new, old) != old);
}

static void CDECL _vcomp_atomic_bool_and_r8(double *dest, double val)
{
    LONG64 old, new;
    do
    {
        old = *(LONG64 *)dest;
        *(double *)&new = (*(double *)&old != 0.0) ? (val != 0.0) : 0.0;
    }
    while (InterlockedCompareExchange64((LONG64 *)dest, new, old) != old);
}

static void CDECL _vcomp_atomic_bool_or_r8(double *dest, double val)
{
    LONG64 old, new;
    do
    {
        old = *(LONG64 *)dest;
        *(double *)&new = (*(double *)&old != 0.0) ? *(double *)&old : (val != 0.0);
    }
    while (InterlockedCompareExchange64((LONG64 *)dest, new, old) != old);
}

void CDECL _vcomp_reduction_r8(unsigned int flags, double *dest, double val)
{
    static void (CDECL * const funcs[])(double *, double) =
    {
        _vcomp_atomic_add_r8,
        _vcomp_atomic_add_r8,
        _vcomp_atomic_mul_r8,
        _vcomp_atomic_bool_or_r8,
        _vcomp_atomic_bool_or_r8,
        _vcomp_atomic_bool_or_r8,
        _vcomp_atomic_bool_and_r8,
        _vcomp_atomic_bool_or_r8,
    };
    unsigned int op = (flags >> 8) & 0xf;
    op = min(op, ARRAY_SIZE(funcs) - 1);
    funcs[op](dest, val);
}

int CDECL omp_get_dynamic(void)
{
    TRACE("stub\n");
    return 0;
}

int CDECL omp_get_max_threads(void)
{
    TRACE("()\n");
    return vcomp_max_threads;
}

int CDECL omp_get_nested(void)
{
    TRACE("stub\n");
    return vcomp_nested_fork;
}

int CDECL omp_get_num_procs(void)
{
    TRACE("stub\n");
    return 1;
}

int CDECL omp_get_num_threads(void)
{
    struct vcomp_team_data *team_data = vcomp_init_thread_data()->team;
    TRACE("()\n");
    return team_data ? team_data->num_threads : 1;
}

int CDECL omp_get_thread_num(void)
{
    TRACE("()\n");
    return vcomp_init_thread_data()->thread_num;
}

int CDECL _vcomp_get_thread_num(void)
{
    TRACE("()\n");
    return vcomp_init_thread_data()->thread_num;
}

/* Time in seconds since "some time in the past" */
double CDECL omp_get_wtime(void)
{
    return GetTickCount() / 1000.0;
}

void CDECL omp_set_dynamic(int val)
{
    TRACE("(%d): stub\n", val);
}

void CDECL omp_set_nested(int nested)
{
    TRACE("(%d)\n", nested);
    vcomp_nested_fork = (nested != 0);
}

void CDECL omp_set_num_threads(int num_threads)
{
    TRACE("(%d)\n", num_threads);
    if (num_threads >= 1)
        vcomp_num_threads = num_threads;
}

void CDECL _vcomp_flush(void)
{
    TRACE("(): stub\n");
}

void CDECL _vcomp_barrier(void)
{
    struct vcomp_team_data *team_data = vcomp_init_thread_data()->team;

    TRACE("()\n");

    if (!team_data)
        return;

    EnterCriticalSection(&vcomp_section);
    if (++team_data->barrier_count >= team_data->num_threads)
    {
        team_data->barrier++;
        team_data->barrier_count = 0;
        WakeAllConditionVariable(&team_data->cond);
    }
    else
    {
        unsigned int barrier = team_data->barrier;
        while (team_data->barrier == barrier)
            SleepConditionVariableCS(&team_data->cond, &vcomp_section, INFINITE);
    }
    LeaveCriticalSection(&vcomp_section);
}

void CDECL _vcomp_set_num_threads(int num_threads)
{
    TRACE("(%d)\n", num_threads);
    if (num_threads >= 1)
        vcomp_init_thread_data()->fork_threads = num_threads;
}

int CDECL _vcomp_master_begin(void)
{
    TRACE("()\n");
    return !vcomp_init_thread_data()->thread_num;
}

void CDECL _vcomp_master_end(void)
{
    TRACE("()\n");
    /* nothing to do here */
}

int CDECL _vcomp_single_begin(int flags)
{
    struct vcomp_thread_data *thread_data = vcomp_init_thread_data();
    struct vcomp_task_data *task_data = thread_data->task;
    int ret = FALSE;

    TRACE("(%x): semi-stub\n", flags);

    EnterCriticalSection(&vcomp_section);
    thread_data->single++;
    if ((int)(thread_data->single - task_data->single) > 0)
    {
        task_data->single = thread_data->single;
        ret = TRUE;
    }
    LeaveCriticalSection(&vcomp_section);

    return ret;
}

void CDECL _vcomp_single_end(void)
{
    TRACE("()\n");
    /* nothing to do here */
}

void CDECL _vcomp_sections_init(int n)
{
    struct vcomp_thread_data *thread_data = vcomp_init_thread_data();
    struct vcomp_task_data *task_data = thread_data->task;

    TRACE("(%d)\n", n);

    EnterCriticalSection(&vcomp_section);
    thread_data->section++;
    if ((int)(thread_data->section - task_data->section) > 0)
    {
        task_data->section       = thread_data->section;
        task_data->num_sections  = n;
        task_data->section_index = 0;
    }
    LeaveCriticalSection(&vcomp_section);
}

int CDECL _vcomp_sections_next(void)
{
    struct vcomp_thread_data *thread_data = vcomp_init_thread_data();
    struct vcomp_task_data *task_data = thread_data->task;
    int i = -1;

    TRACE("()\n");

    EnterCriticalSection(&vcomp_section);
    if (thread_data->section == task_data->section &&
        task_data->section_index != task_data->num_sections)
    {
        i = task_data->section_index++;
    }
    LeaveCriticalSection(&vcomp_section);
    return i;
}

void CDECL _vcomp_for_static_simple_init(unsigned int first, unsigned int last, int step,
                                         BOOL increment, unsigned int *begin, unsigned int *end)
{
    unsigned int iterations, per_thread, remaining;
    struct vcomp_thread_data *thread_data = vcomp_init_thread_data();
    struct vcomp_team_data *team_data = thread_data->team;
    int num_threads = team_data ? team_data->num_threads : 1;
    int thread_num = thread_data->thread_num;

    TRACE("(%u, %u, %d, %u, %p, %p)\n", first, last, step, increment, begin, end);

    if (num_threads == 1)
    {
        *begin = first;
        *end   = last;
        return;
    }

    if (step <= 0)
    {
        *begin = 0;
        *end   = increment ? -1 : 1;
        return;
    }

    if (increment)
        iterations = 1 + (last - first) / step;
    else
    {
        iterations = 1 + (first - last) / step;
        step *= -1;
    }

    per_thread = iterations / num_threads;
    remaining  = iterations - per_thread * num_threads;

    if (thread_num < remaining)
        per_thread++;
    else if (per_thread)
        first += remaining * step;
    else
    {
        *begin = first;
        *end   = first - step;
        return;
    }

    *begin = first + per_thread * thread_num * step;
    *end   = *begin + (per_thread - 1) * step;
}

void CDECL _vcomp_for_static_init(int first, int last, int step, int chunksize, unsigned int *loops,
                                  int *begin, int *end, int *next, int *lastchunk)
{
    unsigned int iterations, num_chunks, per_thread, remaining;
    struct vcomp_thread_data *thread_data = vcomp_init_thread_data();
    struct vcomp_team_data *team_data = thread_data->team;
    int num_threads = team_data ? team_data->num_threads : 1;
    int thread_num = thread_data->thread_num;
    int no_begin, no_lastchunk;

    TRACE("(%d, %d, %d, %d, %p, %p, %p, %p, %p)\n",
          first, last, step, chunksize, loops, begin, end, next, lastchunk);

    if (!begin)
    {
        begin = &no_begin;
        lastchunk = &no_lastchunk;
    }

    if (num_threads == 1 && chunksize != 1)
    {
        *loops      = 1;
        *begin      = first;
        *end        = last;
        *next       = 0;
        *lastchunk  = first;
        return;
    }

    if (first == last)
    {
        *loops = !thread_num;
        if (!thread_num)
        {
            *begin      = first;
            *end        = last;
            *next       = 0;
            *lastchunk  = first;
        }
        return;
    }

    if (step <= 0)
    {
        *loops = 0;
        return;
    }

    if (first < last)
        iterations = 1 + (last - first) / step;
    else
    {
        iterations = 1 + (first - last) / step;
        step *= -1;
    }

    if (chunksize < 1)
        chunksize = 1;

    num_chunks  = ((DWORD64)iterations + chunksize - 1) / chunksize;
    per_thread  = num_chunks / num_threads;
    remaining   = num_chunks - per_thread * num_threads;

    *loops      = per_thread + (thread_num < remaining);
    *begin      = first + thread_num * chunksize * step;
    *end        = *begin + (chunksize - 1) * step;
    *next       = chunksize * num_threads * step;
    *lastchunk  = first + (num_chunks - 1) * chunksize * step;
}

void CDECL _vcomp_for_static_end(void)
{
    TRACE("()\n");
    /* nothing to do here */
}

void CDECL _vcomp_for_dynamic_init(unsigned int flags, unsigned int first, unsigned int last,
                                   int step, unsigned int chunksize)
{
    unsigned int iterations, per_thread, remaining;
    struct vcomp_thread_data *thread_data = vcomp_init_thread_data();
    struct vcomp_team_data *team_data = thread_data->team;
    struct vcomp_task_data *task_data = thread_data->task;
    int num_threads = team_data ? team_data->num_threads : 1;
    int thread_num = thread_data->thread_num;
    unsigned int type = flags & ~VCOMP_DYNAMIC_FLAGS_INCREMENT;

    TRACE("(%u, %u, %u, %d, %u)\n", flags, first, last, step, chunksize);

    if (step <= 0)
    {
        thread_data->dynamic_type = 0;
        return;
    }

    if (flags & VCOMP_DYNAMIC_FLAGS_INCREMENT)
        iterations = 1 + (last - first) / step;
    else
    {
        iterations = 1 + (first - last) / step;
        step *= -1;
    }

    if (type == VCOMP_DYNAMIC_FLAGS_STATIC)
    {
        per_thread = iterations / num_threads;
        remaining  = iterations - per_thread * num_threads;

        if (thread_num < remaining)
            per_thread++;
        else if (per_thread)
            first += remaining * step;
        else
        {
            thread_data->dynamic_type = 0;
            return;
        }

        thread_data->dynamic_type   = VCOMP_DYNAMIC_FLAGS_STATIC;
        thread_data->dynamic_begin  = first + per_thread * thread_num * step;
        thread_data->dynamic_end    = thread_data->dynamic_begin + (per_thread - 1) * step;
    }
    else
    {
        if (type != VCOMP_DYNAMIC_FLAGS_CHUNKED &&
            type != VCOMP_DYNAMIC_FLAGS_GUIDED)
        {
            FIXME("unsupported flags %u\n", flags);
            type = VCOMP_DYNAMIC_FLAGS_GUIDED;
        }

        EnterCriticalSection(&vcomp_section);
        thread_data->dynamic++;
        thread_data->dynamic_type = type;
        if ((int)(thread_data->dynamic - task_data->dynamic) > 0)
        {
            task_data->dynamic              = thread_data->dynamic;
            task_data->dynamic_first        = first;
            task_data->dynamic_last         = last;
            task_data->dynamic_iterations   = iterations;
            task_data->dynamic_step         = step;
            task_data->dynamic_chunksize    = chunksize;
        }
        LeaveCriticalSection(&vcomp_section);
    }
}

int CDECL _vcomp_for_dynamic_next(unsigned int *begin, unsigned int *end)
{
    struct vcomp_thread_data *thread_data = vcomp_init_thread_data();
    struct vcomp_task_data *task_data = thread_data->task;
    struct vcomp_team_data *team_data = thread_data->team;
    int num_threads = team_data ? team_data->num_threads : 1;

    TRACE("(%p, %p)\n", begin, end);

    if (thread_data->dynamic_type == VCOMP_DYNAMIC_FLAGS_STATIC)
    {
        *begin = thread_data->dynamic_begin;
        *end   = thread_data->dynamic_end;
        thread_data->dynamic_type = 0;
        return 1;
    }
    else if (thread_data->dynamic_type == VCOMP_DYNAMIC_FLAGS_CHUNKED ||
             thread_data->dynamic_type == VCOMP_DYNAMIC_FLAGS_GUIDED)
    {
        unsigned int iterations = 0;
        EnterCriticalSection(&vcomp_section);
        if (thread_data->dynamic == task_data->dynamic &&
            task_data->dynamic_iterations != 0)
        {
            iterations = min(task_data->dynamic_iterations, task_data->dynamic_chunksize);
            if (thread_data->dynamic_type == VCOMP_DYNAMIC_FLAGS_GUIDED &&
                task_data->dynamic_iterations > num_threads * task_data->dynamic_chunksize)
            {
                iterations = (task_data->dynamic_iterations + num_threads - 1) / num_threads;
            }
            *begin = task_data->dynamic_first;
            *end   = task_data->dynamic_first + (iterations - 1) * task_data->dynamic_step;
            task_data->dynamic_iterations -= iterations;
            task_data->dynamic_first      += iterations * task_data->dynamic_step;
            if (!task_data->dynamic_iterations)
                *end = task_data->dynamic_last;
        }
        LeaveCriticalSection(&vcomp_section);
        return iterations != 0;
    }

    return 0;
}

int CDECL omp_in_parallel(void)
{
    TRACE("()\n");
    return vcomp_init_thread_data()->parallel;
}

static DWORD WINAPI _vcomp_fork_worker(void *param)
{
    struct vcomp_thread_data *thread_data = param;
    vcomp_set_thread_data(thread_data);

    TRACE("starting worker thread for %p\n", thread_data);

    EnterCriticalSection(&vcomp_section);
    for (;;)
    {
        struct vcomp_team_data *team = thread_data->team;
        if (team != NULL)
        {
            LeaveCriticalSection(&vcomp_section);
            _vcomp_fork_call_wrapper(team->wrapper, team->nargs, team->valist);
            EnterCriticalSection(&vcomp_section);

            thread_data->team = NULL;
            list_remove(&thread_data->entry);
            list_add_tail(&vcomp_idle_threads, &thread_data->entry);
            if (++team->finished_threads >= team->num_threads)
                WakeAllConditionVariable(&team->cond);
        }

        if (!SleepConditionVariableCS(&thread_data->cond, &vcomp_section, 5000) &&
            GetLastError() == ERROR_TIMEOUT && !thread_data->team)
        {
            break;
        }
    }
    list_remove(&thread_data->entry);
    LeaveCriticalSection(&vcomp_section);

    TRACE("terminating worker thread for %p\n", thread_data);

    HeapFree(GetProcessHeap(), 0, thread_data);
    vcomp_set_thread_data(NULL);
    FreeLibraryAndExitThread(vcomp_module, 0);
    return 0;
}

void WINAPIV _vcomp_fork(BOOL ifval, int nargs, void *wrapper, ...)
{
    struct vcomp_thread_data *prev_thread_data = vcomp_init_thread_data();
    struct vcomp_thread_data thread_data;
    struct vcomp_team_data team_data;
    struct vcomp_task_data task_data;
    int num_threads;

    TRACE("(%d, %d, %p, ...)\n", ifval, nargs, wrapper);

    if (prev_thread_data->parallel && !vcomp_nested_fork)
        ifval = FALSE;

    if (!ifval)
        num_threads = 1;
    else if (prev_thread_data->fork_threads)
        num_threads = prev_thread_data->fork_threads;
    else
        num_threads = vcomp_num_threads;

    InitializeConditionVariable(&team_data.cond);
    team_data.num_threads       = 1;
    team_data.finished_threads  = 0;
    team_data.nargs             = nargs;
    team_data.wrapper           = wrapper;
    __ms_va_start(team_data.valist, wrapper);
    team_data.barrier           = 0;
    team_data.barrier_count     = 0;

    task_data.single            = 0;
    task_data.section           = 0;
    task_data.dynamic           = 0;

    thread_data.team            = &team_data;
    thread_data.task            = &task_data;
    thread_data.thread_num      = 0;
    thread_data.parallel        = ifval || prev_thread_data->parallel;
    thread_data.fork_threads    = 0;
    thread_data.single          = 1;
    thread_data.section         = 1;
    thread_data.dynamic         = 1;
    thread_data.dynamic_type    = 0;
    list_init(&thread_data.entry);
    InitializeConditionVariable(&thread_data.cond);

    if (num_threads > 1)
    {
        struct list *ptr;
        EnterCriticalSection(&vcomp_section);

        /* reuse existing threads (if any) */
        while (team_data.num_threads < num_threads && (ptr = list_head(&vcomp_idle_threads)))
        {
            struct vcomp_thread_data *data = LIST_ENTRY(ptr, struct vcomp_thread_data, entry);
            data->team          = &team_data;
            data->task          = &task_data;
            data->thread_num    = team_data.num_threads++;
            data->parallel      = thread_data.parallel;
            data->fork_threads  = 0;
            data->single        = 1;
            data->section       = 1;
            data->dynamic       = 1;
            data->dynamic_type  = 0;
            list_remove(&data->entry);
            list_add_tail(&thread_data.entry, &data->entry);
            WakeAllConditionVariable(&data->cond);
        }

        /* spawn additional threads */
        while (team_data.num_threads < num_threads)
        {
            struct vcomp_thread_data *data;
            HMODULE module;
            HANDLE thread;

            data = HeapAlloc(GetProcessHeap(), 0, sizeof(*data));
            if (!data) break;

            data->team          = &team_data;
            data->task          = &task_data;
            data->thread_num    = team_data.num_threads;
            data->parallel      = thread_data.parallel;
            data->fork_threads  = 0;
            data->single        = 1;
            data->section       = 1;
            data->dynamic       = 1;
            data->dynamic_type  = 0;
            InitializeConditionVariable(&data->cond);

            thread = CreateThread(NULL, 0, _vcomp_fork_worker, data, 0, NULL);
            if (!thread)
            {
                HeapFree(GetProcessHeap(), 0, data);
                break;
            }

            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               (const WCHAR *)vcomp_module, &module);
            team_data.num_threads++;
            list_add_tail(&thread_data.entry, &data->entry);
            CloseHandle(thread);
        }

        LeaveCriticalSection(&vcomp_section);
    }

    vcomp_set_thread_data(&thread_data);
    _vcomp_fork_call_wrapper(team_data.wrapper, team_data.nargs, team_data.valist);
    vcomp_set_thread_data(prev_thread_data);
    prev_thread_data->fork_threads = 0;

    if (team_data.num_threads > 1)
    {
        EnterCriticalSection(&vcomp_section);

        team_data.finished_threads++;
        while (team_data.finished_threads < team_data.num_threads)
            SleepConditionVariableCS(&team_data.cond, &vcomp_section, INFINITE);

        LeaveCriticalSection(&vcomp_section);
        assert(list_empty(&thread_data.entry));
    }

    __ms_va_end(team_data.valist);
}

static CRITICAL_SECTION *alloc_critsect(void)
{
    CRITICAL_SECTION *critsect;
    if (!(critsect = HeapAlloc(GetProcessHeap(), 0, sizeof(*critsect))))
    {
        ERR("could not allocate critical section\n");
        ExitProcess(1);
    }

    InitializeCriticalSection(critsect);
    critsect->DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": critsect");
    return critsect;
}

static void destroy_critsect(CRITICAL_SECTION *critsect)
{
    if (!critsect) return;
    critsect->DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(critsect);
    HeapFree(GetProcessHeap(), 0, critsect);
}

void CDECL omp_init_lock(omp_lock_t *lock)
{
    TRACE("(%p)\n", lock);
    *lock = alloc_critsect();
}

void CDECL omp_destroy_lock(omp_lock_t *lock)
{
    TRACE("(%p)\n", lock);
    destroy_critsect(*lock);
}

void CDECL omp_set_lock(omp_lock_t *lock)
{
    TRACE("(%p)\n", lock);

    if (RtlIsCriticalSectionLockedByThread(*lock))
    {
        ERR("omp_set_lock called while holding lock %p\n", *lock);
        ExitProcess(1);
    }

    EnterCriticalSection(*lock);
}

void CDECL omp_unset_lock(omp_lock_t *lock)
{
    TRACE("(%p)\n", lock);
    LeaveCriticalSection(*lock);
}

int CDECL omp_test_lock(omp_lock_t *lock)
{
    TRACE("(%p)\n", lock);

    if (RtlIsCriticalSectionLockedByThread(*lock))
        return 0;

    return TryEnterCriticalSection(*lock);
}

void CDECL omp_set_nest_lock(omp_nest_lock_t *lock)
{
    TRACE("(%p)\n", lock);
    EnterCriticalSection(*lock);
}

void CDECL omp_unset_nest_lock(omp_nest_lock_t *lock)
{
    TRACE("(%p)\n", lock);
    LeaveCriticalSection(*lock);
}

int CDECL omp_test_nest_lock(omp_nest_lock_t *lock)
{
    TRACE("(%p)\n", lock);
    return TryEnterCriticalSection(*lock) ? (*lock)->RecursionCount : 0;
}

void CDECL _vcomp_enter_critsect(CRITICAL_SECTION **critsect)
{
    TRACE("(%p)\n", critsect);

    if (!*critsect)
    {
        CRITICAL_SECTION *new_critsect = alloc_critsect();
        if (InterlockedCompareExchangePointer((void **)critsect, new_critsect, NULL) != NULL)
            destroy_critsect(new_critsect);  /* someone beat us to it */
    }

    EnterCriticalSection(*critsect);
}

void CDECL _vcomp_leave_critsect(CRITICAL_SECTION *critsect)
{
    TRACE("(%p)\n", critsect);
    LeaveCriticalSection(critsect);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    TRACE("(%p, %d, %p)\n", instance, reason, reserved);

    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
        {
            SYSTEM_INFO sysinfo;

            if ((vcomp_context_tls = TlsAlloc()) == TLS_OUT_OF_INDEXES)
            {
                ERR("Failed to allocate TLS index\n");
                return FALSE;
            }

            GetSystemInfo(&sysinfo);
            vcomp_module      = instance;
            vcomp_max_threads = sysinfo.dwNumberOfProcessors;
            vcomp_num_threads = sysinfo.dwNumberOfProcessors;
            break;
        }

        case DLL_PROCESS_DETACH:
        {
            if (reserved) break;
            if (vcomp_context_tls != TLS_OUT_OF_INDEXES)
            {
                vcomp_free_thread_data();
                TlsFree(vcomp_context_tls);
            }
            break;
        }

        case DLL_THREAD_DETACH:
        {
            vcomp_free_thread_data();
            break;
        }
    }

    return TRUE;
}
