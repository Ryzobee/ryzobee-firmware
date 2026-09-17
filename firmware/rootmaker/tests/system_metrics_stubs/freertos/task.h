#pragma once
#include "freertos/FreeRTOS.h"
configRUN_TIME_COUNTER_TYPE ulTaskGetIdleRunTimeCounterForCore(BaseType_t core);
BaseType_t xPortGetCoreID(void);
