#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/hid.h>

#define USB_MOUSE_VENDOR_ID  0x046d
#define USB_MOUSE_PRODUCT_ID 0xc077

struct usb_mouse {
    char name[128];
    char phys[64];
    struct usb_device *udev;
    struct input_dev *input;
    struct urb *urb;
    unsigned char *data;
    dma_addr_t data_dma;
};

static struct usb_device_id mouse_table[] = {
    { USB_DEVICE(USB_MOUSE_VENDOR_ID, USB_MOUSE_PRODUCT_ID) },
    {}
};
MODULE_DEVICE_TABLE(usb, mouse_table);

static void mouse_irq(struct urb *urb)
{
    struct usb_mouse *mouse = urb->context;
    unsigned char *data = mouse->data;
    struct input_dev *dev = mouse->input;
    int status;
    int i;

    switch (urb->status) {
    case 0: break;
    case -ECONNRESET:
    case -ENOENT:
    case -ESHUTDOWN:
        return;
    default:
        goto resubmit;
    }

    printk(KERN_INFO "MOUSE-DEBUG: Len=%d Data=", urb->actual_length);
    for (i = 0; i < urb->actual_length && i < 8; i++) {
        printk(KERN_CONT "%02x ", data[i]);
    }
    printk(KERN_CONT "\n");
    if (urb->actual_length >= 4) {
        input_report_key(dev, BTN_LEFT,   data[0] & 0x01);
        input_report_key(dev, BTN_RIGHT,  data[0] & 0x02);
        input_report_key(dev, BTN_MIDDLE, data[0] & 0x04);

        input_report_rel(dev, REL_X,     (signed char)data[1]);
        input_report_rel(dev, REL_Y,     (signed char)data[2]);
        input_report_rel(dev, REL_WHEEL, (signed char)data[3]);
    }

    input_sync(dev);

resubmit:
    status = usb_submit_urb(urb, GFP_ATOMIC);
    if (status)
        dev_err(&mouse->udev->dev, "can't resubmit intr, %d\n", status);
}

static int mouse_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    struct usb_host_interface *interface;
    struct usb_endpoint_descriptor *endpoint;
    struct usb_mouse *mouse;
    struct input_dev *input_dev;
    int pipe, maxp, error = -ENOMEM;

    interface = intf->cur_altsetting;

    if (interface->desc.bNumEndpoints != 1) return -ENODEV;
    endpoint = &interface->endpoint[0].desc;
    if (!usb_endpoint_is_int_in(endpoint)) return -ENODEV;

    mouse = kzalloc(sizeof(struct usb_mouse), GFP_KERNEL);
    input_dev = input_allocate_device();
    if (!mouse || !input_dev) goto fail1;

    mouse->data = usb_alloc_coherent(udev, 16, GFP_KERNEL, &mouse->data_dma);
    if (!mouse->data) goto fail1;

    mouse->urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!mouse->urb) goto fail2;

    mouse->udev = udev;
    mouse->input = input_dev;

    if (udev->manufacturer)
        strscpy(mouse->name, udev->manufacturer, sizeof(mouse->name));
    if (udev->product) {
        if (udev->manufacturer) strlcat(mouse->name, " ", sizeof(mouse->name));
        strlcat(mouse->name, udev->product, sizeof(mouse->name));
    }
    usb_make_path(udev, mouse->phys, sizeof(mouse->phys));
    strlcat(mouse->phys, "/input0", sizeof(mouse->phys));

    input_dev->name = mouse->name;
    input_dev->phys = mouse->phys;
    input_dev->id.bustype = BUS_USB;
    input_dev->id.vendor  = le16_to_cpu(udev->descriptor.idVendor);
    input_dev->id.product = le16_to_cpu(udev->descriptor.idProduct);
    input_dev->id.version = le16_to_cpu(udev->descriptor.bcdDevice);
    input_dev->dev.parent = &intf->dev;

    input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);
    input_dev->keybit[BIT_WORD(BTN_MOUSE)] = BIT_MASK(BTN_LEFT) | BIT_MASK(BTN_RIGHT) | BIT_MASK(BTN_MIDDLE);
    input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y) | BIT_MASK(REL_WHEEL);

    error = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
                            HID_REQ_SET_PROTOCOL, 
                            USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                            1, // 1 = Report Protocol
                            interface->desc.bInterfaceNumber,
                            NULL, 0, USB_CTRL_SET_TIMEOUT);
    if (error < 0)
        dev_warn(&udev->dev, "Failed to set Report Protocol (Err: %d)\n", error);

    pipe = usb_rcvintpipe(udev, endpoint->bEndpointAddress);
    maxp = usb_maxpacket(udev, pipe);

    usb_fill_int_urb(mouse->urb, udev, pipe, mouse->data,
                     (maxp > 16 ? 16 : maxp),
                     mouse_irq, mouse, endpoint->bInterval);
    mouse->urb->transfer_dma = mouse->data_dma;
    mouse->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    error = input_register_device(mouse->input);
    if (error) goto fail3;

    usb_set_intfdata(intf, mouse);

    error = usb_submit_urb(mouse->urb, GFP_KERNEL);
    if (error) goto fail4;

    return 0;

fail4:
    input_unregister_device(input_dev);
    input_dev = NULL;
fail3:
    usb_free_urb(mouse->urb);
fail2:
    usb_free_coherent(udev, 16, mouse->data, mouse->data_dma);
fail1:
    input_free_device(input_dev);
    kfree(mouse);
    return error;
}

static void mouse_disconnect(struct usb_interface *intf)
{
    struct usb_mouse *mouse = usb_get_intfdata(intf);
    usb_set_intfdata(intf, NULL);
    if (mouse) {
        usb_kill_urb(mouse->urb);
        input_unregister_device(mouse->input);
        usb_free_urb(mouse->urb);
        usb_free_coherent(mouse->udev, 16, mouse->data, mouse->data_dma);
        kfree(mouse);
    }
}

static struct usb_driver mouse_driver = {
    .name = "Logitech_Mouse_Driver",
    .probe = mouse_probe,
    .disconnect = mouse_disconnect,
    .id_table = mouse_table,
};

module_usb_driver(mouse_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Daniels");
MODULE_DESCRIPTION("Logitech Mouse Device Driver");
