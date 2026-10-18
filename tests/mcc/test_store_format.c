#include <assert.h>
#include <stdio.h>
#include "kernel.h"
#include "../../kernel/gui_store_policy.h"
int main(void) {
    store_format_request_t r={0,32768,7};
    assert(store_format_valid(&r,0,32768,7));
    assert(!store_format_valid(NULL,0,32768,7));
    assert(!store_format_valid(&r,-1,32768,7));
    assert(!store_format_valid(&r,1,32768,7));
    assert(!store_format_valid(&r,0,32769,7));
    assert(!store_format_valid(&r,0,0,7));
    assert(!store_format_valid(&r,0,32768,8));
    assert(!store_format_valid(&r,0,32768,0));
    r.generation=~0u;assert(!store_format_valid(&r,0,32768,1));
    puts("PASS destructive format software policy: exact drive/capacity/nonzero namespace, stale/wrap/missing/null refusal; no I/O");
}
