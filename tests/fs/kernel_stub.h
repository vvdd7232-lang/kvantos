/* Minimal kernel.h replacement so the real drivers compile on the host */
#ifndef HARNESS_KERNEL_H
#define HARNESS_KERNEL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int32_t  i32;

#define T(en, ru) (en)

typedef struct { u8 sec, min, hour, day, month; u16 year; } rtc_time_t;
void rtc_read(rtc_time_t *t);

int  ata_read(int idx, u32 lba, u8 count, void *buf);
int  ata_write(int idx, u32 lba, u8 count, const void *buf);
int  ata_count(void);
int  ata_boot_drive(void);
u32  ata_sectors(int i);

void ksnprintf(char *buf, size_t size, const char *fmt, ...);
void *kmalloc(size_t s);
void kfree(void *p);
void heap_stats(u32 *t, u32 *u, u32 *b);

#define RAMFS_MAX_FILES 32
typedef struct { char name[24]; char *data; u32 size; int used; } rfile_t;
rfile_t *ramfs_table(void);
rfile_t *ramfs_find(const char *n);
int ramfs_create(const char *n, const char *d, u32 s);
int ramfs_delete(const char *n);

int kvfs_mounted(void);
int kvfs_exists(const char *n);
u32 kvfs_size(const char *n);
int kvfs_read(const char *n, void *b, u32 m);
int kvfs_write(const char *n, const void *d, u32 s, int e);
int kvfs_delete(const char *n);
int kvfs_list(int i, char *n, u32 *s, int *e);
void kvfs_stats(u32 *mb, u32 *kb, u32 *f);
#endif
