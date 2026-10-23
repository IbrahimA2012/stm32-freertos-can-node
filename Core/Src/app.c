#include "app.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define APP_CAN_RX_QUEUE_LENGTH  16U
#define APP_CAN_TX_QUEUE_LENGTH  16U
#define APP_CAN_TASK_STACK_WORDS 384U

#define CAN_ID_HEARTBEAT  0x080U
#define CAN_ID_COMMAND    0x100U
#define CAN_ID_RESPONSE   0x101U
#define CAN_ID_DIAGNOSTIC 0x180U

#define CMD_SET_OUTPUT 0x01U
#define CMD_GET_STATUS 0x02U

#define STATUS_OK              0x00U
#define STATUS_INVALID_REQUEST 0x01U
#define STATUS_UNSUPPORTED     0x02U

typedef struct {
    uint16_t id;
    uint8_t dlc;
    uint8_t data[8];
} AppCanFrame_t;

typedef struct {
    uint32_t rxDrops;
    uint32_t txDrops;
    uint32_t canErrors;
} AppCanStats_t;

static CAN_HandleTypeDef *s_canHandle;
static QueueHandle_t s_rxQueue;
static QueueHandle_t s_txQueue;
static volatile uint32_t s_rxDrops;
static volatile uint32_t s_txDrops;
static volatile uint32_t s_canErrors;
static uint8_t s_outputEnabled;

static void CanRxTask(void *argument);
static void CanTxTask(void *argument);
static void TelemetryTask(void *argument);
static void StatusLedTask(void *argument);

static void writeLe32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value);
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static void writeLe16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value);
    destination[1] = (uint8_t)(value >> 8);
}

static uint16_t saturateU16(uint32_t value)
{
    return (value > UINT16_MAX) ? UINT16_MAX : (uint16_t)value;
}

static void getStats(AppCanStats_t *stats)
{
    stats->rxDrops = s_rxDrops;
    stats->txDrops = s_txDrops;
    stats->canErrors = s_canErrors;
}

static BaseType_t queueCanTx(uint16_t id, const uint8_t *data, uint8_t dlc)
{
    AppCanFrame_t frame = {0};

    if ((s_txQueue == NULL) || (id > 0x7FFU) || (dlc > 8U) ||
        ((dlc > 0U) && (data == NULL))) {
        ++s_txDrops;
        return pdFAIL;
    }

    frame.id = id;
    frame.dlc = dlc;
    if (dlc > 0U) {
        (void)memcpy(frame.data, data, dlc);
    }

    if (xQueueSend(s_txQueue, &frame, 0U) != pdPASS) {
        ++s_txDrops;
        return pdFAIL;
    }

    return pdPASS;
}

static void initBoardOutputs(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;

    gpio.Pin = GPIO_PIN_0;
    HAL_GPIO_Init(GPIOA, &gpio);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_RESET);

    gpio.Pin = GPIO_PIN_13;
    HAL_GPIO_Init(GPIOC, &gpio);
    /* The Blue Pill onboard LED is active-low. */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
}

static HAL_StatusTypeDef configureCanFilter(CAN_HandleTypeDef *canHandle)
{
    CAN_FilterTypeDef filter = {0};

    filter.FilterBank = 0U;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh = 0U;
    filter.FilterIdLow = 0U;
    filter.FilterMaskIdHigh = 0U;
    filter.FilterMaskIdLow = 0U;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterActivation = ENABLE;
    filter.SlaveStartFilterBank = 14U;

    return HAL_CAN_ConfigFilter(canHandle, &filter);
}

int App_Init(CAN_HandleTypeDef *canHandle)
{
    if (canHandle == NULL) {
        return 0;
    }

    s_canHandle = canHandle;
    s_rxQueue = xQueueCreate(APP_CAN_RX_QUEUE_LENGTH, sizeof(AppCanFrame_t));
    s_txQueue = xQueueCreate(APP_CAN_TX_QUEUE_LENGTH, sizeof(AppCanFrame_t));
    if ((s_rxQueue == NULL) || (s_txQueue == NULL)) {
        return 0;
    }

    if (configureCanFilter(s_canHandle) != HAL_OK) {
        return 0;
    }

    initBoardOutputs();

    if (xTaskCreate(CanRxTask, "CAN-RX", APP_CAN_TASK_STACK_WORDS, NULL,
                    tskIDLE_PRIORITY + 3U, NULL) != pdPASS) {
        return 0;
    }
    if (xTaskCreate(CanTxTask, "CAN-TX", APP_CAN_TASK_STACK_WORDS, NULL,
                    tskIDLE_PRIORITY + 3U, NULL) != pdPASS) {
        return 0;
    }
    if (xTaskCreate(TelemetryTask, "Telemetry", APP_CAN_TASK_STACK_WORDS, NULL,
                    tskIDLE_PRIORITY + 1U, NULL) != pdPASS) {
        return 0;
    }
    if (xTaskCreate(StatusLedTask, "Status-LED", APP_CAN_TASK_STACK_WORDS, NULL,
                    tskIDLE_PRIORITY + 1U, NULL) != pdPASS) {
        return 0;
    }

    return 1;
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *canHandle)
{
    BaseType_t higherPriorityTaskWoken = pdFALSE;

    if ((canHandle == NULL) || (canHandle != s_canHandle) || (s_rxQueue == NULL)) {
        return;
    }

    while (HAL_CAN_GetRxFifoFillLevel(canHandle, CAN_RX_FIFO0) > 0U) {
        CAN_RxHeaderTypeDef header = {0};
        AppCanFrame_t frame = {0};

        if (HAL_CAN_GetRxMessage(canHandle, CAN_RX_FIFO0, &header, frame.data) != HAL_OK) {
            ++s_canErrors;
            break;
        }

        if ((header.IDE != CAN_ID_STD) || (header.RTR != CAN_RTR_DATA) ||
            (header.DLC > 8U)) {
            continue;
        }

        frame.id = (uint16_t)header.StdId;
        frame.dlc = (uint8_t)header.DLC;
        if (xQueueSendFromISR(s_rxQueue, &frame, &higherPriorityTaskWoken) != pdPASS) {
            ++s_rxDrops;
        }
    }

    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *canHandle)
{
    if (canHandle == s_canHandle) {
        ++s_canErrors;
    }
}

