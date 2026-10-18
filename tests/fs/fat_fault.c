/* Reuse the file-backed fixture and real VFS, not a replacement FAT model. */
#define main original_harness_main
#define ata_read fixture_read
#define ata_write fixture_write
#define ata_sync fixture_sync
#include "harness.c"
#undef main
#undef ata_read
#undef ata_write
#undef ata_sync
#include <unistd.h>
#include <fcntl.h>
static int step,cut,armed,tripped;
static const char *fault;
static int before(void) {
    if(!armed) return 0;
    if(!strcmp(fault,"read-eio")) return 0;
    step++;
    if(step!=cut) return 0;
    tripped=1;
    if(!strcmp(fault,"before")) _exit(77);
    return !strcmp(fault,"eio") || !strcmp(fault,"unplug");
}
static void after(void) {
    if(armed && step==cut && !strcmp(fault,"after")) _exit(77);
}
int ata_read(int i,u32 l,u8 n,void *b) {
    if(armed && !strcmp(fault,"read-eio") && ++step==cut) { tripped=1;return -1; }
    return fixture_read(i,l,n,b);
}
int ata_write(int i,u32 l,u8 n,const void *b) {
    if(armed && tripped && !strcmp(fault,"unplug")) return -1;
    if(before()) return -1;
    if(armed && getenv("FAT_TRACE")) printf("write=%d lba=%u\n",step,l);
    if(armed && step==cut && !strcmp(fault,"torn")) {
        /* Simulate a torn target sector. A torn journal header must fence. */
        if(fseeko(disk,(off_t)l*512,SEEK_SET)) abort();
        if(fwrite(b,1,171,disk)!=171 || fflush(disk)) abort();
        _exit(77);
    }
    int r=fixture_write(i,l,n,b);after();return r;
}
int ata_sync(int i) {
    if(armed && tripped && !strcmp(fault,"unplug")) return -1;
    if(before()) return -1;
    int r=fixture_sync(i);if(!r && fsync(fileno(disk))) r=-1;
    after();return r;
}
int fat_test_journal(const char *,u32,u32,int);
int main(int argc,char **argv) {
    if(argc<4) return 2;
    disk=fopen(argv[1],"r+b");if(!disk) return 2;
    setvbuf(disk,NULL,_IONBF,0);
    fseeko(disk,0,SEEK_END);disk_sectors=(u32)(ftello(disk)/512);
    u32 total=(u32)strtoul(argv[2],0,0);
    vfs_init();if(fat_mount("fat",0,0,total)<0) return 3;
    if(strcmp(argv[3],"ram")) {
        if(fat_test_journal("fat",total,49,!strcmp(argv[3],"init"))<0) return 4;
    }
    if(argc==4 || !strcmp(argv[3],"init") || !strcmp(argv[3],"recover")) return 0;
    const char *op=argv[4];fault=argc>5?argv[5]:"none";cut=argc>6?atoi(argv[6]):0;
    static unsigned char body[65535];for(unsigned i=0;i<sizeof(body);i++) body[i]=(unsigned char)(i*19+7);
    armed=1;int r;
    if(!strcmp(op,"overwrite")) r=vfs_write("/mnt/fat/Old long name.txt",body,1300);
    else if(!strcmp(op,"empty")) r=vfs_write("/mnt/fat/Old long name.txt",NULL,0);
    else if(!strcmp(op,"large")) r=vfs_write("/mnt/fat/Large.txt",body,sizeof(body));
    else if(!strcmp(op,"create")) r=vfs_write("/mnt/fat/Новый длинный файл.txt",body,1300);
    else if(!strcmp(op,"mkdir")) r=vfs_mkdir("/mnt/fat/New long directory");
    else if(!strcmp(op,"remove")) r=vfs_remove("/mnt/fat/Old long name.txt");
    else if(!strcmp(op,"rmdir")) r=vfs_remove("/mnt/fat/Empty long directory");
    else if(!strcmp(op,"read")) { unsigned char b[1300];r=vfs_read("/mnt/fat/Old long name.txt",0,b,sizeof(b)); }
    else if(!strcmp(op,"dirwrite")) r=vfs_write("/mnt/fat/Empty long directory",body,17);
    else if(!strcmp(op,"badname")) r=vfs_write("/mnt/fat/bad?.txt",body,17);
    else if(!strcmp(op,"badparent")) r=vfs_write("/mnt/fat/Old long name.txt/child",body,17);
    else if(!strcmp(op,"names")) {
        static const char *names[]={"A.B.C","A+B.TXT","A=B.TXT","A[B].TXT","Alias collision one.txt","Alias collision two.txt"};
        r=0;
        for(unsigned k=0;k<sizeof(names)/sizeof(names[0]);k++) {
            char path[160];snprintf(path,sizeof(path),"/mnt/fat/%s",names[k]);
            int saved=vfs_write(path,body,17);
            if(saved!=17) { fprintf(stderr,"name %s returned %d\n",path,saved);return 11; }
        }
    }
    else if(!strcmp(op,"badutf8")) r=vfs_write("/mnt/fat/bad\xE2\x82",body,17);
    else if(!strcmp(op,"remove-new")) r=vfs_remove("/mnt/fat/Новый длинный файл.txt");
    else if(!strcmp(op,"rmdir-new")) r=vfs_remove("/mnt/fat/New long directory");
    else if(!strcmp(op,"ru-case")) r=vfs_write("/mnt/fat/НОВЫЙ ДЛИННЫЙ ФАЙЛ.TXT",body,17);
    else if(!strcmp(op,"check")) {
        unsigned char b[1300];
        r=vfs_read("/mnt/fat/Old long name.txt",0,b,sizeof(b));
        if(r<0) return 5;
        if(r!=0 && r!=1000 && r!=1300) return 6;
        if(r==1300 && memcmp(body,b,1300)) return 7;
        if(r==1000) for(unsigned i=0;i<1000;i++) if(b[i]!='O') return 8;
    } else return 2;
    armed=0;
    if(tripped && !strcmp(fault,"unplug")) {
        unsigned char b[1300];
        if(vfs_writable("/mnt/fat/") || vfs_read("/mnt/fat/Old long name.txt",0,b,sizeof(b))>=0 ||
           vfs_write("/mnt/fat/Other.txt",body,17)!=-2) return 10;
    }
    printf("result=%d steps=%d tripped=%d fenced=%d\n",r,step,tripped,!vfs_writable("/mnt/fat/"));
    if(tripped && (!strcmp(fault,"eio") || !strcmp(fault,"unplug") || !strcmp(fault,"read-eio")) && r>=0) return 9;
    return fclose(disk)!=0;
}
