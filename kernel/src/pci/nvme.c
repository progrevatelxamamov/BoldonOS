#include <stdint.h>
#include <stdbool.h>
#include <pci/pci.h>
#include <pci/nvme.h>
#include <mm/malloc.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <kernel.h>
#include <bootloader.h>
#include <gconsole.h>
#include <memory.h>
#include <disk/block.h>

static inline uint32_t mmio_read32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}
static inline void mmio_write32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}

static bool nvme_submit_admin_cmd(volatile uint8_t *base, 
                                 void *asq, void *acq,
                                 uint16_t *sq_tail, uint16_t *cq_head, 
                                 uint8_t *phase, uint16_t cid,
                                 struct nvme_sqe *cmd_data) 
{
    struct nvme_sqe *sqe = (struct nvme_sqe *)((uint8_t *)asq + (*sq_tail) * 64);
    memcpy(sqe, cmd_data, sizeof(struct nvme_sqe));
    sqe->cdw0 |= (cid << 16);

    asm volatile("" ::: "memory");

    *sq_tail = (*sq_tail + 1) % NVME_ADMIN_Q_DEPTH;
    mmio_write32(base, 0x1000, *sq_tail);

    volatile struct nvme_cqe *cqe = (volatile struct nvme_cqe *)((uint8_t *)acq + (*cq_head) * 16);
    uint64_t timeout = 100000000;
    while (((cqe->status & 1) != *phase)) {
        if (--timeout == 0) {
            console_print_string("[NVMe] Admin Command Timeout (CID=");
            console_print_hex(cid);
            console_print_string(")\n");
            return false;
        }
    }

    uint16_t status = cqe->status >> 1;
    if (status != 0) {
        console_print_string("[NVMe] Admin Command Failed (CID=");
        console_print_hex(cid);
        console_print_string(", Status=");
        console_print_hex(status);
        console_print_string(")\n");
        return false;
    }

    *cq_head = (*cq_head + 1) % NVME_ADMIN_Q_DEPTH;
    mmio_write32(base, 0x1004, *cq_head);

    if (*cq_head == 0)
        *phase ^= 1;

    return true;
}

int nvme_io_read(struct nvme_ctrl *c, uint64_t lba, uint16_t nlb, uint64_t buf_phys) {
    uint32_t total_bytes = nlb * c->sector_size;

    struct nvme_sqe *sqe = (struct nvme_sqe *)
        ((uint8_t *)c->io_sq + c->io_sq_tail * 64);
    memset(sqe, 0, 64);

    sqe->cdw0 = 0x02 | (c->io_cid << 16);   // Read Opcode
    sqe->nsid = c->nsid;
    sqe->prp1 = buf_phys;

    // Вычисляем, сколько байт помещается в первой физической странице
    uint64_t page_offset = buf_phys & 0xFFF;
    uint64_t first_page_bytes = 4096 - page_offset;

    if (total_bytes > first_page_bytes) {
        sqe->prp2 = (buf_phys + first_page_bytes) & ~0xFFFULL;
    } else {
        sqe->prp2 = 0;
    }

    sqe->cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
    sqe->cdw11 = (uint32_t)(lba >> 32);
    sqe->cdw12 = nlb - 1; // 0-based

    asm volatile("" ::: "memory");

    c->io_sq_tail = (c->io_sq_tail + 1) % 64;
    mmio_write32(c->base, 0x1008, c->io_sq_tail);

    volatile struct nvme_cqe *cqe = (volatile struct nvme_cqe *)
        ((uint8_t *)c->io_cq + c->io_cq_head * 16);

    uint64_t t = 100000000;
    while ((cqe->status & 1) != c->io_phase) {
        if (--t == 0) return -1;
    }

    if ((cqe->status >> 1) != 0) return -2;
 
    c->io_cq_head = (c->io_cq_head + 1) % 64;
    mmio_write32(c->base, 0x100C, c->io_cq_head);

    if (c->io_cq_head == 0)
        c->io_phase ^= 1;

    c->io_cid++;
    return 0;
}

