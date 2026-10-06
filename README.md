This repository has my work for the embedded assessment. There are three tasks,
each in its own folder: task1, task2 and task3. All of them are STM32 projects
made in STM32CubeIDE for the STM32F103 (the Blue Pill board).

I did not have the hardware to flash and run these, so they are verified by a
clean build in STM32CubeIDE rather than on a device. The serial output is 115200
8N1 on all three tasks.

Task 1

This is the Wi-Fi task. I used the option where the STM32 is the main controller
and the ESP32 is used only as a Wi-Fi modem. The ESP32 runs ready-made
ESP-AT firmware, and my STM32 code controls it over UART by sending plain AT
commands, so there is no code running on the ESP32 that I wrote. When the board
starts, it shows a small menu on the serial console and asks for the Wi-Fi name,
password and the MQTT broker, so nothing is hardcoded. After that it joins the
Wi-Fi using CWMODE and CWJAP, connects to the MQTT broker, and sends a small
heartbeat message every 30 seconds so the server knows it is alive. At the same
time it keeps listening on the UART for any command coming back from the cloud,
using an interrupt so it never misses anything. The main file is esp_modem.c.

Task 2

This is the ADC task. It reads an analog input (a voltage on pin PA0, for example
from a potentiometer) ten times every second. Raw analog readings are always a bit
noisy, so it smooths them using a moving average of the last ten samples. The
smoothed value is then printed over UART at 115200 in a readable line, together
with the raw value and the voltage in millivolts. The moving average keeps a
running total so it stays fast, and the ADC and UART calls are checked for errors.
The main file is adc_stream.c.

Task 3

This is the FreeRTOS task. The idea is to keep the fast part and the slow part
separate using a queue. Two buttons are connected to pins with external
interrupts. When a button is pressed, the interrupt only does the quick work: it
notes which button it was and the time, and drops that into a queue, then returns.
A producer task takes that and builds a small JSON message like
{"button_id": 1, "timestamp": 12500}. A consumer task waits on a second queue and
sends the JSON out over UART, so the slow UART work never happens inside the
interrupt. Full and empty queues are handled safely, and the buttons are debounced
so one press is one message. The main file is rtos_logger.c.


Building

Open STM32CubeIDE, use Open Projects from File System and pick the task folder,
then build the project. The build produces an elf file in the Debug folder. No
hardware is needed to build.

To flash on real hardware, the Blue Pill needs an external ST-Link V2 on the SWD
pins (SWDIO to PA13, SWCLK to PA14, plus ground and 3.3V). In CubeIDE press Run to
flash over the ST-Link, then open a serial terminal at 115200 to see the output.
For task 1 you also need an ESP32 with ESP-AT firmware connected to USART1.

