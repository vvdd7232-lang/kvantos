/* Host test harness: runs the real KvantOS FAT and NTFS drivers
   against a disk image file, so bugs are found without booting. */
#include "kernel.h"
#include "vfs.h"
#include <stdarg.h>

static FILE *disk;
static u32   disk_sectors;

/* The real kernel driver returns 0 on success and -1 on failure. The
   harness must match that exactly: an earlier version returned 1 for
   success and hid a bug that only appeared on real hardware. */
int ata_read(int idx, u32 lba, u8 count, void *buf) {
    (void)idx;
    if (fseeko(disk, (off_t)lba * 512, SEEK_SET) != 0) return -1;
    size_t want = (size_t)count * 512;
    return fread(buf, 1, want, disk) == want ? 0 : -1;
}
int ata_write(int idx, u32 lba, u8 count, const void *buf) {
    (void)idx;
    if (fseeko(disk, (off_t)lba * 512, SEEK_SET) != 0) return -1;
    size_t want = (size_t)count * 512;
    int ok = fwrite(buf, 1, want, disk) == want;
    fflush(disk);
    return ok ? 0 : -1;
}
int ata_count(void) { return 1; }
int ata_boot_drive(void) { return 0; }
u32 ata_sectors(int i) { (void)i; return disk_sectors; }

void rtc_read(rtc_time_t *t) {
    t->sec = 30; t->min = 45; t->hour = 12;
    t->day = 16; t->month = 8; t->year = 2026;
}
void ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(buf, size, fmt, ap); va_end(ap);
}
void *kmalloc(size_t s) { return malloc(s); }
void kfree(void *p) { free(p); }
void heap_stats(u32 *t, u32 *u, u32 *b) { if(t)*t=64u<<20; if(u)*u=0; if(b)*b=0; }

static rfile_t ramtbl[RAMFS_MAX_FILES];
rfile_t *ramfs_table(void) { return ramtbl; }
rfile_t *ramfs_find(const char *n) {
    for (int i=0;i<RAMFS_MAX_FILES;i++) if (ramtbl[i].used && !strcmp(ramtbl[i].name,n)) return &ramtbl[i];
    return NULL;
}
int ramfs_create(const char *n, const char *d, u32 s) {
    for (int i=0;i<RAMFS_MAX_FILES;i++) if(!ramtbl[i].used){
        strncpy(ramtbl[i].name,n,23); ramtbl[i].data=malloc(s?s:1);
        memcpy(ramtbl[i].data,d,s); ramtbl[i].size=s; ramtbl[i].used=1; return 1; }
    return 0;
}
int ramfs_delete(const char *n) {
    rfile_t *f = ramfs_find(n); if(!f) return 0; free(f->data); f->used=0; return 1;
}
int kvfs_mounted(void) { return 0; }
int kvfs_exists(const char *n){(void)n;return 0;}
u32 kvfs_size(const char *n){(void)n;return 0;}
int kvfs_read(const char *n,void*b,u32 m){(void)n;(void)b;(void)m;return -1;}
int kvfs_write(const char*n,const void*d,u32 s,int e){(void)n;(void)d;(void)s;(void)e;return -1;}
int kvfs_delete(const char*n){(void)n;return -1;}
int kvfs_list(int i,char*n,u32*s,int*e){(void)i;(void)n;(void)s;(void)e;return 0;}
void kvfs_stats(u32*mb,u32*kb,u32*f){if(mb)*mb=0;if(kb)*kb=0;if(f)*f=0;}

