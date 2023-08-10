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

    QemuThread recv_thread;
    int epfd;
    int srv_fd;
    struct bar_meta_data {
        MemoryRegion region;
        struct EPCBridgeDevState *state;
        uint8_t bar_no;
    } bar[6];
};

#define TYPE_EPC_BRIDGE "epc-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(EPCBridgeDevState, EPC_BRIDGE)

static uint64_t epc_bridge_bar_read(void *opaque, hwaddr addr, unsigned size)
{
    struct bar_meta_data *meta = opaque;

    qemu_log("handle bar %d read\n", meta->bar_no);

    return 0;
}

static void epc_bridge_bar_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    struct bar_meta_data *meta = opaque;
    uint32_t type = QEMU_EPC_MSG_TYPE_ACCESS_BAR;
    EPCBridgeDevState *d = meta->state;
    struct qemu_epc_access_bar req = {
        .offset = addr,
        .size = size,
        .type = QEMU_EPC_ACCESS_BAR_WRITE,
        .bar_no = meta->bar_no,
    };
    ssize_t tsize;

    qemu_log("handle bar %d write\n", meta->bar_no);

    tsize = send(d->epfd, &type, sizeof(type), 0);
    if (tsize != sizeof(type)) {
        qemu_log("failed the send\n");
        return;
    }

    tsize = send(d->epfd, &req, sizeof(req), 0);
    if (tsize != sizeof(req)) {
        qemu_log("failed to send request to write BAR space: %ld != %ld\n", tsize, sizeof(req));
        return;
    }

    tsize = send(d->epfd, &val, size, 0);
    if (tsize != size) {
        qemu_log("failed to send tx data\n");
        return;
    }
}

static const MemoryRegionOps epc_bridge_mmio_ops = {
    .read = epc_bridge_bar_read,
    .write = epc_bridge_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const char *bar_name[] = {
    TYPE_EPC_BRIDGE"/bar0",
    TYPE_EPC_BRIDGE"/bar1",
    TYPE_EPC_BRIDGE"/bar2",
    TYPE_EPC_BRIDGE"/bar3",
    TYPE_EPC_BRIDGE"/bar4",
    TYPE_EPC_BRIDGE"/bar5"
};

static int qemu_ep_load_bar_mask(EPCBridgeDevState *d, uint8_t *bar_mask)
{
    ssize_t size;
    uint32_t type = QEMU_EPC_MSG_TYPE_BAR;
    uint8_t req_type = QEMU_EPC_REQ_BAR_MASK;

    size = send(d->epfd, &type, sizeof(type), 0);
    if (size != sizeof(type)) {
        qemu_log("failed the send\n");
        return -1;
    }

    size = send(d->epfd, &req_type, sizeof(req_type), 0);
    if (size != sizeof(req_type)) {
        qemu_log("failed to send request type\n");
        return -1;
    }

    size = recv(d->epfd, bar_mask, sizeof(*bar_mask), 0);
    if (size != sizeof(*bar_mask)) {
        qemu_log("failed to get mask\n");
        return -1;
    }

    return 0;
}

static int qemu_ep_load_bar_info(EPCBridgeDevState *d, uint8_t bar_no, uint64_t *bar_size)
{
    ssize_t size;
    uint32_t type = QEMU_EPC_MSG_TYPE_BAR;
    uint8_t req_type = QEMU_EPC_REQ_BAR_BAR;

    size = send(d->epfd, &type, sizeof(type), 0);
    if (size != sizeof(type)) {
        qemu_log("failed the send\n");
        return -1;
    }

    size = send(d->epfd, &req_type, sizeof(req_type), 0);
    if (size != sizeof(req_type)) {
        qemu_log("failed to send request type\n");
        return -1;
    }

    size = send(d->epfd, &bar_no, sizeof(bar_no), 0);
    if (size != sizeof(bar_no)) {
        qemu_log("failed to send bar_no\n");
        return -1;
    }

    size = recv(d->epfd, bar_size, sizeof(*bar_size), 0);
    if (size != sizeof(*bar_size))  {
        qemu_log("failed to get bar size\n");
        return -1;
    }

    return 0;
}

static int epc_bridge_dev_setup_bar(PCIDevice *pci_dev, Error ** errp)
{
    EPCBridgeDevState *d = EPC_BRIDGE(pci_dev);
    uint8_t bar_mask;
    uint8_t bar_no;
    int err;
    uint64_t bar_size = 0;

    err = qemu_ep_load_bar_mask(d, &bar_mask);
    if (err) {
        qemu_log("failed to load mask\n");
        return err;
    }

    bar_no = 0;
    while(bar_mask) {
        struct bar_meta_data *bar = &d->bar[bar_no];
        if (!(bar_mask & 1)) {
            bar_mask >>= 1;
            bar_no++;
            continue;
        }

        err = qemu_ep_load_bar_info(d, bar_no, &bar_size);
        if (err) {
            qemu_log("failed to get bar info\n");
            return err;
        }
        
        bar->bar_no = bar_no;
        bar->state = d;

        memory_region_init_io(&bar->region, OBJECT(d),
                              &epc_bridge_mmio_ops, bar, bar_name[bar_no], bar_size);
        pci_register_bar(pci_dev, bar_no, PCI_BASE_ADDRESS_SPACE_MEMORY, &bar->region);

        bar_mask >>= 1;
        bar_no++;
    }
    
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

static int epc_bridge_create_and_pass_fd(EPCBridgeDevState *d, Error ** errp)
{
    ssize_t size;
    struct iovec iov = {NULL, 0};
    char cmsg[CMSG_SPACE(sizeof(d->srv_fd))];
    struct msghdr mhdr = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg,
        .msg_controllen = sizeof(cmsg),
        .msg_flags = 0,
    };
    struct cmsghdr *chdr = CMSG_FIRSTHDR(&mhdr);

    d->srv_fd = memfd_create("epf-bridge.srvfd", 0);
    if (d->srv_fd < 0)  {
        qemu_log("failed to create memfd");
        return -errno;
    }

    chdr->cmsg_level = SOL_SOCKET;
    chdr->cmsg_type = SCM_RIGHTS;
    chdr->cmsg_len = CMSG_LEN(sizeof(d->srv_fd));

    *(int *)CMSG_DATA(chdr) = d->srv_fd;

    size = sendmsg(d->epfd, &mhdr, 0);
    if (size < 0) {
        qemu_log("failed to send srv fd\n");
        return -errno;
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

    err = epc_bridge_create_and_pass_fd(d, errp);
    if (err) {
        qemu_log("failed to send fd\n");
        return;
    }

    qemu_log("connected to server\n");

    err = epc_bridge_load_pci_config_hdr(pci_dev, errp);
    if (err) {
        qemu_log("failed to setup pci config\n");
        return;
    }

    err = epc_bridge_dev_setup_bar(pci_dev, errp);
    if (err) {
        qemu_log("failed to setup PCI BAR");
        return;
    }
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
