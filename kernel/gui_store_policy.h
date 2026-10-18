#ifndef KV_GUI_STORE_POLICY_H
#define KV_GUI_STORE_POLICY_H
/* Software identity only, not physical ATA hotplug qualification. */
typedef struct {int drive;u32 sectors,generation;} store_format_request_t;
static inline int store_format_valid(const store_format_request_t *request,int drive,u32 sectors,u32 generation) {
    return request && drive>=0 && sectors && generation && request->drive==drive &&
           request->sectors==sectors && request->generation==generation;
}
#endif