int nvme_io_write(struct nvme_ctrl *c, uint64_t lba, uint16_t nlb, uint64_t buf_phys) {
    uint32_t total_bytes = nlb * c->sector_size;

    struct nvme_sqe *sqe = (struct nvme_sqe *)
        ((uint8_t *)c->io_sq + c->io_sq_tail * 64);
    memset(sqe, 0, 64);

    sqe->cdw0 = 0x01 | (c->io_cid << 16);   // ← Write Opcode (0x01)
    sqe->nsid = c->nsid;
    sqe->prp1 = buf_phys;

    uint64_t page_offset = buf_phys & 0xFFF;
    uint64_t first_page_bytes = 4096 - page_offset;

    if (total_bytes > first_page_bytes) {
        sqe->prp2 = (buf_phys + first_page_bytes) & ~0xFFFULL;
    } else {
        sqe->prp2 = 0;
    }

    sqe->cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
    sqe->cdw11 = (uint32_t)(lba >> 32);
    sqe->cdw12 = nlb - 1;

    asm volatile("" ::: "memory");

    c->io_sq_tail = (c->io_sq_tail + 1) % 64;
    mmio_write32(c->base, 0x1008, c->io_sq_tail);

    volatile struct nvme_cqe *cqe = (volatile struct nvme_cqe *)
        ((uint8_t *)c->io_cq + c->io_cq_head * 16);

    uint64_t t = 100000000;
    while ((cqe->status & 1) != c->io_phase) {
        if (--t == 0) {
            console_print_string("[NVMe] I/O Write timeout\n");
            return -1;
        }
    }

    if ((cqe->status >> 1) != 0) {
        console_print_string("[NVMe] I/O Write status=");
        console_print_hex(cqe->status);
        console_print_string(" cid=");
        console_print_hex(cqe->cid);
        console_print_string(" sq_head=");
        console_print_hex(cqe->sq_head);
        console_print_string("\n");
        return -2;
    }

    c->io_cq_head = (c->io_cq_head + 1) % 64;
    mmio_write32(c->base, 0x100C, c->io_cq_head);

    if (c->io_cq_head == 0)
        c->io_phase ^= 1;

    c->io_cid++;
    return 0;
}

int nvme_io_flush(struct nvme_ctrl *c) {
    struct nvme_sqe *sqe = (struct nvme_sqe *)
        ((uint8_t *)c->io_sq + c->io_sq_tail * 64);
    memset(sqe, 0, 64);

    sqe->cdw0 = 0x00 | (c->io_cid << 16);
    sqe->nsid = c->nsid;

    asm volatile("" ::: "memory");

    c->io_sq_tail = (c->io_sq_tail + 1) % 64;
    mmio_write32(c->base, 0x1008, c->io_sq_tail);

    volatile struct nvme_cqe *cqe = (volatile struct nvme_cqe *)
        ((uint8_t *)c->io_cq + c->io_cq_head * 16);

    uint64_t t = 100000000;
    while ((cqe->status & 1) != c->io_phase) {
        if (--t == 0) return -1;
    }

    if ((cqe->status >> 1) != 0) return -2;

    c->io_cq_head = (c->io_cq_head + 1) % 64;
    mmio_write32(c->base, 0x100C, c->io_cq_head);

    if (c->io_cq_head == 0)
        c->io_phase ^= 1;

    c->io_cid++;
    return 0;
}

#define NVME_BOUNCE_PAGES 2 // 2 страницы (8 КБ) - максимум для схемы PRP1 + PRP2 без PRP List

