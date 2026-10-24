#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/hid.h>

#define USB_KB_VENDOR_ID  0x413c
#define USB_KB_PRODUCT_ID 0x2107
#define DATA_SIZE 8

/* Standard HID to Linux Keycode Mapping */
static const unsigned char usb_kbd_keycode[256] = {
      0,   0,   0,   0,  30,  48,  46,  32,  18,  33,  34,  35,  23,  36,  37,  38,
     50,  49,  24,  25,  16,  19,  31,  20,  22,  47,  17,  45,  21,  44,   2,   3,
      4,   5,   6,   7,   8,   9,  10,  11,  28,   1,  14,  15,  57,  12,  13,  26,
     27,  43,  43,  39,  40,  41,  51,  52,  53,  58,  59,  60,  61,  62,  63,  64,
     65,  66,  67,  68,  87,  88,  99,  70, 119, 110, 102, 104, 111, 107, 109, 106,
    105, 108, 103,  69,  98,  55,  74,  78,  96,  79,  80,  81,  75,  76,  77,  71,
     72,  73,  82,  83,  86, 127, 116, 117, 183, 184, 185, 186, 187, 188, 189, 190,
    191, 192, 193, 194, 134, 138, 130, 132, 128, 129, 131, 137, 133, 135, 136, 113,
    115, 114,   0,   0,   0, 121,   0,  89,  93, 124,  92,  94,  95,   0,   0,   0,
    122, 123,  90,  91,  85,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

struct usb_kbd {
    struct usb_device *udev;
    struct input_dev *input;
    struct usb_interface *interface;
    struct urb *irq_urb;
    struct urb *led_urb;
    
    unsigned char *data;
    dma_addr_t data_dma;
    unsigned char *leds;
    dma_addr_t leds_dma;
    struct usb_ctrlrequest *cr;
    dma_addr_t cr_dma;
    
    unsigned char old_keys[6]; 
};

static int is_key_pressed(unsigned char key, unsigned char *buffer) {
    int i;
    if (key <= 3) return 0;
    for (i = 0; i < 6; i++) {
        if (buffer[i] == key) return 1;
    }
    return 0;
}

static void kbd_led_complete(struct urb *urb) {
    if (urb->status && urb->status != -ENOENT && urb->status != -ECONNRESET && urb->status != -ESHUTDOWN)
        pr_warn("led urb failed: %d\n", urb->status);
}

static int kbd_event(struct input_dev *dev, unsigned int type, unsigned int code, int value) {
    struct usb_kbd *kbd = input_get_drvdata(dev);
    if (type != EV_LED) return -1;
    if (kbd->led_urb->status == -EINPROGRESS) return 0;

    *kbd->leds = (test_bit(LED_NUML,    dev->led) ? 1 : 0)
               | (test_bit(LED_CAPSL,   dev->led) ? 2 : 0)
               | (test_bit(LED_SCROLLL, dev->led) ? 4 : 0);

    kbd->led_urb->dev = kbd->udev;
    if (usb_submit_urb(kbd->led_urb, GFP_ATOMIC))
        pr_err("usb_submit_urb(leds) failed\n");

    return 0;
}

static void kbd_irq(struct urb *urb) {
    struct usb_kbd *kbd = urb->context;
    int i, status;

    switch (urb->status) {
        case 0: break;
        case -ECONNRESET: case -ENOENT: case -ESHUTDOWN: return;
        default: goto resubmit; /* transient error: drop this report, keep polling */
    }

    /* 1. Modifiers: Directly map USB bits to Input Keys */
    input_report_key(kbd->input, KEY_LEFTCTRL,   !!(kbd->data[0] & 0x01));
    input_report_key(kbd->input, KEY_LEFTSHIFT,  !!(kbd->data[0] & 0x02));
    input_report_key(kbd->input, KEY_LEFTALT,    !!(kbd->data[0] & 0x04));
    input_report_key(kbd->input, KEY_LEFTMETA,   !!(kbd->data[0] & 0x08));
    input_report_key(kbd->input, KEY_RIGHTCTRL,  !!(kbd->data[0] & 0x10));
    input_report_key(kbd->input, KEY_RIGHTSHIFT, !!(kbd->data[0] & 0x20));
    input_report_key(kbd->input, KEY_RIGHTALT,   !!(kbd->data[0] & 0x40));
    input_report_key(kbd->input, KEY_RIGHTMETA,  !!(kbd->data[0] & 0x80));

    /* 2. Key Release */
    for (i = 0; i < 6; i++) {
        if (kbd->old_keys[i] > 3 && !is_key_pressed(kbd->old_keys[i], kbd->data + 2)) {
            input_report_key(kbd->input, usb_kbd_keycode[kbd->old_keys[i]], 0);
        }
    }

    /* 3. Key Press */
    for (i = 0; i < 6; i++) {
        if (kbd->data[i+2] > 3 && !is_key_pressed(kbd->data[i+2], kbd->old_keys)) {
            input_report_key(kbd->input, usb_kbd_keycode[kbd->data[i+2]], 1);
        }
    }

    memcpy(kbd->old_keys, kbd->data + 2, 6);
    input_sync(kbd->input);

resubmit:
    status = usb_submit_urb(urb, GFP_ATOMIC);
    if (status)
        dev_err(&kbd->udev->dev, "can't resubmit intr, %d\n", status);
}

static int kbd_probe(struct usb_interface *intf, const struct usb_device_id *id) {
    struct usb_device *udev = interface_to_usbdev(intf);
    struct usb_endpoint_descriptor *ep_irq;
    struct usb_kbd *kbd;
    struct input_dev *input_dev;
    int i, pipe, maxp, error = -ENOMEM;

    if (intf->cur_altsetting->desc.bNumEndpoints < 1) return -ENODEV;
    ep_irq = &intf->cur_altsetting->endpoint[0].desc;
    if (!usb_endpoint_is_int_in(ep_irq)) return -ENODEV;

    kbd = kzalloc(sizeof(struct usb_kbd), GFP_KERNEL);
    input_dev = input_allocate_device();
    if (!kbd || !input_dev) goto fail1;

    kbd->udev = udev;
    kbd->input = input_dev;
    kbd->interface = intf;

    kbd->data = usb_alloc_coherent(udev, DATA_SIZE, GFP_KERNEL, &kbd->data_dma);
    if (!kbd->data) goto fail1;
    kbd->leds = usb_alloc_coherent(udev, 1, GFP_KERNEL, &kbd->leds_dma);
    if (!kbd->leds) goto fail2;
    kbd->cr   = usb_alloc_coherent(udev, sizeof(struct usb_ctrlrequest), GFP_KERNEL, &kbd->cr_dma);
    if (!kbd->cr) goto fail3;
    kbd->irq_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!kbd->irq_urb) goto fail4;
    kbd->led_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!kbd->led_urb) goto fail5;

    input_dev->name = "My Dell Keyboard (Shortcuts Fixed)";
    input_dev->phys = "usb/input0";
    input_dev->id.bustype = BUS_USB;
    input_dev->id.vendor  = USB_KB_VENDOR_ID;
    input_dev->id.product = USB_KB_PRODUCT_ID;
    input_set_drvdata(input_dev, kbd);

    input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_LED) | BIT_MASK(EV_REP);
    input_dev->ledbit[0] = BIT_MASK(LED_NUML) | BIT_MASK(LED_CAPSL) | BIT_MASK(LED_SCROLLL);

    /* 1. Register Standard Keys (A, B, C...) */
    for (i = 0; i < 255; i++) 
        set_bit(usb_kbd_keycode[i], input_dev->keybit);
    
    /* 2. CRITICAL FIX: Register Modifier Keys (Ctrl, Shift, Alt...) */
    /* Without this, Linux ignores these keys entirely! */
    set_bit(KEY_LEFTCTRL,   input_dev->keybit);
    set_bit(KEY_LEFTSHIFT,  input_dev->keybit);
    set_bit(KEY_LEFTALT,    input_dev->keybit);
    set_bit(KEY_LEFTMETA,   input_dev->keybit);
    set_bit(KEY_RIGHTCTRL,  input_dev->keybit);
    set_bit(KEY_RIGHTSHIFT, input_dev->keybit);
    set_bit(KEY_RIGHTALT,   input_dev->keybit);
    set_bit(KEY_RIGHTMETA,  input_dev->keybit);

    /* Snappy Repeat Rate for selection (Shift+Arrows) */
    /* Setting rep[] by hand would tell the input core the driver repeats keys itself;
       input_enable_softrepeat() sets the values and keeps the core's repeat timer. */
    input_enable_softrepeat(input_dev, 250, 33);
    input_dev->event = kbd_event;

    kbd->cr->bRequestType = USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    kbd->cr->bRequest     = 0x09; 
    kbd->cr->wValue       = cpu_to_le16(0x200);
    kbd->cr->wIndex       = cpu_to_le16(intf->cur_altsetting->desc.bInterfaceNumber);
    kbd->cr->wLength      = cpu_to_le16(1);

    usb_fill_control_urb(kbd->led_urb, udev, usb_sndctrlpipe(udev, 0),
                         (unsigned char *)kbd->cr, kbd->leds, 1,
                         kbd_led_complete, kbd);
    kbd->led_urb->setup_dma = kbd->cr_dma;
    kbd->led_urb->transfer_dma = kbd->leds_dma;
    kbd->led_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    pipe = usb_rcvintpipe(udev, ep_irq->bEndpointAddress);
    maxp = usb_maxpacket(udev, pipe);
    usb_fill_int_urb(kbd->irq_urb, udev, pipe, kbd->data,
                     (maxp > DATA_SIZE ? DATA_SIZE : maxp),
                     kbd_irq, kbd, ep_irq->bInterval);
    kbd->irq_urb->transfer_dma = kbd->data_dma;
    kbd->irq_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    usb_control_msg(udev, usb_sndctrlpipe(udev, 0), 0x0B, USB_TYPE_CLASS | USB_RECIP_INTERFACE, 0, 
                    intf->cur_altsetting->desc.bInterfaceNumber, NULL, 0, USB_CTRL_SET_TIMEOUT);

    error = input_register_device(kbd->input);
    if (error) goto fail6;
    usb_set_intfdata(intf, kbd);

    error = usb_submit_urb(kbd->irq_urb, GFP_KERNEL);
    if (error) goto fail7;

    pr_info("Dell Keyboard Driver Loaded (Fully Functional).\n");
    return 0;

