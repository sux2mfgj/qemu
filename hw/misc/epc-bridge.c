/*
 * QEMU PCIe Endpoint device
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"

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
};

#define TYPE_EPC_BRIDGE "epc-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(EPCBridgeDevState, EPC_BRIDGE)


#if 0
static void epc_bridge_dev_class_init (ObjectClass *klass, void *data)
{
//     DeviceClass *dc = DEVICE_CLASS(klass);
//     PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
}
#endif

static uint64_t epc_bridge_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log("%s addr 0x%lx, size 0x%x\n", __func__, addr, size);

#define PCI_ENDPOINT_TEST_STATUS		0x8

    switch(addr){ 
        case PCI_ENDPOINT_TEST_STATUS:
            return 1 << 6;
        default:
            qemu_log("unknown command 0x%lx\n", addr);
    }

    return 0;
}

static void epc_bridge_bar0_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    EPCBridgeDevState *s = opaque;
    PCIDevice *dev = PCI_DEVICE(s);

    qemu_log("%s addr 0x%lx, val 0x%lx, size 0x%x\n", __func__, addr, val, size);

#define PCI_ENDPOINT_TEST_COMMAND 0x4

#define COMMAND_READ (1 << 3)
#define COMMAND_WRITE (1 << 4)
#define COMMAND_COPY (1 << 5)
    if (addr == PCI_ENDPOINT_TEST_COMMAND) {
        switch(val) {
            case COMMAND_WRITE:
                if (!msi_enabled(dev)) {
                    qemu_log("failed to send msi\n");
                    return;
                }
                msi_notify(dev, 0);
                break;
            case COMMAND_READ:
            case COMMAND_COPY:
            default:
                qemu_log("not support command: %ld\n", val);
                break;
        }
    }
}

static const MemoryRegionOps epc_bridge_mmio_ops = {
    .read = epc_bridge_bar0_read,
    .write = epc_bridge_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static int epc_bridge_dev_setup_bar(EPCBridgeDevState *d, PCIDevice *pci_dev, Error ** errp)
{
    memory_region_init_io(&d->space, OBJECT(d), &epc_bridge_mmio_ops, d, TYPE_EPC_BRIDGE"/bar0", 32);

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
