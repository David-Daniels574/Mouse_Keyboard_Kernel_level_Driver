# 2. USB and HID primer

This chapter covers only the parts of USB and HID that the two drivers use.

## 2.1 Device topology: device, configuration, interface, endpoint

A USB device describes itself with a tree of descriptors:

```text
 usb_device                    struct usb_device           (one per physical device)
  `-- configuration            usually exactly one is active
       `-- interface           struct usb_interface        (what a driver binds to)
            `-- altsetting     struct usb_host_interface   (intf->cur_altsetting)
                 `-- endpoint  struct usb_endpoint_descriptor
```

- **Device descriptor**: vendor ID (`idVendor`), product ID (`idProduct`),
  device release (`bcdDevice`), string indexes for manufacturer and product.
  The mouse driver copies these into `input_dev->id`. They are stored
  little-endian, hence `le16_to_cpu()`.
- **Interface**: a function of the device. Simple mice and keyboards have one
  HID interface. `bInterfaceNumber` identifies it and is used as `wIndex` in
  HID class requests.
- **Endpoint**: a one-directional data pipe. Each descriptor has an address
  (`bEndpointAddress`, bit 7 = direction, 1 = IN = device to host), a transfer
  type (`bmAttributes`), a maximum packet size (`wMaxPacketSize`) and, for
  interrupt endpoints, a polling interval (`bInterval`).
- **Endpoint 0** is the default control pipe. Every device has it and it is
  not listed in the interface descriptor. Both drivers use it for HID class
  requests.

`intf->cur_altsetting->desc.bNumEndpoints` is the number of endpoints in the
current alternate setting, excluding endpoint 0. The mouse requires exactly 1,
the keyboard at least 1, and both require `endpoint[0]` to be interrupt IN
(`usb_endpoint_is_int_in()`).

Inspect the real descriptors with:

```bash
lsusb -v -d 046d:c077
```

## 2.2 Transfer types used here

| Type      | Used for                                     | Characteristics                                                          |
| --------- | -------------------------------------------- | ------------------------------------------------------------------------ |
| Interrupt | input reports (mouse movement, key state)    | host polls every `bInterval`; guaranteed latency; small packets          |
| Control   | HID class requests (`SET_PROTOCOL`, `SET_REPORT`) | 8-byte setup packet + optional data stage; always on endpoint 0     |

"Interrupt" is a misnomer: the device never interrupts the host. The host
controller polls the endpoint on a schedule. If the device has nothing new, it
answers NAK and the URB stays pending; the driver's completion handler only
runs when the device actually sends data.

For low- and full-speed devices (all ordinary mice and keyboards),
`bInterval` is in milliseconds (frames). For high-speed and faster it is an
exponent (2^(bInterval-1) microframes). `usb_fill_int_urb()` performs that
conversion, which is why the drivers just pass `bInterval` through.

## 2.3 Pipes

A *pipe* is an encoded integer that combines device address, endpoint number,
direction and transfer type. The drivers build them with helper macros:

| Macro                                    | Meaning                                  |
| ---------------------------------------- | ---------------------------------------- |
| `usb_rcvintpipe(udev, ep->bEndpointAddress)` | interrupt IN pipe for the input endpoint |
| `usb_sndctrlpipe(udev, 0)`               | control OUT pipe on endpoint 0           |

`usb_maxpacket(udev, pipe)` returns the endpoint's `wMaxPacketSize`. Since
Linux 5.19 it takes two arguments; older kernels took a third `is_out`
argument, so this code does not build on kernels older than 5.19.

## 2.4 URBs (USB Request Blocks)

A URB is the kernel's asynchronous I/O request for USB. The lifecycle the
drivers use:

