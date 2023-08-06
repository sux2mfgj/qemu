/*
 * QEMU PCI endpoint controller device
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/thread.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_regs.h"

#include "qemu-epc.h"

#define TYPE_PCI_EPC "pci-epc"

/*
 * PCI EPC register space
 * - offset 0: pci configuration space
 * u16 vendor id
 * u16 device id
 * u8 revison id
 * u8 prog_if
 * u8 sub class code
 * u8 class code
 * u8 cache line size
 * u8 reserved0
 * u16 reserved1
 * u16 subsys vendor id
 * u16 subsys id
 * u8 irq_pin
 * u8 reserved
 * u16 reserved
 *
 * - offset 0x10: bar configuration
 * u8 bar_mask
 * u8 bar_no
 * u16 reserved
 * u32 flags
 * u64 bar_size
 * u64 phys_addr
 */

#define QEMU_PCI_EPC_VER 0x00
enum {
    // for pci configration space
    REG_OFFSET_VENDOR_ID = 0x0,
    REG_OFFSET_DEVICE_ID = 0x2,
    REG_OFFSET_REVISON_ID = 0x4,
    REG_OFFSET_PROG_ID = 0x5,
    REG_OFFSET_SUB_CLASS_CODE = 0x6,
    REG_OFFSET_CLASS_CODE = 0x7,
    REG_OFFSET_CACHE_LINE_SIZE = 0x8,
    REG_OFFSET_SUBSYS_VENDOR_ID = 0xc,
    REG_OFFSET_SUBSYS_ID = 0xe,
    REG_OFFSET_IRQ_PIN = 0x10,

    // for BAR configuration
    REG_OFFSET_BAR_START = 0x14,
    REG_OFFSET_BAR_MASK = REG_OFFSET_BAR_START,
    REG_OFFSET_BAR_NO = 0x15,
    REG_OFFSET_BAR_FLAGS = 0x18,
    REG_OFFSET_BAR_PHYS_ADDR = 0x1c,
    REG_OFFSET_BAR_SIZE = 0x24,

    REG_SIZE = 0x2c
};

struct pci_epc_bar {
    uint64_t phys_addr;
    uint64_t size;
    uint32_t flags;
};

typedef struct PCIEPCState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    QemuThread srv_thread;
    int srv_fd, clt_fd;

    MemoryRegion ctrl, cfg;

    uint8_t config_space[0x10];

    uint8_t bar_mask;
    uint8_t bar_no;
    struct pci_epc_bar bars[6];

} PCIEPCState;

OBJECT_DECLARE_SIMPLE_TYPE(PCIEPCState, PCI_EPC);

static uint64_t pciepc_mmio_ctl_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log("%s:%d\n", __func__, __LINE__);
    return 0;
}

static void pciepc_mmio_ctl_write(void* opaque,
                              hwaddr addr,
                              uint64_t val,
                              unsigned size)
{
    qemu_log("%s:%d\n", __func__, __LINE__);
}

