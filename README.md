🖱️⌨️ Custom Linux USB HID Drivers (Kernel Modules)

This project contains **two functional Linux Kernel Modules** that act as custom device drivers for:

1. **Logitech Optical Mouse** (`Logitech_driver.ko`)
2. **Dell USB Keyboard** (`my_keyboard.ko`)

These modules demonstrate how to **interface directly with the Linux USB subsystem**, **manually parse raw HID packets**, and **register Linux input events** — all **without relying on the standard `usbhid` driver**.

> **Educational / Low-Level Systems Project**  
> These drivers intentionally bypass standard HID abstractions to expose how USB devices actually communicate with the kernel via **Interrupt URBs**, **DMA buffers**, and **input subsystem hooks**.

---

## 🚀 Features

### 🔌 Core USB Integration (Both Drivers)

- **Direct USB Core Binding**  
  Registers directly with the Linux USB Core using Vendor/Product IDs.

- **DMA-Capable Buffers**  
  Uses `usb_alloc_coherent()` for efficient and safe DMA transfers.

- **Interrupt Transfers**  
  Submits and manages USB Interrupt URBs for real-time input polling.

- **Hot-Plug Support**  
  Correctly handles device insertion and removal.

---

### 🖱️ Mouse Driver — Logitech Optical Mouse

- **Manual HID Parsing**  
  Decodes raw USB byte streams for:
  - X/Y movement
  - Scroll wheel
  - Button states

- **Full Input Support**
  - ✅ Left, Right, Middle click
  - ✅ Smooth pointer movement
  - ✅ Scroll wheel support

- **Protocol Management**
  - Forces **HID Report Protocol (Protocol 1)** for extended functionality

---

### ⌨️ Keyboard Driver — Dell USB Keyboard

- **Scancode Mapping**
  - Maps USB HID usage codes to Linux input event keycodes

- **Modifier Key Handling**
  - Full support for **Ctrl, Shift, Alt, Meta (Windows)** keys using bitwise masks

- **Bi-Directional Communication**
  - Sends control messages to toggle:
    - Caps Lock
    - Num Lock
    - Scroll Lock LEDs

- **Optimized Typing Feel**
  - Tuned **key repeat rate** and **delay**
  - Smooth text selection and navigation

---

## 📦 Prerequisites

-- Install required build tools and kernel headers:

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

---

## ⚙️ Configuration & Hardware IDs

-- These drivers are **hardcoded for specific USB devices**.
-- You may need to modify the `VENDOR_ID` and `PRODUCT_ID` definitions in the source files to match your hardware.

| Device         | Vendor ID | Product ID |
| -------------- | --------- | ---------- |
| Logitech Mouse | `0x046d`  | `0xc077`   |
| Dell Keyboard  | `0x413c`  | `0x2107`   |

-- Use the following command to find your device IDs:

```bash
lsusb
```

---

## 🚀 Installation & Usage

### -- Step 1: Build the Modules

-- Compile both drivers using their respective Makefiles:

```bash
make
```

-- Output:

-- `Mouse/Logitech_driver.ko`
-- `Keyboard/my_keyboard.ko`

---

### -- Step 2: Unbind the Default `usbhid` Driver

-- Linux automatically binds USB HID devices to the generic `usbhid` driver.
-- You must detach it before loading your custom drivers.

#### -- Option A: Temporary Unbind

-- Replace the interface ID with your device’s interface path
-- (find it via `dmesg` or `lsusb -t`).

```bash
# Example for Mouse
sudo sh -c 'echo "1-2:1.0" > /sys/bus/usb/drivers/usbhid/unbind'

# Example for Keyboard
sudo sh -c 'echo "1-3:1.0" > /sys/bus/usb/drivers/usbhid/unbind'
```

---

### -- Step 3: Load the Custom Drivers

-- Insert the kernel modules:

```bash
# Load Mouse Driver
sudo insmod Mouse/Logitech_driver.ko

# Load Keyboard Driver
sudo insmod Keyboard/my_keyboard.ko
```

---

### -- Step 4: Verify

-- Check kernel logs:

```bash
sudo dmesg | tail
```

-- Expected Output:

-- `Dell Keyboard Driver Loaded (Fully Functional).`
-- `Logitech Mouse Driver Initialized.`

---

## 📂 Project Structure

```text
├── Keyboard
│   ├── Makefile
│   ├── Module.symvers
│   ├── modules.order
│   ├── my_keyboard.c
│   ├── my_keyboard.ko
│   ├── my_keyboard.mod
│   ├── my_keyboard.mod.c
│   ├── my_keyboard.mod.o
│   └── my_keyboard.o
│
├── Mouse
│   ├── Makefile
│   ├── Module.symvers
│   ├── modules.order
│   ├── Logitech_driver.c
│   ├── Logitech_driver.ko
│   ├── Logitech_driver.mod
│   ├── Logitech_driver.mod.c
│   ├── Logitech_driver.mod.o
│   └── Logitech_driver.o
│
└── README.md
```

---

## 📜 License

-- This project is licensed under the **GNU General Public License (GPL)**.
-- It links against and interfaces directly with the **Linux Kernel**.
