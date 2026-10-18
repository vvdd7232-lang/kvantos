#ifndef KV_USB_BOT_H
#define KV_USB_BOT_H
#include <stdint.h>
/* One synchronous BOT transaction; transport owns endpoint toggles/timeouts.
 * No command replay: an OUT error can mean bytes already reached the medium. */
typedef struct {
    void *context;
    int (*transfer)(void *,int input,void *buffer,unsigned size);
    void (*reset)(void *);
} usb_bot_io_t;
/* byte count, -1 transport/phase error (reset), -2 valid SCSI command failure */
int usb_bot_command(const usb_bot_io_t *io,uint32_t tag,const uint8_t *cdb,
                    unsigned cdb_len,int input,void *data,unsigned size);
#endif
