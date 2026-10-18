#ifndef KV_NATIVE_NAME_H
#define KV_NATIVE_NAME_H
/* Bounded single-component byte name. No path/controls/dot entries; UTF-8
   bytes are preserved, not normalized. Returns length or -1. */
static inline int native_name_length(const char *name,u32 capacity) {
    if(!name || !capacity)return -1;
    u32 n=0;while(n<capacity && name[n]) {
        u8 c=(u8)name[n];if(c<' ' || c==127 || c=='/' || c=='\\')return -1;n++;
    }
    if(!n || n==capacity || (name[0]=='.' && (n==1 || (n==2 && name[1]=='.'))))return -1;
    return (int)n;
}
#endif
