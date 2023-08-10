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
};

enum {
	QEMU_EP_BAR_CFG_OFF_MASK = 0x00,
	QEMU_EP_BAR_CFG_OFF_NUMBER = 0x01,
	QEMU_EP_BAR_CFG_OFF_FLAGS = 0x02,
	QEMU_EP_BAR_CFG_OFF_RSV = 0x04,
	QEMU_EP_BAR_CFG_OFF_PHYS_ADDR = 0x08,
	QEMU_EP_BAR_CFG_OFF_SIZE = 0x10,

	QEMU_EP_BAR_CFG_SIZE = 0x18
};

struct pci_epc_bar {
    uint64_t phys_addr;
    uint64_t size;
    uint8_t flags;
};

typedef struct PCIEPCState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    QemuThread srv_thread;
    bool srv_thread_running;
    int srv_fd, clt_fd;
    int ofd;

    MemoryRegion ctrl, pci_cfg, bar_cfg;

    uint8_t config_space[PCIE_CONFIG_SPACE_SIZE];

    uint8_t bar_mask;
    uint8_t bar_no;
    struct pci_epc_bar bars[6];

} PCIEPCState;

OBJECT_DECLARE_SIMPLE_TYPE(PCIEPCState, PCI_EPC);

static int qemu_epc_handle_msg_ver(PCIEPCState *state)
{
    ssize_t size;
    uint32_t version = QEMU_EPC_PROTOCOL_VERSION;

    size = send(state->clt_fd, &version, sizeof(version), 0);
    if (size != sizeof(version)) {
        qemu_log("failed to send message\n");
        return -1;
    }

    return 0;
}

static int qemu_epc_handle_msg_fd(PCIEPCState *state)
{
    ssize_t size;
    char buf[512];
    struct iovec iov = {buf, 512};
    char cmsg[CMSG_SPACE(sizeof(state->ofd))];
    struct msghdr msg = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg,
        .msg_controllen = sizeof(cmsg),
        .msg_flags = 0
    };
    struct cmsghdr *cmsghdr;

    size = recvmsg(state->clt_fd, &msg, 0);
    if (size < 0) {
        qemu_log("failed the recvmsg\n");
        return -1;
    }

    cmsghdr = CMSG_FIRSTHDR(&msg);
    state->ofd = *(int*)CMSG_DATA(cmsghdr);

    return 0;
}

static int qemu_epc_handle_msg_hdr(PCIEPCState *state)
{
    ssize_t size;
    struct qemu_epc_req_pci_config config_req;

    size = recv(state->clt_fd, &config_req, sizeof(config_req), 0);
    if (size != sizeof(config_req)) {
        qemu_log("not enough data size\n");
        return -1;
    }

    if (config_req.offset + config_req.size > PCIE_CONFIG_SPACE_SIZE) {
        qemu_log("found invalid request size\n");
        return -1;
    }

    size = send(state->clt_fd, &state->config_space[config_req.offset], config_req.size, 0);
    if (size != config_req.size) {
        qemu_log("failed to send data\n");
        return -1;
    }

    return 0;
}

static int qemu_epc_handle_msg_bar(PCIEPCState *state)
{
    uint8_t type;
    ssize_t size;
    uint8_t bar_no;

    size = recv(state->clt_fd, &type, sizeof(type), 0);
    if (size != sizeof(type)) {
        qemu_log("not enough data size\n");
        return -1;
    }

    switch(type) {
        case QEMU_EPC_REQ_BAR_MASK:
            size = send(state->clt_fd, &state->bar_mask, sizeof (state->bar_mask), 0);
            if (size != sizeof(state->bar_mask)) {
                qemu_log("failed to send all data\n");
                return -1;
            }
            break;
        case QEMU_EPC_REQ_BAR_BAR:
            size = recv(state->clt_fd, &bar_no, sizeof(bar_no), 0);
            if (size != sizeof(bar_no)) {
                qemu_log("failed to get bar_no");
                return -1;
            }

            if (bar_no > 5) {
                qemu_log("invalid bar_no found\n");
                return -1;
            }
            
            size = send(state->clt_fd, &state->bars[bar_no].size, sizeof(state->bars[bar_no].size), 0);
            if (size != sizeof(state->bars[bar_no].size)) {
                qemu_log("failed to pass bar info\n");
                return -1;
            }
            break;
        default:
            qemu_log("invalid bar request type found: %d\n", type);
            break;
    }

    return 0;
}