static int nvme_block_read(void* local_device_struct, uint64_t lba, uint32_t count, void *buf_virt) {
    struct nvme_ctrl *g = (struct nvme_ctrl*)local_device_struct;
    uint32_t sector_size = g->sector_size;

    if (lba + count > g->nsze) {
        console_print_string("[block] read out of range\n");
        return -1;
    }

    uint32_t max_sectors_per_bounce = (4096 * NVME_BOUNCE_PAGES) / sector_size;

    uint8_t *out = (uint8_t *)buf_virt;
    uint64_t cur_lba = lba;
    uint32_t remaining = count;

    uint64_t bounce_phys = (uint64_t)pmm_alloc_pages(NVME_BOUNCE_PAGES);
    if (!bounce_phys) return -1;
    void *bounce_virt = (void *)(bounce_phys + get_bootinfo().hhdm->offset);

    while (remaining > 0) {
        uint32_t chunk = remaining;
        if (chunk > max_sectors_per_bounce)
            chunk = max_sectors_per_bounce;

        int rc = nvme_io_read(g, cur_lba, chunk, bounce_phys);
        if (rc != 0) {
            console_print_string("[block] nvme read failed\n");
            for (uint64_t cnt = 0; cnt < NVME_BOUNCE_PAGES; cnt++) {
                pmm_free_page((void*)(bounce_phys + (4096 * cnt)));
            }
            return rc;
        }

        memcpy(out, bounce_virt, chunk * sector_size);

        out += chunk * sector_size;
        cur_lba += chunk;
        remaining -= chunk;
    }

    for (uint64_t cnt = 0; cnt < NVME_BOUNCE_PAGES; cnt++) {
        pmm_free_page((void*)(bounce_phys + (4096 * cnt)));
    }
    return 0;
}

static int nvme_block_write(void* local_device_struct, uint64_t lba, uint32_t count, void *buf_virt) {
    struct nvme_ctrl *g = (struct nvme_ctrl*)local_device_struct;
    uint32_t sector_size = g->sector_size;

    if (lba + count > g->nsze) {
        console_print_string("[block] write out of range\n");
        return -1;
    }

    uint32_t max_sectors_per_bounce = (4096 * NVME_BOUNCE_PAGES) / sector_size;

    uint8_t *in = (uint8_t *)buf_virt;
    uint64_t cur_lba = lba;
    uint32_t remaining = count;

    uint64_t bounce_phys = (uint64_t)pmm_alloc_pages(NVME_BOUNCE_PAGES);
    if (!bounce_phys) return -1;
    void *bounce_virt = (void *)(bounce_phys + get_bootinfo().hhdm->offset);

    while (remaining > 0) {
        uint32_t chunk = remaining;
        if (chunk > max_sectors_per_bounce)
            chunk = max_sectors_per_bounce;

        memcpy(bounce_virt, in, chunk * sector_size);

        int rc = nvme_io_write(g, cur_lba, chunk, bounce_phys);
        if (rc != 0) {
            console_print_string("[block] nvme write failed\n");
            for (uint64_t cnt = 0; cnt < NVME_BOUNCE_PAGES; cnt++) {
                pmm_free_page((void*)(bounce_phys + (4096 * cnt)));
            }
            return rc;
        }

        in += chunk * sector_size;
        cur_lba += chunk;
        remaining -= chunk;
    }

    for (uint64_t cnt = 0; cnt < NVME_BOUNCE_PAGES; cnt++) {
        pmm_free_page((void*)(bounce_phys + (4096 * cnt)));
    }
    return 0;
}

static int nvme_block_flush(void* local_device_struct) {
    return nvme_io_flush((struct nvme_ctrl *)local_device_struct);
}

