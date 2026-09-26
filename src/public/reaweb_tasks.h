#ifndef REAWEB_TASKS_H
#define REAWEB_TASKS_H
#include "reaweb_service.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef uint64_t ReaWeb_TimerHandle;
typedef void (*ReaWeb_TimerCallback)(void* user_data, ReaWeb_TimerHandle);
/* Main-thread API and callbacks. interval_ms == 0 is one-shot. Repeating timers
   skip missed periods. Service unload cancels owner-bound timers. Timers share
   the low-frequency Runtime scheduler and must not drive frame producers. */
typedef int (*ReaWeb_CreateTimerFn)(uint32_t delay_ms, uint32_t interval_ms, ReaWeb_ServiceHandle owner,
                                  ReaWeb_TimerCallback, void* user_data, ReaWeb_TimerHandle*);
typedef int (*ReaWeb_CancelTimerFn)(ReaWeb_TimerHandle);
#ifdef __cplusplus
}
#endif
#endif
