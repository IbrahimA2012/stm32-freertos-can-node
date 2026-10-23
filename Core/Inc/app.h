#ifndef APP_H
#define APP_H

#include "stm32f1xx_hal.h"

/* Call after MX_CAN_Init() and before vTaskStartScheduler(). */
int App_Init(CAN_HandleTypeDef *canHandle);

#endif /* APP_H */
