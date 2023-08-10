#ifndef QEMU_EPC_H
#define QEMU_EPC_H

#include <stdint.h>

#define QEMU_EPC_PROTOCOL_VERSION 0xdeadbeef
#define QEMU_EPC_SOCK_PATH "/tmp/qemu-epc.sock"

enum {
    QEMU_EPC_MSG_TYPE_VER,
    QEMU_EPC_MSG_TYPE_FD,
    QEMU_EPC_MSG_TYPE_HDR,
    QEMU_EPC_MSG_TYPE_BAR,
    QEMU_EPC_MSG_TYPE_ACCESS_BAR,
};

struct qemu_epc_req_hdr {
    uint32_t type;
};

struct qemu_epc_req_pci_config {
    uint32_t offset;
    uint32_t size;
} __attribute__((packed));

#define QEMU_EPC_REQ_BAR_MASK 1
#define QEMU_EPC_REQ_BAR_BAR 2
struct qemu_epc_resp_bar {
    uint64_t size;
};

struct qemu_epc_req_bar {
    uint8_t type;
};

struct qemu_epc_access_bar {
    uint64_t offset;
    uint64_t size;
#define QEMU_EPC_ACCESS_BAR_READ 0
#define QEMU_EPC_ACCESS_BAR_WRITE 1
    uint8_t type;
    uint8_t bar_no;
} __attribute__((packed));

/*
 * send request to qemu epc bridge
 */
enum {
    EPF_BRIDGE_MSG_IRQ,
};

struct epf_bridge_irq {
#define EPF_BRIDGE_IRQ_TYPE_IRQ 1
#define EPF_BRIDGE_IRQ_TYPE_MSI 2
#define EPF_BRIDGE_IRQ_TYPE_MSIX 3
    uint32_t type;
    uint32_t num;
} __attribute__((packed));

#endif /* QEMU_EPC_H */
