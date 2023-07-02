/*
 * QEMU PCIe Endpoint device
 */

#include "qom/object.h"
#include "hw/pci/pci_device.h"

typedef struct EPCBridgeDevHdr {

} EPCBridgeDevHdr;

struct EPCBridgeDevState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

};

#define TYPE_EPC_BRIDGE_DEV_NAME "pci-ep"

static void epc_bridge_dev_class_init (ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
}

static const TypeInfo epc_bridge_dev_info = {
    .name = TYPE_EPC_BRIDGE_DEV_NAME,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(EPCBridgeDevHdr),
    .class_init    = epc_bridge_dev_class_init,
};

static void pci_epdev_register_types(void)
{
    type_register_static(&epc_bridge_dev_info);
}

type_init(pci_epdev_register_types);
