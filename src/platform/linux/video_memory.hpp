#pragma once
#include "runtime/video_memory.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <unistd.h>

namespace reaweb {
struct LinuxVideoMemory final : VideoMemory {
  int fd = -1;
  struct stat identity{};
  explicit LinuxVideoMemory(uint32_t bytes) {
    if (!bytes || bytes > 40u * 1024 * 1024) return;
    frame_bytes = bytes;
    fd = int(syscall(SYS_memfd_create, "reaweb-project-video", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd < 0) return;
    const size_t size = size_t(bytes) * 2;
    if (ftruncate(fd, off_t(size)) || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) || fstat(fd, &identity)) {
      ::close(fd); fd = -1; return;
    }
    auto mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) { ::close(fd); fd = -1; return; }
    data = static_cast<unsigned char*>(mapping);
  }
  ~LinuxVideoMemory() override {
    if (data) munmap(data, size_t(frame_bytes) * 2);
    if (fd >= 0) ::close(fd);
  }
};

struct LinuxVideoResource {
  int pid = 0, fd = -1;
  uint64_t device = 0, inode = 0;
  uint32_t bytes = 0;
  int open() const {
    if (pid <= 0 || fd < 0 || !bytes || bytes > 40u * 1024 * 1024) return -1;
    const auto path = "/proc/" + std::to_string(pid) + "/fd/" + std::to_string(fd);
    const int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file < 0) return -1;
    struct stat info{};
    if (fstat(file, &info) || uint64_t(info.st_dev) != device || uint64_t(info.st_ino) != inode ||
        info.st_size != off_t(size_t(bytes) * 2)) { ::close(file); return -1; }
    return file;
  }
};
}
