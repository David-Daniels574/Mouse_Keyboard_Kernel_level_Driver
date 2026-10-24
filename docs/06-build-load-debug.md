# 6. Build, load and debug

## 6.1 Build system

Each directory has a minimal kbuild Makefile:

```make
obj-m += my_keyboard.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
```

- `obj-m += my_keyboard.o` tells kbuild to build `my_keyboard.c` as a
  loadable module named `my_keyboard.ko`.
- `make -C /lib/modules/$(uname -r)/build` runs the kernel's own build system
  from the headers of the running kernel. The module must be built against
  the exact kernel it will be loaded into.
- `M=$(PWD)` tells kbuild where the external module source is.

Build from the repository root:

```bash
make -C Mouse
```

```bash
make -C Keyboard
```

Build artifacts (`*.o`, `*.ko`, `*.mod`, `*.mod.c`, `Module.symvers`,
`modules.order`, `.*.cmd`) are listed in `.gitignore`.

Common build problems:

| Error                                                      | Cause                                                       |
| ---------------------------------------------------------- | ----------------------------------------------------------- |
| `/lib/modules/.../build: No such file or directory`        | kernel headers for the running kernel are not installed     |
| `too many arguments to function 'usb_maxpacket'`           | wrong number of arguments for the kernel version: the code needs 5.19 or newer |
| `insmod: ERROR: could not insert module ...: Key was rejected by service` | Secure Boot requires signed modules        |
| `Invalid module format`                                    | built against a different kernel than the one running       |

## 6.2 Identify the device and its interface

```bash
lsusb
```

```text
Bus 001 Device 004: ID 046d:c077 Logitech, Inc. ...
Bus 001 Device 005: ID 413c:2107 Dell Computer Corp. ...
```

```bash
lsusb -t
```

```text
/:  Bus 01.Port 1: Dev 1, Class=root_hub, Driver=xhci_hcd/12p, 480M
    |__ Port 2: Dev 4, If 0, Class=Human Interface Device, Driver=usbhid, 1.5M
    |__ Port 3: Dev 5, If 0, Class=Human Interface Device, Driver=usbhid, 1.5M
```

The interface name used in sysfs is `<bus>-<port path>:<config>.<interface>`.
For "Bus 01, Port 2, If 0" with configuration 1 that is `1-2:1.0`. Behind a
hub the port path has dots, for example `1-1.3:1.0`. You can confirm the name
with:

```bash
ls /sys/bus/usb/drivers/usbhid/
```

## 6.3 Hand the interface over

```bash
echo -n "1-2:1.0" | sudo tee /sys/bus/usb/drivers/usbhid/unbind
```

```bash
sudo insmod Mouse/Logitech_driver.ko
```

When `usb_register()` runs, the USB core walks all interfaces without a
driver and calls `probe` for those matching the ID table. If you load the
module before unbinding, nothing happens; you can bind afterwards instead:

```bash
echo -n "1-2:1.0" | sudo tee /sys/bus/usb/drivers/Logitech_Mouse_Driver/bind
```

The keyboard is the same with `my_keyboard.ko` and
`/sys/bus/usb/drivers/my_dell_kbd/`.

Safety note: unbinding the keyboard you are typing on disconnects it from the
system immediately. Run the unbind and `insmod` as one command line, or work
over SSH, or keep a second keyboard connected:

```bash
echo -n "1-3:1.0" | sudo tee /sys/bus/usb/drivers/usbhid/unbind && sudo insmod Keyboard/my_keyboard.ko
```

## 6.4 Verify

Kernel log:

```bash
sudo dmesg -w
```

| Message                                                         | Source                                         |
| --------------------------------------------------------------- | ---------------------------------------------- |
| `usbcore: registered new interface driver Logitech_Mouse_Driver` | `usb_register()` on `insmod`                  |
| `usbcore: registered new interface driver my_dell_kbd`          | `usb_register()` on `insmod`                   |
| `input: <name> as /devices/.../input/inputN`                    | `input_register_device()` in `probe`           |
| `Dell Keyboard Driver Loaded (Fully Functional).`               | end of `kbd_probe()`                           |
| `Failed to set Report Protocol (Err: ...)`                      | mouse `SET_PROTOCOL` failed (non-fatal)        |
| `can't resubmit intr, ...`                                      | mouse or keyboard URB resubmission failed      |
| `led urb failed: ...` / `usb_submit_urb(leds) failed`           | keyboard LED path errors                       |

