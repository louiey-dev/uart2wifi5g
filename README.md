# UART to Wi-Fi Bridge (ESP32-C5)

A high-performance transparent serial bridge implemented on the **ESP32-C5** RISC-V SoC using ESP-IDF (v5.5). This firmware bridges an external hardware UART serial device (such as an Orange Pi, Raspberry Pi, or embedded Linux console) to Wi-Fi via **UDP** and **TCP** using 5 GHz or 2.4 GHz Wi-Fi 6 SoftAP.

---

## Key Features

- **Dual-Band SoftAP (2.4 GHz & 5 GHz Wi-Fi 6):**
  - Configurable band mode: 5 GHz Only (`WIFI_BAND_MODE_5G_ONLY`, Channel 36 by default), 2.4 GHz Only, or Dual-band Auto.
  - High throughput, low latency, and low 2.4 GHz congestion resistance.
  - Built-in DHCP server leasing IP `192.168.4.2` to the primary connected station.
- **Dual Network Terminal Bridges (Port 3333):**
  - **UDP Bridge:** Transparent bidirectional forwarding between UDP clients and external UART. Automatically tracks active client endpoints to prevent duplicate broadcast packets.
  - **TCP Server:** Built-in Telnet/Raw TCP socket server listening on `192.168.4.1:3333`. Allows direct interactive serial terminal sessions using **Tera Term**, **PuTTY**, or `nc`.
- **Robust UART Driver (UART1):**
  - Event-driven FreeRTOS task handling hardware FIFO and ring buffer data flow.
  - Configured with dedicated digital General-Purpose I/O pins (**GPIO 8** for TX, **GPIO 9** for RX) to avoid strapping/LP conflicts.
  - Hardware drive strength maximized (`GPIO_DRIVE_CAP_3`), internal pull-ups enabled, and synchronous TX flush.
  - Real-time hexadecimal dump logging for protocol diagnosis.

---

## Hardware Pinout & Wiring

### ESP32-C5-DevKitC-1 to Orange Pi Zero 2W (UART0 Console)

Connect the ESP32-C5 J1 header pins to the Orange Pi Zero 2W 40-pin GPIO header:

| Signal Direction | ESP32-C5 DevKit (J1 Header) | Orange Pi Zero 2W (40-Pin Header) | Function Description |
| :--- | :--- | :--- | :--- |
| **ESP32 TX $\rightarrow$ Orange Pi RX** | **J1 Pin 9 (`GPIO8`)** | **Pin 10 (`UART0_RX`)** | Serial data to Orange Pi console |
| **Orange Pi TX $\rightarrow$ ESP32 RX** | **J1 Pin 10 (`GPIO9`)** | **Pin 8 (`UART0_TX`)** | Serial data from Orange Pi console |
| **Common Ground** | **J1 Pin 16 (`GND`)** | **Pin 6 (`GND`)** | Shared electrical reference |

> [!NOTE]
> `GPIO8` and `GPIO9` are dedicated digital pins without strapping or Low-Power (LP) multiplexing, ensuring stable 3.3V logic levels.

---

## Project Configuration

Access configuration settings via `idf.py menuconfig`:

```bash
idf.py menuconfig
```

### 1. Wi-Fi SoftAP Settings (`Component config -> Wi-Fi SoftAP Configuration`)

- **Wi-Fi SSID / Password:** SoftAP credentials (default: `myssid` / `mypassword`).
- **Wi-Fi Channel:** SoftAP channel (default: `36` for 5 GHz band).
- **Wi-Fi Band:**
  - `5 GHz Only` (default, requires ESP32-C5 and 5 GHz-capable PC)
  - `2.4 GHz Only`
  - `Dual-band Auto`
- **Max Station Connections:** Maximum allowed connected clients (default: `4`).

### 2. External UART Settings (`Component config -> External UART Configuration`)

- **UART Port Number:** `1` (UART1)
- **UART TX Pin:** `8` (J1 Pin 9)
- **UART RX Pin:** `9` (J1 Pin 10)
- **Baud Rate:** `115200` (8 data bits, 1 stop bit, no parity)
- **RX Buffer Size:** `1024` bytes

### 3. UDP Settings (`Component config -> UDP Configuration`)

- **Destination IP:** `192.168.4.2` (or broadcast fallback `192.168.4.255`)
- **Port:** `3333`

---

## Build & Flash

### Prerequisites

- ESP-IDF v5.5 or later installed and exported in your shell:
  
  ```bash
  . $HOME/esp/v5.5/esp-idf/export.sh
  ```

### Build

```bash
idf.py build
```

### Flash & Monitor

```bash
idf.py flash monitor
```

To exit the serial monitor, press `Ctrl + ]`.

---

## Usage Guide

### Method 1: Interactive Terminal via TCP (Tera Term / PuTTY)

1. Connect your PC to the ESP32 Wi-Fi SoftAP:
   - **SSID:** `myssid`
   - **Password:** `mypassword`
2. Open **Tera Term** on your PC:
   - Select **TCP/IP**
   - **Host:** `192.168.4.1`
   - **Service:** `Other`, **TCP Port#:** `3333`
3. Press **Enter** or type commands (`ls -al`, `top`, `htop`). Keystrokes are immediately sent to the target device, and responses stream directly to your terminal window.

### Method 2: Network Datagram via UDP (Port 3333)

- **Send Command:** Send UDP datagrams to `192.168.4.1:3333`.
  
  ```bash
  # Using Netcat from Linux / macOS
  echo -e "ls -al\r\n" | nc -u -w1 192.168.4.1 3333
  ```
  
- **Receive Output:** Listen on local port `3333`. Incoming serial bytes from the external device are packed into UDP packets and unicast directly to the active client.

---

## Diagnostic Tips

1. **Verify ESP32 UART Hardware (Self-Loopback):**
   - Disconnect the external device.
   - Bridge **J1 Pin 9 (`GPIO8`)** directly to **J1 Pin 10 (`GPIO9`)** using a single jumper wire.
   - Send any packet over UDP or TCP; verify that the ESP32 monitor prints matching `UART1 RX` logs.
2. **Hex Debug Logs:**
   - All received UDP packets and UART TX/RX bytes are dumped in hexadecimal format (`ESP_LOG_BUFFER_HEX_LEVEL`) on the ESP32 console for instant protocol verification.
