#include "runtime/file_writer.hpp"
#include <fstream>
#include <iostream>
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed: " #value); } while (false)
using namespace reaweb;
int main(int argc, char** argv) {
  try {
    const auto root = fs::current_path() / ("file-writer-test-" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    FileWriters writers;
    int serial = 0;
    auto invoke = [&](const std::string& method, Json args, int session = 1, uint64_t generation = 1) {
      Work work; work.session = session; work.generation = generation; work.path = root;
      work.data = {{"id", ++serial}, {"document", std::to_string(generation)}, {"method", method}, {"args", std::move(args)}};
      try { writers.submit(std::move(work)); }
      catch (const std::exception& error) { throw std::runtime_error(method + ": " + error.what()); }
      const auto deadline = Clock::now() + std::chrono::seconds(30);
      while (!writers.take(work)) { CHECK(Clock::now() < deadline); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
      return work.data;
    };
    auto call = [&](const std::string& method, Json args) {
      auto response = invoke(method, std::move(args));
      if (response.contains("error")) throw std::runtime_error(response.dump());
      return response.at("result");
    };
    auto read = [&](const fs::path& path) { std::ifstream f(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); };
    auto open = [&](const std::string& path, Json options) { return call("ReaWeb_OpenWrite", {path, options}).at("id").get<std::string>(); };
    auto write = [&](const Json& id, const std::string& bytes, uint64_t offset) { return call("ReaWeb_WriteChunk", {id, encode_binary(bytes.data(), bytes.size()), offset}); };
    auto id = open("empty", {{"expectedBytes", 0}});
    CHECK(call("ReaWeb_CommitWrite", {id}).at("bytes") == 0);
    CHECK(fs::file_size(root / "empty") == 0);
    CHECK(call("ReaWeb_AbortWrite", {id}).at("state") == "committed");
    CHECK(call("ReaWeb_CommitWrite", {id}).at("state") == "committed");
    std::ofstream(root / "target") << "original";
    CHECK(invoke("ReaWeb_OpenWrite", {"target"}).at("error").at("code") == "FILE_EXISTS");
    id = open("target", {{"overwrite", true}, {"expectedBytes", 4}});
    CHECK(invoke("ReaWeb_WriteStatus", {id}, 2).at("error").at("code") == "WRITER_INVALID");
    CHECK(invoke("ReaWeb_WriteStatus", {id}, 1, 2).at("error").at("code") == "WRITER_INVALID");
    CHECK(invoke("ReaWeb_OpenWrite", {"other"}).at("error").at("code") == "WRITE_BUSY");
    CHECK(invoke("ReaWeb_WriteChunk", {id, encode_binary("x", 1), 1}).at("error").at("code") == "WRITE_OFFSET_MISMATCH");
    write(id, "new!", 0); CHECK(read(root / "target") == "original");
    CHECK(call("ReaWeb_CommitWrite", {id}).at("bytes") == 4); CHECK(read(root / "target") == "new!");
    id = open("target", {{"overwrite", true}, {"expectedBytes", 5}});
    write(id, "abc", 0);
    CHECK(invoke("ReaWeb_CommitWrite", {id}).at("error").at("code") == "WRITE_LENGTH_MISMATCH");
    CHECK(call("ReaWeb_WriteStatus", {id}).at("state") == "failed"); CHECK(read(root / "target") == "new!");
    id = open("cancelled", Json::object()); write(id, "abc", 0);
    CHECK(call("ReaWeb_AbortWrite", {id}).at("state") == "aborted");
    CHECK(call("ReaWeb_AbortWrite", {id}).at("cleanupPending") == false); CHECK(!fs::exists(root / "cancelled"));
    CHECK(invoke("ReaWeb_CommitWrite", {id}).at("error").at("code") == "WRITER_CLOSED");
    id = open("race", Json::object()); write(id, "abc", 0); std::ofstream(root / "race") << "competitor";
    CHECK(invoke("ReaWeb_CommitWrite", {id}).at("error").at("code") == "FILE_EXISTS"); CHECK(read(root / "race") == "competitor");
    id = open("closed", Json::object()); write(id, "abc", 0); writers.cancel_window(1);
    CHECK(invoke("ReaWeb_WriteStatus", {id}, 1, 3).at("error").at("code") == "WRITER_INVALID");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(!fs::exists(root / "closed"));
    for (const auto& item : fs::directory_iterator(root)) CHECK(item.path().filename().string().rfind(".reaweb-write-", 0) != 0);
    const uint64_t total = argc > 1 ? (4ull * 1024 * 1024 * 1024 + 1024 * 1024) : (16ull * 1024 * 1024 + 1);
    id = open("large.bin", {{"expectedBytes", total}});
    std::string block(1024 * 1024, 0);
    for (size_t i = 0; i < block.size(); ++i) block[i] = char(i % 251);
    uint64_t position = 0;
    while (position < total) {
      const auto bytes = static_cast<size_t>(std::min<uint64_t>(block.size(), total - position));
      write(id, block.substr(0, bytes), position); position += bytes;
    }
    CHECK(call("ReaWeb_CommitWrite", {id}).at("bytes") == total);
    CHECK(fs::file_size(root / "large.bin") == total);
    std::ifstream data(root / "large.bin", std::ios::binary); std::string actual(block.size(), 0); position = 0;
    while (position < total) {
      const auto bytes = static_cast<size_t>(std::min<uint64_t>(block.size(), total - position));
      data.read(actual.data(), bytes); CHECK(static_cast<size_t>(data.gcount()) == bytes);
      CHECK(actual.compare(0, bytes, block, 0, bytes) == 0); position += bytes;
    }
    data.close();
    std::cout << Json{{"passed", true}, {"bytesVerified", total}, {"largeFile", (root / "large.bin").u8string()}}.dump() << '\n';
    if (argc == 1) { for (const auto& item : fs::directory_iterator(root)) fs::remove(item.path()); fs::remove(root); }
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
