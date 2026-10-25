#ifndef NVME_H
#define NVME_H

#include <stdint.h>
#include <pci/pci.h>

#define NVME_REG_CAP    0x0000  // Controller Capabilities
#define NVME_REG_VS     0x0008  // Version
#define NVME_REG_INTMS  0x000C  // Interrupt Mask Set
#define NVME_REG_INTMC  0x0010  // Interrupt Mask Clear
#define NVME_REG_CC     0x0014  // Controller Configuration
#define NVME_REG_CSTS   0x001C  // Controller Status
#define NVME_REG_AQA    0x0024  // Admin Queue Attributes
#define NVME_REG_ASQ    0x0028  // Admin Submission Queue Base Address
#define NVME_REG_ACQ    0x0030  // Admin Completion Queue Base Address

#define NVME_REG_DBS    0x1000

#define NVME_ADMIN_QUEUE_SIZE 64
#define NVME_IO_QUEUE_SIZE    64

#define NVME_ADMIN_IDENTIFY       0x06
#define NVME_ADMIN_CREATE_SQ      0x01
#define NVME_ADMIN_CREATE_CQ      0x05
#define NVME_ADMIN_DELETE_SQ      0x00
#define NVME_ADMIN_DELETE_CQ      0x04
#define NVME_ADMIN_GET_FEATURES   0x0A
#define NVME_ADMIN_SET_FEATURES   0x09

#define NVME_IO_READ              0x02
#define NVME_IO_WRITE             0x01
#define NVME_IO_FLUSH             0x00

#define NVME_CNS_NAMESPACE        0x00
#define NVME_CNS_CONTROLLER       0x01
#define NVME_CNS_ACTIVE_NS_LIST   0x02

#define NVME_ADMIN_Q_DEPTH 64

#define NVME_CQE_PHASE(status)  ((status) & 1)
#define NVME_CQE_STATUS(status) ((status) >> 1)
#define NVME_CQE_IS_SUCCESS(status) (NVME_CQE_STATUS(status) == 0)

struct nvme_sqe {
    uint32_t cdw0;   // Dword 0: Opcode (7:0), Fuse (9:8), CID (31:16)
    uint32_t nsid;   // Namespace Identifier
    uint32_t cdw2;   // Reserved
    uint32_t cdw3;   // Reserved
    uint64_t mptr;   // Metadata Pointer
    uint64_t prp1;   // PRP Entry 1
    uint64_t prp2;   // PRP Entry 2
    uint32_t cdw10;  // Command-specific
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed));

struct nvme_cqe {
    uint32_t result;   // Command-specific result
    uint32_t rsvd;     // Reserved
    uint16_t sq_head;  // Submission Queue Head Pointer
    uint16_t sq_id;    // Submission Queue Identifier
    uint16_t cid;      // Command Identifier
    uint16_t status;   // Status: Phase (0), Status (15:1)
} __attribute__((packed));

struct nvme_cap {
    uint16_t mqes;      // Maximum Queue Entries Supported (0-based)
    uint8_t  cqr;       // Contiguous Queues Required
    uint8_t  ams;       // Arbitration Mechanism Supported
    uint8_t  rsvd1[2];
    uint8_t  to;        // Timeout
    uint8_t  dstrd;     // Doorbell Stride (2^(2+dstrd) bytes)
    uint8_t  nssrs;     // NVM Subsystem Reset Supported
    uint8_t  css;       // Command Sets Supported
    uint8_t  rsvd2;
    uint8_t  mpsmin;    // Memory Page Size Minimum
    uint8_t  mpsmax;    // Memory Page Size Maximum
    uint8_t  rsvd3[22];
} __attribute__((packed));

struct nvme_cc {
    uint16_t en      : 1;   // Enable
    uint16_t rsvd1   : 3;
    uint16_t css     : 3;   // I/O Command Set Selected
    uint16_t mps     : 4;   // Memory Page Size
    uint16_t ams     : 3;   // Arbitration Mechanism Selected
    uint16_t shn     : 2;   // Shutdown Notification
    uint16_t iosqes  : 4;   // I/O Submission Queue Entry Size
    uint16_t iocqes  : 4;   // I/O Completion Queue Entry Size
    uint16_t rsvd2   : 8;
} __attribute__((packed));

struct nvme_csts {
    uint32_t rdy    : 1;   // Ready
    uint32_t cfs    : 1;   // Controller Fatal Status
    uint32_t shst   : 2;   // Shutdown Status
    uint32_t rsvd   : 28;
} __attribute__((packed));

struct nvme_aqa {
    uint16_t asqs;  // Admin Submission Queue Size (0-based)
    uint16_t acqs;  // Admin Completion Queue Size (0-based)
} __attribute__((packed));