static const MemoryRegionOps pciepc_mmio_ctl_ops = {
    .read = pciepc_mmio_ctl_read,
    .write = pciepc_mmio_ctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t pciepc_mmio_cfg_read(void* opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void pciepc_mmio_cfg_write(void* opaque,
                              hwaddr addr,
                              uint64_t val,
                              unsigned size)
{
    PCIEPCState* state = opaque;

    qemu_log("%s addr %lx, val 0x%lx, size %d\n", __func__, addr, val, size);
    if (addr + size <= REG_OFFSET_BAR_START) {
        memcpy(&state->config_space[addr], &val, size);
        return;
    }

    switch(addr) {
        case REG_OFFSET_BAR_MASK:
            if (!size)
                break;
            state->bar_mask = (uint8_t)val;
            val >>= 8;
            size -= 1;
            /* fall through */
        case REG_OFFSET_BAR_NO:
            if (!size)
                break;
            state->bar_no = (uint8_t)val;
            val >>= 8;
            size -= 1;
            qemu_log("set bar no %d, size %d\n", (uint8_t)val, size);
            /* fall through */
        case REG_OFFSET_BAR_FLAGS:
            if (size < 4)
                break;
            state->bars[state->bar_no].flags = (uint32_t)val;
            val >>= 32;
            size -= 4;
            qemu_log("set bar[%d] flags 0x%x, size %d\n", state->bar_no, (uint32_t)val, size);
            /* fall through */
        case REG_OFFSET_BAR_PHYS_ADDR:
        case REG_OFFSET_BAR_PHYS_ADDR + 4:
            if (size < 4)
                break;
            if (REG_OFFSET_BAR_PHYS_ADDR) {
                state->bars[state->bar_no].phys_addr &= 0xffffffff00000000;
                state->bars[state->bar_no].phys_addr  |= (uint32_t)val;
            }
            else {
                state->bars[state->bar_no].phys_addr &= 0xffffffff;
                state->bars[state->bar_no].phys_addr  |= val << 32;
            }
            qemu_log("set bar[%d] addr 0x%lx, size %d\n", state->bar_no, val, size);
            break;
        case REG_OFFSET_BAR_SIZE:
        case REG_OFFSET_BAR_SIZE + 4:
            if (size < 4)
                break;
            if (REG_OFFSET_BAR_SIZE) {
                state->bars[state->bar_no].size &= 0xffffffff00000000;
                state->bars[state->bar_no].size |= (uint32_t)val;
            } else {
                state->bars[state->bar_no].size &= 0xffffffff;
                state->bars[state->bar_no].size |= val << 32;
            }
            qemu_log("set bar[%d] size 0x%lx, size %d\n", state->bar_no, val, size);
            break;
        default:
            qemu_log("invalid register access: addr 0x%lx val 0x%lx, size %d\n", addr, val, size);
    }
}

static const MemoryRegionOps pciepc_mmio_cfg_ops = {
    .read = pciepc_mmio_cfg_read,
    .write = pciepc_mmio_cfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static int qemu_epc_handle_msg_ver(PCIEPCState *state)
{
    ssize_t size;
    qemu_epc_msg_version_t version = QEMU_EPC_PROTOCOL_VERSION;

    size = send(state->clt_fd, &version, sizeof(version), 0);
    if (size != sizeof(version)) {
        qemu_log("failed to send message\n");
        return -1;
    }

    return 0;
}

static void *pci_epc_srv_thread(void *opaque)
{
    PCIEPCState* state = opaque;
    int err;
    struct sockaddr_un sun;
    socklen_t socklen;

    state->srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (state->srv_fd < 0) {
        qemu_log("failed to allocate socket\n");
        return NULL;
    }

    sun.sun_family = AF_UNIX;
    strcpy(sun.sun_path, QEMU_EPC_SOCK_PATH);

    err = bind(state->srv_fd, (const struct sockaddr*)&sun, sizeof(sun));
    if (err == -1) {
        qemu_log("failed to bind\n");
        return NULL;
    }

    err = listen(state->srv_fd, 1);
    if (err == -1) {
        qemu_log("failed to listen\n");
        return NULL;
    }

    socklen = sizeof(sun);

    state->clt_fd = accept(state->srv_fd, (struct sockaddr *)&sun, &socklen);
    if (state->clt_fd == -1) {
        qemu_log("failed to accept\n");
        return NULL;
    }

    while(1) {
        uint8_t type;
        ssize_t size;

        size = recv(state->clt_fd, &type, sizeof(type), 0);
        if (size != sizeof(type)) {
            qemu_log("failed recv: %ld\n", size);
            return NULL;
        }

        qemu_log("%d type message handling...\n", type);

        switch(type) {
            case QEMU_EPC_MSG_TYPE_VER:
                err = qemu_epc_handle_msg_ver(state);
                if (err) {
                    qemu_log("failed to handle VER message\n");
                }
                break;
            default:
                qemu_log("found unknown message type: %d\n", type);
                return NULL;
        }
    }

    return NULL;
}

static void pci_epc_realize(PCIDevice* pci_dev, Error** errp)
{
    PCIEPCState* state = PCI_EPC(pci_dev);

    memory_region_init_io(&state->ctrl, OBJECT(state), &pciepc_mmio_ctl_ops, state,
            "pci-epf/ctl", 64);
    memory_region_init_io(&state->cfg, OBJECT(state), &pciepc_mmio_cfg_ops, state,
                          "pci-epc/cfg", PCI_CONFIG_SPACE_SIZE);

    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &state->cfg);
    pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &state->ctrl);

    qemu_thread_create(&state->srv_thread, "qemu-epc", pci_epc_srv_thread, state, QEMU_THREAD_JOINABLE);
}

static void pci_epc_class_init(ObjectClass* obj, void* data)
{
    DeviceClass* dev = DEVICE_CLASS(obj);
    PCIDeviceClass* pcidev = PCI_DEVICE_CLASS(obj);

    pcidev->realize = pci_epc_realize;
    pcidev->vendor_id = PCI_VENDOR_ID_REDHAT;
    pcidev->device_id = PCI_DEVICE_ID_REDHAT_PCIE_EP;
    pcidev->revision = QEMU_PCI_EPC_VER;
    pcidev->class_id = PCI_CLASS_OTHERS;

    qemu_log("%s:%d\n", __func__, __LINE__);
    dev->desc = "";
    set_bit(DEVICE_CATEGORY_MISC, dev->categories);
    //     dev->reset;
    //     device_class_set_props(dc, pci_testdev_properties);
}

static const TypeInfo pci_epc_info = {
    .name = TYPE_PCI_EPC,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIEPCState),
    .class_init = pci_epc_class_init,
    .interfaces =
        (InterfaceInfo[]){
            {INTERFACE_CONVENTIONAL_PCI_DEVICE},
            {},
        },
};

static void pci_epc_register_types(void)
{
    type_register_static(&pci_epc_info);
}

type_init(pci_epc_register_types)