```text
 usb_alloc_urb(0, GFP_KERNEL)         allocate (0 = no isochronous packets)
 usb_fill_int_urb(...)                describe the transfer
   or usb_fill_control_urb(...)
 usb_submit_urb(urb, mem_flags)       hand it to the host controller; returns at once
         ...
 urb->complete(urb)                   called once, in atomic context, when done
   urb->status                        0 = success, negative errno otherwise
   urb->actual_length                 bytes actually transferred
 usb_submit_urb(urb, GFP_ATOMIC)      optionally resubmit from the handler
         ...
 usb_kill_urb(urb)                    cancel and wait for completion (may sleep)
 usb_free_urb(urb)                    drop the reference
```

Important `urb->status` values in the completion handlers:

| Status         | Meaning                                                      | What the drivers do                         |
| -------------- | ------------------------------------------------------------ | ------------------------------------------- |
| `0`            | transfer completed                                           | decode and resubmit                         |
| `-ENOENT`      | killed by `usb_kill_urb()`                                   | return, do not resubmit                     |
| `-ECONNRESET`  | unlinked asynchronously by `usb_unlink_urb()`                | return, do not resubmit                     |
| `-ESHUTDOWN`   | device removed or host controller shut down                  | return, do not resubmit                     |
| other (`-EPROTO`, `-EILSEQ`, `-EOVERFLOW`, ...) | transient bus/protocol error  | skip the report, resubmit                   |

`usb_kill_urb()` also marks the URB so that any attempt to resubmit it while
the kill is in progress fails with `-EPERM`. That is what makes it safe for
the completion handler to resubmit unconditionally on success: a concurrent
`disconnect` cannot be defeated by the resubmission.

`mem_flags` on `usb_submit_urb()` says how the host controller driver may
allocate memory for its own bookkeeping. In `probe` (process context) the
drivers pass `GFP_KERNEL`. In completion handlers and in `kbd_event` they
must pass `GFP_ATOMIC`, because sleeping is not allowed there.

## 2.5 DMA-coherent buffers

The host controller reads and writes transfer data via DMA. By default the USB
core maps `urb->transfer_buffer` for DMA on every submission and unmaps it on
completion. For a URB that is resubmitted hundreds of times per second, that
is wasted work, so both drivers use the pattern from the upstream drivers:

```c
buf = usb_alloc_coherent(udev, size, GFP_KERNEL, &buf_dma);  /* CPU and DMA address */
urb->transfer_buffer = buf;            /* set by usb_fill_*_urb */
urb->transfer_dma    = buf_dma;
urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;   /* "already mapped, use transfer_dma" */
...
usb_free_coherent(udev, size, buf, buf_dma);
```

The buffer is allocated once, stays mapped for the lifetime of the device, and
is cache-coherent, so the CPU can read the bytes in the completion handler
without explicit sync calls.