struct nvme_id_ctrl {
    uint16_t vid;       // PCI Vendor ID
    uint16_t ssvid;     // PCI Subsystem Vendor ID
    char     sn[20];    // Serial Number
    char     mn[40];    // Model Number
    char     fr[8];     // Firmware Revision
    uint8_t  rab;       // Recommended Arbitration Burst
    uint8_t  ieee[3];   // IEEE OUI Identifier
    uint8_t  cmic;      // Controller Multi-Path I/O
    uint8_t  mdts;      // Maximum Data Transfer Size (2^n pages)
    uint16_t cntlid;    // Controller ID
    uint32_t ver;       // Version
    uint32_t rtd3r;     // RTD3 Resume Latency
    uint32_t rtd3e;     // RTD3 Entry Latency
    uint32_t oaes;      // Optional Asynchronous Events Supported
    uint32_t ctratt;    // Controller Attributes
    uint8_t  rsvd100[156];
    uint16_t oacs;      // Optional Admin Command Support
    uint8_t  acl;       // Abort Command Limit
    uint8_t  aerl;      // Asynchronous Event Request Limit
    uint8_t  frmw;      // Firmware Updates
    uint8_t  lpa;       // Log Page Attributes
    uint8_t  elpe;      // Error Log Page Entries
    uint8_t  npss;      // Number of Power States Support
    uint8_t  avscc;     // Admin Vendor Specific Command Config
    uint8_t  apsta;     // Autonomous Power State Transition Attr
    uint16_t wctemp;    // Warning Composite Temperature
    uint16_t cctemp;    // Critical Composite Temperature
    uint8_t  rsvd270[242];
    uint8_t  sqes;      // Submission Queue Entry Size
    uint8_t  cqes;      // Completion Queue Entry Size
    uint8_t  rsvd514[2];
    uint32_t nn;        // Number of Namespaces
    uint16_t oncs;      // Optional NVM Command Support
    uint16_t fuses;     // Fused Operation Support
    uint8_t  fna;       // Format NVM Attributes
    uint8_t  vwc;       // Volatile Write Cache
    uint16_t awun;      // Atomic Write Unit Normal
    uint16_t awupf;     // Atomic Write Unit Power Fail
    uint8_t  nvscc;     // NVM Vendor Specific Command Config
    uint8_t  rsvd531;
    uint16_t acwu;      // Atomic Compare & Write Unit
    uint8_t  rsvd534[2];
    uint32_t sgls;      // SGL Support
    uint8_t  rsvd540[228];
    char     subnqn[256]; // NVM Subsystem NVMe Qualified Name
    uint8_t  rsvd1024[768];
    uint32_t ioccs;     // I/O Command Set Combined Size
} __attribute__((packed));

struct nvme_lbaf {
    uint16_t ms;    // Metadata Size
    uint8_t  ds;    // LBA Data Size (2^n)
    uint8_t  rp;    // Relative Performance
} __attribute__((packed));

struct nvme_id_ns {
    uint64_t nsze;   // Namespace Size (total logical blocks)
    uint64_t ncap;   // Namespace Capacity
    uint64_t nuse;   // Namespace Utilization
    uint8_t  nsfeat; // Namespace Features
    uint8_t  nlbaf;  // Number of LBA Formats (0-based)
    uint8_t  flbas;  // Formatted LBA Size
    uint8_t  mc;     // Metadata Capabilities
    uint8_t  dpc;    // End-to-end Data Protection Capabilities
    uint8_t  dps;    // End-to-end Data Protection Type Settings
    uint8_t  nmic;   // Namespace Multi-path I/O and Sharing Cap
    uint8_t  rescap; // Reservation Capabilities
    uint8_t  fpi;    // Format Progress Indicator
    uint8_t  dlfeat; // Deallocate Logical Block Features
    uint16_t nawun;  // Namespace Atomic Write Unit Normal
    uint16_t nawupf; // Namespace Atomic Write Unit Power Fail
    uint16_t nacwu;  // Namespace Atomic Compare & Write Unit
    uint16_t nabsn;  // Namespace Atomic Boundary Size Normal
    uint16_t nabo;   // Namespace Atomic Boundary Offset
    uint16_t nabspf; // Namespace Atomic Boundary Size Power Fail
    uint16_t noiob;  // Namespace Optimal I/O Boundary
    uint8_t  nvmcap[16]; // NVM Capacity
    uint8_t  rsvd64[40];
    uint8_t  nguid[16];  // Namespace Globally Unique Identifier
    uint8_t  eui64[8];   // IEEE Extended Unique Identifier
    struct nvme_lbaf lbaf[64]; // LBA Formats
    uint8_t  rsvd192[192];
    uint8_t  vs[3712];   // Vendor Specific
} __attribute__((packed));

struct nvme_queue {
    struct nvme_sqe *sq;
    struct nvme_cqe *cq;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t qid;
    uint16_t q_depth;
    uint8_t  phase;
    volatile uint32_t *sq_doorbell;
    volatile uint32_t *cq_doorbell;
    uint16_t cid;
} __attribute__((aligned(4096)));

struct nvme_ctrl {
    volatile uint8_t *base;
    void *asq, *acq;
    uint64_t asq_phys, acq_phys;
    uint16_t admin_sq_tail, admin_cq_head, admin_cid;
    uint8_t  admin_phase;

    void *io_sq, *io_cq;
    uint64_t io_sq_phys, io_cq_phys;
    uint16_t io_sq_tail, io_cq_head, io_cid;
    uint8_t  io_phase;

    uint32_t nsid;
    uint64_t nsze;
    uint32_t sector_size;
};

void nvme_init(pci_device_t *dev);

#endif