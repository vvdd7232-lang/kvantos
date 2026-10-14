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
    puts("RAM file replacement: overwrite, empty save and OOM preservation OK");
    return 0;
}
