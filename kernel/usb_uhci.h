/* Private, pinned UHCI DMA per published controller, including fault paths.
 * Queue/list links are detached and retired before reuse. A stopped frame
 * clock quarantines the slot and disables PCI bus mastering, not a retry. */
#ifndef UHCI_PAUSE
#define UHCI_PAUSE() __asm__ volatile("pause")
#endif
static td_t uhci_tds[HC_MAX][TD_MAX] __attribute__((aligned(16)));
static u8 uhci_data[HC_MAX][4096] __attribute__((aligned(4096)));
static u32 token(u8 pid, usb_disk_t *d, u8 ep, int toggle, unsigned len) {
    return pid | ((u32)d->address << 8) | ((u32)ep << 15) |
           ((u32)toggle << 19) | (((len - 1) & 2047) << 21);
}
static int uhci_transfer(usb_disk_t *d,u8 pid,u8 ep,u16 mps,
                         int *toggle,void *buf,unsigned len) {
    if(d->hc->dead || !toggle || (len && !buf) || len>4096 || ep>15 || d->address>127 ||
       (mps!=8 && mps!=16 && mps!=32 && mps!=64) || (pid!=IN && pid!=OUT && pid!=SETUP)) return -1;
    td_t *tds=uhci_tds[d->hc-hcs];
    u8 *data=uhci_data[d->hc-hcs];
    if((port_status(d->hc,d->port)&5)!=5) return -1;
    unsigned count = len ? (len + mps - 1) / mps : 1;
    if (count > TD_MAX) return -1;
    if (pid != IN && len) memcpy(data, buf, len);
    unsigned off = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned bytes = len - off;
        if (bytes > mps) bytes = mps;
        tds[i].link = i + 1 < count ? physical(&tds[i + 1]) | 4 : TERM;
        tds[i].status = ACTIVE | (3u << 27) | (pid == IN ? 1u << 29 : 0);
        tds[i].token = token(pid, d, ep, (*toggle + i) & 1, bytes);
        tds[i].buffer = physical(data + off);
        off += bytes;
    }
    qh_t *q = &queues[d->hc - hcs];
    __asm__ volatile("" ::: "memory");
    q->element = physical(tds);
    unsigned start = inw(d->hc->io + 6) & 2047, done = 0;
    int result = -1, total = 0;
    u64 until = clock_now() + 1000000000ULL;
    while (((inw(d->hc->io + 6) - start) & 2047u) < 250 && clock_now() < until) {
        u32 port=port_status(d->hc,d->port);
        if((port&5)!=5 || (d->generation && (port&0xA))) break;
        while (done < count) {
            u32 status = tds[done].status;
            if (status & ACTIVE) break;
            if (status & TD_ERRORS) goto detach;
            unsigned actual = (status + 1) & 2047;
            unsigned expected = ((tds[done].token >> 21) + 1) & 2047;
            if (actual > expected) goto detach;
            total += actual; done++;
            if (actual < expected || done == count) { result = total; goto detach; }
        }
        if (inw(d->hc->io + 2) & 0x20) break;
        UHCI_PAUSE();
    }
detach:
    q->element = TERM;
    __asm__ volatile("" ::: "memory");
    if (!frames_wait(d->hc, 2)) { usb_stop(d->hc);return -1; }
    if (result >= 0) {
        *toggle = (*toggle + done) & 1;
        if (pid == IN && result) memcpy(buf, data, (u32)result);
    }
    return result;
}