static int qemu_epc_handle_access_bar(PCIEPCState *state)
{
    ssize_t rsize;
    struct qemu_epc_access_bar req;
    PCIDevice *dev = PCI_DEVICE(state);
    dma_addr_t base;
    void *buf;
    MemTxResult result;

    rsize = recv(state->clt_fd, &req, sizeof(req), 0);
    if (rsize != sizeof(req)) {
        qemu_log("not enough data\n");
        return -1;
    }

    switch(req.type) {
        case QEMU_EPC_ACCESS_BAR_READ:
            qemu_log("a read request to bar is not supported yet\n");
            return -1;
        case QEMU_EPC_ACCESS_BAR_WRITE:
            qemu_log("found a write request to bar\n");
            buf = malloc(req.size);
            if (!buf) {
                qemu_log("failed to allocate memory\n");
                return -1;
            }

            rsize = recv(state->clt_fd, buf, req.size, 0);
            if (rsize != req.size) {
                qemu_log("failed to receve payload\n");
                return -1;
            }

            if (req.bar_no > 5) {
                qemu_log("invalid request");
                return -1;
            }

            base = state->bars[req.bar_no].phys_addr + req.offset;

            result = pci_dma_write(dev, base, buf, req.size);
            if (result) {
                qemu_log("faield to write data\n");
                return -result;
            }
            free(buf);
            break;
        default:
            qemu_log("invalid access type found\n");
            break;
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
        uint32_t type;
        ssize_t size;

        size = recv(state->clt_fd, &type, sizeof(type), 0);
        if (size != sizeof(type)) {
            qemu_log("failed recv: %ld\n", size);
            return NULL;
        }

        qemu_log("handles %d type message\n", type);

        switch(type) {
            case QEMU_EPC_MSG_TYPE_VER:
                err = qemu_epc_handle_msg_ver(state);
                if (err) {
                    qemu_log("failed to handle VER message\n");
                }
                break;
            case QEMU_EPC_MSG_TYPE_FD:
                err = qemu_epc_handle_msg_fd(state);
                if (err)
                    qemu_log("failed to recv fd\n");
                break;
            case QEMU_EPC_MSG_TYPE_HDR:
                err = qemu_epc_handle_msg_hdr(state);
                if (err)
                    qemu_log("failed to handle HDR message\n");
                break;
            case QEMU_EPC_MSG_TYPE_BAR:
                err = qemu_epc_handle_msg_bar(state);
                if (err)
                    qemu_log("failed to handle BAR message\n");
                break;
            case QEMU_EPC_MSG_TYPE_ACCESS_BAR:
                err = qemu_epc_handle_access_bar(state);
                if (err)
                    qemu_log("failed to handle accessing bar request\n");
                break;
            default:
                qemu_log("found unknown message type: %d\n", type);
                return NULL;
        }
    }

    return NULL;
}

enum {
    QEMU_EP_CTRL_OFF_START = 0x00,
};


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
    PCIEPCState* state = opaque;
    qemu_log("%s:%d addr 0x%lx, val 0x%lx\n", __func__, __LINE__, addr, val);

    if (addr == QEMU_EP_CTRL_OFF_START && val == 1 && !state->srv_thread_running) {
        if (val == 1 && !state->srv_thread_running) {
            state->srv_thread_running = true;
            qemu_thread_create(&state->srv_thread, "qemu-epc", pci_epc_srv_thread, state, QEMU_THREAD_JOINABLE);
        } else if (val == 0 && state->srv_thread_running){
            //TODO qemu_thread_join();
            state->srv_thread_running = false;
        }
    }
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

    if (addr + size <= PCIE_CONFIG_SPACE_SIZE) {
        memcpy(&state->config_space[addr], &val, size);
        return;
    }
}

static const MemoryRegionOps pciepc_mmio_pci_cfg_ops = {
    .read = pciepc_mmio_cfg_read,
    .write = pciepc_mmio_cfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};


