# 3. The Linux input subsystem

The input subsystem is the layer that turns "a device did something" into the
uniform event stream that user space reads from `/dev/input/eventN`. Both
drivers are *input device drivers*: they create a `struct input_dev`, declare
what it can do, and push events into it.

## 3.1 Lifecycle of an input device

```c
struct input_dev *dev = input_allocate_device();   /* refcounted, zeroed */

dev->name = "...";            /* shown in /proc/bus/input/devices, evtest */
dev->phys = "...";            /* physical path, should be unique */
dev->id.bustype = BUS_USB;    /* plus vendor, product, version */
dev->dev.parent = &intf->dev; /* place it under the USB interface in sysfs */

/* declare capabilities (see 3.2) */

input_register_device(dev);   /* now visible to handlers and user space */
...
input_unregister_device(dev); /* also drops the last reference: do NOT free afterwards */
```

Error-handling rules that the drivers follow:

- Before `input_register_device()` succeeds, release with
  `input_free_device()`.
- After it succeeds, release only with `input_unregister_device()`. Calling
  `input_free_device()` afterwards is a double free. This is why the mouse
  driver's error path sets `input_dev = NULL` after unregistering, making the
  later `input_free_device(input_dev)` a no-op.

If `dev.parent` is not set, the input device is created under
`/sys/devices/virtual/input/` instead of under the USB device. The mouse
driver sets it; the keyboard driver does not.

## 3.2 Capability bitmaps

An input device advertises what it can produce through bitmaps in
`struct input_dev`. The input core **silently discards** any event whose type
or code is not advertised. This is the single most important rule for
understanding the keyboard driver's "CRITICAL FIX" comment.

| Bitmap      | Indexed by        | Mouse sets                             | Keyboard sets                          |
| ----------- | ----------------- | -------------------------------------- | -------------------------------------- |
| `evbit`     | event type `EV_*` | `EV_KEY`, `EV_REL`                     | `EV_KEY`, `EV_LED`, `EV_REP`           |
| `keybit`    | `KEY_*` / `BTN_*` | `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE`  | every keycode in the table + 8 modifiers |
| `relbit`    | `REL_*`           | `REL_X`, `REL_Y`, `REL_WHEEL`          | -                                      |
| `ledbit`    | `LED_*`           | -                                      | `LED_NUML`, `LED_CAPSL`, `LED_SCROLLL` |

Bitmaps are arrays of `unsigned long`. The code writes them two ways:

- `dev->evbit[0] = BIT_MASK(EV_KEY) | ...` assigns a whole word directly. This
  only works when all bits are in word 0, which is true for small enums like
  `EV_*`, `REL_*`, `LED_*`.
- `dev->keybit[BIT_WORD(BTN_MOUSE)] = BIT_MASK(BTN_LEFT) | ...` is the same
  idea for keycodes that live in a later word. `BTN_LEFT`, `BTN_RIGHT` and
  `BTN_MIDDLE` (0x110-0x112) are all in the same word as `BTN_MOUSE` (0x110).
- `set_bit(code, dev->keybit)` sets a single bit anywhere, used by the
  keyboard for its large, sparse set of keycodes.

## 3.3 Reporting events

```c
input_report_key(dev, code, value);  /* EV_KEY: 1 = press, 0 = release */
input_report_rel(dev, code, delta);  /* EV_REL: signed relative motion */
input_sync(dev);                     /* EV_SYN/SYN_REPORT: end of one atomic frame */
```

Details that matter for these drivers:

- `input_report_key()` normalises its value with `!!value`, so the mouse can
  pass `data[0] & 0x02` (which is 0 or 2) directly. The keyboard's explicit
  `!!(...)` is harmless but redundant.
- For `EV_KEY`, the input core keeps the current state of every key in
  `dev->key` and **only forwards changes**. The keyboard driver reports all 8
  modifier states on every report; the core turns that into events only when
  a modifier actually changes.
- For `EV_REL`, a value of 0 is dropped, so reporting zero motion costs
  nothing.
- `input_sync()` tells consumers that the preceding events belong together
  (for example X and Y of the same movement, or a release and a press in the
  same keyboard report). Both drivers call it exactly once per URB
  completion.

Events go from the input core to every connected *handler*. The two that
matter here are:

- **evdev**: exposes `/dev/input/eventN`; read by libinput, X.org, `evtest`.
- **kbd** (`drivers/tty/vt/keyboard.c`): drives the text console (VT).

## 3.4 The LED path: events flowing back into the driver

LEDs work in the opposite direction from key presses. Something above the
driver decides Caps Lock is on, and the driver must tell the hardware:

1. The VT keyboard handler, or a user-space program writing an `EV_LED` event
   to the evdev node (libinput-based compositors and X.org do this), injects
   `EV_LED, LED_CAPSL, 1` into the device.
2. The input core checks `ledbit`, updates the `dev->led` bitmap, and, because
   the device has an `event` callback, calls
   `dev->event(dev, EV_LED, LED_CAPSL, 1)`.
3. The keyboard driver's `kbd_event()` reads the *whole* current LED state
   from `dev->led` with `test_bit()`, packs it into the 1-byte HID output
   report and submits the LED control URB.

`dev->event` is called with the device's `event_lock` spinlock held and
interrupts disabled, so it must not sleep. That is why the driver submits an
asynchronous URB with `GFP_ATOMIC` instead of calling `usb_control_msg()`.

The core also calls `dev->event` for other event types it passes to the
device (for example `EV_REP` updates). `kbd_event()` returns `-1` for
anything that is not `EV_LED`; the input core ignores the return value.

## 3.5 Autorepeat

When a key is held down, something has to generate repeated key events. In
Linux this can happen in two places:

- **In the kernel (software autorepeat).** If `EV_REP` is set in `evbit`,
  `input_register_device()` checks `rep[REP_DELAY]` and `rep[REP_PERIOD]`.
  If both are 0 it calls `input_enable_softrepeat(dev, 250, 33)`, which arms a
  timer in the input core that emits `EV_KEY` events with value 2 (repeat)
  after 250 ms and then every 33 ms. If the driver has already set either
  value, the core assumes **the driver implements repeat itself** and does
  not arm the timer. The relevant comment in `drivers/input/input.c` reads:
  "If delay and period are pre-set by the driver, then autorepeating is
  handled by the driver itself and we don't do it in input.c."
- **In user space.** libinput-based Wayland compositors and X.org generate
  repeat themselves from the press/release stream and their own settings,
  and ignore kernel repeat events.

The keyboard driver calls `input_enable_softrepeat(input_dev, 250, 33)`
before registering. That sets both `rep[]` values *and* installs the core's
repeat timer, so the input core generates repeat events for held keys. (Simply
assigning `rep[REP_DELAY]` and `rep[REP_PERIOD]` would have the opposite
effect: the core would assume the driver repeats keys itself and leave its
timer off.)
In a graphical session the compositor's own repeat settings apply instead.

## 3.6 Inspecting input devices

```bash
cat /proc/bus/input/devices
```

shows every input device with its name, phys, sysfs path, handlers
(`event5`, `kbd`, ...) and capability bitmaps (`B: EV=...`, `B: KEY=...`).

```bash
sudo evtest
```

lists the devices, lets you pick one and prints every event with its type,
code and value, including `SYN_REPORT` boundaries. This is the best way to
check that the drivers emit what you expect.
