#include "usb_bot.h"
static void put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t get32(const uint8_t *p) { return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
int usb_bot_command(const usb_bot_io_t *io,uint32_t tag,const uint8_t *cdb,
                    unsigned cdb_len,int input,void *data,unsigned size) {
    if(!io || !io->transfer || !io->reset || !cdb || !cdb_len || cdb_len>16 ||
       size>4096 || (size && !data)) return -1;
    uint8_t cbw[31]={0},csw[13];
    put32(cbw,0x43425355); put32(cbw+4,tag); put32(cbw+8,size);
    cbw[12]=input?0x80:0; cbw[14]=(uint8_t)cdb_len;
    for(unsigned i=0;i<cdb_len;i++) cbw[15+i]=cdb[i];
    if(io->transfer(io->context,0,cbw,31)!=31) goto bad;
    int bytes=size?io->transfer(io->context,input!=0,data,size):0;
    if(bytes<0 || (unsigned)bytes>size || (!input && (unsigned)bytes!=size)) goto bad;
    if(io->transfer(io->context,1,csw,13)!=13) goto bad;
    uint32_t residue=get32(csw+8);
    if(get32(csw)!=0x53425355 || get32(csw+4)!=tag || csw[12]>1 || residue>size ||
       (input && residue!=size-(unsigned)bytes)) goto bad;
    /* A USB OUT acknowledgement is not a SCSI write acknowledgement. */
    if(!input && !csw[12] && residue) goto bad;
    return csw[12]?-2:bytes;
bad:
    io->reset(io->context); return -1;
}
