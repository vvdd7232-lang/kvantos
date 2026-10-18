#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/ramfs.c"
static int oom;
void *kmalloc(size_t size) { return oom ? NULL : malloc(size); }
void kfree(void *ptr) { free(ptr); }
int main(void) {
    assert(ramfs_write("hello.c", "old source", 10) == 0);
    rfile_t *f = ramfs_find("hello.c");
    assert(f && f->size == 10 && !strcmp(f->data,"old source"));
    oom = 1;
    assert(ramfs_write("hello.c", "new", 3) < 0);
    assert(f->size == 10 && !strcmp(f->data,"old source"));
    oom = 0;
    assert(ramfs_write("hello.c", "new", 3) == 0);
    assert(f->size == 3 && !strcmp(f->data,"new"));
    assert(ramfs_write("hello.c", "", 0) == 0 && f->size == 0);
    assert(ramfs_write("hello.c", NULL, 0xFFFFFFFFu) < 0);
    assert(ramfs_delete("hello.c") == 0);
    /* A full table and forced OOM still permit an in-place no-replace rename. */
    for(int i=0;i<RAMFS_MAX_FILES;i++){char name[24];snprintf(name,sizeof(name),"f%d",i);assert(!ramfs_write(name,"data",4));}
    rfile_t *original=ramfs_find("f0");char *body=original->data;oom=1;
    assert(!ramfs_rename("f0","new0") && !ramfs_find("f0") && ramfs_find("new0")==original && original->data==body && original->size==4);
    assert(ramfs_rename("new0","f1")==-5 && ramfs_find("f1") && ramfs_find("new0"));
    assert(!ramfs_rename("new0","new0"));
    const char *bad[]={NULL,"",".","..","a/b","a\\b","bad\n","xxxxxxxxxxxxxxxxxxxxxxxx"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(ramfs_rename("new0",bad[i])==-4 && ramfs_find("new0"));
    assert(ramfs_rename("missing","unused")==-3);
    assert(!ramfs_rename(original->name,original->name+3) && ramfs_find("0")==original && original->data==body);
    assert(!ramfs_rename("0","файл.c") && ramfs_find("файл.c")==original);
    char limit[24];memset(limit,'x',23);limit[23]=0;
    assert(!ramfs_rename("файл.c",limit) && ramfs_find(limit)==original);
    memset(limit,'x',sizeof(limit));assert(ramfs_rename(limit,"valid")==-4);
    oom=0;for(int i=0;i<RAMFS_MAX_FILES;i++)if(files[i].used){char name[24];strcpy(name,files[i].name);assert(!ramfs_delete(name));}
    puts("RAM file replacement: overwrite/empty/OOM +full-table allocation-free no-replace rename/alias/names/UTF8 preserved OK");
    return 0;
}
