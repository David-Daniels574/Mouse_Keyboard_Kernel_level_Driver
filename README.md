# Linux USB HID Drivers (Kernel Modules)

Two out-of-tree Linux kernel modules that drive a specific USB mouse and a
specific USB keyboard directly, without the generic `usbhid` / `hid-generic`
stack:

| Module                  | Source                                                  | Device                       | VID:PID     | usb_driver name         |
| ----------------------- | ------------------------------------------------------- | ---------------------------- | ----------- | ----------------------- |
| `Logitech_driver.ko`    | [Mouse/Logitech_driver.c](Mouse/Logitech_driver.c)      | Logitech USB optical mouse   | `046d:c077` | `Logitech_Mouse_Driver` |
| `my_keyboard.ko`        | [Keyboard/my_keyboard.c](Keyboard/my_keyboard.c)        | Dell USB keyboard            | `413c:2107` | `my_dell_kbd`           |

Each module registers a `struct usb_driver` with the USB core, claims the
device by vendor/product ID, reads input reports over an interrupt IN endpoint
using a URB backed by a DMA-coherent buffer, decodes the raw bytes by hand, and
reports the result to user space through the Linux input subsystem (so the
devices show up as normal `/dev/input/eventN` nodes).

This is a learning project. The drivers are modelled on the in-tree reference
drivers `drivers/hid/usbhid/usbmouse.c` and `drivers/hid/usbhid/usbkbd.c` and
deliberately skip HID report-descriptor parsing: they assume a fixed report
layout.

For an in-depth explanation of the code, see the [docs](docs/README.md).

---

## What each driver does

### Both drivers

- Bind through `usbcore` using a `usb_device_id` table built with
  `USB_DEVICE(VID, PID)`, exported with `MODULE_DEVICE_TABLE`.
- In `probe`, check that the interface's first endpoint is an interrupt IN
  endpoint, then allocate the driver state, an `input_dev`, a URB and a
  DMA-coherent receive buffer (`usb_alloc_coherent`, used with
  `URB_NO_TRANSFER_DMA_MAP`).
- Fill an interrupt URB with `usb_fill_int_urb` using the endpoint's
  `bInterval`, and submit it once at the end of `probe`.
- In the URB completion handler (atomic context), decode the report, emit input
  events, call `input_sync`, and resubmit the URB with `GFP_ATOMIC`.
- In `disconnect` (hot-unplug or driver unbind), stop the URBs with
  `usb_kill_urb`, unregister the input device, and free the URBs and DMA
  buffers.

### Mouse (`Mouse/Logitech_driver.c`)

- Sends the HID class request `SET_PROTOCOL` with value 1 (Report Protocol).
  A failure is logged as a warning and is not fatal.
- Decodes a fixed 4-byte layout: byte 0 = button bitmask (left, right,
  middle), byte 1 = X delta, byte 2 = Y delta, byte 3 = wheel delta, each
  delta a signed 8-bit value.
- Reports `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE`, `REL_X`, `REL_Y`,
  `REL_WHEEL`.
- Builds the input device name from the USB manufacturer and product strings
  and the physical path from `usb_make_path`, and parents the input device to
  the USB interface in sysfs.

### Keyboard (`Keyboard/my_keyboard.c`)

- Sends the HID class request `SET_PROTOCOL` with value 0 (Boot Protocol), so
  the keyboard sends the standard 8-byte boot report:
  `[modifiers][reserved][key1..key6]`.
- Maps the 8 modifier bits in byte 0 directly to `KEY_LEFTCTRL` ...
  `KEY_RIGHTMETA`.
- Translates HID usage IDs in bytes 2-7 to Linux keycodes with a lookup table,
  and generates press/release events by comparing the current 6-key array with
  the previous one.
- Registers the modifier keycodes explicitly. The lookup table only covers
  HID usages 0-175, so without this the modifier keys would not be advertised
  and the input core would silently drop their events.
- Drives the Num Lock, Caps Lock and Scroll Lock LEDs: an `EV_LED` event from
  the input core triggers the driver's `event` callback, which builds a 1-byte
  LED bitmap and sends it with an asynchronous HID `SET_REPORT` (Output report)
  control URB.
- Configures kernel software autorepeat with a 250 ms delay and 33 ms period
  via `input_enable_softrepeat()`.
- Resubmits the interrupt URB after transient USB errors, and unwinds every
  allocation in `probe` through a goto ladder on failure.

---

## Requirements

