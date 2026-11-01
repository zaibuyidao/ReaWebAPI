#include "runtime/runtime.hpp"
#include <cmath>
#include <fstream>

namespace reaweb {
Runtime::JsfxSize& Runtime::jsfx_size(const JsfxAssociation& association) {
  const auto key = association.ident + "\n" + association.entry.generic_u8string();
  auto found = jsfx_sizes_.find(key);
  if (found != jsfx_sizes_.end()) {
    if (!found->second.size.exact) found->second.size = {association.width, association.height};
    return found->second;
  }
  if (jsfx_sizes_.size() >= 1024) throw Error("RESOURCE_LIMIT", "Too many JSFX editor size preferences");
  JsfxSize value{{association.width, association.height}};
  const auto path = resource_ / "ReaWebAPI" / "JSFXEditorSizes" / (state_key(key, 0) + ".json");
  try {
    std::error_code error;
    if (fs::file_size(path, error) <= 4096 && !error) {
      std::ifstream stream(path);
      const auto saved = Json::parse(stream);
      if (saved.value("schema", Json()) == 1 && saved.value("key", Json()) == key &&
          saved.value("width", Json()).is_number_integer() && saved.value("height", Json()).is_number_integer() &&
          saved["width"] >= 100 && saved["width"] <= 8192 && saved["height"] >= 100 && saved["height"] <= 8192)
        value.size = {saved["width"].get<int>(), saved["height"].get<int>(), true};
    }
  } catch (const std::exception&) {}
  return jsfx_sizes_.emplace(key, value).first->second;
}
Json Runtime::jsfx_size_state(Session& session) {
  const auto association = jsfx_association(resource_ / "Effects", session.jsfx->instance->binding->ident());
  if (!association) throw Error("FX_UNBOUND", "The JSFX Web UI association is unavailable");
  const auto& value = jsfx_size(*association);
  return {{"width", value.size.width}, {"height", value.size.height}, {"customized", value.size.exact},
    {"revision", std::to_string(value.revision)}};
}
void Runtime::initialize_jsfx(JsfxDiscovery discovery) {
  check_thread();
  if (jsfx_service_) return;
  jsfx_discovery_ = std::move(discovery);
  ReaWeb_ServiceCallbacks callbacks{sizeof(ReaWeb_ServiceCallbacks), REAWEB_SERVICE_ABI, this,
    [](void* context, uint64_t handle, uint64_t request, int window, const char* method, const char* payload) -> int {
      auto& runtime = *static_cast<Runtime*>(context);
      try {
        if (!request) return REAWEB_INVALID_ARGUMENT;
        const auto result = runtime.jsfx_result(window, method, Json::parse(payload));
        if (request) return runtime.services_.complete(handle, request, result.dump().c_str(), REAWEB_OK, nullptr);
        return REAWEB_OK;
      } catch (const std::exception& error) {
        if (request) return runtime.services_.complete(handle, request, nullptr, REAWEB_SERVICE_ERROR, error.what());
        return REAWEB_SERVICE_ERROR;
      }
    }, nullptr};
  if (services_.add("jsfx", &callbacks, &jsfx_service_) != REAWEB_OK) throw Error("SERVICE_EXISTS", "Cannot register the JSFX service");
  next_jsfx_discovery_ = Clock::now();
}
void Runtime::end_jsfx_edit(Session& session) {
  if (!session.jsfx) return;
  auto& ui = *session.jsfx;
  for (int index : ui.editing) { try { ui.instance->binding->end(index); } catch (...) {} }
  ui.editing.clear(); ui.gesture_id.clear(); ui.group_edit = false;
  if (!ui.undo.empty()) {
    if (undo_owner_ == session.id) finish_undo();
    ui.undo.clear();
  }
}
void Runtime::release_jsfx(Session& session) {
  release_daw_space(session);
  if (!session.jsfx) return;
  if (!session.diagnostic_jsfx_released) trace(session, "jsfx.release", Diagnostics::Level::Info);
  session.diagnostic_jsfx_released = true;
  end_jsfx_edit(session);
  if(session.jsfx->instance->native)session.jsfx->instance->native->close_window(session.id);
  auto host = session.jsfx->editor.host;
  if (host) { host->on_destroy({}); host->restore(); }
  if (session.window && !session.window->closed()) session.window->set_visible(false);
  session.jsfx->initialized = false;
  session.jsfx->presented = false;
}
void Runtime::park_jsfx(Session& session) {
  auto& ui = *session.jsfx;
  if (!ui.editor.host) return;
  trace(session, "jsfx.park", Diagnostics::Level::Info);
  release_daw_space(session);
  end_jsfx_edit(session);
  if(ui.instance->native)ui.instance->native->close_window(session.id);
  auto host = ui.editor.host;
  host->on_destroy({});
  session.window->reparent_host(nullptr);
  host->park();
  ui.editor = {};
  ui.presented = false; ui.initialized = false;
  ui.last_used = Clock::now();
  next_jsfx_discovery_ = {};
  ++ui.activation;
  if (session.ready) service_event(jsfx_service_, "jsfx", session.id, "visibilityChanged", {{"sessionId", ui.instance->session_id}, {"active", false}, {"activation", ui.activation}});
}
Json Runtime::jsfx_state(Session& session) {
  session.startup_timing->mark(StartupTiming::ParametersStart);
  auto& ui = *session.jsfx;
  auto state = ui.instance->binding->state();
  if (ui.editor.host && ui.instance->binding->editor_matches(ui.editor))
    ui.editor.host->read_parameter_metadata(host_, ui.instance->binding->resolve(), state["parameters"]);
  if(ui.instance->native)for(auto& parameter:state["parameters"])
    if(parameter["index"]==ui.instance->native->parameter()) {parameter["reserved"]=true;parameter["readOnly"]=true;}
  ui.count = state["count"]; ui.initialized = bool(ui.editor.host);
  ui.scan = 0; ui.changes = Json::array(); ui.values = Json::array();
  for (const auto& p : state["parameters"])
    ui.values.push_back({{"index", p["index"]}, {"value", p["value"]}, {"normalized", p["normalized"]}, {"formatted", p["formatted"]}});
  state["sessionId"] = ui.instance->session_id;
  state["revision"] = std::to_string(++ui.instance->revision);
  state["activation"] = ui.activation;
  state["active"] = bool(ui.editor.host);
  session.startup_timing->mark(StartupTiming::Parameters);
  return state;
}
void Runtime::attach_jsfx(Session& session, const JsfxEditor& editor) {
  trace(session, "jsfx.attach.begin");
  session.diagnostic_jsfx_released = false;
  auto& ui = *session.jsfx;
  if (session.ready) {
    session.startup_timing->reset("cachedReopen", ui.discovered);
    session.startup_timing->mark(StartupTiming::Discovered, ui.discovered);
  }
  session.window->reparent_host(editor.host->parent());
  session.startup_timing->mark(StartupTiming::ReparentReady);
  ui.editor = editor; ++ui.activation;
  ui.presented = false; ui.last_used = Clock::now();
  if (session.ready) editor.host->set_background(session.window->background_color());
  editor.host->present(session.window->native_handle());
  session.startup_timing->mark(StartupTiming::HostAttached);
  std::weak_ptr<Session> weak = sessions_.at(session.id);
  std::weak_ptr<JsfxEditorHost> expected = editor.host;
  editor.host->on_destroy([this, weak, expected] {
    if (auto s = weak.lock(); s && !s->closing && s->jsfx && s->jsfx->editor.host == expected.lock()) {
      try { park_jsfx(*s); }
      catch (...) { release_jsfx(*s); s->closing = true; }
    }
  });
  if (session.ready) service_event(jsfx_service_, "jsfx", session.id, "visibilityChanged",
    {{"sessionId", ui.instance->session_id}, {"active", true}, {"activation", ui.activation}, {"paramState", jsfx_state(session)},
      {"editorSize", jsfx_size_state(session)}});
  if (session.ready) session.startup_timing->mark(StartupTiming::StateNotified);
  trace(session, "jsfx.attach.end", Diagnostics::Level::Info);
}
Json Runtime::jsfx_result(int window, const std::string& method, const Json& payload) {
  Json result;
  auto it = sessions_.find(window);
  if (it != sessions_.end() && it->second->jsfx) result["sessionId"] = it->second->jsfx->instance->session_id;
  try { result["data"] = jsfx_call(window, method, payload); result["ok"] = true; }
  catch (const Error& error) {
    if (it != sessions_.end()) trace(*it->second, "jsfx.api.fail", Diagnostics::Level::Error, error.code.c_str(),
      Diagnostics::Scope::Runtime, {{"method", method}});
    result["ok"] = false; result["error"] = {{"code", error.code}, {"message", error.what()}};
    if (!error.details.is_null()) result["error"]["details"] = error.details;
  } catch (const std::exception& error) {
    result["ok"] = false; result["error"] = {{"code", "SERVICE_ERROR"}, {"message", error.what()}};
  }
  return result;
}
Json Runtime::jsfx_call(int window, const std::string& method, const Json& payload) {
  check_thread();
  auto it = sessions_.find(window);
  if (it == sessions_.end() || !it->second->jsfx || it->second->closing || it->second->window->closed())
    throw Error("FX_UNBOUND", "The JSFX service requires a bound Hosted WebView");
  auto& session = *it->second; auto& ui = *session.jsfx;
  if (method == "getCapabilities") session.startup_timing->mark(StartupTiming::Connected);
  if (!payload.is_object()) throw Error("INVALID_ARGUMENT", "Expected a JSFX request object");
  ui.instance->binding->resolve();
  if(method=="getCapabilities" && ui.instance->native) {
    auto capabilities = ui.instance->native->capabilities(ui.instance->session_id);
    capabilities["features"]["editorSize"] = true;
    session.startup_timing->mark(StartupTiming::CapabilitiesReady);
    return capabilities;
  }
  if (method == "getCapabilities") {
    Json capabilities = {
    {"protocolVersion", 1}, {"sessionId", ui.instance->session_id}, {"capabilityRevision", "1"}, {"status", "parameterOnly"},
    {"streams", Json::array()}, {"dataStates", Json::array()}, {"commands", Json::array()}, {"resources", Json::array()},
    {"features", {{"params", true}, {"paramGroups", true}, {"nativeChannel", false}, {"editorSize", true}}},
    {"limits", {{"maxParametersPerGesture", 259}}}};
    session.startup_timing->mark(StartupTiming::CapabilitiesReady);
    return capabilities;
  }
  if (payload.value("sessionId", Json()) != ui.instance->session_id) throw Error("STALE_SESSION", "The JSFX instance session changed");
  if (method == "getEditorSize") {
    session.startup_timing->mark(StartupTiming::EditorSizeStart);
    auto size = jsfx_size_state(session);
    session.startup_timing->mark(StartupTiming::EditorSize);
    return size;
  }
  if (method == "setEditorSize") {
    session.startup_timing->mark(StartupTiming::SizeRequested);
    if (!ui.editor.host || !ui.instance->binding->editor_matches(ui.editor) || payload.value("activation", Json()) != ui.activation)
      throw Error("FX_STALE", "The JSFX editor activation changed");
    for (const auto* name : {"width", "height"})
      if (!payload.value(name, Json()).is_number_integer() || payload[name] < 100 || payload[name] > 8192)
        throw Error("INVALID_ARGUMENT", "Expected integer JSFX content dimensions in [100,8192]");
    for (const auto& field : payload.items())
      if (field.key() != "sessionId" && field.key() != "activation" && field.key() != "width" && field.key() != "height")
        throw Error("INVALID_ARGUMENT", "Unexpected JSFX editor size field");
    const auto association = jsfx_association(resource_ / "Effects", ui.instance->binding->ident());
    if (!association) throw Error("FX_UNBOUND", "The JSFX Web UI association is unavailable");
    auto& value = jsfx_size(*association);
    const int width = payload["width"], height = payload["height"];
    if (!value.size.exact || width != value.size.width || height != value.size.height) {
      const auto key = association->ident + "\n" + association->entry.generic_u8string();
      Work save; save.kind = Work::Save;
      save.path = resource_ / "ReaWebAPI" / "JSFXEditorSizes" / (state_key(key, 0) + ".json");
      save.data = {{"schema", 1}, {"key", key}, {"width", width}, {"height", height}};
      if (!worker_.submit(std::move(save))) throw Error("QUEUE_LIMIT", "JSFX editor size save queue is full");
      value.size = {width, height, true}; ++value.revision;
    }
    const auto state = jsfx_size_state(session);
    for (auto& item : sessions_) {
      auto& other = *item.second;
      if (!other.jsfx || other.closing || other.entry != session.entry) continue;
      try {
        if (other.jsfx->instance->binding->ident() != association->ident) continue;
        auto& editor = other.jsfx->editor;
        if (editor.host && editor.host->valid() && other.jsfx->instance->binding->editor_matches(editor))
          editor.host->request_size(width, height, true);
      } catch (const Error&) { continue; }
      service_event(jsfx_service_, "jsfx", other.id, "editorSizeChanged",
        {{"sessionId", other.jsfx->instance->session_id}, {"size", state}});
    }
    return state;
  }
  if (method == "getParamState") {
    if (ui.editor.host && !ui.instance->binding->editor_matches(ui.editor)) park_jsfx(session);
    return jsfx_state(session);
  }
  if (method == "getDiagnostics") {
    Json result{{"sessionId",ui.instance->session_id},{"nativeChannel",bool(ui.instance->native)},
      {"status",ui.instance->native?ui.instance->native->capabilities(ui.instance->session_id)["status"]:Json("parameterOnly")},
      {"active",bool(ui.editor.host)},{"gestureActive",!ui.gesture_id.empty()},{"presentationMs",ui.presentation_ms}};
    if(ui.instance->native)result["native"]=ui.instance->native->diagnostics();
    if(!ui.instance->native_error.empty())result["lastError"]=ui.instance->native_error;
    return result;
  }
  static const std::set<std::string> extensions{
    "openStream", "getDataState", "setDataState", "invokeCommand", "releaseCommands", "getOperation", "findOperation", "cancelOperation",
    "beginResourceUpload", "commitResourceUpload", "abortResourceUpload", "openResourceDownload", "closeResourceDownload"};
  if (extensions.count(method)) {
    if(!ui.instance->native)throw Error("UNSUPPORTED", "This JSFX has no verified native data channel");
    const bool read=method=="getDataState" || method=="getOperation" || method=="findOperation";
    const bool cleanup=method=="cancelOperation" || method=="abortResourceUpload" || method=="closeResourceDownload" || method=="releaseCommands";
    if(!read && !cleanup && (!ui.editor.host || !ui.instance->binding->editor_matches(ui.editor) || payload.value("activation",Json())!=ui.activation))
      throw Error("FX_STALE", "The JSFX editor activation changed");
    return ui.instance->native->call(method,payload,{session.id,session.generation,ui.instance->session_id,session.app->origin});
  }
  const bool begin = method == "beginParamEdit" || method == "beginParamEditGroup";
  const bool end = method == "endParamEdit" || method == "endParamEditGroup";
  const bool group = method == "beginParamEditGroup" || method == "endParamEditGroup" || method == "setParams";
  if (end) {
    if (ui.gesture_id.empty()) return {{"ended", false}};
    if (payload.value("gestureId", Json()) != ui.gesture_id || group != ui.group_edit)
      throw Error("GESTURE_BUSY", "The parameter gesture belongs to another request");
    end_jsfx_edit(session); return {{"ended", true}};
  }
  if (method != "present" && !begin && method != "setParam" && method != "setParams")
    throw Error("UNKNOWN_METHOD", "Unknown JSFX service method");
  if (method == "present" && (!ui.editor.host || payload.value("activation", Json()) != ui.activation)) return {{"presented", false}};
  if (!ui.instance->binding->editor_matches(ui.editor) || payload.value("activation", Json()) != ui.activation)
    throw Error("FX_STALE", "The JSFX editor activation changed");
  if (!ui.initialized) throw Error("FX_NOT_READY", "Read the host state before editing parameters");
  if (method == "present") {
    session.startup_timing->mark(StartupTiming::Present);
    ui.editor.host->set_background(session.window->background_color());
    ui.presentation_ms = std::chrono::duration<double, std::milli>(Clock::now() - ui.last_used).count();
    ui.presented = true; session.window->set_visible(true);
    if (session.window->visible()) { ui.editor.host->reveal(); session.startup_timing->mark(StartupTiming::Revealed); }
    return {{"presented", true}};
  }
  for (auto p = payload.begin(); p != payload.end(); ++p) {
    if (p.key() == "sessionId" || p.key() == "activation" || (!begin && p.key() == "gestureId")) continue;
    if ((begin && p.key() == (group ? "indices" : "index")) ||
        (!begin && (group ? p.key() == "values" : p.key() == "index" || p.key() == "normalized"))) continue;
    throw Error("INVALID_ARGUMENT", "Unexpected JSFX parameter field");
  }
  const auto index = [&](const Json& value) {
    if (!value.is_number_integer() || value.get<double>() < 0 || value.get<double>() >= ui.instance->binding->parameter_count())
      throw Error("INVALID_ARGUMENT", "Invalid JSFX parameter index");
    if(ui.instance->native && value==ui.instance->native->parameter())throw Error("INVALID_ARGUMENT", "The JSFX transport parameter is reserved");
    return value.get<int>();
  };
  std::vector<int> indices;
  Json values = Json::array();
  if (begin) {
    if (!ui.gesture_id.empty()) throw Error("GESTURE_BUSY", "Finish the active parameter gesture first");
    auto input = group ? payload.value("indices", Json()) : Json::array({payload.value("index", Json())});
    if (!input.is_array() || input.empty() || input.size() > 259) throw Error("INVALID_ARGUMENT", "Expected bounded parameter indices");
    for (const auto& item : input) indices.push_back(index(item));
  } else {
    values = group ? payload.value("values", Json()) : Json::array({{{"index", payload.value("index", Json())}, {"normalized", payload.value("normalized", Json())}}});
    if (!values.is_array() || values.empty() || values.size() > 259) throw Error("INVALID_ARGUMENT", "Expected bounded parameter values");
    for (const auto& item : values) {
      if (!item.is_object() || item.size() != 2) throw Error("INVALID_ARGUMENT", "Expected index and normalized fields");
      indices.push_back(index(item.value("index", Json())));
      const auto value = item.value("normalized", Json());
      if (!value.is_number() || !std::isfinite(value.get<double>()) || value < 0 || value > 1)
        throw Error("INVALID_ARGUMENT", "Expected a normalized value in [0,1]");
    }
  }
  if (std::set<int>(indices.begin(), indices.end()).size() != indices.size()) throw Error("INVALID_ARGUMENT", "Duplicate parameter index");
  if (begin) {
    ui.undo = transaction_call(window, "ReaWeb_BeginUndo", Json::array({"JSFX Web UI parameter"})).get<std::string>();
    ui.editing = indices; ui.group_edit = group; ui.gesture_id = external_token(); ui.last_edit = Clock::now();
    return {{"gestureId", ui.gesture_id}};
  }
  const bool discrete = ui.gesture_id.empty();
  if (discrete && (group || payload.contains("gestureId"))) throw Error("GESTURE_BUSY", "The parameter gesture has ended");
  if (!discrete) {
    if (payload.value("gestureId", Json()) != ui.gesture_id || group != ui.group_edit)
      throw Error("GESTURE_BUSY", "The parameter gesture belongs to another request");
    for (int param : indices) if (std::find(ui.editing.begin(), ui.editing.end(), param) == ui.editing.end())
      throw Error("INVALID_ARGUMENT", "Parameter is not part of the active gesture");
  }
  if (discrete) ui.undo = transaction_call(window, "ReaWeb_BeginUndo", Json::array({"JSFX Web UI parameter"})).get<std::string>();
  Json applied = Json::array(), parameters = Json::array();
  try {
    for (const auto& item : values) {
      int param = item["index"];
      ui.instance->binding->set(param, item["normalized"].get<double>(), true);
      applied.push_back(param);
      auto parameter = ui.instance->binding->parameter(param); parameters.push_back(parameter);
      for (auto& change : ui.changes) if (change["index"] == param) change = parameter;
    }
  } catch (...) {
    if (discrete) { try { ui.instance->binding->end(indices.front()); } catch (...) {} }
    end_jsfx_edit(session); ui.next_poll = {};
    if (group && !applied.empty()) throw Error("PARTIAL_WRITE", "Some parameters were applied before the host rejected a write", {{"applied", applied}});
    throw;
  }
  ui.last_edit = Clock::now();
  if (undo_owner_ == window) undo_deadline_ = Clock::now() + std::chrono::seconds(30);
  if (discrete) { ui.instance->binding->end(indices.front()); end_jsfx_edit(session); }
  ui.next_poll = Clock::now();
  return {{group ? "applied" : "parameter", group ? parameters : parameters.front()}, {"revision", std::to_string(++ui.instance->revision)}};
}
void Runtime::tick_jsfx() {
  if (!jsfx_service_) return;
  for(auto& instance:jsfx_instances_)if(instance->native) {
    try {
      const bool changed=instance->native->tick(*instance->binding,[this](const std::string& label) {
        if(undo_owner_)return false;
        transaction_call(-1,"ReaWeb_BeginUndo",Json::array({label}));return true;
      },[this] {if(undo_owner_==-1)finish_undo();});
      if(undo_owner_==-1)undo_deadline_=Clock::now()+std::chrono::seconds(30);
      if(changed) {
        for (auto& item : sessions_) if (item.second->jsfx && item.second->jsfx->instance == instance)
          trace(*item.second, "jsfx.native.session.previous", Diagnostics::Level::Info);
        instance->session_id=external_token();instance->revision=0;
        for(auto& item:sessions_)if(item.second->jsfx && item.second->jsfx->instance==instance) {
          trace(*item.second, "jsfx.native.session.changed", Diagnostics::Level::Info);
          end_jsfx_edit(*item.second);
          service_event(jsfx_service_,"jsfx",item.first,"capabilitiesChanged",
            {{"sessionId",instance->session_id},{"capabilityRevision",instance->native->capabilities(instance->session_id)["capabilityRevision"]},{"reason","sessionChanged"}});
        }
      }
      for(auto& event:instance->native->events()) {
        if(event.first=="projectChanged") {
          using Dirty=void(*)(void*);
          if(auto fn=host_.native_function?reinterpret_cast<Dirty>(host_.native_function("MarkProjectDirty")):nullptr)fn(instance->binding->project());
          continue;
        }
        event.second["sessionId"]=instance->session_id;
        for(auto& item:sessions_)if(item.second->jsfx && item.second->jsfx->instance==instance && !item.second->closing)
          service_event(jsfx_service_,"jsfx",item.first,event.first,event.second);
      }
    } catch(const Error& error) {
      if (instance->native_error != error.code)
        for (auto& item : sessions_) if (item.second->jsfx && item.second->jsfx->instance == instance)
          trace(*item.second, "jsfx.native.fail", Diagnostics::Level::Warn, error.code.c_str());
      instance->native_error=error.code;
      if(error.code=="FX_STALE")instance->native.reset();
    }
      catch(const std::exception& error) {instance->native_error=error.what();}
  }
  for(auto it=jsfx_instances_.begin();it!=jsfx_instances_.end();) {
    const bool used=std::any_of(sessions_.begin(),sessions_.end(),[&](const auto& item){return item.second->jsfx && item.second->jsfx->instance==*it;});
    if(used)(*it)->last_used=Clock::now();
    if(!used && (*it)->native && (*it)->native->idle() && Clock::now()-(*it)->last_used>=std::chrono::seconds(60))it=jsfx_instances_.erase(it);
    else ++it;
  }
  if(jsfx_native_)jsfx_native_->collect();
  const auto now = Clock::now(), deadline = now + std::chrono::milliseconds(1);
  auto item = sessions_.upper_bound(jsfx_poll_cursor_);
  const auto host_revision = host_.event_revision ? host_.event_revision("fx-changed") : 0;
  for (size_t remaining = sessions_.size(); remaining; --remaining) {
    if (item == sessions_.end()) item = sessions_.begin();
    auto& s = *item++->second;
    if (!s.jsfx) continue;
    auto& ui = *s.jsfx;
    if (s.closing || s.window->closed()) { release_jsfx(s); continue; }
    try {
      ui.instance->binding->resolve();
      if (ui.editor.host && !ui.instance->binding->editor_matches(ui.editor)) {
        if (ui.editor.host->retain_during_switch()) { end_jsfx_edit(s); continue; }
        park_jsfx(s);
      }
      if (!ui.editor.host) {
        if (now - ui.last_used > std::chrono::seconds(60)) s.closing = true;
        continue;
      }
      if (s.ready) ui.editor.host->set_background(s.window->background_color());
      ui.editor.host->present(s.window->native_handle());
      if (ui.presented && s.window->visible()) { ui.editor.host->reveal(); s.startup_timing->mark(StartupTiming::Revealed); }
      if (!ui.presented && now - ui.last_used > std::chrono::seconds(15))
        throw Error("FX_NOT_READY", "JSFX page did not acknowledge its host state within 15 seconds");
      if (!ui.editing.empty() && now - ui.last_edit > std::chrono::seconds(30)) end_jsfx_edit(s);
      if (ui.host_revision != host_revision) { ui.host_revision = host_revision; trace(s, "jsfx.host.revision.changed"); ui.next_poll = now; }
      if (!s.ready || !ui.initialized || !s.window->visible() || now < ui.next_poll || Clock::now() >= deadline || s.output_pending >= 8) continue;
      jsfx_poll_cursor_ = s.id;
      if (!ui.scan) {
        const auto count = ui.instance->binding->parameter_count();
        const bool changed = count != ui.count;
        ui.count = count;
        if (changed) trace(s, "jsfx.parameter.count.changed", Diagnostics::Level::Info);
      }
      for (int n = 0; ui.scan < ui.count && n < 32 && Clock::now() < deadline; ++n, ++ui.scan) {
        auto value = ui.instance->binding->parameter(ui.scan);
        if (ui.values.size() <= static_cast<size_t>(ui.scan)) { ui.values.push_back(value); ui.changes.push_back(value); }
        else if (ui.values[ui.scan] != value) { ui.values[ui.scan] = value; ui.changes.push_back(value); }
      }
      if (ui.scan >= ui.count) {
        if (!ui.changes.empty()) service_event(jsfx_service_, "jsfx", s.id, "paramsChanged", {{"sessionId", ui.instance->session_id}, {"parameters", ui.changes}, {"revision", std::to_string(++ui.instance->revision)}, {"activation", ui.activation}});
        ui.changes = Json::array(); ui.scan = 0; ui.next_poll = now + std::chrono::milliseconds(33);
      }
    } catch (const Error& error) {
      if (error.code == "FX_STALE") {
        trace(s, "jsfx.binding.invalid", Diagnostics::Level::Warn, "FX_STALE");
        jsfx_instances_.erase(std::remove(jsfx_instances_.begin(), jsfx_instances_.end(), ui.instance), jsfx_instances_.end());
        release_jsfx(s); s.closing = true;
      }
      else fail(s, error.what(), error.code.c_str());
    } catch (const std::exception& error) { fail(s, error.what()); }
  }
  for (auto it = jsfx_blocked_.begin(); it != jsfx_blocked_.end();) {
    if (!it->second->valid()) it = jsfx_blocked_.erase(it); else ++it;
  }
  const auto trim_cache = [&] {
    std::vector<Session*> inactive;
    for (auto& entry : sessions_) if (entry.second->jsfx && !entry.second->closing && !entry.second->jsfx->editor.host) inactive.push_back(entry.second.get());
    std::sort(inactive.begin(), inactive.end(), [](const Session* a, const Session* b) { return a->jsfx->last_used > b->jsfx->last_used; });
    // Allow one handoff slot while REAPER replaces the native editor.
    if (inactive.size() == 5 && now - inactive.front()->jsfx->last_used < std::chrono::milliseconds(500)) return;
    for (size_t n = 4; n < inactive.size(); ++n) inactive[n]->closing = true;
  };
  if (!jsfx_discovery_ || (now < next_jsfx_discovery_ && !jsfx_discovery_->pending())) { trim_cache(); return; }
  next_jsfx_discovery_ = now + std::chrono::milliseconds(200);
  for (auto editor : jsfx_discovery_->scan(host_, [this](JsfxTarget target, int fx) -> std::optional<JsfxEditorDiscovery::Size> {
      try {
        JsfxBinding binding(host_, target, fx);
        if (auto association = jsfx_association(resource_ / "Effects", binding.ident()))
          return jsfx_size(*association).size;
      } catch (...) {}
      return {};
    })) {
    const auto discovered = Clock::now();
    if (sessions_.size() >= 32) break;
    try {
      auto binding = std::make_unique<JsfxBinding>(host_, editor.target(), editor.fx);
      if (!binding->editor_matches(editor)) continue;
      const auto binding_ready = Clock::now();
      const auto association = jsfx_association(resource_ / "Effects", binding->ident());
      if (!association) continue;
      const auto association_ready = Clock::now();
      editor.host->prepare();
      const auto host_prepared = Clock::now();
      const auto size = jsfx_size(*association).size;
      editor.host->request_size(size.width, size.height, size.exact);
      const auto size_restored = Clock::now();
      Session* cached = nullptr;
      for (auto& entry : sessions_) {
        auto& candidate = *entry.second;
        if (!candidate.jsfx || candidate.closing || candidate.window->closed() || candidate.jsfx->editor.host) continue;
        auto& previous = *candidate.jsfx->instance->binding;
        if (previous.project() == binding->project() && previous.target() == binding->target() && previous.guid() == binding->guid() &&
            candidate.entry == association->entry && candidate.jsfx->daw_keyboard == association->daw_keyboard) { cached = &candidate; break; }
      }
      if (cached) {
        const auto matched = Clock::now();
        cached->jsfx->discovered = discovered; attach_jsfx(*cached, editor);
        auto& timing = *cached->startup_timing;
        timing.mark(StartupTiming::BindingReady, binding_ready);
        timing.mark(StartupTiming::AssociationReady, association_ready);
        timing.mark(StartupTiming::HostPrepared, host_prepared);
        timing.mark(StartupTiming::SizeRestored, size_restored);
        timing.mark(StartupTiming::CacheMatched, matched);
        continue;
      }
      auto ui = std::make_shared<Session::Jsfx>();
      ui->daw_keyboard = association->daw_keyboard;
      ui->discovered = discovered;
      for (auto identity = jsfx_instances_.begin(); identity != jsfx_instances_.end();) {
        try { (*identity)->binding->resolve(); }
        catch (const Error&) { identity = jsfx_instances_.erase(identity); continue; }
        auto& previous = *(*identity)->binding;
        if (previous.project() == binding->project() && previous.target() == binding->target() && previous.guid() == binding->guid())
          ui->instance = *identity;
        ++identity;
      }
      if (!ui->instance) {
        if (jsfx_instances_.size() >= 1024) throw Error("RESOURCE_LIMIT", "Too many JSFX instance sessions");
        ui->instance = std::make_shared<JsfxInstance>(); ui->instance->binding = std::move(binding);
        if(!association->native.is_null()) {
          try {
            const JsfxNativeDeclaration declaration(association->native);
            if(!jsfx_native_)jsfx_native_=std::make_unique<JsfxNativeHub>(host_,streams_);
            ui->instance->native=std::make_unique<JsfxControl>(jsfx_native_->create(declaration,*ui->instance->binding));
          } catch(const Error& error) {ui->instance->native_error=error.code;log_(std::string("JSFX native channel: ")+error.what());}
        }
        jsfx_instances_.push_back(ui->instance);
      }
      ui->editor = editor;
      open_impl(association->entry.u8string(), {}, "", "", true, false, true, false, ui);
    } catch (const std::exception& error) {
      editor.host->restore(); jsfx_blocked_[editor.host->parent()] = editor.host;
      diagnostic_log_.event(Diagnostics::Level::Error, Diagnostics::Scope::Runtime, "jsfx.discovery.fail", {}, 0, "JSFX_ERROR");
      log_(std::string("JSFX Web UI: ") + error.what());
    }
  }
  trim_cache();
}
}
