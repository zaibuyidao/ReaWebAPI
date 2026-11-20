#include "core/core.hpp"
#include "platform/shared/navigation.hpp"
#include <fstream>
#include <iostream>

using namespace reaweb;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("Check failed: " #condition); } while (false)
struct Fixture {
  bool closed = false, inspected = false, docked = false;
  Host host;
  std::unique_ptr<Bridge> bridge;
  Fixture() {
    host.current_project = []() -> void* { return nullptr; };
    bridge = std::make_unique<Bridge>(host, Bridge::Controls{
      [](const std::string& path) { CHECK(path == "Other/index.html"); return 7; },
      [&] { closed = true; }, [&] { inspected = true; },
      [&](bool value) { return docked = value; }, [&] { return docked; }
    });
  }
  Json call(const char* method, std::vector<Json> args = {}) {
    return bridge->dispatch(Json{{"id", 42}, {"document", "page-one"}, {"method", method}, {"args", args}}.dump());
  }
};
int main() {
  try {
    Fixture f;
    const auto caps = f.call("ReaWeb_GetCapabilities").at("result");
    CHECK(!caps.contains("api") && !caps.contains("batchMethods"));
    for (const auto* name : {"CountTracks", "ReaWeb_Batch", "ReaWeb_SetBufferSize"})
      CHECK(f.call(name)["error"]["code"] == "UNKNOWN_API");
    CHECK(f.call("ReaWeb_GetCapabilities")["document"] == "page-one");
    CHECK(f.bridge->dispatch("{bad")["error"]["code"] == "INVALID_REQUEST");
    CHECK(f.bridge->dispatch(std::string(64*1024*1024+1, 'x'))["error"]["code"] == "MESSAGE_LIMIT");
    CHECK(f.bridge->dispatch(std::string(1000, '[') + std::string(1000, ']'))["error"]["code"] == "INVALID_REQUEST");
    CHECK(f.bridge->dispatch("{\"id\":9007199254740992}")["error"]["code"] == "INVALID_REQUEST");
    CHECK(f.call("ReaWeb_Open", {"Other/index.html"})["result"] == 7);
    CHECK(f.call("ReaWeb_Open", {42})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.call("ReaWebOpen", {"Other/index.html"})["error"]["code"] == "UNKNOWN_API");
    f.call("ReaWeb_DevTools"); f.call("ReaWeb_Close");
    CHECK(f.inspected && f.closed);
    CHECK(f.call("ReaWeb_SetDocked", {true})["result"] == true);
    CHECK(f.call("ReaWeb_IsDocked")["result"] == true);
    CHECK(f.call("ReaWeb_SetDocked", {false})["result"] == false);
    CHECK(f.call("ReaWeb_SetDocked", {1})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.call("ReaWeb_GetCapabilities")["result"]["runtime"]["window"]["hiddenStart"] == true);
    CHECK(f.call("ReaWeb_OpenEx", {"index.html", {{"hidden", 1}}})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.call("ReaWeb_OpenEx", {"index.html", {{"id", "child"}}})["error"]["code"] == "INVALID_ARGUMENT");
    CHECK(f.call("ReaWeb_GetCapabilities")["result"]["runtime"]["contract"] == 2);
    CHECK(f.call("ReaWeb_GetCapabilities")["result"]["runtime"]["namespaces"] ==
      Json({"window", "theme", "dialog", "events", "lifecycle", "debug", "fs", "audio",
            "clipboard", "dragDrop", "app", "system", "transaction", "host", "stream", "realtime", "video"}));
    CHECK(f.call("ReaWeb_GetCapabilities")["result"]["runtime"]["reservedNamespaces"] == Json::array());
    CHECK(f.call("ReaWeb_GetCapabilities")["result"]["version"] == REAWEB_VERSION);
    CHECK(same_document("file:///a/index.html#x", "file:///a/index.html"));
    CHECK(!same_document("file:///a/evil.html", "file:///a/index.html"));
    CHECK(!same_document("https://evil.example/", "file:///a/index.html"));
    for (const std::string entry : {"file:///a/index.html", "http://127.0.0.1:19280/index.html",
                                   "http://localhost:5173/tool?mode=dev", "http://[::1]:5173/"}) {
      CHECK(same_document(entry + "#section", entry));
      CHECK(!same_document(entry + "?changed", entry));
      std::vector<std::string> opened, warnings;
      auto navigate = [&](const std::string& target) {
        blocked_navigation(target, entry, [&](const std::string& url) { opened.push_back(url); },
          [&](const std::string& script) { warnings.push_back(script); });
      };
      navigate(entry); navigate(entry + "#section");
      CHECK(opened.empty() && warnings.empty());
      for (const std::string target : {"https://example.com/path?q=1#section", "http://example.com/", "mailto:test@example.com?subject=Hi"}) {
        navigate(target); CHECK(opened.back() == target);
      }
      CHECK(opened.size() == 3 && warnings.empty());
      navigate(entry.substr(0, entry.rfind('/') + 1) + "other.html");
      navigate(entry + "?changed"); navigate("file:///a/other.html");
      CHECK(opened.size() == 3 && warnings.size() == 3);
      CHECK(warnings.back().find("reaper.window.open(path)") != std::string::npos);
      for (const std::string target : {"javascript:alert(1)", "data:text/html,test", "custom:test", "https://", "https://example.com/\n", "https://example.com/\\bad"})
        navigate(target);
      navigate(std::string("https://example.com/\0bad", 24));
      navigate("https://example.com/" + std::string(8192, 'x'));
      CHECK(opened.size() == 3 && warnings.size() == 11);
      blocked_navigation("https://example.com/", entry, [](const std::string&) { throw Error("EXTERNAL_OPEN_FAILED", "No handler"); },
        [&](const std::string& script) { warnings.push_back(script); });
      CHECK(warnings.back().find("No handler") != std::string::npos);
    }
    CHECK(local_navigation("http://localhost:5173?query", "http://localhost:5173/"));
    CHECK(!local_navigation("http://localhost:51730/", "http://localhost:5173/"));
    CHECK(!local_navigation("http://localhost:5173@example.com/", "http://localhost:5173/"));
    CHECK(!local_navigation("http://localhost:5173.example.com/", "http://localhost:5173/"));
    auto root = fs::current_path() / "path-test-fixture";
    fs::create_directories(root);
    const auto file = root / fs::u8path(u8"空 格#%.html");
    std::ofstream(file) << "<html></html>";
    CHECK(resolve_html(root, file.filename().u8string()) == fs::canonical(file));
    auto uri = file_uri(file);
    CHECK(uri.find("%20") != std::string::npos && uri.find("%23%25.html") != std::string::npos);
    for (const auto& bad : {"missing.html", "https://example.com/index.html", "file:///tmp/test.html"}) {
      bool rejected = false;
      try { resolve_html(root, bad); } catch (const Error&) { rejected = true; }
      CHECK(rejected);
    }
    fs::remove(file); fs::remove(root);
    std::cout << "Core: API dispatch, validation, limits, Unicode and paths passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
