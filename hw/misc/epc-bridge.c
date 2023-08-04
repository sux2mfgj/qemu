/*
 * QEMU PCIe Endpoint device
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci.h"
#include "qemu-epc.h"

#include <sys/types.h>
#include <sys/socket.h>

struct EPCBridgeDevState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/
    MemoryRegion space;

    int epfd;
};

#define TYPE_EPC_BRIDGE "epc-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(EPCBridgeDevState, EPC_BRIDGE)

#if 1
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
#endif

static void epc_bridge_realize(PCIDevice *pci_dev, Error ** errp)
{
    EPCBridgeDevState *d = EPC_BRIDGE(pci_dev);

    memory_region_init_io(&d->space, OBJECT(d), &epc_bridge_mmio_ops, d, "qemu-epc", 0x40);

    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->space);

    pci_config_set_interrupt_pin(pci_dev->config, 1);

    // msi_init(pci_dev, 0x50, 1, true, true, NULL);
}

static void epc_bridge_dev_class_init (ObjectClass *klass, void *data)
{
    DeviceClass *d = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_TI;
    k->device_id = 0xb500;
    k->revision = 0x00;
    k->class_id = PCI_CLASS_OTHERS;
    k->realize = epc_bridge_realize;

    set_bit(DEVICE_CATEGORY_MISC, d->categories);
}

static const TypeInfo epc_bridge_dev_info = {
    .name = TYPE_EPC_BRIDGE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(EPCBridgeDevState),
    .class_init    = epc_bridge_dev_class_init,
    //.instance_init = epc_bridge_dev_instance_init,
    .interfaces = (InterfaceInfo[]){
        {INTERFACE_CONVENTIONAL_PCI_DEVICE},
        {},
    },
};

static void pci_epdev_register_types(void)
{
    type_register_static(&epc_bridge_dev_info);
}

type_init(pci_epdev_register_types);
