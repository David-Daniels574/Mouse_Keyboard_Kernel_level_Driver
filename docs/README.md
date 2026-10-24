# Documentation

These documents explain the two drivers in this repository in depth: the
kernel subsystems they rely on, what every function does, and why it is written
the way it is.

Read them in order if you are new to USB or kernel drivers. If you already
know the background, jump straight to the walkthroughs (04 and 05).

1. [Architecture](01-architecture.md)
   Where the drivers sit in the kernel, how a device gets bound to them, and
   the end-to-end path from a key press to a user-space event.
2. [USB and HID primer](02-usb-and-hid-primer.md)
   Devices, configurations, interfaces and endpoints; interrupt and control
   transfers; URBs and their lifecycle; DMA-coherent buffers; HID class
   requests and Boot vs Report Protocol.
3. [Input subsystem](03-input-subsystem.md)
   `struct input_dev`, capability bitmaps, reporting events, `input_sync`,
   the LED path back to the driver, and kernel autorepeat.
4. [Mouse driver walkthrough](04-mouse-driver.md)
   `Mouse/Logitech_driver.c`, function by function.
5. [Keyboard driver walkthrough](05-keyboard-driver.md)
   `Keyboard/my_keyboard.c`, function by function, including the keycode
   table, the press/release diff and the LED control URB.
6. [Build, load and debug](06-build-load-debug.md)
   kbuild, unbinding `usbhid`, loading, testing with `evtest`, tracing with
   `usbmon`, and recovering when something goes wrong.

## Conventions

- Source links point at line numbers, for example
  [my_keyboard.c#L74](../Keyboard/my_keyboard.c#L74). They are correct for the
  code as of this documentation; if the code changes, search for the function
  name instead.
- "Upstream" means the Linux kernel source tree. The two reference drivers
  are `drivers/hid/usbhid/usbmouse.c` and `drivers/hid/usbhid/usbkbd.c`.
- Hex values are written as in the code (`0x413c`). HID usage IDs are written
  as hex (`0x04` = the "a" key).