fail7:
    usb_set_intfdata(intf, NULL);
    input_unregister_device(input_dev);
    input_dev = NULL;
    usb_kill_urb(kbd->led_urb); /* an LED update may have been sent after registration */
fail6:
    usb_free_urb(kbd->led_urb);
fail5:
    usb_free_urb(kbd->irq_urb);
fail4:
    usb_free_coherent(udev, sizeof(struct usb_ctrlrequest), kbd->cr, kbd->cr_dma);
fail3:
    usb_free_coherent(udev, 1, kbd->leds, kbd->leds_dma);
fail2:
    usb_free_coherent(udev, DATA_SIZE, kbd->data, kbd->data_dma);
fail1:
    input_free_device(input_dev);
    kfree(kbd);
    return error;
}

static void kbd_disconnect(struct usb_interface *intf) {
    struct usb_kbd *kbd = usb_get_intfdata(intf);
    usb_set_intfdata(intf, NULL);
    if (kbd) {
        usb_kill_urb(kbd->irq_urb);
        usb_kill_urb(kbd->led_urb);
        input_unregister_device(kbd->input);
        usb_free_urb(kbd->irq_urb);
        usb_free_urb(kbd->led_urb);
        usb_free_coherent(kbd->udev, DATA_SIZE, kbd->data, kbd->data_dma);
        usb_free_coherent(kbd->udev, 1, kbd->leds, kbd->leds_dma);
        usb_free_coherent(kbd->udev, sizeof(struct usb_ctrlrequest), kbd->cr, kbd->cr_dma);
        kfree(kbd);
    }
}

static struct usb_device_id kbd_table[] = {
    { USB_DEVICE(USB_KB_VENDOR_ID, USB_KB_PRODUCT_ID) },
    {}
};
MODULE_DEVICE_TABLE(usb, kbd_table);

static struct usb_driver kbd_driver = {
    .name = "my_dell_kbd",
    .probe = kbd_probe,
    .disconnect = kbd_disconnect,
    .id_table = kbd_table,
};

module_usb_driver(kbd_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Daniels");
MODULE_DESCRIPTION("Keyboard Device Driver");
