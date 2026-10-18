/* Reuse the file-backed fixture and real VFS, not a replacement FAT model. */
#define main original_harness_main
#define ata_read fixture_read
#define ata_write fixture_write
#define ata_sync fixture_sync
#define usb_write fixture_usb_write
#define usb_sync fixture_usb_sync
#define usb_read fixture_usb_read
#define usb_generation fixture_usb_generation
#define usb_fat_authorize fixture_usb_authorize
#define usb_fat_revoke fixture_usb_revoke
#include "harness.c"
#undef usb_write
#undef usb_sync
#undef usb_read
#undef usb_generation
#undef usb_fat_authorize
#undef usb_fat_revoke
#undef main
#undef ata_read
#undef ata_write
#undef ata_sync
#include <unistd.h>
#include <fcntl.h>
static int step,cut,armed,tripped,total_writes,total_reads,total_syncs;
static u32 gen=1,grant_start,grant_count;
static int granted;
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
    total_reads++;
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
    total_writes++;int r=fixture_write(i,l,n,b);after();return r;
}
int ata_sync(int i) {
    total_syncs++;
    if(armed && tripped && !strcmp(fault,"unplug")) return -1;
    if(before()) return -1;
    int r=fixture_sync(i);if(!r && fsync(fileno(disk))) r=-1;
    after();return r;
}
u32 usb_generation(int i) { return i==0?gen:0; }
int usb_fat_authorize(int i,u32 s,u32 n,u32 g) {
    granted=0;
    if(i || !n || g!=gen || !g || s>=disk_sectors || n>disk_sectors-s) return -1;
    grant_start=s;grant_count=n;granted=1;return 0;
}
void usb_fat_revoke(int i) { (void)i;granted=0; }
int usb_read(int i,u32 l,u8 n,void *b) { return i?-1:ata_read(0,l,n,b); }
int usb_write(int i,u32 l,u8 n,const void *b) {
    if(i || !granted || l<grant_start || l-grant_start>=grant_count || n>grant_count-(l-grant_start)) return -1;
    int r=ata_write(0,l,n,b);if(r<0) granted=0;return r;
}
int usb_sync(int i) { if(i || !granted) return -1;int r=ata_sync(0);if(r<0) granted=0;return r; }

