#include "core/core.hpp"
#include "runtime/fs.hpp"
#include "core/file_time.hpp"
#include "core/worker.hpp"
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
  CHECK(io("ReaWeb_MoveFile", {u8"sub/文本.txt", u8"sub/移动 文本.txt"})["bytes"] == std::string(u8"中文\n").size());
  CHECK(!fs::exists(root / fs::u8path(u8"sub/文本.txt")));
  CHECK(io("ReaWeb_ReadFile", {u8"sub/移动 文本.txt"}) == u8"中文\n");
  auto moved = root / "moved.bin";
  CHECK(io("ReaWeb_MoveFile", {"binary", moved.u8string()})["path"] == moved.u8string());
  CHECK(io("ReaWeb_MoveFile", {moved.u8string(), "moved.bin"})["bytes"] == 3);
  io("ReaWeb_WriteFile", {"other", "keep"});
  rejects("FILE_EXISTS", [&] { io("ReaWeb_MoveFile", {"moved.bin", "other"}); });
  CHECK(io("ReaWeb_ReadFile", {"other"}) == "keep");
  CHECK(io("ReaWeb_MoveFile", {"moved.bin", "other", {{"overwrite", true}}})["bytes"] == 3);
  CHECK(decode_binary(io("ReaWeb_ReadFile", {"other", {{"encoding", "binary"}}})) == binary);
  for (const auto& args : {Json::array({}), Json::array({"other"}), Json::array({"other", "target", {{"overwrite", "true"}}}),
      Json::array({"other", "target", {{"unknown", true}}})})
    rejects("INVALID_ARGUMENT", [&] { io("ReaWeb_MoveFile", args); });
  for (const auto& path : {"", "https://example.com/a", "sub/", ".", ".."})
    rejects("INVALID_PATH", [&] { io("ReaWeb_MoveFile", {"other", path}); });
  rejects("INVALID_PATH", [&] { io("ReaWeb_MoveFile", {"other", "sub"}); });
  rejects("FILE_NOT_FOUND", [&] { io("ReaWeb_MoveFile", {"absent", "target"}); });
  rejects("FILE_NOT_FOUND", [&] { io("ReaWeb_MoveFile", {"sub", "target"}); });
  rejects("FILE_IO", [&] { io("ReaWeb_MoveFile", {"other", "absent/target"}); });
  CHECK(fs::exists(root / "other"));
  std::error_code symlink_error;
  fs::create_symlink(root / "other", root / "link", symlink_error);
  if (!symlink_error) {
    rejects("INVALID_PATH", [&] { io("ReaWeb_MoveFile", {"link", "target"}); });
    rejects("INVALID_PATH", [&] { io("ReaWeb_MoveFile", {"other", "link", {{"overwrite", true}}}); });
  }
  // Move large files without the bridge's read/write payload limit.
  { std::ofstream large(root / "large", std::ios::binary); large.seekp(20 * 1024 * 1024); large.put('x'); }
  CHECK(io("ReaWeb_MoveFile", {"large", "large-moved"})["bytes"] == 20 * 1024 * 1024 + 1);
#ifndef _WIN32
  if (const auto volume = std::getenv("REAWEB_TEST_MOVE_VOLUME")) {
    const auto target = fs::path(volume) / root.filename(); fs::create_directory(target);
    CHECK(io("ReaWeb_MoveFile", {"large-moved", (target / "large").u8string()})["bytes"] == 20 * 1024 * 1024 + 1);
    CHECK(!fs::exists(root / "large-moved"));
    std::ifstream content(target / "large", std::ios::binary); content.seekg(20 * 1024 * 1024); CHECK(content.get() == 'x'); content.close();
    fs::remove(target / "large"); fs::remove(target);
  }
#endif
  fs::remove_all(root);

  std::cout << "Host features: files, URLs, large messages passed\n";
  return 0;
} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
