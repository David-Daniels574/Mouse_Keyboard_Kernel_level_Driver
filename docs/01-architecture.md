# 1. Architecture

## 1.1 The normal path for a USB keyboard or mouse

On a stock Linux system, a USB mouse or keyboard goes through several layers
before an application sees it:

```text
 hardware          kernel                                              user space
 --------   -----------------------------------------------------   -------------------
 device  -> host controller driver (xhci_hcd / ehci_hcd / ...)
            -> usbcore           (enumeration, URB routing)
               -> usbhid         (USB transport for HID)
                  -> hid core    (parses the report descriptor)
                     -> hid-generic / hid-logitech-* / ...
                        -> input core
                           -> evdev  -> /dev/input/eventN  -> libinput -> compositor / X
                           -> kbd handler (VT console)
```

`usbhid` + the HID core are generic: they read the device's *report
descriptor* (a small program that describes the layout of every report) and
decode reports according to it. This is why any standards-compliant mouse
works without a device-specific driver.

## 1.2 The path with these drivers

These modules replace everything between `usbcore` and the input core:

```text
 device -> host controller driver
           -> usbcore
              -> Logitech_Mouse_Driver  or  my_dell_kbd     (this repository)
                 -> input core
                    -> evdev -> /dev/input/eventN -> libinput -> ...
                    -> kbd handler (VT console)
```

There is no HID core in the middle, so nothing parses the report descriptor.
Instead the drivers hard-code the byte layout of the report:

- the keyboard forces the device into **Boot Protocol**, whose 8-byte layout
  is fixed by the HID specification, so hard-coding it is safe;
- the mouse requests **Report Protocol** and assumes the reports are
  `[buttons][X][Y][wheel]`, which is true for the target device but is not
  guaranteed by any specification.

Above the driver, nothing changes. The input core, evdev, libinput and the
desktop cannot tell that a custom driver produced the events.

## 1.3 How a device gets bound to the driver

1. **Registration.** `module_usb_driver(...)` expands to module init/exit
   functions that call `usb_register()` / `usb_deregister()`. Registration
   adds the `struct usb_driver` to the USB bus and prints
   `usbcore: registered new interface driver <name>`.
2. **Matching.** Each driver has an ID table with one entry built by
   `USB_DEVICE(VID, PID)`. This macro sets `match_flags` to
   `USB_DEVICE_ID_MATCH_DEVICE`, so it matches any *interface* of a device
   with that vendor and product ID. USB drivers bind to interfaces, not
   devices.
3. **Probing.** When an interface is unclaimed and matches, the USB core calls
   the driver's `probe(intf, id)`. If `probe` returns 0, the driver owns the
   interface. If it returns an error (for example `-ENODEV` because the
   endpoint layout is wrong), the core tries other drivers.
4. **Competing with usbhid.** `usbhid` matches the same interfaces by
   interface class (HID). A given interface can only have one driver. On a
   normal system `usbhid` is loaded long before these modules and claims the
   interface first, which is why the interface must be unbound from `usbhid`
   via sysfs before loading these modules. When the custom driver is then
   registered, the core probes it against the now-unclaimed interface.
5. **Disconnect.** When the device is unplugged, the driver is unbound via
   sysfs, or the module is removed, the core calls `disconnect(intf)`. After
   `disconnect` returns, the driver must not touch the device again.

`MODULE_DEVICE_TABLE(usb, ...)` additionally exports the ID table as a module
alias, so that `depmod`/`udev` could auto-load the module for this device if it
were installed into `/lib/modules`. It does not change which driver wins the
interface.

## 1.4 Runtime data flow

### Input (both drivers)

```text
 probe():  usb_submit_urb(irq_urb, GFP_KERNEL)
              |
              v
 host controller polls the interrupt IN endpoint every bInterval
              |
              | device returns a report (or NAKs if nothing changed)
              v
 DMA into the coherent buffer (mouse->data / kbd->data)
              |
              v
 completion handler runs in atomic context: mouse_irq() / kbd_irq()
   - check urb->status
   - decode bytes, input_report_key()/input_report_rel()
   - input_sync()
   - usb_submit_urb(urb, GFP_ATOMIC)   --> back to the top
```

The URB is a single, permanently recycled request. At any moment there is
at most one outstanding interrupt transfer per device.

### Output (keyboard LEDs only)

```text
 user space or VT presses Caps Lock
   -> EV_LED event injected into the input device (input_inject_event)
      -> input core updates dev->led and calls dev->event == kbd_event()
         -> kbd_event() builds the LED byte and submits led_urb (GFP_ATOMIC)
            -> host controller sends a SET_REPORT control transfer on endpoint 0
               -> keyboard lights the LED
               -> kbd_led_complete() runs (only logs errors)
```

The keyboard does not change its own LEDs. It is the host's job to tell it,
which is why Caps Lock "works" (characters become uppercase) but the light
stays off if a driver forgets this path.

## 1.5 Driver state

Each bound interface gets one heap-allocated state struct, stored with
`usb_set_intfdata(intf, ptr)` and retrieved in `disconnect` with
`usb_get_intfdata(intf)`. The same pointer is passed as the URB `context`
(available as `urb->context` in the completion handler) and, for the keyboard,
also attached to the input device with `input_set_drvdata` so `kbd_event` can
find it.

```text
              usb_interface ---intfdata---> struct usb_kbd <---drvdata--- input_dev
                                                ^
                                                | urb->context
                                         irq_urb, led_urb
```

## 1.6 Execution contexts

Knowing which context each function runs in explains most of the API choices:

| Function                                       | Context                    | Consequences                                                   |
| ---------------------------------------------- | -------------------------- | -------------------------------------------------------------- |
| `mouse_probe`, `kbd_probe`                     | process, may sleep         | `GFP_KERNEL` allocations, synchronous `usb_control_msg` is OK  |
| `mouse_disconnect`, `kbd_disconnect`           | process, may sleep         | `usb_kill_urb` (which sleeps) is OK                            |
| `mouse_irq`, `kbd_irq`, `kbd_led_complete`     | atomic (URB completion)    | no sleeping, `GFP_ATOMIC` only, keep it short                  |
| `kbd_event`                                    | atomic (input `event_lock` held, IRQs off) | no sleeping, `GFP_ATOMIC` only                 |
