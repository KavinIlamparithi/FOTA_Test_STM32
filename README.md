# STM32 Dual-Slot (A/B) Wireless OTA Update System

This repository implements a robust, double-buffered **Over-The-Air (OTA) firmware update system** for the **STM32L476RG** (Nucleo-64) microcontroller. The wireless connection is managed by an **ESP8266 (D1 Mini)** acting as a Wi-Fi modem interfacing via UART AT commands.

---

## 🛠️ System Topology

The diagram below details the data flow between the central firmware server, the ESP8266 Wi-Fi bridge, and the STM32L476RG host microcontroller:

```
  +----------------------------------+
  |        Development PC /          |
  |       Static HTTP Server         |
  |      (Python: ota_server.py)     |
  +-----------------+----------------+
                    |
                    | Wi-Fi (HTTP GET /fw_slotX.bin)
                    v
  +-----------------+----------------+
  |        ESP8266 Module            |
  |        (D1 Mini NodeMCU)         |
  +-----------------+----------------+
                    |
                    | UART      (115200 Baud)
                    | CN5(D8)   (TX)      -->   RX
                    | CN9(D2)   (RX)     <--    TX
                    v
  +-----------------+----------------+
  |     STM32L476RG Microcontroller  |
  |      (Active application slot)   |
  +----------------------------------+
```

---

## 💾 Flash Memory Partition Map

The STM32L476RG internal flash memory layout is partitioned as follows:

```
  Flash Memory Start (0x0800 0000)
  +-----------------------------------------------------------+
  | Sector 0 - 15 (32 KB)     | Bootloader (BOOT_L476)        |
  +---------------------------+-------------------------------+
  | Sector 16 - 255 (480 KB)  | Slot A / Bank 1 (Active)      |
  |                           | Start Address: 0x0800 8000    |
  +---------------------------+-------------------------------+
  | Sector 256 - 510 (480 KB) | Slot B / Bank 2 (Staging)     |
  |                           | Start Address: 0x0808 0000    |
  +---------------------------+-------------------------------+
  | Sector 511 (2 KB)         | Boot Select Flag Metadata     |
  |                           | Start Address: 0x080F F800    |
  +-----------------------------------------------------------+
  Flash Memory End (0x0810 0000)
```

---

## 📂 Repository Structure

*   **[`BOOT_L476`](BOOT_L476/)** - Dual-bank select execution logic bootloader.
*   **[`D1-Mini_FW`](D1-Mini_FW/)** - ESP8266 AT firmware and flashing guide.
*   **[`FW1_blink`](FW1_blink/)** - Baseline application image (v1.0.0, Slow blink: 2000ms).
*   **[`FW2_Blink`](FW2_Blink/)** - Updated low-power optimized application image (v2.0.0, Medium blink: 500ms).
*   **[`FW3_blink`](FW3_blink/)** - Staging update application image (v3.0.0, Fast blink: 200ms).
*   **[`OTA_SERVER`](OTA_SERVER/)** - Python HTTP server script for hosting firmware binaries.

---

## 1. D1 Mini Setup (AT Firmware)

1. Connect the D1 Mini (CH340G) to your PC using a micro-USB cable.
2. Flash the AT firmware to the D1 Mini (follow the steps in the [`D1-Mini_FW` folder](D1-Mini_FW/README.md)).
3. Verify that AT commands are working by typing `AT` in a serial terminal (e.g., TeraTerm, Putty) at baud rate `115200`. The device should respond with `OK`.

### Hardware Connections

Connect the D1 Mini pins to the STM32 board according to the table below:

| D1 Mini (CH340G) Pin | NUCLEO-L476RG Pin  | Description            |
| :---                 | :---               | :---                   |
| **TX**               | **D2 (CN9)**       | Transmit to STM32 UART |
| **RX**               | **D8 (CN5)**       | Receive from STM32 UART|
| **G**                | **GND (CN6)**      | Ground                 |
| **5V**               | **5V (CN6)**       | Power                  |

> [!IMPORTANT]
> Disconnect the hardware connections between the D1 Mini and the STM32 board **before** flashing the firmware to the D1 Mini. Only connect the USB cable to the D1 Mini during flashing.

---

## 2. Steps to Execute OTA
 1. Connect the **NUCLEO-L476RG** to your PC using a mini-USB cable.
2. Erase the entire chip using **STM32CubeProgrammer**.
3. Flash the **Bootloader** (`BOOT_L476`) ELF or binary file to the NUCLEO-L476RG.
   * *Verify:* The green LED (LD2) on the board should blink rapidly, denoting that there is no firmware in either flash slot.
4. Flash the first firmware (`FW1_blink`) to the NUCLEO-L476RG. Open a serial terminal (TeraTerm) at `115200` baud rate to observe FW1 running.
5. Download and install **STM32CubeCLT** from the official ST website.
6. Generate a binary file from the ELF file using `arm-none-eabi-objcopy` (change the path below to match your installed version):
   ```bash
   "C:\ST\STM32CubeCLT_1.21.0\GNU-tools-for-STM32\bin\arm-none-eabi-objcopy.exe" -O binary build\Debug\FW3_blink.elf FW3.bin
   ```
7. Copy the generated `FW3.bin` file to the `OTA_SERVER` folder.
8. Open the `OTA_SERVER` folder and run the Python script to host the server:
   ```bash
   python ota_server.py
   ```
   * *Note:* The hosting PC and the D1 Mini must be connected to the same Wi-Fi network.
9. While the server is running and the NUCLEO-L476RG is running `FW1`, press the blue user button (**B1**) on the NUCLEO board to initiate the OTA update.

---

## 3. Python HTTP OTA Server Script

This simple server hosts your binary files for the D1 Mini to download.

```python
import http.server
import socketserver

PORT = 8080

class H(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        print(f"[OTA] {self.client_address[0]} {format % args}")

print(f"OTA server running on port {PORT}...")
with socketserver.TCPServer(("", PORT), H) as server:
    server.serve_forever()
```

---

## 4. Flash Memory Layout Configurations

The memory configurations in the linker scripts (`STM32L476RGTX_FLASH.ld`) define the application entry points:

### Firmware 1 (Slot A - SWD) / Firmware 3 (Slot A - FOTA)
```ld
MEMORY
{
  RAM    (xrw)    : ORIGIN = 0x20000000,   LENGTH = 96K
  RAM2    (xrw)    : ORIGIN = 0x10000000,   LENGTH = 32K
  FLASH    (rx)    : ORIGIN = 0x08008000,   LENGTH = 480K
}
```

### Firmware 2 (Slot B - FOTA)
```ld
MEMORY
{
  RAM    (xrw)    : ORIGIN = 0x20000000,   LENGTH = 96K
  RAM2    (xrw)    : ORIGIN = 0x10000000,   LENGTH = 32K
  FLASH    (rx)    : ORIGIN = 0x08080000,   LENGTH = 480K
}
```
