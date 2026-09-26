#include <reaper_plugin.h>
#include "reaweb_service.h"
#include <cstring>

namespace {
ReaWeb_ServiceHandle handle;
ReaWeb_UnregisterServiceFn unregister_service;
ReaWeb_CompleteServiceCallFn complete;
ReaWeb_EmitServiceEventFn emit;
int request(void*, ReaWeb_ServiceHandle service, ReaWeb_RequestId id, int window, const char* method, const char* payload) {
  if (!std::strcmp(method, "ping")) return id ? complete(service, id, "\"pong\"", REAWEB_OK, nullptr) : REAWEB_OK;
  if (!std::strcmp(method, "message")) {
    const auto status = emit(service, window, "changed", payload);
    if (status != REAWEB_OK || !id) return status;
    return complete(service, id, "true", REAWEB_OK, nullptr);
  }
  if (!std::strcmp(method, "pending")) return REAWEB_OK;
  if (!std::strcmp(method, "unregister")) return unregister_service(service);
  return REAWEB_METHOD_NOT_FOUND;
}
}
extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t* rec) {
  if (!rec) { if (handle) unregister_service(handle); handle = 0; return 0; }
  auto add = reinterpret_cast<ReaWeb_RegisterServiceFn>(rec->GetFunc("ReaWeb_RegisterService"));
  unregister_service = reinterpret_cast<ReaWeb_UnregisterServiceFn>(rec->GetFunc("ReaWeb_UnregisterService"));
  complete = reinterpret_cast<ReaWeb_CompleteServiceCallFn>(rec->GetFunc("ReaWeb_CompleteServiceCall"));
  emit = reinterpret_cast<ReaWeb_EmitServiceEventFn>(rec->GetFunc("ReaWeb_EmitServiceEvent"));
  if (!add || !unregister_service || !complete || !emit) return 0;
  ReaWeb_ServiceCallbacks callbacks{sizeof(callbacks), REAWEB_SERVICE_ABI, nullptr, request, nullptr};
  return add("test", &callbacks, &handle) == REAWEB_OK;
}