static void CanRxTask(void *argument)
{
    AppCanFrame_t frame;
    (void)argument;

    if ((HAL_CAN_Start(s_canHandle) != HAL_OK) ||
        (HAL_CAN_ActivateNotification(s_canHandle,
             CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_ERROR | CAN_IT_BUSOFF) != HAL_OK)) {
        Error_Handler();
    }

    for (;;) {
        if (xQueueReceive(s_rxQueue, &frame, portMAX_DELAY) == pdPASS) {
            uint8_t response[8] = {0};
            uint8_t sequence = (frame.dlc > 0U) ? frame.data[0] : 0U;
            uint8_t command = (frame.dlc > 1U) ? frame.data[1] : 0U;
            uint8_t status = STATUS_OK;
            uint32_t uptimeSeconds = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);

            if (frame.id != CAN_ID_COMMAND) {
                continue;
            }

            if (frame.dlc < 2U) {
                status = STATUS_INVALID_REQUEST;
            } else if (command == CMD_SET_OUTPUT) {
                if ((frame.dlc < 3U) || (frame.data[2] > 1U)) {
                    status = STATUS_INVALID_REQUEST;
                } else {
                    s_outputEnabled = frame.data[2];
                    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0,
                        s_outputEnabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
                }
            } else if (command != CMD_GET_STATUS) {
                status = STATUS_UNSUPPORTED;
            }

            response[0] = sequence;
            response[1] = command;
            response[2] = status;
            response[3] = s_outputEnabled;
            writeLe32(&response[4], uptimeSeconds);
            (void)queueCanTx(CAN_ID_RESPONSE, response, sizeof(response));
        }
    }
}

static void CanTxTask(void *argument)
{
    AppCanFrame_t frame;
    (void)argument;

    for (;;) {
        if (xQueueReceive(s_txQueue, &frame, portMAX_DELAY) == pdPASS) {
            CAN_TxHeaderTypeDef header = {0};
            uint32_t mailbox = 0U;
            TickType_t waitStart = xTaskGetTickCount();

            header.StdId = frame.id;
            header.IDE = CAN_ID_STD;
            header.RTR = CAN_RTR_DATA;
            header.DLC = frame.dlc;
            header.TransmitGlobalTime = DISABLE;

            while ((HAL_CAN_GetTxMailboxesFreeLevel(s_canHandle) == 0U) &&
                   ((xTaskGetTickCount() - waitStart) < pdMS_TO_TICKS(100U))) {
                vTaskDelay(1U);
            }

            if (HAL_CAN_GetTxMailboxesFreeLevel(s_canHandle) == 0U) {
                ++s_txDrops;
            } else if (HAL_CAN_AddTxMessage(s_canHandle, &header, frame.data, &mailbox) != HAL_OK) {
                ++s_canErrors;
            }
        }
    }
}

static void TelemetryTask(void *argument)
{
    TickType_t lastWake = xTaskGetTickCount();
    (void)argument;

    for (;;) {
        AppCanStats_t stats;
        uint8_t heartbeat[8] = {0};
        uint8_t diagnostics[8] = {0};
        uint32_t uptimeSeconds;

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1000U));
        uptimeSeconds = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
        getStats(&stats);

        heartbeat[0] = 1U; /* Protocol version. */
        heartbeat[1] = 1U; /* Node is operational. */
        writeLe32(&heartbeat[2], uptimeSeconds);
        heartbeat[6] = (stats.rxDrops > UINT8_MAX) ? UINT8_MAX : (uint8_t)stats.rxDrops;
        heartbeat[7] = (stats.canErrors > UINT8_MAX) ? UINT8_MAX : (uint8_t)stats.canErrors;

        writeLe32(&diagnostics[0], uptimeSeconds);
        writeLe16(&diagnostics[4], saturateU16(stats.rxDrops));
        writeLe16(&diagnostics[6], saturateU16(stats.txDrops));

        (void)queueCanTx(CAN_ID_HEARTBEAT, heartbeat, sizeof(heartbeat));
        (void)queueCanTx(CAN_ID_DIAGNOSTIC, diagnostics, sizeof(diagnostics));
    }
}

static void StatusLedTask(void *argument)
{
    (void)argument;

    for (;;) {
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
        vTaskDelay(pdMS_TO_TICKS(250U));
    }
}
