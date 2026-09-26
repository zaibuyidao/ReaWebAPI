#include "runtime/native_tasks.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (0)
using namespace reaweb;
int main() {
  const auto root = fs::temp_directory_path() / ("reaweb-watch-" + std::to_string(Clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  try {
    NativeTasks tasks;
    auto id = tasks.watch(root, true, 1);
    const auto until = [&](const std::string& type) {
      const auto deadline = Clock::now() + std::chrono::seconds(4);
      while (Clock::now() < deadline) {
        for (const auto& event : tasks.tick(Clock::now() + std::chrono::milliseconds(2)))
          if (event.data.value("type", "") == type && event.data["id"] == id) return event.data;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      throw std::runtime_error("Missing file event: " + type);
    };
    until("ready");
    { std::ofstream file(root / "a.txt"); file << "one"; } until("created");
    { std::ofstream file(root / "a.txt"); file << "changed data"; } until("changed");
    fs::rename(root / "a.txt", root / "b.txt"); auto renamed = until("renamed"); CHECK(renamed["oldPath"].get<std::string>().find("a.txt") != std::string::npos);
    fs::remove(root / "b.txt"); until("deleted");
    tasks.cancel_window(1);
    { std::ofstream file(root / "single.txt"); file << "single"; }
    id = tasks.watch(root / "single.txt", false, 1); until("ready");
    fs::rename(root / "single.txt", root / "renamed.txt"); until("renamed");
    { std::ofstream file(root / "renamed.txt"); file << "replacement"; } until("changed");
    fs::remove(root / "renamed.txt"); until("deleted"); tasks.cancel_window(1);
    id = tasks.watch(root, true, 1); until("ready");
    const auto other = tasks.watch(root, true, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(350)); tasks.tick(Clock::now() + std::chrono::milliseconds(5));
    for (int i = 0; i < 300; ++i) { std::ofstream file(root / (std::to_string(i) + ".txt")); file << i; }
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    bool first_overflow = false, second_overflow = false;
    const auto overflow = tasks.tick(Clock::now() + std::chrono::milliseconds(5)); CHECK(overflow.size() <= 256);
    for (const auto& event : overflow) if (event.data.value("type", "") == "overflow") {
      first_overflow = first_overflow || event.data["id"] == id;
      second_overflow = second_overflow || event.data["id"] == other;
    }
    CHECK(first_overflow && second_overflow); tasks.cancel_window(1); tasks.cancel_window(2);
    int calls = 0; uint64_t timer = 0;
    const auto callback = +[](void* context, uint64_t) { ++*static_cast<int*>(context); };
    CHECK(tasks.timer(0, 0, 9, callback, &calls, &timer) == 0);
    tasks.tick(Clock::now() + std::chrono::milliseconds(5)); tasks.tick(Clock::now() + std::chrono::milliseconds(5)); CHECK(calls == 1);
    CHECK(tasks.timer(0, 10, 9, callback, &calls, &timer) == 0);
    tasks.tick(Clock::now() + std::chrono::milliseconds(5)); CHECK(calls == 2);
    tasks.cancel_owner(9); std::this_thread::sleep_for(std::chrono::milliseconds(15));
    tasks.tick(Clock::now() + std::chrono::milliseconds(5)); CHECK(calls == 2);
    CHECK(tasks.timer(0, 1, 0, callback, &calls, &timer) == REAWEB_INVALID_ARGUMENT);
    std::cout << "File create/modify/rename/delete and native timer lifecycle passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; fs::remove_all(root); return 1; }
  fs::remove_all(root);
}
