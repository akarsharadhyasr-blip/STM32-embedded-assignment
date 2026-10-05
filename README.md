# Embedded Systems Assessment — STM32F103 (Blue Pill)

Three firmware tasks covering wireless connectivity, HAL peripheral drivers +
digital filtering, and a FreeRTOS multitasking design. Each task is a separate
STM32CubeIDE project targeting the STM32F103C8/CB ("Blue Pill").

**Hardware note:** I developed and compiled everything in STM32CubeIDE for the
STM32F103. I did not have the board (and, for Task 1, an ESP32) on hand, so the
firmware is verified by a clean build, not run on physical hardware. Build and
flash steps are documented below for anyone with the hardware.

## Toolchain
- STM32CubeIDE (1.13+), arm-none-eabi-gcc (bundled with CubeIDE)
- Target MCU: STM32F103C8Tx / CBTx, 72 MHz (HSE crystal + PLL ×9)
- Serial output: 115200 8N1 (view with any serial terminal / USB-TTL adapter)

## Repository layout
    task1/   STM32 host + ESP32 AT-command Wi-Fi modem (MQTT heartbeat)
    task2/   ADC sampling @ 10 Hz + moving-average filter + UART streaming
    task3/   FreeRTOS producer/consumer: EXTI buttons -> queue -> JSON over UART

---

## Task 1 — Wi-Fi Connectivity via AT Commands (Option B)

STM32 host drives an ESP32 (running Espressif's stock **ESP-AT** firmware) as a
Wi-Fi modem over UART. Wi-Fi/broker credentials are entered at runtime (not
hardcoded); the host joins Wi-Fi and an MQTT broker with AT commands, publishes a
heartbeat every 30 s, and watches the UART for inbound cloud commands.

**Main file:** `task1/Core/Src/esp_modem.c`

**Wiring**
| STM32 | Connects to |
|-------|-------------|
| USART1 PA9 (TX) / PA10 (RX) | ESP32 RX / TX (the modem) |
| USART2 PA2 (TX) / PA3 (RX) | USB-TTL adapter (provisioning menu + logs) |
| GND | common ground with ESP32 |

**How it meets the requirements**
- *Dynamic provisioning:* `provision()` — a serial menu captures SSID / password /
  broker / port at runtime into RAM. Nothing network-related is compiled in.
- *Init & connection via AT:* `bring_up()` issues `AT+CWMODE=1` and
  `AT+CWJAP="ssid","pass"` with the entered credentials.
- *Cloud + heartbeat:* `AT+MQTTUSERCFG` / `AT+MQTTCONN` / `AT+MQTTSUB`, then a
  JSON keep-alive published every 30 s.
- *Async Rx:* UART Rx is interrupt-driven into a ring buffer; `pump()` parses
  inbound `+MQTTSUBRECV` messages and the main loop handles them — so commands are
  received even while publishing.

Uses a public broker (`broker.hivemq.com:1883`) by default. The ESP32 must be
flashed with ESP-AT firmware (Espressif prebuilt binary) — no ESP32 code is
written here; that is the point of the host-modem architecture (Option B).

---

## Task 2 — ADC Sampling, Moving Average, UART Streaming

Samples an analog input at 10 Hz, smooths it with a 10-sample moving average, and
streams the result over UART at 115200.

**Main file:** `task2/Core/Src/adc_stream.c`

**Wiring**
| STM32 | Connects to |
|-------|-------------|
| PA0 (ADC1_IN0) | analog input 0–3.3 V (e.g. potentiometer wiper) |
| USART1 PA9 (TX) | USB-TTL adapter RX (serial output) |

**How it meets the requirements**
- *10 Hz sampling:* a `HAL_GetTick()`-timed loop fires every 100 ms (`next +=
  100` keeps the cadence from drifting). ADC read is polled with error checks.
- *Moving average (window 10):* `ma_update()` keeps a running sum, so each update
  is O(1); commented inline.
- *UART transmission:* readable line `t=.. raw=.. avg=.. (.. mV)` at 115200.
- *Error handling:* every ADC HAL call is checked; transmit is skipped if the
  line overflowed or the UART is busy.

Output sample: `t=1200 ms  raw=2048  avg=2041  (1645 mV)`

---

## Task 3 — FreeRTOS Task Synchronization & Logging

Two push-buttons on EXTI generate events; a FreeRTOS queue decouples the
interrupt from the slow UART I/O; a consumer task logs each event as JSON.

**Main file:** `task3/Core/Src/rtos_logger.c`

**Wiring**
| STM32 | Connects to |
|-------|-------------|
| PB0 (EXTI0) | button 1 to GND (internal pull-up) |
| PB1 (EXTI1) | button 2 to GND (internal pull-up) |
| USART1 PA9 (TX) | USB-TTL adapter RX (serial output) |

**How it meets the requirements**
- *Event Producer (task):* `producer_task` is woken by the EXTI interrupt (which
  only captures button ID + timestamp into a queue, non-blocking), builds the JSON
  payload, and posts it to the log queue.
- *Consumer / UART Logger (task):* `consumer_task` blocks on the log queue and
  transmits the JSON over UART.
- *Two tasks + queue:* the log queue decouples event generation from the slow UART
  I/O; a second small queue keeps the ISR non-blocking. Both boundaries commented.
- *Edge cases:* full queue → event dropped and counted (reported on timeout);
  empty queue → bounded receive timeouts avoid busy-waiting.

Output sample: `{"button_id": 1, "timestamp": 12500}`

---

## Build (any task)
1. Open STM32CubeIDE → File → Open Projects from File System → select the task
   folder (e.g. `task1`).
2. Build: Project → Build Project (or the hammer / Ctrl+B).
3. Output: `Debug/<task>.elf`.

## Flash (requires the hardware)
The Blue Pill has no on-board debugger. Connect an **ST-Link V2** to the SWD
header: SWDIO→PA13, SWCLK→PA14, GND→GND, 3V3→3V3.
1. In CubeIDE: Run → Run (or Debug). It flashes over ST-Link.
2. Open the serial terminal (115200 8N1) on the USB-TTL adapter to see output.
   - Task 1: for the modem link, flash the ESP32 with ESP-AT firmware first.

## Notes
- Clocks are configured for an 8 MHz HSE crystal (72 MHz SYSCLK). On a board
  without a crystal, switch RCC to HSI in the .ioc.
- Task 1 keeps credentials in RAM for the session (entered via the menu) to keep
  the flow simple; they could instead be persisted to flash.
