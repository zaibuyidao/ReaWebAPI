#include "web/loopback_origin.hpp"
#include <httplib.h>
#include <fstream>
#include <iostream>

using namespace reaweb;
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed: " #value); } while (false)
int main() {
  const auto root = fs::temp_directory_path() / ("reaweb-loopback-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    fs::create_directories(root / "app");
    std::ofstream(root / "app/index.html") << "<!doctype html>isolated";
    std::ofstream(root / "app/data.txt") << "0123456789";
    std::ofstream(root / "app/data.wav") << "0123456789";
    std::ofstream(root / "app/test.wasm") << "wasm";
    const auto profile = root / "profile";
    std::string origin;
    for (int cycle = 0; cycle < 2; ++cycle) {
      auto resources = std::make_shared<WebResources>(root / "app", "loopback-test", true);
      LoopbackOrigin server(profile, "loopback-test", resources);
      if (cycle) CHECK(resources->origin() == origin);
      origin = resources->origin();
      httplib::Client client(origin);
      client.set_connection_timeout(2);
      const httplib::Headers auth{{"Authorization", resources->authorization()}};
      auto response = client.Get("/index.html"); CHECK(response && response->status == 403);
      response = client.Get("/index.html", auth); CHECK(response && response->status == 200);
      CHECK(response->body == "<!doctype html>isolated");
      CHECK(response->get_header_value_count("Content-Type") == 1);
      CHECK(response->get_header_value("Cross-Origin-Opener-Policy") == "same-origin");
      CHECK(response->get_header_value("Cross-Origin-Embedder-Policy") == "require-corp");
      const auto cookie = response->get_header_value("Set-Cookie");
      CHECK(cookie.find("HttpOnly") != std::string::npos && cookie.find("SameSite=Strict") != std::string::npos);
      response = client.Get("/index.html", httplib::Headers{{"Cookie", cookie.substr(0, cookie.find(';'))}}); CHECK(response && response->status == 200);
      response = client.Get("/index.html", httplib::Headers{{"Authorization", resources->authorization()}, {"Origin", "https://unrelated.invalid"}});
      CHECK(response && response->status == 403);
      response = client.Get("/index.html", httplib::Headers{{"Authorization", resources->authorization()}, {"Host", "unrelated.invalid"}});
      CHECK(response && response->status == 403);
      response = client.Head("/data.txt", auth); CHECK(response && response->status == 200 && response->get_header_value("Content-Length") == "10");
      response = client.Get("/test.wasm", auth);
      CHECK(response && response->status == 200 && response->get_header_value_count("Content-Type") == 1);
      CHECK(response->get_header_value("Content-Type") == "application/wasm");
      response = client.Get("/data.txt", httplib::Headers{{"Authorization", resources->authorization()}, {"Range", "bytes=2-4"}});
      CHECK(response && response->status == 206 && response->body == "234");
      CHECK(response->get_header_value("Content-Range") == "bytes 2-4/10");
      response = client.Get("/data.txt", httplib::Headers{{"Authorization", resources->authorization()}, {"Range", "bytes=2-4"}, {"If-Range", "\"stale\""}});
      CHECK(response && response->status == 200 && response->body == "0123456789");
      response = client.Get("/data.txt", httplib::Headers{{"Authorization", resources->authorization()}, {"Range", "bytes=0-1,4-5"}});
      CHECK(response && response->status == 200 && response->body == "0123456789");
      response = client.Get("/data.txt", httplib::Headers{{"Authorization", resources->authorization()}, {"Range", "bytes=50-60"}});
      CHECK(response && response->status == 416);
      response = client.Post("/index.html", auth, "", "text/plain"); CHECK(response && response->status == 405);
      bool busy = false;
      try { LoopbackOrigin duplicate(profile, "loopback-test", std::make_shared<WebResources>(root / "app", "loopback-test", true)); }
      catch (const Error& error) { busy = error.code == "APP_ORIGIN_IN_USE"; }
      CHECK(busy);
      auto window = std::make_shared<WebResources>(root / "app", "loopback-test", true);
      server.attach(window);
      const auto media = window->open_media(root / "app/data.wav");
      const auto path = media.at("url").get<std::string>().substr(origin.size());
      response = client.Get(path, auth); CHECK(response && response->status == 200 && response->body == "0123456789");
      response = client.Get(path, httplib::Headers{{"Authorization", resources->authorization()}, {"Range", "bytes=3-6"}});
      CHECK(response && response->status == 206 && response->body == "3456");
      window->clear_media();
      response = client.Get(path, auth); CHECK(response && response->status != 200);
    }
    std::ofstream(profile / "isolation-origin.json") << "{}";
    bool invalid = false;
    try { LoopbackOrigin server(profile, "loopback-test", std::make_shared<WebResources>(root / "app", "loopback-test", true)); }
    catch (const Error& error) { invalid = error.code == "APP_ORIGIN_INVALID"; }
    CHECK(invalid);
    fs::remove_all(root);
    std::cout << "loopback origin: passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