Driver ownership:

```bash
lsusb -t
```

should show `Driver=Logitech_Mouse_Driver` / `Driver=my_dell_kbd`.

Input devices:

```bash
cat /proc/bus/input/devices
```

Look for `N: Name="My Dell Keyboard (Shortcuts Fixed)"` and the mouse's
manufacturer/product name, with `H: Handlers=... eventN`.

Events:

```bash
sudo evtest
```

Pick the device. Expected output for a left click on the mouse:

```text
Event: time ..., type 1 (EV_KEY), code 272 (BTN_LEFT), value 1
Event: time ..., -------------- SYN_REPORT ------------
Event: time ..., type 1 (EV_KEY), code 272 (BTN_LEFT), value 0
Event: time ..., -------------- SYN_REPORT ------------
```

and for Ctrl+C on the keyboard:

```text
type 1 (EV_KEY), code 29 (KEY_LEFTCTRL), value 1
-------------- SYN_REPORT ------------
type 1 (EV_KEY), code 46 (KEY_C), value 1
-------------- SYN_REPORT ------------
type 1 (EV_KEY), code 46 (KEY_C), value 0
-------------- SYN_REPORT ------------
type 1 (EV_KEY), code 29 (KEY_LEFTCTRL), value 0
-------------- SYN_REPORT ------------
```

LED test: toggle Caps Lock and check that the light follows. You can also
drive an LED directly, without a key press, with a tool that writes `EV_LED`
to the event node (for example `evemu-event /dev/input/eventN --type EV_LED --code LED_CAPSL --value 1`
from the `evemu` package).

## 6.5 Watching the USB traffic with usbmon

`usbmon` shows every URB the kernel sends and receives, which lets you see the
raw reports and the control requests the drivers issue.

```bash
sudo modprobe usbmon
```

```bash
sudo cat /sys/kernel/debug/usb/usbmon/1u
```

(`1` is the bus number from `lsusb`.) Wireshark can also capture the
`usbmon1` interface and decode HID traffic. Things to look for:

- On probe: a control `SET_PROTOCOL` (`21 0b 0001 ...` for the mouse,
  `21 0b 0000 ...` for the keyboard).
- While typing: interrupt IN completions with 8 data bytes.
- On Caps Lock: a control `21 09 0200 0000 0001` with one data byte.

## 6.6 Unload and restore

```bash
sudo rmmod my_keyboard
```

```bash
sudo rmmod Logitech_driver
```

`rmmod` deregisters the driver, which calls `disconnect` for each bound
interface. The interface is then unbound; give it back to the stock driver:

```bash
echo -n "1-2:1.0" | sudo tee /sys/bus/usb/drivers/usbhid/bind
```

Replugging the device also works in practice, because `usbhid` is normally
registered before the custom driver and is offered the new interface first.
For the same reason, replugging while the custom module is loaded usually
gives the device back to `usbhid` rather than to the custom driver.

## 6.7 Debugging tips

- **Nothing happens on insmod.** The interface is still owned by `usbhid`
  (check `lsusb -t`), or the VID/PID does not match, or `probe` rejected the
  endpoint layout.
- **Keys type but shortcuts do not work.** A keycode is not advertised in
  `keybit` (this is the bug the explicit modifier registration fixes). Compare
  the `B: KEY=` line in `/proc/bus/input/devices` with what you report.
- **Mouse moves in the wrong direction or jumps.** The report layout is not
  what the driver assumes, or the signed casts are missing. Capture the raw
  reports with `usbmon` (6.5) while moving the mouse in one direction at a
  time.
- **Input stops after a while.** Check `dmesg` for `can't resubmit intr`.
  Both drivers resubmit after transient errors, so polling only stops when
  resubmission itself fails (usually because the device went away).
- **Kernel oops.** A use-after-free in `disconnect` or an unchecked NULL in
  `probe` is the usual cause. Test hot-unplug while moving the mouse or
  holding a key, and test `rmmod` with the device in use.
- **Dynamic debug.** For temporary tracing inside a driver, use `dev_dbg()`
  rather than `printk(KERN_INFO ...)`. The messages are compiled in but off by
  default and can be enabled at runtime:

  ```bash
  echo 'module Logitech_driver +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
  ```