void nvme_init(pci_device_t *dev) {
    uint64_t bar0_phys = (uint64_t)(dev->bars[0] & 0xFFFFFFF0)
                    | ((uint64_t)dev->bars[1] << 32);
    uint64_t bar0_virt = bar0_phys + get_bootinfo().hhdm->offset;
    volatile uint8_t *base = (volatile uint8_t *)bar0_virt;

    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1u << 1) | (1u << 2);
    pci_write_config(dev->bus, dev->slot, dev->func, 0x04, cmd);

    uint64_t bar_size = 0x4000;
    for (uint64_t off = 0; off < bar_size; off += 0x1000) {
        vmm_map_page(get_kernel_pml4(),
                    bar0_phys + off,
                    bar0_virt + off,
                    PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);
    }

    uint32_t csts = mmio_read32(base, 0x1C);
    if (csts & 1) {
        uint32_t cc = mmio_read32(base, 0x14);
        cc &= ~1u; // EN = 0
        mmio_write32(base, 0x14, cc);

        while (mmio_read32(base, 0x1C) & 1) {
        }
    }

    uint32_t cc = 0;
    cc |= (0u << 4);    // MPS = 0 → 4 KB
    cc |= (0u << 7);    // AMS = 0 → Round Robin
    cc |= (0u << 11);   // SHN = 0
    cc |= (6u << 16);   // IOSQES = 6 → 64 bytes
    cc |= (4u << 20);   // IOCQES = 4 → 16 bytes
    cc |= (0u << 1);    // CSS = 0 → NVM Command Set
    cc |= (1u << 0);    // EN = 1

    uint64_t asq_phys = (uint64_t)pmm_alloc_page_and_clear();
    uint64_t acq_phys = (uint64_t)pmm_alloc_page_and_clear();
    void *asq = (void*)(asq_phys + get_bootinfo().hhdm->offset);
    void *acq = (void*)(acq_phys + get_bootinfo().hhdm->offset);

    uint32_t aqa = (NVME_ADMIN_Q_DEPTH - 1) | ((NVME_ADMIN_Q_DEPTH - 1) << 16);
    mmio_write32(base, 0x24, aqa);  

    mmio_write32(base, 0x28, (uint32_t)(asq_phys & 0xFFFFFFFF));
    mmio_write32(base, 0x2C, (uint32_t)(asq_phys >> 32));

    mmio_write32(base, 0x30, (uint32_t)(acq_phys & 0xFFFFFFFF));
    mmio_write32(base, 0x34, (uint32_t)(acq_phys >> 32));

    mmio_write32(base, 0x14, cc);
    
    while (!(mmio_read32(base, 0x1C) & 1)) {
    }

    uint16_t admin_sq_tail = 0;
    uint16_t admin_cq_head = 0;
    uint8_t  admin_phase   = 1;
    uint16_t admin_cid     = 1;

    uint64_t id_phys = (uint64_t)pmm_alloc_page();
    void *id_buf = (void *)(id_phys + get_bootinfo().hhdm->offset);

    struct nvme_sqe cmd_id_ctrl;
    memset(&cmd_id_ctrl, 0, sizeof(cmd_id_ctrl));
    cmd_id_ctrl.cdw0 = 0x06;
    cmd_id_ctrl.nsid = 0;
    cmd_id_ctrl.prp1 = id_phys;
    cmd_id_ctrl.cdw10 = 0x01;

    if (!nvme_submit_admin_cmd(base, asq, acq, &admin_sq_tail, &admin_cq_head, 
                              &admin_phase, admin_cid++, &cmd_id_ctrl)) return;

    char model[41];
    memcpy(model, (uint8_t *)id_buf + 24, 40);
    model[40] = 0;
    console_print_string("[NVMe] Model: ");
    console_print_string(model);
    console_print_string("\n");

    struct nvme_sqe cmd_id_ns;
    memset(&cmd_id_ns, 0, sizeof(cmd_id_ns));
    cmd_id_ns.cdw0 = 0x06;
    cmd_id_ns.nsid = 1;
    cmd_id_ns.prp1 = id_phys;
    cmd_id_ns.cdw10 = 0x00;

    if (!nvme_submit_admin_cmd(base, asq, acq, &admin_sq_tail, &admin_cq_head, 
                              &admin_phase, admin_cid++, &cmd_id_ns)) return;

    uint64_t nsze = *(uint64_t *)((uint8_t *)id_buf + 0);
    uint8_t flbas = *(uint8_t *)((uint8_t *)id_buf + 26);
    uint8_t ds = *(uint8_t *)((uint8_t *)id_buf + 128 + (flbas & 0x0F) * 4 + 2);
    uint32_t sector_size = 1u << ds;

    uint64_t io_cq_phys = (uint64_t)pmm_alloc_page_and_clear();
    struct nvme_sqe cmd_create_cq;
    memset(&cmd_create_cq, 0, sizeof(cmd_create_cq));
    cmd_create_cq.cdw0 = 0x05;
    cmd_create_cq.prp1 = io_cq_phys;
    cmd_create_cq.cdw10 = 1 | ((64 - 1) << 16); // QID=1, Size=63 (0-based)
    cmd_create_cq.cdw11 = (1u << 0);            // PC = 1

    if (!nvme_submit_admin_cmd(base, asq, acq, &admin_sq_tail, &admin_cq_head, 
                              &admin_phase, admin_cid++, &cmd_create_cq)) return;

    uint64_t io_sq_phys = (uint64_t)pmm_alloc_page_and_clear();
    struct nvme_sqe cmd_create_sq;
    memset(&cmd_create_sq, 0, sizeof(cmd_create_sq));
    cmd_create_sq.cdw0 = 0x01;
    cmd_create_sq.prp1 = io_sq_phys;
    cmd_create_sq.cdw10 = 1 | ((64 - 1) << 16);  // QID=1, Size=63 (0-based)
    cmd_create_sq.cdw11 = (1u << 0) | (1u << 16); // PC=1, CQID=1

    if (!nvme_submit_admin_cmd(base, asq, acq, &admin_sq_tail, &admin_cq_head, 
                              &admin_phase, admin_cid++, &cmd_create_sq)) return;
    
    // volatile uint8_t *base;
    // void *asq, *acq;
    // uint64_t asq_phys, acq_phys;
    // uint16_t admin_sq_tail, admin_cq_head, admin_cid;
    // uint8_t  admin_phase;

    // void *io_sq, *io_cq;
    // uint64_t io_sq_phys, io_cq_phys;
    // uint16_t io_sq_tail, io_cq_head, io_cid;
    // uint8_t  io_phase;

    // uint32_t nsid;
    // uint64_t nsze;
    // uint32_t sector_size;

    struct nvme_ctrl *g_nvme = (struct nvme_ctrl*)malloc_alloc(sizeof(struct nvme_ctrl));

    g_nvme->base = base;
    g_nvme->asq = asq;
    g_nvme->acq = acq;
    g_nvme->asq_phys = asq_phys;
    g_nvme->acq_phys = acq_phys;
    g_nvme->admin_sq_tail = admin_sq_tail;
    g_nvme->admin_cq_head = admin_cq_head;
    g_nvme->admin_cid = admin_cid;
    g_nvme->io_sq = (void*)(io_sq_phys + get_bootinfo().hhdm->offset);
    g_nvme->io_cq = (void*)(io_cq_phys + get_bootinfo().hhdm->offset);
    g_nvme->io_sq_phys = io_sq_phys;
    g_nvme->io_cq_phys = io_cq_phys;
    g_nvme->io_sq_tail = 0;
    g_nvme->io_cq_head = 0;
    g_nvme->io_cid = 1;
    g_nvme->io_phase = 1;
    g_nvme->nsid = 1;
    g_nvme->nsze = nsze;
    g_nvme->sector_size = sector_size;

    struct block_device *blockdev = (struct block_device*)malloc_alloc(sizeof(struct block_device));
    blockdev->type = DISK_TYPE_NVME;
    blockdev->sector_size = g_nvme->sector_size;
    blockdev->sector_count = g_nvme->nsze;
    blockdev->local_device_struct = (void*)g_nvme;
    blockdev->read = (block_read_fn)nvme_block_read;
    blockdev->write = (block_write_fn)nvme_block_write;
    blockdev->flush = (block_flush_fn)nvme_block_flush;

    register_device(blockdev);
    malloc_free((void*)blockdev);

    console_print_string("[NVMe] Initialized Successfully!\n");
}