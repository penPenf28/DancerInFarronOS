#include "../libs/types.h"
#include "../libs/param.h"
#include "../libs/memlayout.h"
#include "../libs/riscv.h"

#include "../libs/buf.h"

#ifndef QEMU
#include "../libs/sdcard.h"
#include "../libs/dmac.h"
#else
#include "../libs/virtio.h"
#endif 

void disk_init(void)
{
    #ifdef QEMU
    virtio_disk_init();
	#else 
	sdcard_init();
    #endif
}

void disk_read(struct buf *b)
{
    #ifdef QEMU
	virtio_disk_rw(b, 0);
    #else 
	sdcard_read_sector(b->data, b->sectorno);
	#endif
}

void disk_write(struct buf *b)
{
    #ifdef QEMU
	virtio_disk_rw(b, 1);
    #else 
	sdcard_write_sector(b->data, b->sectorno);
	#endif
}

void disk_intr(void)
{
    #ifdef QEMU
    virtio_disk_intr();
    #else 
    dmac_intr(DMAC_CHANNEL0);
    #endif
}
