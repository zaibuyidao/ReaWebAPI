#ifndef REAWEB_SERVICE_H
#define REAWEB_SERVICE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define REAWEB_SERVICE_ABI 1
typedef uint64_t ReaWeb_ServiceHandle;
typedef uint64_t ReaWeb_RequestId;
enum ReaWeb_ServiceStatus {
  REAWEB_OK = 0, REAWEB_SERVICE_NOT_FOUND = 1, REAWEB_METHOD_NOT_FOUND = 2,
  REAWEB_INVALID_ARGUMENT = 3, REAWEB_EXTENSION_UNLOADED = 4,
  REAWEB_MAIN_THREAD_REQUIRED = 5, REAWEB_TIMEOUT = 6,
  REAWEB_SERVICE_EXISTS = 7, REAWEB_QUEUE_LIMIT = 8, REAWEB_REQUEST_GONE = 9,
  REAWEB_SERVICE_ERROR = 10
};
/* request == 0 denotes send. Return a status. For invoke, complete exactly once
   with ReaWeb_CompleteServiceCall, synchronously or from a worker. Arguments are
   borrowed until on_request returns. REAPER APIs remain main-thread only. */
typedef struct ReaWeb_ServiceCallbacks {
  uint32_t size;
  uint32_t abi_version;
  void* user_data;
  int (*on_request)(void*, ReaWeb_ServiceHandle, ReaWeb_RequestId, int window_id,
                    const char* method, const char* payload_json);
  /* Optional, main thread. Cancel/join associated work before unload. */
  void (*on_cancel)(void*, ReaWeb_RequestId);
} ReaWeb_ServiceCallbacks;

/* Resolve these functions with REAPER GetFunc. Register/unregister are main
   thread only. Completion and event emission are thread safe and copy JSON.
   Unregister before unloading, stop/join workers before ReaWebAPI unloads.
   Handles never alias a subsequent registration with the same name. */
typedef int (*ReaWeb_RegisterServiceFn)(const char*, const ReaWeb_ServiceCallbacks*, ReaWeb_ServiceHandle*);
typedef int (*ReaWeb_UnregisterServiceFn)(ReaWeb_ServiceHandle);
typedef int (*ReaWeb_CompleteServiceCallFn)(ReaWeb_ServiceHandle, ReaWeb_RequestId, const char* result_json,
                                         int status, const char* message);
/* window_id == 0 broadcasts to this service's subscribers. */
typedef int (*ReaWeb_EmitServiceEventFn)(ReaWeb_ServiceHandle, int window_id, const char* event, const char* payload_json);

#ifdef __cplusplus
}
#endif
#endif