- Linux kernel 5.19 or newer (the code uses the two-argument
  `usb_maxpacket()`), with headers for the running kernel.
- The exact devices above, or edit the `*_VENDOR_ID` / `*_PRODUCT_ID`
  defines to match yours (find them with `lsusb`). Another keyboard that
  supports Boot Protocol will usually work; another mouse only works if its
  reports use the same 4-byte layout.
- If Secure Boot enforces module signing, unsigned modules will be rejected
  by `insmod`.

On Debian/Ubuntu:

```bash
sudo apt install build-essential linux-headers-$(uname -r)
```

---

## Build

There is no top-level Makefile. Each driver is built with its own kbuild
Makefile:

```bash
make -C Mouse
```

```bash
make -C Keyboard
```

This produces `Mouse/Logitech_driver.ko` and `Keyboard/my_keyboard.ko`.
`make -C Mouse clean` / `make -C Keyboard clean` remove the build output.

---

## Load

Linux binds HID devices to `usbhid` automatically, and a USB interface can
only be owned by one driver, so the interface has to be released first.

1. Find the interface name (format `bus-port:config.interface`, for example
   `1-2:1.0`):

   ```bash
   lsusb -t
   ```

   or look at `dmesg` right after plugging the device in.

2. Unbind it from `usbhid` (use your own interface name):

   ```bash
   echo -n "1-2:1.0" | sudo tee /sys/bus/usb/drivers/usbhid/unbind
   ```

   Unbinding your only keyboard leaves you without a keyboard until the custom
   driver is loaded. Keep a second input device, or SSH access, available.

3. Load the module. Registering the driver makes the USB core probe any
   unclaimed matching interface:

   ```bash
   sudo insmod Mouse/Logitech_driver.ko
   ```

   ```bash
   sudo insmod Keyboard/my_keyboard.ko
   ```

4. Verify:

   ```bash
   sudo dmesg | tail
   ```

   Expected lines:

   - `usbcore: registered new interface driver Logitech_Mouse_Driver`
   - `usbcore: registered new interface driver my_dell_kbd`
   - `Dell Keyboard Driver Loaded (Fully Functional).` (printed when the
     keyboard is probed)
   - `input: ... as /devices/.../input/inputN` for each new input device

   `lsusb -t` should now show `Driver=Logitech_Mouse_Driver` or
   `Driver=my_dell_kbd` on the interface, and `evtest` will list the new
   input devices.

## Unload

```bash
sudo rmmod Logitech_driver
```

```bash
sudo rmmod my_keyboard
```

Then give the interface back to `usbhid`:

```bash
echo -n "1-2:1.0" | sudo tee /sys/bus/usb/drivers/usbhid/bind
```

Unplugging and replugging the device will usually also hand it back to
`usbhid`, because `usbhid` is normally registered first and claims the new
interface before the custom driver is tried.

The full workflow, including debugging with `evtest` and `usbmon`, is in
[docs/06-build-load-debug.md](docs/06-build-load-debug.md).

---

## Project structure

```text
.
|-- Keyboard/
|   |-- Makefile          kbuild Makefile for my_keyboard.ko
|   `-- my_keyboard.c     Dell keyboard driver
|-- Mouse/
|   |-- Makefile          kbuild Makefile for Logitech_driver.ko
|   `-- Logitech_driver.c Logitech mouse driver
|-- docs/                 in-depth documentation
`-- README.md
```

## Documentation

| Document                                                          | Contents                                                       |
| ----------------------------------------------------------------- | -------------------------------------------------------------- |
| [docs/01-architecture.md](docs/01-architecture.md)                | Where the drivers sit in the kernel, end-to-end data flow      |
| [docs/02-usb-and-hid-primer.md](docs/02-usb-and-hid-primer.md)    | Descriptors, endpoints, URBs, DMA, HID class requests          |
| [docs/03-input-subsystem.md](docs/03-input-subsystem.md)          | `input_dev`, capability bits, events, LEDs, autorepeat         |
| [docs/04-mouse-driver.md](docs/04-mouse-driver.md)                | Walkthrough of `Logitech_driver.c`                             |
| [docs/05-keyboard-driver.md](docs/05-keyboard-driver.md)          | Walkthrough of `my_keyboard.c`                                 |
| [docs/06-build-load-debug.md](docs/06-build-load-debug.md)        | Building, binding, testing and debugging                       |

## License

GPL, as declared by `MODULE_LICENSE("GPL")` in both modules.
