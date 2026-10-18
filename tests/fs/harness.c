/* Host test harness: runs the real KvantOS FAT and NTFS drivers
   against a disk image file, so bugs are found without booting. */
#include "kernel.h"
#include "vfs.h"
#include <stdarg.h>

static FILE *disk;
static u32   disk_sectors;
static u32   disk_read_oob;
static u32   disk_write_calls;
static int usb_mode;

/* The real kernel driver returns 0 on success and -1 on failure. The
   harness must match that exactly: an earlier version returned 1 for
   success and hid a bug that only appeared on real hardware. */
int ata_read(int idx, u32 lba, u8 count, void *buf) {
    (void)idx;
    if(!count || lba>=disk_sectors || count>disk_sectors-lba){disk_read_oob++;return -1;}
    if (fseeko(disk, (off_t)lba * 512, SEEK_SET) != 0) return -1;
    size_t want = (size_t)count * 512;
    return fread(buf, 1, want, disk) == want ? 0 : -1;
}
int ata_write(int idx, u32 lba, u8 count, const void *buf) {
    (void)idx;
    disk_write_calls++;
    if (fseeko(disk, (off_t)lba * 512, SEEK_SET) != 0) return -1;
    size_t want = (size_t)count * 512;
    int ok = fwrite(buf, 1, want, disk) == want;
    fflush(disk);
    return ok ? 0 : -1;
}
int ata_count(void) { return !usb_mode; }
int ata_boot_drive(void) { return 0; }
u32 ata_sectors(int i) { return i==0 && !usb_mode ? disk_sectors : 0; }
int ahci_count(void) { return 0; }
u32 ahci_generation(int i) { (void)i;return 0; }
void ahci_rescan(void) {}
int ahci_fat_authorize(int i,u32 s,u32 n,u32 g) { (void)i;(void)s;(void)n;(void)g;return -1; }
void ahci_fat_revoke(int i) { (void)i; }
int ahci_write(int i,u32 l,u8 n,const void *b) { (void)i;(void)l;(void)n;(void)b;return -1; }
int ahci_sync(int i) { (void)i;return -1; }
u32 ahci_sectors(int i) { (void)i;return 0; }
int ahci_read(int i,u32 l,u8 n,void *b) { (void)i;(void)l;(void)n;(void)b;return -1; }
void usb_rescan(void) {}
int usb_count(void) { return usb_mode; }
u32 usb_sectors(int i) { return i==0 && usb_mode ? disk_sectors : 0; }
u32 usb_generation(int i) { return usb_sectors(i)?1:0; }
void kputs(const char *s) { fputs(s,stdout); }
int usb_read(int i,u32 lba,u8 n,void *buf) { return i==0 ? ata_read(0,lba,n,buf) : -1; }

/* Default harness cannot grant USB writes; owned-WAL fixture overrides these. */
int usb_fat_authorize(int i,u32 s,u32 n,u32 g) { (void)i;(void)s;(void)n;(void)g;return -1; }
void usb_fat_revoke(int i) { (void)i; }
int usb_write(int i,u32 l,u8 n,const void *b) { (void)i;(void)l;(void)n;(void)b;return -1; }
int usb_sync(int i) { (void)i;return -1; }
int ata_sync(int i) { return i==0 && fflush(disk)==0?0:-1; }

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

