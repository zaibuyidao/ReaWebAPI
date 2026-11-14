#include "runtime/file_writer.hpp"
#include <cerrno>
#include <limits>
#include <random>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <stdio.h>
#else
#include <sys/syscall.h>
#endif
#endif

namespace reaweb {
namespace {
void io_error(const char* operation) {
#ifdef _WIN32
  const std::error_code error(GetLastError(), std::system_category());
  const auto code = error.value() == ERROR_DISK_FULL || error.value() == ERROR_HANDLE_DISK_FULL ? "DISK_FULL" :
    error.value() == ERROR_ACCESS_DENIED ? "ACCESS_DENIED" :
    error.value() == ERROR_FILE_EXISTS || error.value() == ERROR_ALREADY_EXISTS ? "FILE_EXISTS" : "FILE_IO";
#else
  const std::error_code error(errno, std::generic_category());
  const auto code = errno == ENOSPC || errno == EDQUOT ? "DISK_FULL" :
    errno == EACCES || errno == EPERM ? "ACCESS_DENIED" : errno == EEXIST ? "FILE_EXISTS" : "FILE_IO";
#endif
  throw Error(code, std::string(operation) + ": " + error.message(), {{"systemCode", error.value()}});
}
uint64_t length(const Json& value, const char* name) {
  if (!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>() < 0) ||
      value.get<uint64_t>() > FileWriters::file_limit)
    throw Error("INVALID_ARGUMENT", std::string(name) + " must be an integer from 0 to 64 GiB");
  return value.get<uint64_t>();
}
fs::path destination(const fs::path& base, const Json& value) {
  if (!value.is_string()) throw Error("INVALID_PATH", "Expected a local file path");
  const auto text = value.get<std::string>();
  if (text.empty() || text.size() > 32768 || text.find('\0') != std::string::npos || text.find("://") != std::string::npos)
    throw Error("INVALID_PATH", "Expected a local file path");
  auto path = fs::u8path(text);
  if (path.filename().empty() || path.filename() == "." || path.filename() == ".." || (!path.is_absolute() && path.has_root_path()))
    throw Error("INVALID_PATH", "Expected a file name");
  if (!path.is_absolute()) path = base / path;
  return fs::canonical(path.parent_path()) / path.filename();
}
void check_target(const fs::path& path, bool overwrite) {
  const auto status = fs::symlink_status(path);
  if (fs::is_symlink(status) || (fs::exists(status) && !fs::is_regular_file(status)))
    throw Error("INVALID_PATH", "Destination must be a regular file, not a link or directory");
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    throw Error("INVALID_PATH", "Reparse destinations are unsupported");
#endif
  if (!overwrite && fs::exists(status)) throw Error("FILE_EXISTS", "Destination already exists");
}
}

struct FileWriters::Entry {
  std::shared_ptr<Owner> owner;
  fs::path target, directory, temporary;
  std::string state = "open";
  uint64_t bytes = 0, expected = 0;
  bool has_expected = false, overwrite = false;
  Clock::time_point touched = Clock::now();
#ifdef _WIN32
  HANDLE file = INVALID_HANDLE_VALUE;
  void close() { if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); file = INVALID_HANDLE_VALUE; } }
#else
  int file = -1;
  void close() { if (file >= 0) { ::close(file); file = -1; } }
#endif
  bool cleanup() {
    close();
    std::error_code error;
    if (!temporary.empty()) { fs::remove(temporary, error); if (error) return false; temporary.clear(); }
    if (!directory.empty()) { fs::remove(directory, error); if (error) return false; directory.clear(); }
    return true;
  }
  ~Entry() { cleanup(); }
  Json status() const { return {{"state", state}, {"path", target.u8string()}, {"bytes", bytes}, {"cleanupPending", (!temporary.empty() || !directory.empty()) && state != "open"}}; }
};