int main(int argc,char **argv) {
    if(argc<5) return 2;
    disk=fopen(argv[1],"r+b");if(!disk) return 2;
    setvbuf(disk,NULL,_IONBF,0);usb_mode=1;
    fseeko(disk,0,SEEK_END);disk_sectors=(u32)(ftello(disk)/512);
    u32 first=(u32)strtoul(argv[2],0,0),total=(u32)strtoul(argv[3],0,0);
    const char *op=argv[4];fault=argc>5?argv[5]:"none";cut=argc>6?atoi(argv[6]):0;
    armed=!strcmp(op,"mount");
    vfs_init();int mounted=fat_mount("fat",4,first,total);
    if(mounted<0) { printf("mount=-1 writes=%d\n",total_writes);return granted?20:4; }
    if(vfs_writable("/mnt/fat/") || granted) return 21;
    if(!strcmp(op,"mount")) { printf("mount=0 steps=%d writes=%d\n",step,total_writes);return 0; }
    if(!strcmp(op,"probe")) {
        int r=fat_write_mode("fat",1);unsigned char b[1300];
        if(r>=0 || vfs_write("/mnt/fat/X.TXT","x",1)!=-2 || vfs_read("/mnt/fat/Old long name.txt",0,b,sizeof(b))!=1000 || total_writes) return 22;
        puts("PASS unprepared volume stays readable RO; no writes");return 0;
    }
    if(fat_write_mode("fat",1)<0) return 23;
    if(!vfs_writable("/mnt/fat/") || granted) return 24;
    if(!strcmp(op,"disable")) {
        if(fat_write_mode("fat",0) || vfs_write("/mnt/fat/X.TXT","x",1)!=-2 || granted) return 25;
        puts("PASS explicit disable");return 0;
    }
    if(!strcmp(op,"stale")) {
        gen++;
        if(vfs_write("/mnt/fat/X.TXT","x",1)>=0 || vfs_writable("/mnt/fat/") || total_writes || granted) return 26;
        puts("PASS generation fence");return 0;
    }
    if(!strcmp(op,"guard")) {
        if(vfs_write("/mnt/fat/KVANTWAL.SYS","x",1)>=0 || vfs_remove("/mnt/fat/kvantwal.sys")>=0 || granted) return 27;
        puts("PASS reserved file aliases protected");return 0;
    }
    static unsigned char body[65535];for(unsigned i=0;i<sizeof(body);i++) body[i]=(unsigned char)(i*19+7);
    int op_reads=total_reads,op_writes=total_writes,op_syncs=total_syncs;
    armed=1;int r;
    if(!strcmp(op,"overwrite")) r=vfs_write("/mnt/fat/Old long name.txt",body,1300);
    else if(!strcmp(op,"empty")) r=vfs_write("/mnt/fat/Old long name.txt",NULL,0);
    else if(!strcmp(op,"large")) r=vfs_write("/mnt/fat/Large.txt",body,sizeof(body));
    else if(!strcmp(op,"create")) r=vfs_write("/mnt/fat/Новый длинный файл.txt",body,1300);
    else if(!strcmp(op,"mkdir")) r=vfs_mkdir("/mnt/fat/New long directory");
    else if(!strcmp(op,"remove")) r=vfs_remove("/mnt/fat/Old long name.txt");
    else if(!strcmp(op,"rmdir")) r=vfs_remove("/mnt/fat/Empty long directory");
    else if(!strcmp(op,"list-batch")) {
        vfs_dirent_t entries[32];r=0;
        for(;;) {
            int n=vfs_readdir_many("/mnt/fat/",r,entries,32);
            if(n<0) { r=-1;break; }
            if(!n) break;
            r+=n;
        }
    }
    else if(!strcmp(op,"list-check")) {
        vfs_dirent_t entries[7],single;r=0;
        for(;;) {
            int n=vfs_readdir_many("/mnt/fat/",r,entries,7);
            if(n<0) return 30;
            for(int j=0;j<n;j++)
                if(vfs_readdir("/mnt/fat/",r+j,&single)!=1 || memcmp(entries+j,&single,sizeof(single))) return 31;
            r+=n;if(n<7) break;
        }
        if(vfs_readdir("/mnt/fat/",r,&single) || vfs_readdir_many("/mnt/fat/",0,entries,0)!=-1 ||
           vfs_readdir_many("/mnt/fat/",-1,entries,1)!=-1 || vfs_readdir_many("/mnt/fat/",0,NULL,1)!=-1 ||
           vfs_readdir_many("/mnt/fat/Old long name.txt",0,entries,1)!=-1) return 32;
    }
    else if(!strcmp(op,"list")) {
        vfs_dirent_t entry;r=0;
        int listed;while((listed=vfs_readdir("/mnt/fat/",r,&entry))==1)r++;
        if(listed<0)r=-1;
    }
    else if(!strcmp(op,"read-large")) {
        unsigned char b[65535];r=vfs_read("/mnt/fat/Large.txt",0,b,sizeof(b));
        if(r!=sizeof(body) || memcmp(b,body,sizeof(body))) return 29;
    }
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
    armed=0;if(granted) return 28;
    if(tripped && !strcmp(fault,"unplug")) {
        unsigned char b[1300];
        if(vfs_writable("/mnt/fat/") || vfs_read("/mnt/fat/Old long name.txt",0,b,sizeof(b))>=0 ||
           vfs_write("/mnt/fat/Other.txt",body,17)!=-2) return 10;
    }
    printf("result=%d steps=%d tripped=%d fenced=%d reads=%d writes=%d syncs=%d\n",r,step,tripped,!vfs_writable("/mnt/fat/"),total_reads-op_reads,total_writes-op_writes,total_syncs-op_syncs);
    if(tripped && (!strcmp(fault,"eio") || !strcmp(fault,"unplug") || !strcmp(fault,"read-eio")) && r>=0) return 9;
    return fclose(disk)!=0;
}