int kvfs_mounted(void) { return 0; }
int kvfs_exists(const char *n){(void)n;return 0;}
u32 kvfs_size(const char *n){(void)n;return 0;}
int kvfs_read(const char *n,void*b,u32 m){(void)n;(void)b;(void)m;return -1;}
int kvfs_write(const char*n,const void*d,u32 s,int e){(void)n;(void)d;(void)s;(void)e;return -1;}
int kvfs_rename(const char*f,const char*t){(void)f;(void)t;return -2;}
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
        int listed=vfs_readdir(path,i,&e);
        if(listed<0){printf("    [listing unavailable/read error]\n");break;}
        if(!listed)break;
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
    int r;while((r=vfs_readdir(path,n,&e))==1)n++;
    return r<0?-1:n;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: harness <disk.img>\n"); return 1; }
    int usb_empty=argc>2 && !strcmp(argv[2],"--usb-empty");
    int ntfs_image=argc>2 && !strcmp(argv[2],"--ntfs-image");
    int ntfs_refuse=argc>2 && !strcmp(argv[2],"--ntfs-refuse");
    int ntfs_large_refuse=argc>2 && !strcmp(argv[2],"--ntfs-large-refuse");
    usb_mode = usb_empty || (argc>2 && !strcmp(argv[2],"--usb"));
    disk = fopen(argv[1], (usb_mode || ntfs_image || ntfs_refuse || ntfs_large_refuse) ? "rb" : "r+b");
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

    if (ntfs_refuse) {
        int found=0;
        for(int i=0;i<vfs_volume_count();i++)if(vfs_volume(i)->kind==FS_NTFS)found++;
        ok(n==0 && found==0,"malformed NTFS geometry is refused without raw-volume alias");
        ok(disk_read_oob==0,"partition rejection performs no out-of-bounds block reads");
        ok(disk_write_calls==0,"malformed private image receives zero writes");
        printf("=== %d passed, %d failed ===\n",pass,fail);
        fclose(disk);
        return fail?1:0;
    }

    if (ntfs_large_refuse) {
        vfs_volume_t *ntfs=NULL;int count=0;
        for(int i=0;i<vfs_volume_count();i++)if(vfs_volume(i)->kind==FS_NTFS){ntfs=vfs_volume(i);count++;}
        ok(n==1 && count==1 && ntfs && !ntfs->writable,"oversized NTFS file fixture mounts only as read-only volume");
        if(ntfs && count==1) {
            char root[64],path[128];snprintf(root,sizeof(root),"/mnt/%s",ntfs->name);
            snprintf(path,sizeof(path),"%s/oversized.bin",root);
            ok(vfs_stat(path)==0,"file with nonzero 64-bit size high word is not exposed as an aliased 32-bit file");
            unsigned char bytes[32];
            ok(vfs_read(path,0,bytes,sizeof(bytes))<0,"oversized file reads fail closed instead of wrapping to low DWORD");
            vfs_dirent_t entry;
            ok(vfs_readdir(root,0,&entry)<0,"directory listing refuses an unrepresentable file size");
        }
        ok(disk_read_oob==0,"oversized-file refusal performs no out-of-bounds block reads");
        ok(disk_write_calls==0,"oversized private image receives zero block writes");
        printf("=== %d passed, %d failed ===\n",pass,fail);
        fclose(disk);
        return fail?1:0;
    }

    if (ntfs_image) {
        if (argc < 4) { fprintf(stderr, "usage: harness <private-image> --ntfs-image <expected-file> [expected-start-lba] [root-name]\n"); return 2; }
        FILE *expected_file=fopen(argv[3],"rb");
        if(!expected_file){perror("expected file");return 2;}
        if(fseeko(expected_file,0,SEEK_END)!=0){fclose(expected_file);return 2;}
        off_t expected_size=ftello(expected_file);
        if(expected_size<0 || expected_size>4*1024*1024 || fseeko(expected_file,0,SEEK_SET)!=0){fclose(expected_file);return 2;}
        unsigned char *expected=malloc(expected_size?(size_t)expected_size:1);
        unsigned char *actual=malloc(expected_size?(size_t)expected_size:1);
        if(!expected || !actual){free(expected);free(actual);fclose(expected_file);return 2;}
        size_t expected_bytes=fread(expected,1,(size_t)expected_size,expected_file);
        fclose(expected_file);
        u32 expected_start=0;
        if(argc>4){char *end=NULL;unsigned long value=strtoul(argv[4],&end,10);if(!end || *end || value>0xFFFFFFFFul){fprintf(stderr,"invalid expected LBA\\n");free(expected);free(actual);return 2;}expected_start=(u32)value;}
        const char *expected_name=argc>5?argv[5]:"proof.txt";
        if(!*expected_name || strlen(expected_name)>=96 || strchr(expected_name,'/') || !strcmp(expected_name,".") || !strcmp(expected_name,"..")){fprintf(stderr,"invalid root fixture name\\n");free(expected);free(actual);return 2;}
        ok(expected_bytes==(size_t)expected_size,"read bounded expected payload fixture");
        vfs_volume_t *ntfs=NULL;int ntfs_count=0;
        for(int i=0;i<vfs_volume_count();i++){
            vfs_volume_t *candidate=vfs_volume(i);
            if(candidate->kind==FS_NTFS){ntfs=candidate;ntfs_count++;}
        }
        ok(n==1 && ntfs_count==1 && ntfs && !ntfs->writable,"one actual NTFS volume mounted read-only");
        if(ntfs && ntfs_count==1){
            ok(ntfs->lba_start==expected_start,"VFS retains the exact NTFS partition start LBA");
            char root[64],path[256];snprintf(root,sizeof(root),"/mnt/%s",ntfs->name);
            snprintf(path,sizeof(path),"%s/%s",root,expected_name);
            ok(vfs_stat(path)==1 && vfs_size(path)==(u32)expected_size,"actual NTFS root index finds exact-size expected file");
            int got=vfs_read(path,0,actual,(u32)expected_size);
            ok(got==(int)expected_size && !memcmp(actual,expected,(size_t)expected_size),"actual NTFS adapter reads byte-exact file payload");
            unsigned char sample[4096];u32 size=(u32)expected_size;
            u32 span_off=size>8192?509:(size>32?size-32:0);
            u32 span_len=size-span_off;if(span_len>sizeof(sample))span_len=sizeof(sample);
            int span_got=vfs_read(path,span_off,sample,span_len);
            ok(span_got==(int)span_len && !memcmp(sample,expected+span_off,span_len),"nonzero-offset read returns exact requested bytes");
            u32 tail_off=size>17?size-17:0,tail_len=size-tail_off;
            int tail_got=vfs_read(path,tail_off,sample,sizeof(sample));
            ok(tail_got==(int)tail_len && !memcmp(sample,expected+tail_off,tail_len),"read near EOF returns only exact remaining bytes");
            ok(vfs_read(path,size,sample,sizeof(sample))==0 && vfs_read(path,size+1,sample,sizeof(sample))==0,"EOF and beyond-EOF reads return zero");
            vfs_dirent_t entry;int found=0,listing_error=0;
            for(int i=0;i<64;i++){
                int r=vfs_readdir(root,i,&entry);
                if(r<0){listing_error=1;break;}
                if(!r)break;
                if(!strcmp(entry.name,expected_name)){found=1;break;}
            }
            ok(found && !listing_error,"actual NTFS root directory lists expected UTF-8/name entry");
            ok(vfs_write(path,"X",1)==-2 && vfs_remove(path)==-2 && vfs_mkdir(path)==-2,"NTFS writes/removal/directory creation remain refused");
        }
        ok(disk_read_oob==0,"actual-image read path issued no out-of-bounds block requests");
        ok(disk_write_calls==0,"host image received zero block writes");
        printf("=== %d passed, %d failed ===\n",pass,fail);
        free(expected);free(actual);
        fclose(disk);
        return fail?1:0;
    }

    if (usb_empty) {
        ok(n==0 && vfs_volume_count()==1,"invalid GPT never mounts a partition or raw/hybrid alias");
        return fail?1:0;
    }
    if (usb_mode) {
        char path[256], root[64]; unsigned char data[512];
        vfs_volume_t *v=NULL;
        for (int i=0;i<vfs_volume_count();i++) {
            vfs_volume_t *candidate=vfs_volume(i);
            if(candidate->disk==4 && candidate->kind==FS_FAT32) { v=candidate; break; }
        }
        ok(v && v->kind==FS_FAT32 && !v->writable,"USB FAT32 mounted read-only");
        ok(blk_count()==5 && blk_sectors(0)==0 && blk_sectors(3)==0 && blk_sectors(4)==disk_sectors,"stable USB block ID with absent ATA slots");
        if (!v) return 1;
        snprintf(root,sizeof(root),"/mnt/%s",v->name);
        snprintf(path,sizeof(path),"%s/HELLO.C",root);
        ok(vfs_read(path,0,data,sizeof(data))>0,"read FAT32 file through USB block routing");
        ok(!vfs_writable(path) && vfs_write(path,"X",1)==-2,"VFS overwrite refused");
        ok(vfs_remove(path)==-2 && vfs_mkdir(root)==-2,"VFS delete/mkdir refused");
        ok(blk_write(4,0,1,data)<0,"USB block writes refused independently of VFS");
        ok(blk_read(4,disk_sectors,1,data)<0 && blk_read(4,0xFFFFFFFFu,2,data)<0 && blk_read(4,0,0,data)<0,"out-of-bounds and zero-count reads refused");
        int stable=1;
        for(int i=0;i<24;i++) {
            vfs_init();
            if(vfs_autoscan()!=n || vfs_stat(path)!=1) stable=0;
        }
        ok(stable,"24 rescans do not leak filesystem mount slots");
        printf("=== %d passed, %d failed ===\n",pass,fail);
        return fail?1:0;
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
        for (int i = 0; vfs_readdir("/mnt/hda1/Documents", i, &e)==1; i++)
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
