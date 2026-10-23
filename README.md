# STM32 FreeRTOS CAN Node

<p align="center">
  <strong>A real-time CAN firmware module for the STM32F103C8T6 (Blue Pill)</strong><br>
  FreeRTOS task design · interrupt-to-task handoff · classic CAN 2.0A
</p>

<p align="center">
  <img src="https://img.shields.io/badge/MCU-STM32F103C8T6-03234B?style=for-the-badge&logo=stmicroelectronics&logoColor=white" alt="STM32F103C8T6">
  <img src="https://img.shields.io/badge/RTOS-FreeRTOS-6DB33F?style=for-the-badge&logo=freertos&logoColor=white" alt="FreeRTOS">
  <img src="https://img.shields.io/badge/Protocol-Classic_CAN-1565C0?style=for-the-badge" alt="Classic CAN">
  <img src="https://img.shields.io/badge/Language-Embedded_C-A8B9CC?style=for-the-badge&logo=c&logoColor=black" alt="Embedded C">
</p>

> **Portfolio project** — an application module to integrate into a CubeMX-generated STM32 project. Build and hardware validation are still pending.

The node accepts command frames over CAN, controls a GPIO output, and periodically publishes heartbeat and diagnostic telemetry. CAN reception is handled by the STM32 HAL FIFO0 interrupt and handed to a FreeRTOS task through a queue. A dedicated transmit task serializes access to bxCAN mailboxes.

## At a glance

| Item | Details |
| --- | --- |
| Target | STM32F103C8T6 (Blue Pill) |
| Language | Embedded C |
| RTOS | FreeRTOS |
| Bus | Classic CAN 2.0A, 11-bit identifiers |
| Example bitrate | 500 kbit/s |
| CAN pins | PA11 (RX), PA12 (TX) |
| Source module | `Core/Inc/app.h`, `Core/Src/app.c` |

## What it demonstrates

- FreeRTOS task separation, priorities, queues, blocking waits, and periodic scheduling with `vTaskDelayUntil`.
- Safe ISR-to-task handoff with `xQueueSendFromISR` and `portYIELD_FROM_ISR`.
- STM32 HAL bxCAN setup, standard-frame filtering, FIFO0 receive handling, and queued transmission.
- A small command, response, heartbeat, and diagnostics protocol over classic CAN.
- Explicit reporting of queue drops and CAN errors.

## Repository layout

```text
Core/
├── Inc/
│   └── app.h
└── Src/
    └── app.c
```

This repository contains the application module; it does not include the vendor HAL, startup code, linker script, or generated CubeMX project.

## Hardware and dependencies

- STM32F103C8T6 with STM32Cube HAL and FreeRTOS, configured through STM32CubeMX / STM32CubeIDE.
- CAN1 on PA11 (RX) and PA12 (TX).
- An external CAN transceiver, such as SN65HVD230, between the MCU and CANH/CANL. Check logic levels for the selected transceiver and board.
- A CAN bus peer or USB-CAN adapter to send commands.

> **Electrical note:** Do not connect PA11/PA12 directly to CANH/CANL. Connect common ground and fit 120 Ω termination at the two physical ends of the bus.

## CubeMX and integration

1. Configure an 8 MHz HSE and 72 MHz system clock, with APB1 at 36 MHz. The CAN timing below assumes this APB1 clock.
2. Enable CAN1 in normal mode on PA11/PA12. Set prescaler 6, SJW 1 TQ, BS1 9 TQ, and BS2 2 TQ for 500 kbit/s: `36 MHz / (6 × (1 + 9 + 2)) = 500 kbit/s`.
3. Enable FreeRTOS with a 1 kHz tick and sufficient heap for two 16-frame queues and four tasks. The module creates queues using `xQueueCreate`.
4. Enable `USB_LP_CAN1_RX0_IRQn`. Set its priority to `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY + 1` or another valid value at or below the FreeRTOS syscall urgency ceiling. `FromISR` APIs must not run from an interrupt above that ceiling.
5. Route the CAN FIFO0 interrupt through HAL in the generated `stm32f1xx_it.c`:

   ```c
   extern CAN_HandleTypeDef hcan;

   void USB_LP_CAN1_RX0_IRQHandler(void)
   {
       HAL_CAN_IRQHandler(&hcan);
   }
   ```

6. In `main.c`, call `App_Init(&hcan)` after `MX_CAN_Init()` and before starting the scheduler:

   ```c
   #include "app.h"
   #include "can.h"
   #include "FreeRTOS.h"
   #include "task.h"

   /* After HAL_Init(), clock setup, GPIO setup, and MX_CAN_Init(): */
   if (App_Init(&hcan) != 0) {
       vTaskStartScheduler();
   }
   Error_Handler();
   ```

The CAN peripheral is started by the receive task after the scheduler runs, so the queue handoff is ready before RX interrupts are enabled.

## Wiring

| STM32F103 pin | Connect to |
| --- | --- |
| PA12 (CAN TX) | Transceiver TXD |
| PA11 (CAN RX) | Transceiver RXD |
| Transceiver CANH / CANL | Matching CAN bus lines |
| GND | Common ground with the transceiver and CAN peer |
| PA0 | Optional output indicator or logic analyzer input; high means enabled |
| PC13 | Blue Pill onboard status LED; toggles at 2 Hz |

## CAN 2.0A protocol

All frames use 11-bit standard identifiers, data frames, and a maximum DLC of 8. Multibyte values are little-endian.

| ID | Direction | Period | Payload |
| --- | --- | --- | --- |
| `0x080` | Node → bus | 1 s | `[version, state, uptime_s(4), rx_drops(1), can_errors(1)]` |
| `0x100` | Bus → node | On demand | `[sequence, command, value, reserved(5)]` |
| `0x101` | Node → bus | On command | `[sequence, command, status, output_state, uptime_s(4)]` |
| `0x180` | Node → bus | 1 s | `[uptime_s(4), rx_drops(2), tx_drops(2)]` |

### Commands (`0x100`)

| Command | Code | Argument |
| --- | --- | --- |
| Set output | `0x01` | Byte 2: `0` = off, `1` = on |
| Get status | `0x02` | No argument; byte 2 is ignored |

Response status: `0` success, `1` malformed/invalid argument, `2` unsupported command. The sequence byte is echoed so a host can match a response to its request. This is a small application protocol; it is not CANopen or CAN-FD.

## Build, flash, and try it

1. Generate an STM32CubeIDE project using the configuration above.
2. Add `Core/Inc/app.h` and `Core/Src/app.c` to the generated project and wire up the interrupt and `App_Init` call.
3. Build and flash using STM32CubeIDE or the command-line tools for your installed STM32 toolchain.
4. Connect a CAN peer at 500 kbit/s and send an 8-byte standard data frame with ID `0x100`.

Example: send `01 01 01 00 00 00 00 00` to enable PA0. The node should respond on `0x101` with sequence `01`, command `01`, and status `00`.

The exact generated project and linker settings depend on the CubeMX version and board configuration. This repository does not yet include a generated build project or hardware test results. Validate clock configuration, CAN timing, transceiver wiring, bus termination, interrupt priorities, and FreeRTOS heap/stack sizes on the actual board before relying on the module.

## Contact



**Ibrahim Alasmar** · Embedded Software Engineer
[LinkedIn profile](https://www.linkedin.com/in/ibrahim-alasmar-9930691b6/) · [ibrahimalasmar25@gmail.com](mailto:ibrahimalasmar25@gmail.com)
