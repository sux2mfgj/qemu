/*
 * QEMU PCIe Endpoint device
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/guest-random.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include <zlib.h>

#include <sys/types.h>
#include <sys/socket.h>

typedef struct EPCBridgeDevHdr {

} EPCBridgeDevHdr;

struct EPCBridgeDevState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/
    MemoryRegion space;

    int epfd;
    struct {
        uint64_t addr;
        size_t size;
        uint32_t checksum;
#define STATUS_READ_SUCCESS			BIT(0)
#define STATUS_READ_FAIL			BIT(1)
#define STATUS_WRITE_SUCCESS			BIT(2)
#define STATUS_WRITE_FAIL			BIT(3)
#define STATUS_COPY_SUCCESS			BIT(4)
#define STATUS_COPY_FAIL			BIT(5)
#define STATUS_IRQ_RAISED			BIT(6)
#define STATUS_SRC_ADDR_INVALID			BIT(7)
#define STATUS_DST_ADDR_INVALID			BIT(8)
        uint32_t status;
    } test;
};

#define TYPE_EPC_BRIDGE "epc-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(EPCBridgeDevState, EPC_BRIDGE)

#define PCI_ENDPOINT_TEST_COMMAND 0x4
#define PCI_ENDPOINT_TEST_STATUS 0x8
#define PCI_ENDPOINT_TEST_LOWER_DST_ADDR 0x14
#define PCI_ENDPOINT_TEST_UPPER_DST_ADDR 0x18
#define PCI_ENDPOINT_TEST_SIZE 0x1c
#define PCI_ENDPOINT_TEST_CHECKSUM 0x20
#define PCI_ENDPOINT_TEST_IRQ_TYPE 0x24
#define PCI_ENDPOINT_TEST_IRQ_NUMBER 0x28
#define PCI_ENDPOINT_TEST_FLAGS 0x2c

#if 0
static void epc_bridge_dev_class_init (ObjectClass *klass, void *data)
{
//     DeviceClass *dc = DEVICE_CLASS(klass);
//     PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
}
#endif

static uint64_t epc_bridge_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    EPCBridgeDevState *s = opaque;

    qemu_log("%s addr 0x%lx, size 0x%x\n", __func__, addr, size);

    switch(addr) {
        case PCI_ENDPOINT_TEST_STATUS:
            qemu_log("read test status 0x%d\n", s->test.status);
            return s->test.status;
        case PCI_ENDPOINT_TEST_CHECKSUM:
            qemu_log("read checksum 0x%x\n", s->test.checksum);
            return s->test.checksum;
        default:
            qemu_log("found no handled read access: addr 0x%lx\n", addr);
    }

    return 0;
}

static void epc_bridge_handle_test_write_cmd(EPCBridgeDevState *s, PCIDevice *dev)
{
    int *data = malloc(s->test.size);

    qemu_log("%s\n", __func__);

    qemu_guest_getrandom(data, s->test.size, NULL);

    pci_dma_write(dev, s->test.addr, data, s->test.size);

    {
        uint32_t *b = (uint32_t*)data;
        for(int i=0; i<1; i++)
            qemu_log("%02x: 0x%x\n", i * 4, b[i]);
    }

    s->test.checksum = crc32(0x0, (uint8_t*)data, s->test.size) ^ 0xffffffff;
    qemu_log("checksum 0x%x\n", s->test.checksum);

    free(data);

    if (!msi_enabled(dev)) {
        qemu_log("failed to send msi\n");
        return;
    }

    s->test.status = STATUS_IRQ_RAISED;
    msi_notify(dev, 0);
}

static void epc_bridge_bar0_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    EPCBridgeDevState *s = opaque;
    PCIDevice *dev = PCI_DEVICE(s);

    qemu_log("%s addr 0x%lx, val 0x%lx, size 0x%x\n", __func__, addr, val, size);


#define COMMAND_READ (1 << 3)
#define COMMAND_WRITE (1 << 4)
#define COMMAND_COPY (1 << 5)
    switch (addr) {
    case PCI_ENDPOINT_TEST_COMMAND:
        switch(val) {
            case COMMAND_WRITE:
                epc_bridge_handle_test_write_cmd(s, dev);
                break;
            case COMMAND_READ:
            case COMMAND_COPY:
            default:
                qemu_log("not support command: %ld\n", val);
                break;
        }
        break;
    case PCI_ENDPOINT_TEST_LOWER_DST_ADDR:
    {
        uint64_t taddr =  (s->test.addr & 0xffffffff00000000) | (uint32_t)val;
        s->test.addr = taddr;
        break;
    }
    case PCI_ENDPOINT_TEST_UPPER_DST_ADDR:
    {
        s->test.addr = (val << 32) | (s->test.addr & 0xffffffff);
        qemu_log("set addr 0x%lx\n", s->test.addr);
        break;
    }
    case PCI_ENDPOINT_TEST_SIZE:
    {
        s->test.size = val;
        qemu_log("size 0x%x\n", (uint32_t)val);
        break;
    }
    case PCI_ENDPOINT_TEST_STATUS:
    {
        s->test.status = (uint32_t)val;
        qemu_log("set status 0x%x\n", (uint32_t)val);
        break;
    }
    case PCI_ENDPOINT_TEST_IRQ_TYPE:
    {
        qemu_log("set irq type 0x%x\n", (uint32_t)val);
        break;
    }
    case PCI_ENDPOINT_TEST_IRQ_NUMBER:
    {
        qemu_log("set irq number %d\n", (uint32_t)val);
        break;
    }
    default:
        qemu_log("found no handled write access: addr 0x%lx, val 0x%lx\n", addr, val);
    }
}

static const MemoryRegionOps epc_bridge_mmio_ops = {
    .read = epc_bridge_bar0_read,
    .write = epc_bridge_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static int epc_bridge_dev_setup_bar(EPCBridgeDevState *d, PCIDevice *pci_dev, Error ** errp)
{
    memory_region_init_io(&d->space, OBJECT(d), &epc_bridge_mmio_ops, d, TYPE_EPC_BRIDGE"/bar0", 0x40);

    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->space);

//     error_setg("Failed to connect endpoint controler");

    return 0;
}

static void epc_bridge_realize(PCIDevice *pci_dev, Error ** errp)
{
    EPCBridgeDevState *d = EPC_BRIDGE(pci_dev);

    epc_bridge_dev_setup_bar(d, pci_dev, errp);
    msi_init(pci_dev, 0x50, 1, true, true, NULL);
}

static int epc_bridge_dev_load_pci_configs(EPCBridgeDevState *dev, PCIDeviceClass *pci)
{
    //TODO should be access epc device

    pci->vendor_id = PCI_VENDOR_ID_TI;
    pci->device_id = 0xb500;
    pci->revision = 0x00;
    pci->class_id = PCI_CLASS_OTHERS;

    return 0;
}

#if 0
static void dummy() {
struct sockaddr_un saddr;
    char buf[32];

    d->epfd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(d->epfd > 0);

    memset(&saddr, 0x00, sizeof(saddr));
    saddr.sun_family = AF_UNIX;
    strcpy(saddr.sun_path, "/tmp/qemu-epc-bridge.sock");
    err = connect(d->epfd, (const struct sockaddr *)&saddr, sizeof(saddr));
    assert(err == 0);

#define MSG "get vendor"
    err = send(d->epfd, MSG, sizeof(MSG), 0);
    assert(err == sizeof (MSG));

    memset(buf, 0x00, 32);
    err = recv(d->epfd, buf, 32, 0);
    qemu_log("recv %s\n", buf);

}
#endif

static void epc_bridge_dev_instance_init(Object *obj)
{
    EPCBridgeDevState *d = EPC_BRIDGE(obj);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(obj->class);

    epc_bridge_dev_load_pci_configs(d, k);

    k->realize = epc_bridge_realize;
}

static const TypeInfo epc_bridge_dev_info = {
    .name = TYPE_EPC_BRIDGE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(EPCBridgeDevHdr),
//     .class_init    = epc_bridge_dev_class_init,
    .instance_init = epc_bridge_dev_instance_init,
    .interfaces = (InterfaceInfo[]){
        {INTERFACE_PCIE_DEVICE},
        {},
    },
};

static void pci_epdev_register_types(void)
{
    type_register_static(&epc_bridge_dev_info);
}

type_init(pci_epdev_register_types);
