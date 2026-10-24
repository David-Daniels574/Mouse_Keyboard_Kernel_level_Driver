# 5. Keyboard driver walkthrough: `Keyboard/my_keyboard.c`

Source: [Keyboard/my_keyboard.c](../Keyboard/my_keyboard.c)

The keyboard driver is based on the upstream `drivers/hid/usbhid/usbkbd.c`.
Compared with the mouse it adds three things: a translation table from HID
usage IDs to Linux keycodes, a diff between consecutive reports to find
presses and releases, and an output path (a second URB) for the lock LEDs.

## 5.1 Overview

| Function / object         | Lines                                             | Role                                         |
| ------------------------- | ------------------------------------------------- | -------------------------------------------- |
| `USB_KB_VENDOR_ID`, `USB_KB_PRODUCT_ID`, `DATA_SIZE` | [8-10](../Keyboard/my_keyboard.c#L8) | target `413c:2107`, 8-byte report |
| `usb_kbd_keycode[256]`    | [13-25](../Keyboard/my_keyboard.c#L13)            | HID usage ID -> Linux keycode                |
| `struct usb_kbd`          | [27-42](../Keyboard/my_keyboard.c#L27)            | per-device state                             |
| `is_key_pressed()`        | [44-51](../Keyboard/my_keyboard.c#L44)            | "is this usage in that 6-key array?"         |
| `kbd_led_complete()`      | [53-56](../Keyboard/my_keyboard.c#L53)            | LED URB completion                           |
| `kbd_event()`             | [58-72](../Keyboard/my_keyboard.c#L58)            | input core -> driver: LED changes            |
| `kbd_irq()`               | [74-115](../Keyboard/my_keyboard.c#L74)           | input URB completion: decode and report      |
| `kbd_probe()`             | [117-231](../Keyboard/my_keyboard.c#L117)         | bind to the interface                        |
| `kbd_disconnect()`        | [233-247](../Keyboard/my_keyboard.c#L233)         | unbind and free                              |
| `kbd_table`, `kbd_driver` | [249-262](../Keyboard/my_keyboard.c#L249)         | matching and registration                    |

## 5.2 The keycode table

```c
static const unsigned char usb_kbd_keycode[256] = {
      0,   0,   0,   0,  30,  48,  46,  32,  18,  33,  34,  35,  23,  36,  37,  38,
     ...
};
```

HID and Linux number keys differently. HID usage IDs (HID Usage Tables,
"Keyboard/Keypad" page 0x07) are ordered alphabetically: `0x04` = a,
`0x05` = b, ... Linux keycodes (`<linux/input-event-codes.h>`) follow the old
PC scancode order: `KEY_A` = 30, `KEY_B` = 48, `KEY_Q` = 16. The table is
indexed by HID usage ID and yields the Linux keycode.

Some example entries:

| HID usage | Key         | Table value | Linux constant   |
| --------- | ----------- | ----------- | ---------------- |
| `0x04`    | a           | 30          | `KEY_A`          |
| `0x1E`    | 1           | 2           | `KEY_1`          |
| `0x28`    | Enter       | 28          | `KEY_ENTER`      |
| `0x29`    | Escape      | 1           | `KEY_ESC`        |
| `0x2C`    | Space       | 57          | `KEY_SPACE`      |
| `0x39`    | Caps Lock   | 58          | `KEY_CAPSLOCK`   |
| `0x4F`    | Right arrow | 106         | `KEY_RIGHT`      |
| `0x53`    | Num Lock    | 69          | `KEY_NUMLOCK`    |

The array is declared with 256 entries but only the first 176 (11 rows of 16,
usages `0x00`-`0xAF`) are written; C zero-initialises the rest. The values
match the beginning of the table in upstream `usbkbd.c`. The upstream table
continues up to usage `0xFF` and, crucially, includes the modifier usages
`0xE0`-`0xE7` (mapping to `KEY_LEFTCTRL` ... `KEY_RIGHTMETA`) and a few media
keys. Their absence here is what made the explicit modifier registration in
`probe` necessary (see 5.6 step 4).

A table value of 0 means "no Linux key" (`KEY_RESERVED`).

## 5.3 Per-device state

```c
struct usb_kbd {
    struct usb_device *udev;
    struct input_dev *input;
    struct usb_interface *interface;
    struct urb *irq_urb;            /* interrupt IN: key reports */
    struct urb *led_urb;            /* control OUT: LED reports */

    unsigned char *data;            /* 8-byte input report buffer (DMA) */
    dma_addr_t data_dma;
    unsigned char *leds;            /* 1-byte LED output report buffer (DMA) */
    dma_addr_t leds_dma;
    struct usb_ctrlrequest *cr;     /* setup packet for SET_REPORT (DMA) */
    dma_addr_t cr_dma;

    unsigned char old_keys[6];      /* key array from the previous report */
};
```

Three separate coherent allocations are used: the input report, the LED
byte and the control setup packet. `old_keys` is not touched by DMA, so it
lives in the struct.

## 5.4 `is_key_pressed()`

```c
static int is_key_pressed(unsigned char key, unsigned char *buffer) {
    int i;
    if (key <= 3) return 0;
    for (i = 0; i < 6; i++)
        if (buffer[i] == key) return 1;
    return 0;
}
```

Returns 1 if `key` is one of the 6 entries of `buffer`. Values 0-3 are never
"pressed": 0 is an empty slot and 1-3 are error codes (ErrorRollOver,
POSTFail, ErrorUndefined). This is the same test that upstream performs with
`memscan()`.

## 5.5 `kbd_irq()`: decoding a report

Runs in atomic context each time the keyboard sends its 8-byte report. A boot
keyboard sends a report whenever the set of pressed keys changes.

```text
 data[0]   data[1]   data[2] data[3] data[4] data[5] data[6] data[7]
 modifiers reserved  key1    key2    key3    key4    key5    key6
```

### Status check ([L78-82](../Keyboard/my_keyboard.c#L78))

```c
switch (urb->status) {
    case 0: break;
    case -ECONNRESET: case -ENOENT: case -ESHUTDOWN: return;
    default: goto resubmit; /* transient error: drop this report, keep polling */
}
```

On success, decode. On cancellation or shutdown, stop without resubmitting.
Any other error (for example `-EPROTO` or `-EOVERFLOW` from a noisy cable) is
treated as transient: the report is skipped and the URB is resubmitted, as in
the mouse driver and upstream `usbkbd.c`, so a single bus error does not stop
the keyboard.

### Modifiers ([L85-92](../Keyboard/my_keyboard.c#L85))

```c
input_report_key(kbd->input, KEY_LEFTCTRL,   !!(kbd->data[0] & 0x01));
input_report_key(kbd->input, KEY_LEFTSHIFT,  !!(kbd->data[0] & 0x02));
...
input_report_key(kbd->input, KEY_RIGHTMETA,  !!(kbd->data[0] & 0x80));
```

In Boot Protocol, modifiers never appear in the 6-key array; they are bits in
byte 0. Each bit is reported as the current state of its key. All 8 are
reported on every report, and the input core turns that into actual events
only for modifiers whose state changed (see
[03-input-subsystem.md](03-input-subsystem.md#33-reporting-events)).

Upstream does the same with a loop over `usb_kbd_keycode[i + 224]`; this
driver writes the 8 keycodes out explicitly, which does not depend on the
table containing entries 224-231.

### Releases ([L95-99](../Keyboard/my_keyboard.c#L95))

```c
for (i = 0; i < 6; i++) {
    if (kbd->old_keys[i] > 3 && !is_key_pressed(kbd->old_keys[i], kbd->data + 2))
        input_report_key(kbd->input, usb_kbd_keycode[kbd->old_keys[i]], 0);
}
```

A key that was in the previous array but is not in the new one has been
released.

### Presses ([L102-106](../Keyboard/my_keyboard.c#L102))

```c
for (i = 0; i < 6; i++) {
    if (kbd->data[i+2] > 3 && !is_key_pressed(kbd->data[i+2], kbd->old_keys))
        input_report_key(kbd->input, usb_kbd_keycode[kbd->data[i+2]], 1);
}
```

A key that is in the new array but was not in the previous one has been
pressed. Keys present in both arrays are still held and produce no event.

### Save state, sync, resubmit ([L108-114](../Keyboard/my_keyboard.c#L108))

```c
    memcpy(kbd->old_keys, kbd->data + 2, 6);
    input_sync(kbd->input);

resubmit:
    status = usb_submit_urb(urb, GFP_ATOMIC);
    if (status)
        dev_err(&kbd->udev->dev, "can't resubmit intr, %d\n", status);
```

The current array is copied out of the DMA buffer before the URB is
resubmitted, because resubmission lets the hardware overwrite `data`.
`GFP_ATOMIC` is required because this is a completion handler. If
resubmission fails (for example `-EPERM` while `usb_kill_urb` is running, or
`-ENODEV` right after an unplug), the error is logged and polling stops.

### Worked example

Typing a capital "A" (Left Shift, then a, then release both):

| Report (hex)                 | old_keys      | Events emitted                              |
| ---------------------------- | ------------- | ------------------------------------------- |
| `02 00 00 00 00 00 00 00`    | all 0         | `KEY_LEFTSHIFT 1`, `SYN`                    |
| `02 00 04 00 00 00 00 00`    | all 0         | `KEY_A 1`, `SYN` (shift unchanged, filtered by core) |
| `02 00 00 00 00 00 00 00`    | `04 ...`      | `KEY_A 0`, `SYN`                            |
| `00 00 00 00 00 00 00 00`    | all 0         | `KEY_LEFTSHIFT 0`, `SYN`                    |

Why diffing is needed: the report is a *snapshot* of what is held, not a list
of changes. Order within the array is not meaningful, and a key can move
between slots when another key is released, so slot-by-slot comparison would
produce false events. Comparing set membership (`is_key_pressed`) is order
independent.

## 5.6 `kbd_probe()`

### Step 1: validate ([L124-126](../Keyboard/my_keyboard.c#L124))

```c
if (intf->cur_altsetting->desc.bNumEndpoints < 1) return -ENODEV;
ep_irq = &intf->cur_altsetting->endpoint[0].desc;
if (!usb_endpoint_is_int_in(ep_irq)) return -ENODEV;
```

The interface must have at least one endpoint and the first must be
interrupt IN.

### Step 2: allocate ([L128-145](../Keyboard/my_keyboard.c#L128))

```c
int i, pipe, maxp, error = -ENOMEM;
...
kbd = kzalloc(sizeof(struct usb_kbd), GFP_KERNEL);
input_dev = input_allocate_device();
if (!kbd || !input_dev) goto fail1;
...
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
```

Five resources beyond the struct and the input device. Every allocation is
checked, and each failure jumps to the label that frees exactly what has been
allocated so far (the full ladder is in step 9). `error` starts as `-ENOMEM`,
which is the right return value for all of these failures.

### Step 3: identity and event types ([L147-155](../Keyboard/my_keyboard.c#L147))

```c
input_dev->name = "My Dell Keyboard (Shortcuts Fixed)";
input_dev->phys = "usb/input0";
input_dev->id.bustype = BUS_USB;
input_dev->id.vendor  = USB_KB_VENDOR_ID;
input_dev->id.product = USB_KB_PRODUCT_ID;
input_set_drvdata(input_dev, kbd);

input_dev->evbit[0]  = BIT_MASK(EV_KEY) | BIT_MASK(EV_LED) | BIT_MASK(EV_REP);
input_dev->ledbit[0] = BIT_MASK(LED_NUML) | BIT_MASK(LED_CAPSL) | BIT_MASK(LED_SCROLLL);
```

- The name is a fixed string, visible in `evtest` and
  `/proc/bus/input/devices`. "Shortcuts Fixed" refers to the modifier fix in
  step 4.
- `input_set_drvdata` lets `kbd_event` get from the `input_dev` back to
  `struct usb_kbd`.
- `EV_KEY`: the device produces keys. `EV_LED` + `ledbit`: the device accepts
  these three LEDs. `EV_REP`: the device supports autorepeat (see step 5).

### Step 4: register keycodes ([L158-170](../Keyboard/my_keyboard.c#L158))

```c
for (i = 0; i < 255; i++)
    set_bit(usb_kbd_keycode[i], input_dev->keybit);

set_bit(KEY_LEFTCTRL,   input_dev->keybit);
set_bit(KEY_LEFTSHIFT,  input_dev->keybit);
...
set_bit(KEY_RIGHTMETA,  input_dev->keybit);
```

The loop advertises every keycode that the table can produce. The table only
covers usages up to `0xAF`, so the 8 modifier keycodes would not be set by
the loop. The input core drops events for keycodes that are not in `keybit`,
so before this fix `input_report_key(..., KEY_LEFTCTRL, ...)` in `kbd_irq` was
silently discarded: typing worked but Ctrl/Shift/Alt/Meta and therefore every
shortcut did not. Setting the 8 bits explicitly fixes it.

The loop also sets bit 0 (`KEY_RESERVED`), because many table entries are 0.
Upstream clears it afterwards with `clear_bit(0, input_dev->keybit)`.

### Step 5: autorepeat values and the event callback ([L172-176](../Keyboard/my_keyboard.c#L172))

```c
input_enable_softrepeat(input_dev, 250, 33);
input_dev->event = kbd_event;
```

This configures kernel software autorepeat with a 250 ms initial delay and a
33 ms period (about 30 repeats per second). `input_enable_softrepeat()` sets
`rep[REP_DELAY]` and `rep[REP_PERIOD]` and installs the input core's repeat
timer callback, so the core generates repeat events (value 2) for held keys.

The function has to be used instead of assigning `rep[]` directly. If the
`rep[]` values are already set when `input_register_device()` runs, the core
assumes the driver repeats keys itself and does not start its timer (see
[03-input-subsystem.md](03-input-subsystem.md#35-autorepeat)).

Graphical sessions (libinput / X.org) ignore kernel repeat events and repeat
keys in user space with their own settings, so these values mainly affect the
text console and other consumers of the raw evdev stream.

`input_dev->event` is the hook through which the input core forwards
`EV_LED` (and other output) events to the driver.

### Step 6: prepare the LED control URB ([L178-189](../Keyboard/my_keyboard.c#L178))

```c
kbd->cr->bRequestType = USB_TYPE_CLASS | USB_RECIP_INTERFACE;  /* 0x21: host->device, class, interface */
kbd->cr->bRequest     = 0x09;                                  /* HID SET_REPORT */
kbd->cr->wValue       = cpu_to_le16(0x200);                    /* type 2 = Output, report ID 0 */
kbd->cr->wIndex       = cpu_to_le16(intf->cur_altsetting->desc.bInterfaceNumber);
kbd->cr->wLength      = cpu_to_le16(1);                        /* 1 data byte */

usb_fill_control_urb(kbd->led_urb, udev, usb_sndctrlpipe(udev, 0),
                     (unsigned char *)kbd->cr, kbd->leds, 1,
                     kbd_led_complete, kbd);
kbd->led_urb->setup_dma    = kbd->cr_dma;
kbd->led_urb->transfer_dma = kbd->leds_dma;
kbd->led_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
```

The setup packet never changes, so it is built once here. Each later LED
update only rewrites the single byte at `*kbd->leds` and resubmits the same
URB. On the wire one LED update looks like:

```text
 SETUP: 21 09 00 02 00 00 01 00     (bRequestType, bRequest, wValue LE, wIndex LE, wLength LE)
 DATA:  02                          (Caps Lock on)
 STATUS
```

`URB_NO_TRANSFER_DMA_MAP` makes the host controller use `leds_dma` for the
data stage. Setup packets are always mapped by the host controller driver on
current kernels, so the `setup_dma` assignment is not required (upstream
allocates `cr` with plain `kmalloc` and does not set it). It is harmless.

### Step 7: prepare the input interrupt URB ([L191-197](../Keyboard/my_keyboard.c#L191))

```c
pipe = usb_rcvintpipe(udev, ep_irq->bEndpointAddress);
maxp = usb_maxpacket(udev, pipe);
usb_fill_int_urb(kbd->irq_urb, udev, pipe, kbd->data,
                 (maxp > DATA_SIZE ? DATA_SIZE : maxp),
                 kbd_irq, kbd, ep_irq->bInterval);
kbd->irq_urb->transfer_dma = kbd->data_dma;
kbd->irq_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
```

Same pattern as the mouse, with an 8-byte cap.

### Step 8: force Boot Protocol ([L199-200](../Keyboard/my_keyboard.c#L199))

```c
usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
                0x0B,                                 /* HID SET_PROTOCOL */
                USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                0,                                    /* wValue 0 = Boot Protocol */
                intf->cur_altsetting->desc.bInterfaceNumber,
                NULL, 0, USB_CTRL_SET_TIMEOUT);
```

This is what makes the hard-coded 8-byte layout in `kbd_irq` valid. In Boot
Protocol the HID specification fixes the report format, regardless of what
the keyboard's report descriptor says. Upstream `usbkbd.c` does not send this
request; it relies on the keyboard's report format matching the boot format.
Sending it explicitly is more robust. The return value is ignored.

### Step 9: register, start, and the error unwind ([L202-230](../Keyboard/my_keyboard.c#L202))

```c
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
```

Same order as the mouse: register, store state, start polling. The message
is printed on each successful probe, not on module load.

The unwind is a goto ladder: each label undoes one step, in reverse order of
acquisition, and falls through to the labels below it.

| Failure point                        | Jumps to | Undoes                                          |
| ------------------------------------ | -------- | ----------------------------------------------- |
| `kzalloc` / `input_allocate_device`  | `fail1`  | whichever of the two succeeded                  |
| `usb_alloc_coherent` (data)          | `fail1`  | struct and input device                         |
| `usb_alloc_coherent` (leds)          | `fail2`  | + data buffer                                   |
| `usb_alloc_coherent` (cr)            | `fail3`  | + LED buffer                                    |
| `usb_alloc_urb` (irq)                | `fail4`  | + setup packet buffer                           |
| `usb_alloc_urb` (led)                | `fail5`  | + interrupt URB                                 |
| `input_register_device`              | `fail6`  | + LED URB                                       |
| `usb_submit_urb` (irq)               | `fail7`  | + clear intfdata, unregister the input device, stop the LED URB |

Details of `fail7`:

- After `input_unregister_device()` the input device is freed, so
  `input_dev` is set to NULL to make the `input_free_device()` at `fail1` a
  no-op (same trick as the mouse).
- Once the input device is registered, input handlers such as the VT keyboard
  handler may already have pushed the current LED state, which submits the
  LED URB. It is killed after unregistering (so no new LED events can
  arrive) and before it is freed.
- `error` holds the code returned by the call that failed, which is what
  `probe` returns. The USB core logs it and may try other drivers.

## 5.7 `kbd_event()`: LED output

```c
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
```

1. Only `EV_LED` is handled.
2. A URB can only be submitted once at a time. If the previous LED transfer
   is still in flight (`status == -EINPROGRESS`), the update is skipped.
3. The input core has already updated `dev->led` before calling the hook, so
   the function ignores `code`/`value` and rebuilds the whole LED byte from
   the current state. That way the byte is always consistent even if several
   LEDs changed.
4. The byte is written into the DMA buffer and the pre-built control URB is
   submitted with `GFP_ATOMIC` (this function runs with a spinlock held and
   interrupts disabled).

Linux LED index to HID bit mapping:

| Linux        | Bit | Mask |
| ------------ | --- | ---- |
| `LED_NUML`   | 0   | 0x01 |
| `LED_CAPSL`  | 1   | 0x02 |
| `LED_SCROLLL`| 2   | 0x04 |

Step 2 can lose an update: if Caps Lock and Num Lock change in quick
succession, the second change arrives while the first URB is in flight, is
dropped, and nothing resends it. Upstream avoids this with a spinlock, a
"submitted" flag and a resubmit from the completion handler when the LED
state changed meanwhile.

## 5.8 `kbd_led_complete()`

```c
static void kbd_led_complete(struct urb *urb) {
    if (urb->status && urb->status != -ENOENT && urb->status != -ECONNRESET && urb->status != -ESHUTDOWN)
        pr_warn("led urb failed: %d\n", urb->status);
}
```

Only logs real failures. Cancellation and shutdown are expected during
disconnect and are not logged.

## 5.9 `kbd_disconnect()`

```c
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
```

Same reasoning as the mouse (see
[04-mouse-driver.md](04-mouse-driver.md#46-mouse_disconnect)), with two URBs
and three buffers. Both URBs are killed before the input device is
unregistered, so neither completion handler can run against a freed device.
Sizes passed to `usb_free_coherent` must match the sizes used at allocation.

There is a narrow window between `usb_kill_urb(kbd->led_urb)` and
`input_unregister_device()` in which an LED event could resubmit the LED
URB; upstream closes it with the same spinlock mentioned in 5.7.

## 5.10 Registration ([L249-265](../Keyboard/my_keyboard.c#L249))

```c
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
```

Identical in structure to the mouse. The sysfs driver directory is
`/sys/bus/usb/drivers/my_dell_kbd/`.
