"""Source-only fixture: compiled by MCC, never by host GCC."""
SOURCE = b'''#include <kvui.h>
#include "kvtypes.h"
#define N 2
typedef struct Item {char pad[5];int value;int *ptr;} Item;
int clock_proof[3];int marks[N];int count;int *values;char buffer[128];char entry[40];int bytes;
int sum8(int a,int b,int c,int d,int e,int f,int g,int h){return a+b+c+d+e+f+g+h;}
int calculate(void){
 int x=5;int *p=&x;int **q=&p;int a[N];char c[3];int *ptrs[1];
 a[0]=**q;a[1]=sum8(1,2,3,4,5,6,7,8);c[0]=0;ptrs[0]=&a[0];
 int n=sizeof(a[0]++);int k=sizeof(marks[1]=99);
 if(sizeof(a)!=8 || sizeof(c)!=3 || marks[1]!=0)return -1;
 Item items[N];Item *ip=items;++ip;ip->value=a[1];ip->ptr=ptrs[0];
 if(sizeof(Item)!=sizeof(int*)*2+8)return -2;
 return *ip->ptr+ip->value+n-k;
}
void open(void){
 int h;int m;int s;clock(&h,&m,&s);clock(0,0,0);
 clock_proof[0]=h;clock_proof[1]=m;clock_proof[2]=s;
 if(h<0 || h>23 || m<0 || m>59 || s<0 || s>59){str_copy(buffer,"clock bad",128);return;}
 int alias;clock(&alias,&alias,&alias);clock(0,&m,0);
 if(alias<0 || alias>59 || m<0 || m>59){str_copy(buffer,"clock alias bad",128);return;}
 if(file_exists("verify.c")){str_copy(buffer,"grant leak",128);return;}
 values=alloc(32);if(values==0)return;values[0]=calculate();
 format(buffer,128,"%d:%u:%x:%s:%c",values[0],2,255,"ok",65);
 file_write("local.txt",buffer,str_len(buffer)+1);
 mem_set(buffer,0,128);file_read("local.txt",buffer,128);
 if(file_list(0,entry,&bytes)!=0 || str_cmp(entry,"local.txt")!=0)str_copy(buffer,"list bad",128);
 if(bytes!=str_len(buffer)+1 || file_list(1,entry,&bytes)!=-9)str_copy(buffer,"list scope bad",128);
}
void draw(void){
 kv_point origin;origin.x=12;origin.y=20;
 clear(KV_BG);text(origin.x,origin.y,buffer,KV_TRANSPARENT,KV_TRANSPARENT);status(buffer);
 fill(20+count*12,90,40,40,rgb(60,160,240));
 line(-10,70,120,70,rgb(252,106,131));
}
void click(int x,int y,int b){count=count+1;}
void close(void){release(values);file_delete("local.txt");}
'''
EXPECTED = b'41:2:ff:ok:A'

# Tiny source-only hostile/limit fixtures. All are compiled INSIDE the guest.
RESOURCE_SOURCES = {
 'kernel.c': b'void draw(void){int *p=1048576;*p=7;}\n',
 'quota.c': b'void draw(void){for(int i=0;i<70;i++){pixel(i,0,1);}}\n',
 'textq.c': b'char big[128];\nvoid open(void){mem_set(big,120,127);big[127]=0;}\nvoid draw(void){for(int i=0;i<10;i++){text(0,0,big,1,-1);}}\n',
 'spin.c': b'void draw(void){while(1){}}\n',
 'heap.c': b'int ok;\nvoid open(void){\n int *a=alloc(32760);int *b=alloc(8);\n if(a!=0 && b==0){release(a);b=alloc(32760);if(b!=0)ok=1;release(b); }\n}\nvoid draw(void){if(ok==1)status("heap bounded");else status("heap bad");}\n',
 'files.c': b'char data[1025];char name[8];int ok=1;\nvoid open(void){\n mem_set(data,120,1025);\n for(int i=0;i<8;i++){format(name,8,"f%d",i);if(file_write(name,data,1024)!=1024)ok=0;}\n if(file_write("ninth",data,1)!=-5)ok=0;\n if(file_write("f0",data,1025)!=-5)ok=0;\n if(file_read("f0",data,1025)!=1024)ok=0;\n if(file_delete("f7")!=0 || file_write("ninth",data,1)!=1)ok=0;\n}\nvoid draw(void){if(ok==1)status("files bounded");else status("files bad");}\n'
}

# Rebuild in the existing quota.c/kapp slots: no extra RAMFS fixture files.
REUSED_NATIVE_FAULTS = {
 'invalid-status': (b'void draw(void){char s[2];s[0]=1;s[1]=0;status(s);}\n', b'value 33'),
 'invalid-line': (b'void draw(void){line(-32769,0,10,10,1);}\n', b'value 34'),
 'clock-kernel-pointer': (b'void draw(void){clock(1048576,0,0);}\n', b'reason 2, vector 14'),
}