DMA buffers must not live on the stack or inside a struct that shares cache
lines with other fields that the CPU writes concurrently. Allocating them
separately with `usb_alloc_coherent` avoids both problems; this is also why
`old_keys[6]` in the keyboard struct is a plain array (it is never DMA'd)
while `data` and `leds` are separate allocations.

## 2.6 Control transfers and the setup packet

Every control transfer starts with an 8-byte setup packet,
`struct usb_ctrlrequest`:

| Field          | Size | Meaning                                                                 |
| -------------- | ---- | ----------------------------------------------------------------------- |
| `bRequestType` | 1    | direction (bit 7), type (standard/class/vendor), recipient (device/interface/endpoint) |
| `bRequest`     | 1    | request code                                                            |
| `wValue`       | 2    | request-specific                                                        |
| `wIndex`       | 2    | request-specific, usually interface or endpoint number                  |
| `wLength`      | 2    | bytes in the data stage                                                 |

The 16-bit fields are little-endian on the wire, hence `cpu_to_le16()` when
the keyboard driver fills the struct by hand.

There are two ways to send one:

- `usb_control_msg(...)`: synchronous, sleeps until done or timed out. The
  drivers use it in `probe` for `SET_PROTOCOL`, with `USB_CTRL_SET_TIMEOUT`
  (5000 ms). It builds the setup packet internally.
- `usb_fill_control_urb(...)` + `usb_submit_urb(...)`: asynchronous. The
  keyboard uses this for LEDs because `kbd_event` runs in atomic context and
  cannot sleep. The driver owns the setup packet buffer.

## 2.7 HID class requests

HID defines class-specific requests, sent with
`bRequestType = USB_TYPE_CLASS | USB_RECIP_INTERFACE` (host-to-device, 0x21)
and `wIndex = bInterfaceNumber`. Constants are in `<linux/hid.h>`:

| Request         | Code | `wValue`                                  | Used by                                |
| --------------- | ---- | ----------------------------------------- | -------------------------------------- |
| `SET_REPORT`    | 0x09 | high byte report type, low byte report ID | keyboard LEDs (type 2 = Output, ID 0 -> `0x0200`) |
| `SET_IDLE`      | 0x0A | idle rate / report ID                     | not used                               |
| `SET_PROTOCOL`  | 0x0B | 0 = Boot, 1 = Report                      | keyboard (0), mouse (1)                |

The mouse uses the named constant `HID_REQ_SET_PROTOCOL`; the keyboard uses
the raw values `0x0B` and `0x09`.

## 2.8 Boot Protocol vs Report Protocol

HID devices normally run in **Report Protocol**: the layout of each report is
defined by the device's report descriptor, and the host must parse that
descriptor to decode reports. Report layouts can include report IDs, 12- or
16-bit axes, extra buttons, horizontal scroll, consumer keys and so on.

Devices in the *boot interface subclass* (`bInterfaceSubClass == 1`) also
support **Boot Protocol**, a fixed layout defined in the HID specification so
that a BIOS can use a keyboard and mouse without a descriptor parser.

Boot keyboard input report (8 bytes), used by the keyboard driver:

| Byte | Content                                                              |
| ---- | -------------------------------------------------------------------- |
| 0    | modifier bitmask (see below)                                         |
| 1    | reserved (ignored)                                                   |
| 2-7  | up to 6 HID usage IDs of currently pressed non-modifier keys, 0 = empty |

Modifier bits in byte 0:

| Bit | Mask | Key           | Linux keycode     |
| --- | ---- | ------------- | ----------------- |
| 0   | 0x01 | Left Ctrl     | `KEY_LEFTCTRL`    |
| 1   | 0x02 | Left Shift    | `KEY_LEFTSHIFT`   |
| 2   | 0x04 | Left Alt      | `KEY_LEFTALT`     |
| 3   | 0x08 | Left GUI      | `KEY_LEFTMETA`    |
| 4   | 0x10 | Right Ctrl    | `KEY_RIGHTCTRL`   |
| 5   | 0x20 | Right Shift   | `KEY_RIGHTSHIFT`  |
| 6   | 0x40 | Right Alt     | `KEY_RIGHTALT`    |
| 7   | 0x80 | Right GUI     | `KEY_RIGHTMETA`   |

Special usage IDs in the key array: `0x00` = no key, `0x01` = ErrorRollOver
(more keys pressed than the keyboard can report; the whole array is filled
with 0x01), `0x02` = POSTFail, `0x03` = ErrorUndefined. Real keys start at
`0x04` ("a"). This is why the keyboard driver ignores values `<= 3`.

Boot keyboard output report (1 byte), used for the LEDs:

| Bit | LED         |
| --- | ----------- |
| 0   | Num Lock    |
| 1   | Caps Lock   |
| 2   | Scroll Lock |
| 3   | Compose     |
| 4   | Kana        |

The boot mouse report is 3 bytes: buttons, X, Y (no wheel). The mouse driver
deliberately does *not* use Boot Protocol, because that would lose the wheel.
It requests Report Protocol (which is also the state a device is in after
reset) and relies on the target mouse sending `[buttons][X][Y][wheel]` with no
report ID. You can confirm that layout on a real device by capturing its
interrupt IN traffic with `usbmon` (see
[06-build-load-debug.md](06-build-load-debug.md#65-watching-the-usb-traffic-with-usbmon)).