bool FileWriters::handles(const std::string& method) {
  return method == "ReaWeb_OpenWrite" || method == "ReaWeb_WriteChunk" || method == "ReaWeb_CommitWrite" ||
    method == "ReaWeb_AbortWrite" || method == "ReaWeb_WriteStatus";
}
FileWriters::FileWriters() : thread_([this] { run(); }) {}
FileWriters::~FileWriters() {
  { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; for (auto& item : owners_) item.second->cancelled = true; }
  wake_.notify_one(); thread_.join();
}
void FileWriters::submit(Work request) {
  const auto method = request.data.at("method").get<std::string>();
  const auto& args = request.data.at("args");
  const bool data = method == "ReaWeb_WriteChunk";
  const bool busy = data || method == "ReaWeb_OpenWrite" || method == "ReaWeb_CommitWrite";
  if (!args.is_array() || (data ? args.size() != 3 : method == "ReaWeb_OpenWrite" ? args.size() < 1 || args.size() > 2 : args.size() != 1))
    throw Error("INVALID_ARGUMENT", "Invalid file writer arguments");
  if (!args[0].is_string() || args[0].get_ref<const std::string&>().size() > 32768)
    throw Error("INVALID_ARGUMENT", "Expected a path or writer handle");
  if (data) {
    const auto& chunk = args[1];
    if (!chunk.is_object() || chunk.size() != 1 || !chunk.contains("__reawebBytes") || !chunk["__reawebBytes"].is_string())
      throw Error("INVALID_ARGUMENT", "Expected a binary chunk");
    if (chunk["__reawebBytes"].get_ref<const std::string&>().size() > ((chunk_limit + 2) / 3) * 4)
      throw Error("BUFFER_LIMIT", "Write chunk exceeds 1 MiB");
    length(args[2], "offset");
  }
  request.text.clear();
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || input_.size() + output_.size() >= 16 || (data && writes_ >= 4))
    throw Error("WRITE_BUSY", "File writer queue is full");
  auto& owner = owners_[{request.session, request.generation}];
  if (!owner) owner = std::make_shared<Owner>();
  if (owner->cancelled) throw Error("WRITER_INVALID", "Document has closed");
  if (busy && owner->busy) throw Error("WRITE_BUSY", "Await the previous writer operation");
  if (busy) owner->busy = true;
  if (data) ++writes_;
  input_.push_back({std::move(request), owner, data}); wake_.notify_one();
}
bool FileWriters::take(Work& response) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (output_.empty()) return false;
  response = std::move(output_.front()); output_.pop_front(); return true;
}
void FileWriters::cancel_window(int window) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = owners_.begin(); it != owners_.end();) {
    if (it->first.first == window) { it->second->cancelled = true; it = owners_.erase(it); }
    else ++it;
  }
  wake_.notify_one();
}
void FileWriters::reap() {
  const auto now = Clock::now();
  for (auto it = entries_.begin(); it != entries_.end();) {
    auto& entry = *it->second;
    if (entry.owner->cancelled || now - entry.touched > std::chrono::minutes(5)) {
      if (entry.state == "open") entry.state = "aborted";
      if (entry.cleanup()) { it = entries_.erase(it); continue; }
    } else if (entry.state != "open") entry.cleanup();
    ++it;
  }
}
Json FileWriters::execute(Job& job) {
  if (job.owner->cancelled) throw Error("WRITER_INVALID", "Document has closed");
  const auto method = job.work.data.at("method").get<std::string>();
  const auto& args = job.work.data.at("args");
  if (method == "ReaWeb_OpenWrite") {
    if (entries_.size() >= 128) throw Error("WRITE_BUSY", "Writer status storage is full");
    size_t active = 0;
    for (const auto& item : entries_) if (item.second->state == "open") {
      ++active;
      if (item.second->owner == job.owner) throw Error("WRITE_BUSY", "This document already has an open writer");
    }
    if (active >= 4) throw Error("WRITE_BUSY", "At most four file writers may be active");
    auto entry = std::make_unique<Entry>(); entry->owner = job.owner;
    if (args.size() == 2) {
      const auto& options = args[1];
      if (!options.is_object()) throw Error("INVALID_ARGUMENT", "Expected writer options");
      for (const auto& option : options.items()) {
        if (option.key() == "overwrite" && option.value().is_boolean()) entry->overwrite = option.value().get<bool>();
        else if (option.key() == "expectedBytes") { entry->expected = length(option.value(), "expectedBytes"); entry->has_expected = true; }
        else throw Error("INVALID_ARGUMENT", "Unknown or invalid writer option");
      }
    }
    entry->target = destination(job.work.path, args[0]); check_target(entry->target, entry->overwrite);
    std::random_device random;
    for (int attempt = 0; attempt < 32; ++attempt) {
      auto path = entry->target.parent_path() / (".reaweb-write-" + std::to_string(random()) + "-" + std::to_string(random()));
      if (fs::create_directory(path)) { entry->directory = path; break; }
    }
    if (entry->directory.empty()) throw Error("FILE_IO", "Cannot reserve a staging directory");
    entry->temporary = entry->directory / "data";
#ifdef _WIN32
    entry->file = CreateFileW(entry->temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (entry->file == INVALID_HANDLE_VALUE) io_error("Create temporary file");
#else
    entry->file = ::open(entry->temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (entry->file < 0) io_error("Create temporary file");
#endif
    const auto id = std::to_string(++sequence_);
    entries_.emplace(id, std::move(entry));
    return {{"id", id}, {"maxChunkBytes", chunk_limit}, {"maxFileBytes", file_limit}};
  }
  const auto found = entries_.find(args[0].get<std::string>());
  if (found == entries_.end() || found->second->owner != job.owner) throw Error("WRITER_INVALID", "Writer does not belong to this document");
  auto& entry = *found->second;
  entry.touched = Clock::now();
  if (method == "ReaWeb_WriteStatus") return entry.status();
  if (method == "ReaWeb_AbortWrite") {
    if (entry.state == "open") entry.state = "aborted";
    entry.cleanup(); return entry.status();
  }
  if (method == "ReaWeb_CommitWrite" && entry.state == "committed") return entry.status();
  if (entry.state != "open") throw Error("WRITER_CLOSED", "Writer is already " + entry.state);
  if (method == "ReaWeb_WriteChunk") {
    if (length(args[2], "offset") != entry.bytes) throw Error("WRITE_OFFSET_MISMATCH", "Expected the acknowledged offset");
    const auto data = decode_binary(args[1]);
    if (data.empty() || data.size() > chunk_limit) throw Error("BUFFER_LIMIT", "Chunk must contain 1 byte to 1 MiB");
    if (data.size() > file_limit - entry.bytes || (entry.has_expected && data.size() > entry.expected - entry.bytes))
      throw Error("WRITE_LENGTH_MISMATCH", "Chunk exceeds the expected or maximum file length");
    try {
      size_t position = 0;
      while (position < data.size()) {
        if (entry.owner->cancelled) throw Error("WRITE_CANCELLED", "Document has closed");
#ifdef _WIN32
        DWORD written = 0;
        if (!WriteFile(entry.file, data.data() + position, static_cast<DWORD>(data.size() - position), &written, nullptr)) io_error("Write file");
#else
        const auto written = ::write(entry.file, data.data() + position, data.size() - position);
        if (written < 0) { if (errno == EINTR) continue; io_error("Write file"); }
#endif
        if (!written) throw Error("FILE_IO", "File write made no progress");
        position += static_cast<size_t>(written);
      }
      entry.bytes += data.size(); return {{"bytes", data.size()}, {"totalBytes", entry.bytes}};
    } catch (...) { entry.state = "failed"; entry.cleanup(); throw; }
  }
  try {
    if (entry.has_expected && entry.bytes != entry.expected) throw Error("WRITE_LENGTH_MISMATCH", "File length does not match expectedBytes");
#ifdef _WIN32
    if (!FlushFileBuffers(entry.file)) io_error("Flush file");
    const auto file = entry.file; entry.file = INVALID_HANDLE_VALUE;
    if (!CloseHandle(file)) io_error("Close file");
#else
    if (fsync(entry.file) != 0) io_error("Flush file");
    const auto file = entry.file; entry.file = -1;
    if (::close(file) != 0) io_error("Close file");
#endif
    check_target(entry.target, entry.overwrite);
    if (entry.owner->cancelled) throw Error("WRITE_CANCELLED", "Document has closed");
    // Cancellation after this point observes the actual publication result.
#ifdef _WIN32
    if (!MoveFileExW(entry.temporary.c_str(), entry.target.c_str(), entry.overwrite ? MOVEFILE_REPLACE_EXISTING : 0)) io_error("Publish file");
#else
    if (entry.overwrite) {
      if (::rename(entry.temporary.c_str(), entry.target.c_str()) != 0) io_error("Publish file");
    } else {
#ifdef __APPLE__
      if (renamex_np(entry.temporary.c_str(), entry.target.c_str(), RENAME_EXCL) != 0) io_error("Publish file");
#else
      if (syscall(SYS_renameat2, AT_FDCWD, entry.temporary.c_str(), AT_FDCWD, entry.target.c_str(), 1) != 0) {
        if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) io_error("Publish file");
        // link also publishes without replacing a concurrently created destination.
        if (::link(entry.temporary.c_str(), entry.target.c_str()) != 0) io_error("Publish file");
      }
#endif
    }
#endif
  } catch (...) { entry.state = "failed"; entry.cleanup(); throw; }
  entry.state = "committed"; entry.cleanup(); return entry.status();
}
void FileWriters::run() {
  for (;;) {
    Job job; bool have_job = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(200), [this] { return stopping_ || !input_.empty(); });
      if (stopping_) break;
      if (!input_.empty()) { job = std::move(input_.front()); input_.pop_front(); have_job = true; }
    }
    reap();
    if (!have_job) continue;
    const auto request = job.work.data;
    Json response;
    try { response = {{"id", request.at("id")}, {"document", request.at("document")}, {"result", execute(job)}}; }
    catch (const Error& error) { response = error_response(request, error.code, error.what(), error.details); }
    catch (const std::exception& error) { response = error_response(request, "FILE_IO", error.what()); }
    job.work.data = std::move(response); job.work.kind = Work::Script;
    job.work.text = "window.__reawebReceive(" + job.work.data.dump(-1, ' ', true, Json::error_handler_t::replace) + ");";
    std::lock_guard<std::mutex> lock(mutex_);
    const auto method = request.at("method").get<std::string>();
    if (job.data || method == "ReaWeb_OpenWrite" || method == "ReaWeb_CommitWrite") job.owner->busy = false;
    if (job.data) --writes_;
    output_.push_back(std::move(job.work));
  }
  entries_.clear();
}
}
