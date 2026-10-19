#include <stdio.h>
#include <stdlib.h>

#include <FreeRTOS.h>
#include "semphr.h"
#if defined(CONFIG_BACKTRACE)
#include "backtrace.h"
#endif

/* R01: minimal fault record, readable in a debugger after the halt. */
volatile uint32_t g_fault_reason = 0;   /* 1 = assert, 2 = stack overflow, 3 = malloc */
volatile uint32_t g_fault_arg = 0;

static void fault_halt(uint32_t reason, uint32_t arg)
{
    g_fault_arg = arg;
    g_fault_reason = reason;
    portDISABLE_INTERRUPTS();
    for (;;) {
    }
}

__attribute__((weak)) void vAssertCalled(void)
{
    /* R01: no printf/backtrace here — this can run in interrupt context or on
     * an already-corrupted stack. Leave a readable record, then halt. */
    fault_halt(1u, 0u);
}

__attribute__((weak)) void vApplicationTickHook(void)
{
    //printf("vApplicationTickHook\r\n");
}

__attribute__((weak)) void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)pcTaskName;
    fault_halt(2u, (uint32_t)(uintptr_t)xTask);
}

__attribute__((weak)) void vApplicationMallocFailedHook(void)
{
    fault_halt(3u, 0u);
}

__attribute__((weak)) void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize)
{
    /* If the buffers to be provided to the Idle task are declared inside this
    function then they must be declared static - otherwise they will be allocated on
    the stack and so not exists after this function exits. */
    static StaticTask_t xIdleTaskTCB;
    static StackType_t uxIdleTaskStack[configMINIMAL_STACK_SIZE];

    /* Pass out a pointer to the StaticTask_t structure in which the Idle task's
    state will be stored. */
    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;

    /* Pass out the array that will be used as the Idle task's stack. */
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;

    /* Pass out the size of the array pointed to by *ppxIdleTaskStackBuffer.
    Note that, as the array is necessarily of type StackType_t,
    configMINIMAL_STACK_SIZE is specified in words, not bytes. */
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

/* configSUPPORT_STATIC_ALLOCATION and configUSE_TIMERS are both set to 1, so the
application must provide an implementation of vApplicationGetTimerTaskMemory()
to provide the memory that is used by the Timer service task. */
__attribute__((weak)) void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer, StackType_t **ppxTimerTaskStackBuffer, uint32_t *pulTimerTaskStackSize)
{
    /* If the buffers to be provided to the Timer task are declared inside this
    function then they must be declared static - otherwise they will be allocated on
    the stack and so not exists after this function exits. */
    static StaticTask_t xTimerTaskTCB;
    static StackType_t uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH];

    /* Pass out a pointer to the StaticTask_t structure in which the Timer
    task's state will be stored. */
    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;

    /* Pass out the array that will be used as the Timer task's stack. */
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;

    /* Pass out the size of the array pointed to by *ppxTimerTaskStackBuffer.
    Note that, as the array is necessarily of type StackType_t,
    configTIMER_TASK_STACK_DEPTH is specified in words, not bytes. */
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

int *__errno()
{
    //#if(configUSE_POSIX_ERRNO == 1)
      {
      extern int FreeRTOS_errno;
      return &FreeRTOS_errno;
      }
    //#endif
    //return &_REENT->_errno;
}
