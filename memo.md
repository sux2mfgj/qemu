

### pci device の初期化の流れについて

- vendor id, device id, configuration spaceはいつ設定されていないといけないのか？

pci/pci.c

↓この関数は、pci_device_class_init で DeviceClass->realize に登録されている
- pci_qdev_realize()
    - do_pci_register_device()
        - pci_dev->vendor_id
        - pci_dev->device-id


core/qdev.c が怪しい

↓ device_set_realized() は、 device_class_init() で、object_class_property_add_bool に渡されている
- device_set_realized()
    - dc->realize()


↓いつ呼ばれるのか？
- qdev_realize() が怪しい
    - object_property_set_bool(,"realized",)
        - object_property_set_object()
            - object_property_set()
                - prop->set()
qdev_realize()でしらべると、アーキテクチャからの呼び出しが見える。arm/raspi,bananapi,
ほかには、sysbusから呼ばれたりしている。

qdev_realize(, BUS(pci_bus)) ってのもみえる

- qdev_realize_and_unref()
    - qdev_realize()




qom/object.c

- object_class_property_add_bool
    - object_class_property_add()
        - prop->set に device_set_realized()が渡されてきて登録される

- prop->set が呼ばれるのは property_set_bool() か、 property_set_enum()


そもそも、realize で instance の変数が渡されていそうだからよさそうにみえるが、
インスタンス生成のタイミングを見ておきたい。

### インスタンス生成と初期化について

qom/object.c

- object_initialize()
    - object_initialize_with_type()
        - object_init_with_type()
            - instance_init()

