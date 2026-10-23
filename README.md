# STM32 FreeRTOS CAN Node

A portfolio firmware module for an STM32F103C8T6 (Blue Pill) that demonstrates FreeRTOS task design and a small, documented classic CAN 2.0A application protocol.

The node receives commands from a CAN bus, controls an output, and publishes periodic heartbeat and diagnostic frames. CAN reception enters through the HAL FIFO0 interrupt callback and is handed to a FreeRTOS queue; a dedicated task parses commands, while a separate transmit task serializes access to bxCAN mailboxes.

## What this demonstrates

- FreeRTOS task separation, priorities, queues, blocking waits, and periodic scheduling with `vTaskDelayUntil`.
- ISR-to-task handoff using `xQueueSendFromISR` and `portYIELD_FROM_ISR`.
- STM32 HAL bxCAN setup, standard data-frame filtering, FIFO0 receive handling, and queued transmission.
- A compact CAN 2.0A command, response, heartbeat, and diagnostics protocol.
- Explicit handling and reporting of queue drops and CAN errors.

## Target and dependencies

- STM32F103C8T6 with STM32Cube HAL and FreeRTOS configured by STM32CubeMX / STM32CubeIDE.
- CAN1 on the default PA11 (RX) / PA12 (TX) pins.
- An external 3.3 V logic CAN transceiver such as SN65HVD230 or a suitable 5 V transceiver with 3.3 V compatible MCU logic.
- A CAN bus peer or USB-CAN adapter for sending command frames.

This repository contains the application module, not the vendor HAL, startup code, linker script, or generated CubeMX project. Integrate `Core/Inc/app.h` and `Core/Src/app.c` into a CubeMX-generated STM32F103 project.

## CubeMX setup

1. Set the system clock to 72 MHz from an 8 MHz HSE, with APB1 at 36 MHz. The CAN timing below assumes this APB1 clock.
2. Enable CAN1 in normal mode on PA11/PA12. Configure 500 kbit/s with prescaler 6, SJW 1 TQ, BS1 9 TQ, and BS2 2 TQ: `36 MHz / (6 × (1 + 9 + 2)) = 500 kbit/s`.
3. Enable FreeRTOS with a 1 kHz tick and enough heap for two 16-frame queues and four tasks. The module creates its queues dynamically with `xQueueCreate`.
4. Enable `USB_LP_CAN1_RX0_IRQn` and set its priority to `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY + 1` (or another valid priority numerically greater than or equal to that threshold). FreeRTOS `FromISR` APIs must not run from an interrupt above the syscall priority ceiling.
5. In the generated `stm32f1xx_it.c`, route the CAN FIFO0 interrupt to HAL:

   ```c
   extern CAN_HandleTypeDef hcan;

   void USB_LP_CAN1_RX0_IRQHandler(void)
   {
       HAL_CAN_IRQHandler(&hcan);
   }
   ```

6. In `main.c`, call `App_Init(&hcan)` after `MX_CAN_Init()` and before starting the FreeRTOS scheduler:

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

The CAN peripheral is started inside the receive task after the scheduler is running, so the ISR-to-queue path is ready before RX interrupts are enabled.

## Board wiring

| STM32F103 pin | Connect to |
| --- | --- |
| PA12 (CAN TX) | Transceiver TXD |
| PA11 (CAN RX) | Transceiver RXD |
| Transceiver CANH / CANL | The corresponding CAN bus lines |
| GND | Common ground with the transceiver and peer |
| PA0 | Optional output indicator or logic analyzer input; high means enabled |
| PC13 | Blue Pill onboard status LED; toggles at 2 Hz |

Do not connect PA11/PA12 directly to CANH/CANL. Fit 120 Ω termination at the two physical ends of the bus, not at every node. Verify the transceiver supply and MCU logic levels for the selected board.

## CAN 2.0A protocol

All frames use 11-bit standard identifiers, data frames, and a maximum DLC of 8. Multibyte values are little-endian.

| Identifier | Direction | Period | Payload |
| --- | --- | --- | --- |
| `0x080` | Node → bus | 1 s | `[version, state, uptime_s(4), rx_drops(1), can_errors(1)]` |
| `0x100` | Bus → node | On demand | `[sequence, command, value, reserved(5)]` |
| `0x101` | Node → bus | On command | `[sequence, command, status, output_state, uptime_s(4)]` |
| `0x180` | Node → bus | 1 s | `[uptime_s(4), rx_drops(2), tx_drops(2)]` |

Commands on `0x100`:

| Command | Code | Arguments |
| --- | --- | --- |
| Set output | `0x01` | Byte 2: `0` off, `1` on |
| Get status | `0x02` | No argument; byte 2 is ignored |

Response status values: `0` success, `1` malformed/invalid argument, `2` unsupported command. The sequence byte is copied into the response so a host can match replies to requests. This is a small application protocol over classic CAN; it is not CANopen or CAN-FD.

## Build and flash

Generate the STM32CubeIDE project with the settings above, add the two files from `Core/`, then build and flash using CubeIDE or the command-line tools for your installed STM32 toolchain. The exact generated project and linker settings depend on the selected CubeMX version and board configuration.

To exercise the node, connect a CAN peer at 500 kbit/s and send an 8-byte standard data frame with ID `0x100`. For example, send `01 01 01 00 00 00 00 00` to enable PA0; expect a response on `0x101` with sequence `01`, command `01`, and status `00`.

## Limitations

This is an application-layer demonstration. Validate clock configuration, CAN timing, transceiver wiring, bus termination, interrupt priorities, and FreeRTOS heap/stack sizes on the actual board before relying on it in a product.