static uint64_t pciepc_mmio_bar_cfg_read(void* opaque, hwaddr addr, unsigned size)
{
    PCIEPCState* state = opaque;

    qemu_log("%s addr %lx, size %d\n", __func__, addr, size);

    switch (addr) {
        case QEMU_EP_BAR_CFG_OFF_MASK:
            return state->bar_mask;
        default:
            return 0;
    }
}

static void pciepc_mmio_bar_cfg_write(void* opaque,
                              hwaddr addr,
                              uint64_t val,
                              unsigned size)
{
    PCIEPCState* state = opaque;

    qemu_log("%s addr %lx, val 0x%lx, size %d\n", __func__, addr, val, size);

    switch(addr) {
        case QEMU_EP_BAR_CFG_OFF_MASK:
            state->bar_mask = (uint8_t)val;
            size -= 1;
            val >>= 8;
            if (size == 0)
                break;
            /* fallthrough */
        case QEMU_EP_BAR_CFG_OFF_NUMBER:
            state->bar_no = val;
            size -= 1;
            val >>= 8;
            if (size == 0)
                break;
            /* fallthrough */
        case QEMU_EP_BAR_CFG_OFF_FLAGS:
            if (state->bar_no > 6)
                break;
            state->bars[state->bar_no].flags = val;
            size -= 1;
            val >>= 8;
            if (size == 0)
                break;
            /* fallthrough*/
        case QEMU_EP_BAR_CFG_OFF_RSV:
            size -= 1;
            val >>= 8;
            if (size == 0)
                break;
            /* fallthrough*/
        case QEMU_EP_BAR_CFG_OFF_PHYS_ADDR:
            if (state->bar_no > 6)
                break;
            if (size == sizeof(uint64_t)) {
                state->bars[state->bar_no].phys_addr = val;
                break;
            }
            else if (size == sizeof(uint32_t)) {
                uint64_t tmp = state->bars[state->bar_no].phys_addr & 0xffffffff00000000;
                state->bars[state->bar_no].phys_addr = tmp | (uint32_t)val;
            } else {
                break;
            }
        case QEMU_EP_BAR_CFG_OFF_PHYS_ADDR + 4:
            if (state->bar_no > 6)
                break;
            if (size != sizeof(uint32_t))
                break;
            state->bars[state->bar_no].phys_addr =
                (state->bars[state->bar_no].phys_addr & 0xffffffff) | (val << 32);
            break;
        case QEMU_EP_BAR_CFG_OFF_SIZE:
            if (state->bar_no > 6)
                break;
            if (size == sizeof(uint64_t)) {
                state->bars[state->bar_no].size = val;
            } else if (size == sizeof(uint32_t)) {
                uint64_t tmp = state->bars[state->bar_no].size & 0xffffffff00000000;
                state->bars[state->bar_no].size = tmp | (uint32_t)val;
            }
            break;
        case QEMU_EP_BAR_CFG_OFF_SIZE + 4:
            if (state->bar_no > 6)
                break;
            if (size == sizeof(uint32_t)) {
                uint64_t tmp = state->bars[state->bar_no].size & 0xffffffff;
                state->bars[state->bar_no].size = tmp | (val << 32);
            }
            break;
        default:
            break;
    }
}

static const MemoryRegionOps pciepc_mmio_bar_cfg_ops = {
    .read = pciepc_mmio_bar_cfg_read,
    .write = pciepc_mmio_bar_cfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void pci_epc_realize(PCIDevice* pci_dev, Error** errp)
{
    PCIEPCState* state = PCI_EPC(pci_dev);

    memory_region_init_io(&state->ctrl, OBJECT(state), &pciepc_mmio_ctl_ops, state,
            "pci-epf/ctl", 64);
    memory_region_init_io(&state->pci_cfg, OBJECT(state), &pciepc_mmio_pci_cfg_ops, state,
                          "pci-epc/pci_cfg", PCIE_CONFIG_SPACE_SIZE);
    memory_region_init_io(&state->bar_cfg, OBJECT(state), &pciepc_mmio_bar_cfg_ops, state,
                          "pci-epc/bar_cfg", pow2ceil(QEMU_EP_BAR_CFG_SIZE));

    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &state->pci_cfg);
    pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &state->bar_cfg);
    pci_register_bar(pci_dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &state->ctrl);

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
