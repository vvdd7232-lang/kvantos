#ifndef KV_MCC_H
#define KV_MCC_H
#ifdef MCC_HOSTTEST
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint32_t u32;
#else
#include "kernel.h"
#endif

#ifndef MCC_TARGET64
#if defined(__x86_64__) && !defined(MCC_HOSTTEST)
#define MCC_TARGET64 1
#else
#define MCC_TARGET64 0
#endif
#endif
#define MCC_WORD (MCC_TARGET64 ? 8 : 4)

#define MCC_SOURCE_MAX 16000
#define MCC_OUTPUT_MAX 32768
#define MCC_ISOLATED_OUTPUT_MAX 49152
/* Single-threaded, bounded C-subset -> native i386/x86-64 .kapp compiler.
   On failure returns -1, sets size=0 and leaves output untouched. */
int mcc_compile(const char *source, const char *title, u8 *output, u32 capacity, u32 *size);
/* Explicit opt-in. mode=0 legacy ABI3, mode=1 format2/ABI4 user-only adapter. */
int mcc_compile_target(const char *source,const char *title,u8 *output,u32 capacity,u32 *size,int mode);
const char *mcc_error(void);
int mcc_error_line(void);
#endif
