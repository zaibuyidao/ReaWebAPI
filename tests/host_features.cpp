#include "core/core.hpp"
#include "runtime/fs.hpp"
#include "core/file_time.hpp"
#include "core/worker.hpp"
#include "core/batch.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
using namespace reaweb;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("Check failed: " #x); } while(false)
template<class F> void rejects(const char* code, F run) {
  try { run(); } catch(const Error& e) { CHECK(e.code == code); return; }
  throw std::runtime_error(std::string("Expected ") + code);
}
static int project_a, project_b, track_a, track_b, writes, begins, ends, refresh;
static Guid guid{};
static void* resolve(const char* name) {
  if (!std::strcmp(name, "EnumProjects")) return reinterpret_cast<void*>(+[](int, char* path, int) -> void* { path[0] = 0; return &project_b; });
  if (!std::strcmp(name, "GetTrack")) return reinterpret_cast<void*>(+[](void* project, int) -> void* { return project == &project_b ? &track_b : &track_a; });
  if (!std::strcmp(name, "GetTrackGUID")) return reinterpret_cast<void*>(+[](void*) -> void* { return guid.data(); });
  if (!std::strcmp(name, "ValidatePtr2")) return reinterpret_cast<void*>(+[](void* project, void* object, const char* type) -> bool {
    if (!std::strcmp(type, "ReaProject*")) return object == &project_a || object == &project_b;
    return (project == &project_b && object == &track_b) || ((project == &project_a || !project) && object == &track_a);
  });
  if (!std::strcmp(name, "SetTrackColor")) return reinterpret_cast<void*>(+[](void*, int color) { if (color == 999) throw Error("NATIVE_ERROR", "mock failure"); ++writes; });
  if (!std::strcmp(name, "GetPlayPositionEx")) return reinterpret_cast<void*>(+[](void* project) -> double { CHECK(!project || project == &project_a); return 12.5; });
  if (!std::strcmp(name, "GetPlayStateEx")) return reinterpret_cast<void*>(+[](void* project) -> int { CHECK(!project || project == &project_a); return 5; });
  return nullptr;
}
int main() { try {
  CHECK(file_time_ticks(0) == "0");
  CHECK(file_time_ticks(123456789) == "123456789");
  CHECK(file_time_ticks(-123456789) == "-123456789");
  CHECK(file_time_ticks(std::numeric_limits<int64_t>::max()) == "9223372036854775807");
  CHECK(file_time_ticks(std::numeric_limits<int64_t>::min()) == "-9223372036854775808");
#if defined(__SIZEOF_INT128__)
  // Exercise the macOS libc++ representation even on Linux/libstdc++ CI.
  using WideTicks = __int128_t;
  const auto beyond64 = static_cast<WideTicks>(std::numeric_limits<int64_t>::max()) + 1;
  CHECK(file_time_ticks(beyond64) == "9223372036854775808");
  CHECK(file_time_ticks(beyond64 + 1) == "9223372036854775809");
  CHECK(file_time_ticks(-beyond64 - 1) == "-9223372036854775809");
  CHECK(file_time_ticks(std::numeric_limits<WideTicks>::max()) == "170141183460469231731687303715884105727");
  CHECK(file_time_ticks(std::numeric_limits<WideTicks>::min()) == "-170141183460469231731687303715884105728");
#endif
  using FileTime = fs::file_time_type;
  const FileTime epoch{};
  CHECK(file_time_ticks(epoch.time_since_epoch().count()) == "0");
  CHECK(file_time_ticks((epoch + FileTime::duration(1)).time_since_epoch().count()) == "1");
  CHECK(file_time_ticks((epoch - FileTime::duration(1)).time_since_epoch().count()) == "-1");
  Host host; host.current_project = []() -> void* { return &project_a; }; host.native_function = resolve;
  host.begin_undo = [](void* project) { CHECK(project == &project_a); ++begins; };
  host.end_undo = [](void* project, const std::string&) { CHECK(project == &project_a); ++ends; };
  host.prevent_refresh = [](int delta) { refresh += delta; };
  Bridge bridge(host, {}, "features");
  auto call = [&](const char* method, Json args) { return bridge.dispatch_request({{"id", 1}, {"method", method}, {"args", args}}); };
  auto foreign_project = call("EnumProjects", {1})["result"][0];
  auto foreign_track = call("GetTrack", {foreign_project, 0})["result"];
  auto own_track = call("GetTrack", {0, 0})["result"];
  Json foreign = {{"method", "SetTrackColor"}, {"args", {foreign_track, 1}}};
  Json own = {{"method", "SetTrackColor"}, {"args", {own_track, 1}}};
  CHECK(call("ReaWeb_Batch", {Json::array({own, foreign}), {{"undoLabel", "scope"}}})["error"]["code"] == "UNSUPPORTED_PROJECT");
  CHECK(writes == 0 && begins == 0);
  rejects("UNSUPPORTED_PROJECT", [&] { bridge.validate_managed_call("SetTrackColor", foreign["args"]); });
  rejects("UNDO_BUSY", [&] { bridge.validate_managed_call("Main_OnCommand", {0, 0}); });
  Json get = {{"method", "GetTrack"}, {"args", {0, 0}}};
  Json dependent = {{"method", "SetTrackColor"}, {"args", {Json{{"$ref", 0}}, 2}}};
  CHECK(call("ReaWeb_Batch", {Json::array({get, dependent}), {{"undoLabel", "dependent"}}})["result"].size() == 2);
  CHECK(writes == 1 && begins == 1 && ends == 1 && refresh == 0);
  dependent["args"][0]["$ref"] = 1;
  CHECK(call("ReaWeb_Batch", {Json::array({get, dependent})})["error"]["code"] == "INVALID_ARGUMENT");
  own["args"][1] = 999;
  auto failed = call("ReaWeb_Batch", {Json::array({get, own}), {{"undoLabel", "failure"}}});
  CHECK(failed["error"]["code"] == "BATCH_FAILED" && failed["error"]["details"]["completed"] == 1);
  CHECK(failed["error"]["details"]["rolledBack"] == false && begins == 2 && ends == 2 && refresh == 0);
  CHECK(batch_methods().size() > 150);
  auto query_batch = [&](Json calls) { return call("ReaWeb_Batch", Json::array({calls})); };
  auto transport = query_batch(Json::array({
    {{"method", "GetPlayPositionEx"}, {"args", {0}}},
    {{"method", "GetPlayStateEx"}, {"args", {0}}}
  }));
  if (transport.contains("error")) throw std::runtime_error(transport.dump());
  CHECK(transport["result"] == Json::array({12.5, 5}));
  Json foreign_query = {{"method", "GetPlayPositionEx"}, {"args", Json::array({foreign_project})}};
  const int previous_writes = writes;
  CHECK(query_batch(Json::array({own, foreign_query}))["error"]["code"] == "UNSUPPORTED_PROJECT");
  CHECK(writes == previous_writes);
  rejects("UNSUPPORTED_PROJECT", [&] { bridge.validate_managed_call("GetPlayPositionEx", Json::array({foreign_project})); });
  CHECK(query_batch(Json::array({
    {{"method", "ValidatePtr2"}, {"args", {0, foreign_track, "MediaTrack*"}}}
  }))["error"]["code"] == "UNSUPPORTED_PROJECT");
  for (const auto* name : {"Main_OnCommand", "GetUserInputs", "EnumInstalledFX", "get_config_var_string",
      "EnumProjects", "SelectProjectInstance", "GetAudioAccessorSamples", "PCM_Source_CreateFromFile",
      "Undo_BeginBlock2", "PreventUIRefresh", "TrackFX_GetEQ", "TakeFX_GetEnvelope"}) {
    CHECK(query_batch(Json::array({{{"method", name}, {"args", Json::array()}}}))["error"]["code"] == "INVALID_ARGUMENT");
  }
  const std::string big(100000, 'x');
  CHECK(parse_request(Json{{"id", 1}, {"method", "ReaWeb_WriteFile"}, {"args", {"large.txt", big}}}.dump())["args"][1] == big);
  CHECK(validate_dev_url("http://127.0.0.1:5173") == "http://127.0.0.1:5173/");
  CHECK(validate_dev_url("http://[::1]:5173/app/") == "http://[::1]:5173/app/");
  rejects("INVALID_URL", [&] { validate_dev_url(std::string("http://localhost:5173/") + '\0'); });
  for (const auto* url : {"https://example.com/", "http://localhost.evil:5173/", "http://localhost:0/", "http://localhost:65536/", "http://user@localhost:5173/"})
    rejects("INVALID_URL", [&] { validate_dev_url(url); });
  for (const auto* url : {"javascript:alert(1)", "file:///tmp/a", "https://example.com/\n", "http://", "mailto:"})
    rejects("INVALID_URL", [&] { validate_external_url(url); });
  validate_external_url("https://www.reaper.fm/"); validate_external_url("mailto:dev@example.com");

  auto root = fs::current_path() / ("host-test-" + std::to_string(Clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  auto io = [&](const char* method, Json args) { return file_call(root, method, args); };
  CHECK(io("ReaWeb_Stat", {"absent"})["exists"] == false);
  CHECK(io("ReaWeb_MakeDirectory", {"sub/inner", {{"recursive", true}}}) == true);
  CHECK(io("ReaWeb_WriteFile", {u8"sub/文本.txt", big})["bytes"] == big.size());
  CHECK(io("ReaWeb_ReadFile", {u8"sub/文本.txt"}) == big);
  rejects("FILE_EXISTS", [&] { io("ReaWeb_WriteFile", {u8"sub/文本.txt", "new"}); });
  io("ReaWeb_WriteFile", {u8"sub/文本.txt", u8"中文\n", {{"overwrite", true}}});
  CHECK(io("ReaWeb_ReadFile", {u8"sub/文本.txt"}) == u8"中文\n");
  std::string binary("\0\xff\xfe", 3);
  io("ReaWeb_WriteFile", {"binary", encode_binary(binary.data(), binary.size()), {{"encoding", "binary"}}});
  CHECK(decode_binary(io("ReaWeb_ReadFile", {"binary", {{"encoding", "binary"}}})) == binary);
  rejects("FILE_ENCODING", [&] { io("ReaWeb_ReadFile", {"binary"}); });
  rejects("INVALID_PATH", [&] { io("ReaWeb_ReadFile", {"https://example.com/data"}); });
  CHECK(io("ReaWeb_ReadDirectory", {"sub"}).size() == 2);
  rejects("FILE_NOT_FOUND", [&] { io("ReaWeb_ReadFile", {"missing"}); });
  fs::remove_all(root);

  std::cout << "Host features: project scope, dependent batch cleanup, files, URLs, large messages passed\n";
  return 0;
} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
