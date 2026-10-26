#ifndef KV_PROCFS_H
#define KV_PROCFS_H
/* Read-only telemetry. No handles, kernel pointers or storage writes. */
#define PROCFS_TEXT_MAX 1024u
int procfs_vfs_register(void);
#endif
