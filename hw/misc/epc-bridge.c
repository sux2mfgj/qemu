/*
 * QEMU PCIe Endpoint device
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/thread.h"
#include "qemu/guest-random.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include <zlib.h>

#include "qemu-epc.h"

#include <sys/types.h>
#include <sys/socket.h>

struct EPCBridgeDevState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/
    MemoryRegion space;

    QemuThread recv_thread;

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

static int epc_bridge_check_protocol_version(EPCBridgeDevState *d)
{
    ssize_t size;
    uint32_t type = QEMU_EPC_MSG_TYPE_VER;
    uint32_t version;

    size = send(d->epfd, &type, sizeof(type), 0);
    if (size != sizeof(type)) {
        qemu_log("failed the send\n");
        return -1;
    }

    size = recv(d->epfd, &version, sizeof(version), 0);
    if (size != sizeof(version)) {
        qemu_log("failed to receive enough data size\n");
        return -1;
    }

    if (version != QEMU_EPC_PROTOCOL_VERSION) {
        qemu_log("found invalid protocol verison. 0x%x is expected but found 0x%d\n", QEMU_EPC_PROTOCOL_VERSION, version);
        return -1;
    }

    return 0;
}

static int epc_bridge_connect_server(EPCBridgeDevState *d, Error ** errp)
{
    int err;
    struct sockaddr_un sun = {};

    d->epfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (d->epfd == -1) {
        error_setg(errp, "failed to create socket");
        return -1;
    }

    sun.sun_family = AF_UNIX;
    strcpy(sun.sun_path, QEMU_EPC_SOCK_PATH);

    err = connect(d->epfd, (const struct sockaddr *)&sun, sizeof(sun));
    if (err == -1) {
        error_setg(errp, "failed to connect server\n");
        return -1;
    }

    err = epc_bridge_check_protocol_version(d);
    if (err) {
        error_setg(errp, "invalid server version\n");
        return err;
    }

    return 0;
}

static int epc_bridge_req_pci_config(EPCBridgeDevState *d, uint32_t offset, uint32_t size, void *buf)
{
    ssize_t tsize;
    uint32_t type = QEMU_EPC_MSG_TYPE_HDR;
    struct qemu_epc_req_pci_config req = {
        .offset = offset,
        .size = size,
    };

    tsize = send(d->epfd, &type, sizeof(type), 0);
    if (tsize != sizeof(type)) {
        qemu_log("failed to send type\n");
        return -1;
    }

    tsize = send(d->epfd, &req, sizeof(req), 0);
    if (tsize != sizeof(req)) {
        qemu_log("failed to send request\n");
        return -1;
    }

    tsize = recv(d->epfd, buf, size, 0);
    if (tsize != size) {
        qemu_log("failed to receive data");
        return -1;
    }

    return 0;
}

static int epc_bridge_load_pci_config_hdr(PCIDevice *pci_dev, Error ** errp)
{
    uint16_t vendor_id, device_id;
    uint8_t revision, class_id;
    int err;
    EPCBridgeDevState *d = EPC_BRIDGE(pci_dev);

    err = epc_bridge_req_pci_config(d, PCI_VENDOR_ID, sizeof(vendor_id), &vendor_id);
    if (err) {
        qemu_log("failed to load vendor_id\n");
        return err;
    }

    err = epc_bridge_req_pci_config(d, PCI_DEVICE_ID, sizeof(device_id), &device_id);
    if (err) {
        qemu_log("failed to load device_id\n");
        return err;
    }

    err = epc_bridge_req_pci_config(d, PCI_REVISION_ID, sizeof(revision), &revision);
    if (err) {
        qemu_log("failed to load revision\n");
        return err;
    }

    err = epc_bridge_req_pci_config(d, PCI_CLASS_DEVICE, sizeof(class_id), &class_id);
    if (err) {
        qemu_log("failed to load class\n");
        return err;
    }

    pci_config_set_vendor_id(pci_dev->config, vendor_id);
    pci_config_set_device_id(pci_dev->config, device_id);
    pci_config_set_revision(pci_dev->config, revision);
    pci_config_set_class(pci_dev->config, class_id);

    return 0;
}

static void epc_bridge_realize(PCIDevice *pci_dev, Error ** errp)
{
    EPCBridgeDevState *d = EPC_BRIDGE(pci_dev);
    int err;

    err = epc_bridge_connect_server(d, errp);
    if (err) {
        qemu_log("failed to connect server\n");
        return;
    }

    qemu_log("connected to server\n");

    err = epc_bridge_load_pci_config_hdr(pci_dev, errp);
    if (err) {
        qemu_log("failed to setup pci config\n");
        return;
    }

    epc_bridge_dev_setup_bar(d, pci_dev, errp);
    msi_init(pci_dev, 0x50, 1, true, true, NULL);
}

static void epc_bridge_dev_class_init (ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    dc->desc = "PCI EP function bridge device";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    k->realize = epc_bridge_realize;
}

static const TypeInfo epc_bridge_dev_info = {
    .name = TYPE_EPC_BRIDGE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(EPCBridgeDevState),
    .class_init    = epc_bridge_dev_class_init,
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
