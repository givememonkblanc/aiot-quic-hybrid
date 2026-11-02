
/* AIOT_BATCH_RUNTIME_SNAPSHOT: opt-in read-only accounting probe. Include the
 * running task's current slice; vTaskGetInfo alone only returns closed slices. */
#if AIOT_QUIC_BATCH_EXPERIMENT && (configGENERATE_RUN_TIME_STATS == 1)
uint32_t aiot_task_runtime_now(void)
{
    uint32_t value;
    taskENTER_CRITICAL(&xKernelLock);
    BaseType_t core=xPortGetCoreID();
    uint32_t now=portGET_RUN_TIME_COUNTER_VALUE();
    value=(uint32_t)pxCurrentTCBs[core]->ulRunTimeCounter +
          (uint32_t)(now-ulTaskSwitchedInTime[core]);
    taskEXIT_CRITICAL(&xKernelLock);
    return value;
}
#endif
