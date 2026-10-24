# 4. Mouse driver walkthrough: `Mouse/Logitech_driver.c`

Source: [Mouse/Logitech_driver.c](../Mouse/Logitech_driver.c)

The mouse driver is the simpler of the two and closely follows the upstream
`drivers/hid/usbhid/usbmouse.c`. It has one URB, one buffer and no output path.

## 4.1 Overview

| Function / object                                   | Lines | Role                                             |
| --------------------------------------------------- | ----- | ------------------------------------------------ |
| `USB_MOUSE_VENDOR_ID`, `USB_MOUSE_PRODUCT_ID`        | [8-9](../Mouse/Logitech_driver.c#L8)     | target device `046d:c077`          |
| `struct usb_mouse`                                   | [11-19](../Mouse/Logitech_driver.c#L11)  | per-device state                   |
| `mouse_table`                                        | [21-25](../Mouse/Logitech_driver.c#L21)  | ID table for matching              |
| `mouse_irq()`                                        | [27-60](../Mouse/Logitech_driver.c#L27)  | URB completion: decode and report  |
| `mouse_probe()`                                      | [62-150](../Mouse/Logitech_driver.c#L62) | bind to the interface              |
| `mouse_disconnect()`                                 | [152-163](../Mouse/Logitech_driver.c#L152) | unbind and free                  |
| `mouse_driver`, `module_usb_driver()`                | [165-172](../Mouse/Logitech_driver.c#L165) | registration with usbcore        |

## 4.2 Per-device state

```c
struct usb_mouse {
    char name[128];          /* backing storage for input_dev->name */
    char phys[64];           /* backing storage for input_dev->phys */
    struct usb_device *udev; /* the USB device, needed for submit/free */
    struct input_dev *input; /* the input device we report into */
    struct urb *urb;         /* the single interrupt-IN URB */
    unsigned char *data;     /* CPU address of the 16-byte DMA buffer */
    dma_addr_t data_dma;     /* bus address of the same buffer */
};
```

`input_dev->name` and `->phys` are `const char *`: the input core does not
copy them. They must stay valid for as long as the input device exists, so
they are stored inside the struct rather than on the stack.

## 4.3 Device matching

```c
static struct usb_device_id mouse_table[] = {
    { USB_DEVICE(USB_MOUSE_VENDOR_ID, USB_MOUSE_PRODUCT_ID) },
    {}                                   /* terminating entry */
};
MODULE_DEVICE_TABLE(usb, mouse_table);
```

`USB_DEVICE()` matches on vendor and product ID only, so `probe` is called for
every interface of the device. The endpoint check at the start of `probe`
rejects interfaces that do not look like a mouse.

## 4.4 `mouse_probe()`

`probe` runs in process context when the USB core offers an interface to the
driver.

### Step 1: validate the interface ([L71-75](../Mouse/Logitech_driver.c#L71))

```c
interface = intf->cur_altsetting;
if (interface->desc.bNumEndpoints != 1) return -ENODEV;
endpoint = &interface->endpoint[0].desc;
if (!usb_endpoint_is_int_in(endpoint)) return -ENODEV;
```

A boot-style mouse interface has exactly one endpoint, interrupt IN.
Returning `-ENODEV` tells the core "not mine" without logging an error.

### Step 2: allocate state, input device, buffer and URB ([L77-85](../Mouse/Logitech_driver.c#L77))

```c
mouse = kzalloc(sizeof(struct usb_mouse), GFP_KERNEL);
input_dev = input_allocate_device();
if (!mouse || !input_dev) goto fail1;

mouse->data = usb_alloc_coherent(udev, 16, GFP_KERNEL, &mouse->data_dma);
if (!mouse->data) goto fail1;

mouse->urb = usb_alloc_urb(0, GFP_KERNEL);
if (!mouse->urb) goto fail2;
```

- `kzalloc` zeroes the struct, so `name` and `phys` start as empty strings.
- The receive buffer is 16 bytes, more than the 4 bytes that are decoded, so
  a device with a larger `wMaxPacketSize` still fits (up to 16).
- Every allocation is checked, and each failure jumps to a label that undoes
  exactly what has been done so far (see 4.4 step 8).

### Step 3: name and physical path ([L90-97](../Mouse/Logitech_driver.c#L90))

```c
if (udev->manufacturer)
    strscpy(mouse->name, udev->manufacturer, sizeof(mouse->name));
if (udev->product) {
    if (udev->manufacturer) strlcat(mouse->name, " ", sizeof(mouse->name));
    strlcat(mouse->name, udev->product, sizeof(mouse->name));
}
usb_make_path(udev, mouse->phys, sizeof(mouse->phys));
strlcat(mouse->phys, "/input0", sizeof(mouse->phys));
```

`udev->manufacturer` and `udev->product` are the string descriptors that
usbcore read during enumeration (they may be NULL). The result is something
like `Logitech USB Optical Mouse`. `usb_make_path` produces a topology path
such as `usb-0000:00:14.0-2`, and `/input0` is appended, matching the
convention used by upstream drivers. Both strings are truncated safely to the
buffer size.

### Step 4: identity and capabilities ([L99-109](../Mouse/Logitech_driver.c#L99))

```c
input_dev->id.bustype = BUS_USB;
input_dev->id.vendor  = le16_to_cpu(udev->descriptor.idVendor);
input_dev->id.product = le16_to_cpu(udev->descriptor.idProduct);
input_dev->id.version = le16_to_cpu(udev->descriptor.bcdDevice);
input_dev->dev.parent = &intf->dev;

input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);
input_dev->keybit[BIT_WORD(BTN_MOUSE)] =
        BIT_MASK(BTN_LEFT) | BIT_MASK(BTN_RIGHT) | BIT_MASK(BTN_MIDDLE);
input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y) | BIT_MASK(REL_WHEEL);
```

- The ID fields come from the device descriptor, which is little-endian.
- `dev.parent` places the input device under the USB interface in sysfs, so
  udev rules and `libinput list-devices` see it as a USB device.
- The capability bitmaps declare exactly the events `mouse_irq` emits. Any
  event not declared here would be dropped by the input core. Declaring
  `EV_KEY` with `BTN_LEFT` and `EV_REL` with `REL_X`/`REL_Y` is also what makes
  udev tag the device as `ID_INPUT_MOUSE`, which is how libinput decides to
  treat it as a pointer.

### Step 5: select Report Protocol ([L111-118](../Mouse/Logitech_driver.c#L111))

```c
error = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
                        HID_REQ_SET_PROTOCOL,                 /* bRequest 0x0B */
                        USB_TYPE_CLASS | USB_RECIP_INTERFACE, /* bRequestType 0x21 */
                        1,                                    /* wValue: 1 = Report Protocol */
                        interface->desc.bInterfaceNumber,     /* wIndex */
                        NULL, 0, USB_CTRL_SET_TIMEOUT);
if (error < 0)
    dev_warn(&udev->dev, "Failed to set Report Protocol (Err: %d)\n", error);
```

This is a synchronous control transfer on endpoint 0 (allowed here because
`probe` may sleep). It asks the mouse to use its full report format instead of
the 3-byte boot format, so the wheel byte is present. Devices start in Report
Protocol after reset anyway, so this mainly undoes a previous driver or BIOS
having switched the mouse into Boot Protocol. A failure is only a warning:
the mouse may still work, and some devices stall class requests they do not
support.

Note that `error` is reused below, so the warning does not leak into the
return value.

### Step 6: fill the interrupt URB ([L120-127](../Mouse/Logitech_driver.c#L120))

```c
pipe = usb_rcvintpipe(udev, endpoint->bEndpointAddress);
maxp = usb_maxpacket(udev, pipe);

usb_fill_int_urb(mouse->urb, udev, pipe, mouse->data,
                 (maxp > 16 ? 16 : maxp),
                 mouse_irq, mouse, endpoint->bInterval);
mouse->urb->transfer_dma = mouse->data_dma;
mouse->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
```

| Argument                    | Meaning                                                           |
| --------------------------- | ----------------------------------------------------------------- |
| `pipe`                      | interrupt IN pipe for the mouse endpoint                          |
| `mouse->data`               | CPU address of the receive buffer                                 |
| `min(maxp, 16)`             | transfer length: one packet, capped to the buffer size            |
| `mouse_irq`                 | completion handler                                                |
| `mouse`                     | `urb->context`, gives the handler its state                       |
| `endpoint->bInterval`       | polling interval from the descriptor                              |

The two lines after the fill switch the URB to the pre-mapped DMA buffer
(see [02-usb-and-hid-primer.md](02-usb-and-hid-primer.md#25-dma-coherent-buffers)).

### Step 7: register and start ([L129-137](../Mouse/Logitech_driver.c#L129))

```c
error = input_register_device(mouse->input);
if (error) goto fail3;

usb_set_intfdata(intf, mouse);

error = usb_submit_urb(mouse->urb, GFP_KERNEL);
if (error) goto fail4;

return 0;
```

The order matters:

1. The input device is registered before the URB is submitted, so that the
   very first report has somewhere to go.
2. `usb_set_intfdata` stores the state so that `disconnect` can find it.
3. The URB is submitted. From here on it keeps itself alive by resubmitting
   from the completion handler.

The upstream `usbmouse.c` submits the URB in the input device's `open`
callback and kills it in `close`, so the device is only polled while someone
has the event node open. This driver polls from `probe` to `disconnect`
regardless.

### Step 8: the error unwind ([L139-149](../Mouse/Logitech_driver.c#L139))

```c
fail4:
    input_unregister_device(input_dev);
    input_dev = NULL;            /* unregister already freed it */
fail3:
    usb_free_urb(mouse->urb);
fail2:
    usb_free_coherent(udev, 16, mouse->data, mouse->data_dma);
fail1:
    input_free_device(input_dev); /* NULL-safe */
    kfree(mouse);                 /* NULL-safe */
    return error;
```

This is the standard kernel "goto ladder": each label undoes one step, in
reverse order of acquisition, and falls through to the labels below it.

| Failure point                        | Jumps to | Undoes                                              |
| ------------------------------------ | -------- | --------------------------------------------------- |
| `kzalloc` / `input_allocate_device`  | `fail1`  | whichever of the two succeeded                      |
| `usb_alloc_coherent`                 | `fail1`  | struct and input device                             |
| `usb_alloc_urb`                      | `fail2`  | + DMA buffer                                        |
| `input_register_device`              | `fail3`  | + URB                                               |
| `usb_submit_urb`                     | `fail4`  | + unregister the input device                       |

Setting `input_dev = NULL` after `input_unregister_device` is required:
unregistering drops the last reference, so the later `input_free_device` must
become a no-op. `error` still holds the real error code, which is returned.

## 4.5 `mouse_irq()`: the completion handler

Runs in atomic context every time the mouse sends a report.

### Status check ([L34-42](../Mouse/Logitech_driver.c#L34))

```c
switch (urb->status) {
case 0: break;                     /* success: decode */
case -ECONNRESET:
case -ENOENT:
case -ESHUTDOWN:
    return;                        /* URB was cancelled or device is gone: stop */
default:
    goto resubmit;                 /* transient error: skip decoding, try again */
}
```

The three "stop" statuses mean the URB was killed or unlinked by the driver,
or the device/host controller is going away. Resubmitting would either fail
or fight with `disconnect`. Any other error is treated as transient: the
report is discarded and the URB is resubmitted so the mouse keeps working.

The handler deliberately does not log each report: it runs hundreds of times
per second in atomic context. To inspect raw reports, use `usbmon` as
described in
[06-build-load-debug.md](06-build-load-debug.md#65-watching-the-usb-traffic-with-usbmon).

### Decoding ([L44-54](../Mouse/Logitech_driver.c#L44))

```c
if (urb->actual_length >= 4) {
    input_report_key(dev, BTN_LEFT,   data[0] & 0x01);
    input_report_key(dev, BTN_RIGHT,  data[0] & 0x02);
    input_report_key(dev, BTN_MIDDLE, data[0] & 0x04);

    input_report_rel(dev, REL_X,     (signed char)data[1]);
    input_report_rel(dev, REL_Y,     (signed char)data[2]);
    input_report_rel(dev, REL_WHEEL, (signed char)data[3]);
}
input_sync(dev);
```

Report layout assumed by the driver:

| Byte | Bits    | Meaning                         | Event        |
| ---- | ------- | ------------------------------- | ------------ |
| 0    | bit 0   | left button                     | `BTN_LEFT`   |
| 0    | bit 1   | right button                    | `BTN_RIGHT`  |
| 0    | bit 2   | middle button / wheel click     | `BTN_MIDDLE` |
| 1    | 8 bits  | X delta, signed, right is +     | `REL_X`      |
| 2    | 8 bits  | Y delta, signed, down is +      | `REL_Y`      |
| 3    | 8 bits  | wheel delta, signed, up is +    | `REL_WHEEL`  |

- The `(signed char)` casts are essential. The buffer is `unsigned char`, so
  a movement of -2 arrives as `0xfe` = 254. Casting reinterprets it as -2
  (two's complement). Without the cast the pointer would jump far right
  instead of slightly left.
- HID Y and Linux `REL_Y` share the same convention (positive = down) and HID
  wheel and `REL_WHEEL` share the same convention (positive = away from the
  user), so no sign flips are needed.
- Button values are 0 or a non-zero bit value. `input_report_key` normalises
  them to 0/1, and the input core only emits an event when the state changes.
- Reports shorter than 4 bytes are ignored, but `input_sync` is still sent.
  An empty sync frame is harmless.

### Resubmit ([L56-59](../Mouse/Logitech_driver.c#L56))

```c
resubmit:
    status = usb_submit_urb(urb, GFP_ATOMIC);
    if (status)
        dev_err(&mouse->udev->dev, "can't resubmit intr, %d\n", status);
```

`GFP_ATOMIC` because this is a completion handler. If resubmission fails,
for example with `-EPERM` because `usb_kill_urb` is in progress or `-ENODEV`
because the device was just unplugged, the error is logged and polling stops.

## 4.6 `mouse_disconnect()`

```c
struct usb_mouse *mouse = usb_get_intfdata(intf);
usb_set_intfdata(intf, NULL);
if (mouse) {
    usb_kill_urb(mouse->urb);              /* 1. stop I/O, wait for the handler */
    input_unregister_device(mouse->input); /* 2. remove /dev/input/eventN */
    usb_free_urb(mouse->urb);              /* 3. free the URB */
    usb_free_coherent(mouse->udev, 16, mouse->data, mouse->data_dma); /* 4. free DMA buffer */
    kfree(mouse);                          /* 5. free the state */
}
```

Why this order:

1. `usb_kill_urb` must come first. It cancels the in-flight URB, waits until
   `mouse_irq` has returned (it sees `-ENOENT` and does not resubmit), and
   blocks any resubmission attempted during the kill. After it returns, the
   handler cannot run again, so nothing will touch `mouse->input` or
   `mouse->data`.
2. Only then is it safe to unregister the input device that the handler
   reports into.
3. The URB and buffer can be freed once nothing can use them.
4. The state struct goes last because the previous steps read from it.

`disconnect` is called both on physical unplug and on a software unbind
(`rmmod`, sysfs `unbind`). It cannot fail and must not leave anything behind.

## 4.7 Registration ([L165-175](../Mouse/Logitech_driver.c#L165))

```c
static struct usb_driver mouse_driver = {
    .name       = "Logitech_Mouse_Driver",
    .probe      = mouse_probe,
    .disconnect = mouse_disconnect,
    .id_table   = mouse_table,
};
module_usb_driver(mouse_driver);
```

`module_usb_driver()` generates the module's `init` and `exit` functions,
which call `usb_register()` and `usb_deregister()`. Deregistration calls
`disconnect` for every interface the driver still owns before the module is
unloaded. `.name` is the directory under `/sys/bus/usb/drivers/` and the name
shown by `lsusb -t`.
