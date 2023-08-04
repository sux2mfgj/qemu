#ifndef QEMU_PCI_EPC_H
#define QEMU_PCI_EPC_H

#include <stdint.h>

#define QEMU_EPC_SOCK_PATH "/tmp/qemu.epc.sock"
#define QPCI_EPC_VER 0x00

#define QEPC_MSG_TYPE_VER 0
#define QEPC_MSG_TYPE_PCI_HDR 1

struct qepc_msg_pci_hdr_payload {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision;
    uint16_t class_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
} __attribute__ ((__packed__));

#define QEPC_MSG_TYPE_BAR 2
#define QEPC_MSG_TYPE_IRQ 3

struct qemu_epc_message {
    uint8_t type;
};

#define dbg qemu_log("%s:%d\n", __func__, __LINE__)

#endif /* QEMU_PCI_EPC_H */
