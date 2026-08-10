#pragma once

#include "FreeRTOS.h"

struct FakeSemaphore;
using SemaphoreHandle_t = FakeSemaphore *;

SemaphoreHandle_t xSemaphoreCreateMutex();
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait_ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
