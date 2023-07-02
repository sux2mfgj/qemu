/*
 * QEMU PCI endpoint controller device
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/sysbus.h"

#define TYPE_PCI_EPC "pci-epc"

typedef struct PCIEPCState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    MemoryRegion mmio;
} PCIEPCState;

OBJECT_DECLARE_SIMPLE_TYPE(PCIEPCState, PCI_EPC);

static const MemoryRegionOps pciepc_mmio_ops = {
};

static void pci_epc_realize(PCIDevice *pci_dev, Error **errp)
{
    PCIEPCState *state = PCI_EPC(pci_dev);

    memory_region_init_io(&state->mmio, OBJECT(state),
            &pciepc_mmio_ops, state, "pci-epc", 16);

    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY,
            &state->mmio);
}

static void pci_epc_class_init(ObjectClass *obj, void *data)
{
    DeviceClass *dev = DEVICE_CLASS(obj);
    PCIDeviceClass *pcidev = PCI_DEVICE_CLASS(obj);

    pcidev->realize = pci_epc_realize;
    pcidev->vendor_id = PCI_VENDOR_ID_QEMU;
    pcidev->device_id = PCI_DEVICE_ID_QEMU_PCIEC;
    pcidev->revision = 0x00;
    pcidev->class_id = PCI_CLASS_OTHERS;

    dev->desc = "";
    set_bit(DEVICE_CATEGORY_MISC, dev->categories);
//     dev->reset;
//     device_class_set_props(dc, pci_testdev_properties);
}

static const TypeInfo pci_epc_info = {
    .name          = TYPE_PCI_EPC,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIEPCState),
    .class_init    = pci_epc_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void pci_epc_register_types(void)
{
    type_register_static(&pci_epc_info);
}

type_init(pci_epc_register_types)