/* ---------------- tests ---------------- */
static int pass = 0, fail = 0;
static void ok(int cond, const char *what) {
    printf("  %s %s\n", cond ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m", what);
    if (cond) pass++; else fail++;
}

static void dump_dir(const char *path, int depth) {
    vfs_dirent_t e;
    for (int i = 0; i < 500; i++) {
        if (!vfs_readdir(path, i, &e)) break;
        for (int d=0;d<depth;d++) printf("    ");
        printf("    %-40s %8u %s\n", e.name, e.size, e.is_dir ? "<DIR>" : "");
        if (e.is_dir && depth < 1) {
            char sub[512];
            vfs_join(sub, sizeof(sub), path, e.name);
            dump_dir(sub, depth + 1);
        }
        if (i > 12 && depth > 0) { 
            for (int d=0;d<depth;d++) printf("    ");
            printf("    ... (truncated)\n"); break; }
    }
}

static int count_entries(const char *path) {
    vfs_dirent_t e; int n = 0;
    while (vfs_readdir(path, n, &e)) n++;
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: harness <disk.img>\n"); return 1; }
    disk = fopen(argv[1], "r+b");
    if (!disk) { perror("open"); return 1; }
    fseeko(disk, 0, SEEK_END);
    disk_sectors = (u32)(ftello(disk) / 512);

    vfs_init();
    int n = vfs_autoscan();
    printf("\n=== Autoscan: %d volume(s) ===\n", n);
    for (int i = 0; i < vfs_volume_count(); i++) {
        vfs_volume_t *v = vfs_volume(i);
        u32 tk=0, fk=0;
        char p[64]; snprintf(p,sizeof(p),"/mnt/%s",v->name);
        vfs_space(p, &tk, &fk);
        printf("  /mnt/%-6s %-6s label=%-12s %6u MiB total, %6u MiB free %s\n",
               v->name, vfs_kind_name(v->kind), v->label,
               tk/1024, fk/1024, v->writable?"rw":"ro");
    }

    /* A second mode: probe an arbitrary image and just list it. */
    if (argc > 2 && !strcmp(argv[2], "--list")) {
        for (int i = 0; i < vfs_volume_count(); i++) {
            vfs_volume_t *v = vfs_volume(i);
            if (v->kind == FS_RAMFS) continue;
            char p[64]; snprintf(p,sizeof(p),"/mnt/%s",v->name);
            printf("  --- %s ---\n", p);
            dump_dir(p, 0);
            char f[256]; snprintf(f,sizeof(f),"%s/Long Name On FAT16.txt",p);
            char b[256]; int r = vfs_read(f,0,b,sizeof(b)-1);
            if (r>0){b[r]=0; printf("  read long-name file: %d bytes = %s", r, b);}
            ok(r==19, "FAT16 long name read");
            snprintf(f,sizeof(f),"%s/sub/inner.txt",p);
            r = vfs_read(f,0,b,sizeof(b)-1);
            ok(r==19, "FAT16 subdirectory file read");
            snprintf(f,sizeof(f),"%s/written16.txt",p);
            ok(vfs_write(f,"hello fat16",11)==11, "FAT16 write");
            r = vfs_read(f,0,b,sizeof(b)-1);
            ok(r==11, "FAT16 read back");
            snprintf(f,sizeof(f),"%s/Ещё каталог",p);
            ok(vfs_mkdir(f)==0, "FAT16 mkdir with Cyrillic name");
            ok(vfs_stat(f)==2, "FAT16 Cyrillic directory exists");
        }
        printf("\n=== %d passed, %d failed ===\n", pass, fail);
        return fail?1:0;
    }

    /* ---------- FAT32 ---------- */
    printf("\n=== FAT32 (/mnt/hda1) ===\n");
    dump_dir("/mnt/hda1", 0);

    vfs_volume_t *v0 = vfs_find("hda1");
    ok(v0 && v0->kind == FS_FAT32, "FAT32 detected");
    ok(v0 && !strcmp(v0->label, "KVANTDATA"), "FAT32 volume label = KVANTDATA");
    ok(vfs_stat("/mnt/hda1/HELLO.TXT") == 1, "HELLO.TXT exists");
    ok(vfs_stat("/mnt/hda1/Documents") == 2, "Documents is a directory");

    char buf[64000];
    int r = vfs_read("/mnt/hda1/HELLO.TXT", 0, buf, sizeof(buf)-1);
    if (r > 0) buf[r] = 0;
    ok(r == 38 && strstr(buf, "Hello from FAT32"), "HELLO.TXT content read");

    ok(vfs_size("/mnt/hda1/Documents/A very long file name.txt") == 12292,
       "long file name resolved, size 12292");
    r = vfs_read("/mnt/hda1/Documents/A very long file name.txt", 0, buf, sizeof(buf));
    ok(r == 12292, "long-name file fully read (multi-cluster)");
    ok(!memcmp(buf, "Line 1 of a longer test file", 28), "multi-cluster content start ok");
    ok(!memcmp(buf + 12292 - 31, "Line 400 of a longer test file", 30), "multi-cluster content end ok");

    /* offset read across a cluster boundary */
    r = vfs_read("/mnt/hda1/Documents/A very long file name.txt", 5000, buf, 100);
    ok(r == 100, "partial read at offset 5000");

    /* Cyrillic long name: mtools stores non-ASCII names byte-wise rather
       than as UTF-16, so we round-trip one written by our own driver. */
    ok(vfs_write("/mnt/hda1/Documents/Привет мир.txt", "russian name", 12) == 12,
       "write file with Cyrillic long name");
    ok(vfs_stat("/mnt/hda1/Documents/Привет мир.txt") == 1, "UTF-8 Cyrillic long name found");
    {
        vfs_dirent_t e; int found = 0;
        for (int i = 0; vfs_readdir("/mnt/hda1/Documents", i, &e); i++)
            if (!strcmp(e.name, "Привет мир.txt")) found = 1;
        ok(found, "Cyrillic name survives a directory listing round-trip");
    }

    /* ---------- FAT32 writing ---------- */
    printf("\n--- FAT32 write tests ---\n");
    const char *msg = "Written by KvantOS FAT32 driver.";
    ok(vfs_write("/mnt/hda1/NEWFILE.TXT", msg, strlen(msg)) == (int)strlen(msg), "write short file");
    r = vfs_read("/mnt/hda1/NEWFILE.TXT", 0, buf, sizeof(buf)-1);
    if (r>0) buf[r]=0;
    ok(r == (int)strlen(msg) && !strcmp(buf, msg), "read back short file");

    static char big[300000];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)('A' + (i % 26));
    ok(vfs_write("/mnt/hda1/Documents/big_written_file.bin", big, sizeof(big)) == (int)sizeof(big),
       "write 300000-byte file with a long name");
    static char back[300000];
    r = vfs_read("/mnt/hda1/Documents/big_written_file.bin", 0, back, sizeof(back));
    ok(r == (int)sizeof(big) && !memcmp(big, back, sizeof(big)), "read back 300000 bytes identical");

    ok(vfs_mkdir("/mnt/hda1/NewFolder") == 0, "mkdir NewFolder");
    ok(vfs_stat("/mnt/hda1/NewFolder") == 2, "NewFolder is a directory");
    ok(vfs_write("/mnt/hda1/NewFolder/inside.txt", "inside", 6) == 6, "write into new directory");
    ok(vfs_stat("/mnt/hda1/NewFolder/inside.txt") == 1, "file inside new directory exists");

    ok(vfs_remove("/mnt/hda1/NewFolder") == -4, "refuse to delete non-empty directory");
    ok(vfs_remove("/mnt/hda1/NewFolder/inside.txt") == 0, "delete file");
    ok(vfs_stat("/mnt/hda1/NewFolder/inside.txt") == 0, "deleted file is gone");
    ok(vfs_remove("/mnt/hda1/NewFolder") == 0, "delete now-empty directory");

    /* overwrite must not leak clusters */
    u32 t1=0,f1=0; vfs_space("/mnt/hda1",&t1,&f1);
    vfs_write("/mnt/hda1/NEWFILE.TXT", big, 100000);
    vfs_write("/mnt/hda1/NEWFILE.TXT", big, 100000);
    vfs_write("/mnt/hda1/NEWFILE.TXT", big, 100000);
    u32 t2=0,f2=0; vfs_space("/mnt/hda1",&t2,&f2);
    printf("    free before=%u KiB after 3 overwrites=%u KiB\n", f1, f2);
    ok(f1 - f2 < 200, "repeated overwrite does not leak clusters");
    vfs_remove("/mnt/hda1/NEWFILE.TXT");

    /* ---------- NTFS ---------- */
    printf("\n=== NTFS (/mnt/hda2) ===\n");
    dump_dir("/mnt/hda2", 0);

    vfs_volume_t *v1 = vfs_find("hda2");
    ok(v1 && v1->kind == FS_NTFS, "NTFS detected");
    ok(v1 && !strcmp(v1->label, "WINDATA"), "NTFS volume label = WINDATA");
    ok(v1 && !v1->writable, "NTFS mounted read-only");

    ok(vfs_stat("/mnt/hda2/readme.txt") == 1, "readme.txt exists");
    ok(vfs_stat("/mnt/hda2/Windows") == 2, "Windows is a directory");

    r = vfs_read("/mnt/hda2/readme.txt", 0, buf, sizeof(buf)-1);
    if (r>0) buf[r]=0;
    ok(r == 56 && strstr(buf, "real NTFS volume"), "readme.txt content read");

    ok(vfs_size("/mnt/hda2/Windows/tiny.txt") == 5, "tiny.txt size 5 (resident $DATA)");
    r = vfs_read("/mnt/hda2/Windows/tiny.txt", 0, buf, sizeof(buf)-1);
    if (r>0) buf[r]=0;
    ok(r == 5 && !strcmp(buf, "tiny\n"), "resident $DATA read correctly");

    ok(vfs_size("/mnt/hda2/Windows/bigdata.bin") == 921600, "bigdata.bin size 921600");
    static char nbig[921600];
    r = vfs_read("/mnt/hda2/Windows/bigdata.bin", 0, nbig, sizeof(nbig));
    ok(r == 921600, "non-resident 900 KiB file fully read");
    FILE *ref = fopen("mnt_bigdata.ref", "rb");
    if (ref) {
        static char refbuf[921600];
        size_t got = fread(refbuf, 1, sizeof(refbuf), ref);
        fclose(ref);
        ok(got == 921600 && !memcmp(refbuf, nbig, 921600), "non-resident data byte-identical to original");
    }

    int pf = count_entries("/mnt/hda2/Program Files");
    printf("    'Program Files' entries: %d\n", pf);
    ok(pf == 120, "B-tree directory with 120 entries fully listed ($INDEX_ALLOCATION)");

    ok(vfs_stat("/mnt/hda2/Users/Привет.txt") == 1, "UTF-16 Cyrillic name read as UTF-8");
    r = vfs_read("/mnt/hda2/Users/Public/notes.txt", 0, buf, sizeof(buf)-1);
    ok(r == 19392, "nested deep file read (19392 bytes)");

    ok(vfs_write("/mnt/hda2/nope.txt", "x", 1) == -2, "NTFS write refused with 'read-only'");
    ok(vfs_remove("/mnt/hda2/readme.txt") == -2, "NTFS delete refused");

    printf("\n=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
