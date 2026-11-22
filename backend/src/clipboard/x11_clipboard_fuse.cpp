// Read-only FUSE directory backing remote clipboard files.
//
// The viewer's local core mounts one directory per clipboard announcement. The
// clipboard carries the mount paths only; a file manager that pastes reads
// them, which turns each FUSE read into one `clipboard-pull` relayed over the
// remote session. Nothing is transferred when the remote user copies, and the
// file manager draws its own copy progress box on paste.

#include "clipboard/virtual_mount.h"

#if defined(__linux__) && MOLINK_HAVE_FUSE

#define FUSE_USE_VERSION 31
#include <fuse.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace molink {
namespace {

#ifdef MOLINK_FUSE_TRACE
#define FUSE_TRACE(...) std::fprintf(stderr, __VA_ARGS__)
#else
#define FUSE_TRACE(...) do { } while (0)
#endif

std::string unescape_mountinfo(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\\' && i + 3 < value.size()) {
      const int code = std::strtol(value.substr(i + 1, 3).c_str(), nullptr, 8);
      out.push_back(static_cast<char>(code));
      i += 3;
    } else {
      out.push_back(value[i]);
    }
  }
  return out;
}

/// Never stat() the path here: a stale FUSE mount with no daemon would block
/// the caller in the kernel forever.
bool mountinfo_has(const std::string& path) {
  std::ifstream in("/proc/self/mountinfo");
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    std::string value;
    for (int field = 0; field < 5 && fields >> value; ++field) {
      if (field == 4) {
        if (unescape_mountinfo(value) == path) return true;
        break;
      }
    }
  }
  return false;
}

/// A lazy detach succeeds even while a file manager still has a source file
/// open (the "clipboard changed mid-paste" case). Try umount2 first, then the
/// setuid helper; both are best effort and the caller re-checks mountinfo.
void lazy_detach(const std::string& path) {
  (void)::umount2(path.c_str(), MNT_DETACH);
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::execlp("fusermount3", "fusermount3", "-uz", path.c_str(),
             static_cast<char*>(nullptr));
    ::_exit(127);
  }
  if (pid > 0) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }
}

struct fuse_state {
  std::vector<virtual_mount_file> files;
  std::map<std::string, const virtual_mount_file*> by_name;
  virtual_file_reader* reader = nullptr;

  const virtual_mount_file* find(const char* path) const {
    if (path == nullptr) return nullptr;
    const char* name = path;
    while (*name == '/') ++name;
    if (*name == '\0') return nullptr;
    const auto it = by_name.find(name);
    return it == by_name.end() ? nullptr : it->second;
  }
};

void fill_root(struct stat* st) {
  std::memset(st, 0, sizeof(*st));
  st->st_mode = S_IFDIR | 0555;
  st->st_nlink = 2;
  st->st_uid = ::getuid();
  st->st_gid = ::getgid();
  st->st_blksize = 4096;
  st->st_atime = st->st_mtime = st->st_ctime = std::time(nullptr);
}

void fill_file(const virtual_mount_file& file, struct stat* st) {
  std::memset(st, 0, sizeof(*st));
  st->st_mode = S_IFREG | 0444;
  st->st_nlink = 1;
  st->st_uid = ::getuid();
  st->st_gid = ::getgid();
  st->st_size = static_cast<off_t>(file.size);
  st->st_blksize = 4096;
  st->st_blocks = static_cast<blkcnt_t>((file.size + 511) / 512);
  st->st_atime = st->st_mtime = st->st_ctime = std::time(nullptr);
}

int op_getattr(const char* path, struct stat* st, struct fuse_file_info*) {
  auto* state = static_cast<fuse_state*>(fuse_get_context()->private_data);
  if (state == nullptr || st == nullptr) return -EIO;
  const char* name = path;
  while (name != nullptr && *name == '/') ++name;
  if (name == nullptr || *name == '\0') {
    fill_root(st);
    return 0;
  }
  const virtual_mount_file* file = state->find(path);
  if (file == nullptr) return -ENOENT;
  fill_file(*file, st);
  return 0;
}

int op_readdir(const char* path, void* buffer, fuse_fill_dir_t filler,
               off_t offset, struct fuse_file_info*, enum fuse_readdir_flags) {
  auto* state = static_cast<fuse_state*>(fuse_get_context()->private_data);
  if (state == nullptr) return -EIO;
  const char* name = path;
  while (name != nullptr && *name == '/') ++name;
  if (name == nullptr || *name != '\0') return -ENOENT;

  const std::size_t count = state->files.size();
  std::size_t index = offset < 0 ? 0 : static_cast<std::size_t>(offset);
  struct stat st;
  if (index == 0) {
    fill_root(&st);
    if (filler(buffer, ".", &st, 1, static_cast<fuse_fill_dir_flags>(0)) != 0) {
      return 0;
    }
    index = 1;
  }
  if (index == 1) {
    fill_root(&st);
    if (filler(buffer, "..", &st, 2, static_cast<fuse_fill_dir_flags>(0)) != 0) {
      return 0;
    }
    index = 2;
  }
  for (std::size_t i = index - 2; i < count; ++i) {
    fill_file(state->files[i], &st);
    const off_t next = static_cast<off_t>(i + 3);
    if (filler(buffer, state->files[i].name.c_str(), &st, next,
               static_cast<fuse_fill_dir_flags>(0)) != 0) {
      return 0;
    }
  }
  return 0;
}

int op_open(const char* path, struct fuse_file_info* info) {
  auto* state = static_cast<fuse_state*>(fuse_get_context()->private_data);
  if (state == nullptr || state->find(path) == nullptr) return -ENOENT;
  if (info == nullptr) return 0;
  if ((info->flags & O_ACCMODE) != O_RDONLY || (info->flags & O_TRUNC) != 0) {
    return -EROFS;
  }
  return 0;
}

int op_read(const char* path, char* buffer, size_t size, off_t offset,
            struct fuse_file_info*) {
  auto* state = static_cast<fuse_state*>(fuse_get_context()->private_data);
  if (state == nullptr || state->reader == nullptr) return -EIO;
  const virtual_mount_file* file = state->find(path);
  if (file == nullptr) return -ENOENT;
  if (offset < 0) return -EINVAL;
  if (size == 0 || static_cast<uint64_t>(offset) >= file->size) return 0;
  uint64_t want = file->size - static_cast<uint64_t>(offset);
  if (want > size) want = size;
  const int got = state->reader->read_file_range(
      file->id, static_cast<uint64_t>(offset), want, buffer,
      static_cast<std::size_t>(want));
  if (got < 0) return -EIO;
  return got;
}

int op_access(const char* path, int mask) {
  auto* state = static_cast<fuse_state*>(fuse_get_context()->private_data);
  if (state == nullptr) return -EIO;
  if ((mask & W_OK) != 0) return -EROFS;
  const char* name = path;
  while (name != nullptr && *name == '/') ++name;
  if (name != nullptr && *name != '\0' && state->find(path) == nullptr) {
    return -ENOENT;
  }
  return 0;
}

int op_statfs(const char*, struct statvfs* st) {
  auto* state = static_cast<fuse_state*>(fuse_get_context()->private_data);
  if (state == nullptr || st == nullptr) return -EIO;
  std::memset(st, 0, sizeof(*st));
  uint64_t total = 0;
  for (const virtual_mount_file& file : state->files) total += file.size;
  const uint64_t block = 4096;
  const uint64_t blocks = (total + block - 1) / block;
  st->f_bsize = block;
  st->f_frsize = block;
  st->f_blocks = blocks;
  // The mount is a stream, not a disk. Report the selection's own size as
  // free so a file manager's pre-flight space check does not refuse the copy.
  st->f_bfree = blocks;
  st->f_bavail = blocks;
  st->f_files = state->files.size() + 2;
  st->f_ffree = 0;
  st->f_favail = 0;
  st->f_namemax = 255;
  return 0;
}

void* op_init(struct fuse_conn_info* conn, struct fuse_config* config) {
  if (config != nullptr) {
    // The generation is immutable; letting the kernel keep file data across
    // opens makes sequential copies fast.
    config->kernel_cache = 1;
    config->entry_timeout = 1.0;
    config->attr_timeout = 1.0;
    config->negative_timeout = 0.0;
  }
  if (conn != nullptr) {
    // The mount option already asked for 256 KiB; init must agree with it.
    // One network round trip then covers the host's whole pull range.
    conn->max_read = 256u * 1024u;
    conn->max_readahead = 256u * 1024u;
  }
  return fuse_get_context()->private_data;
}

const struct fuse_operations kOperations = [] {
  struct fuse_operations ops {};
  ops.getattr = op_getattr;
  ops.readdir = op_readdir;
  ops.open = op_open;
  ops.read = op_read;
  ops.access = op_access;
  ops.statfs = op_statfs;
  ops.init = op_init;
  return ops;
}();

class fuse_virtual_mount final : public virtual_mount {
 public:
  fuse_virtual_mount(std::string path,
                     std::shared_ptr<fuse_state> state)
      : path_(std::move(path)), state_(std::move(state)) {}

  ~fuse_virtual_mount() override { stop(); }

  const std::string& path() const override { return path_; }
  bool mounted() const override { return mounted_; }

  bool start(std::string& error) {
    fuse_args args = FUSE_ARGS_INIT(0, nullptr);
    fuse_opt_add_arg(&args, "molink-clip");
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args,
                     "ro,default_permissions,fsname=molink-clip,"
                     "subtype=molink-clip,nodev,nosuid,noexec,"
                     "max_read=262144");
    fuse_ = fuse_new(&args, &kOperations, sizeof(kOperations), state_.get());
    fuse_opt_free_args(&args);
    if (fuse_ == nullptr) {
      error = "libfuse could not create the filesystem";
      return false;
    }
    if (fuse_mount(fuse_, path_.c_str()) != 0) {
      fuse_destroy(fuse_);
      fuse_ = nullptr;
      error = "libfuse could not mount the clipboard directory";
      return false;
    }
    mounted_ = true;
    thread_ = std::thread([this] {
      if (fuse_ != nullptr) fuse_loop_mt(fuse_, 0);
    });
    return true;
  }

  bool stop() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fuse_ == nullptr) return detached_;
    FUSE_TRACE("fuse stop: exit\n");
    fuse_exit(fuse_);
    // Unmount (and therefore close the /dev/fuse fd) BEFORE joining the loop:
    // a worker blocked in read() on the device only wakes when the fd goes
    // away. Joining first hangs for as long as the kernel keeps the session.
    if (mounted_) {
      fuse_unmount(fuse_);
      mounted_ = false;
    }
    FUSE_TRACE("fuse stop: unmounted\n");
    if (thread_.joinable()) thread_.join();
    FUSE_TRACE("fuse stop: joined\n");
    fuse_destroy(fuse_);
    fuse_ = nullptr;
    FUSE_TRACE("fuse stop: destroyed; mountinfo=%d\n",
               mountinfo_has(path_) ? 1 : 0);
    // With an open file the ordinary unmount fails. Detach lazily so the
    // caller can remove the directory and a mid-paste reader gets EIO instead
    // of blocking. mountinfo is the only safe way to test a possibly stale
    // mount.
    if (mountinfo_has(path_)) lazy_detach(path_);
    detached_ = !mountinfo_has(path_);
    FUSE_TRACE("fuse stop: detached=%d\n", detached_ ? 1 : 0);
    return detached_;
  }

 private:
  std::string path_;
  std::shared_ptr<fuse_state> state_;
  struct fuse* fuse_ = nullptr;
  std::thread thread_;
  std::mutex mutex_;
  bool mounted_ = false;
  bool detached_ = true;
};

}  // namespace

bool platform_virtual_mount_supported() { return true; }

bool platform_path_is_mountpoint(const std::string& path) {
  return mountinfo_has(path);
}

virtual_mount_factory platform_virtual_mount_factory() {
  return [](const std::string& mountpoint,
            const std::vector<virtual_mount_file>& files,
            virtual_file_reader* reader, std::string& error)
             -> std::shared_ptr<virtual_mount> {
    if (reader == nullptr) {
      error = "no file reader";
      return nullptr;
    }
    auto state = std::make_shared<fuse_state>();
    state->files = files;
    state->reader = reader;
    for (const virtual_mount_file& file : state->files) {
      state->by_name[file.name] = &file;
    }
    auto mount = std::make_shared<fuse_virtual_mount>(mountpoint, state);
    if (!mount->start(error)) return nullptr;
    return mount;
  };
}

}  // namespace molink

#else  // !linux || !MOLINK_HAVE_FUSE

namespace molink {

bool platform_virtual_mount_supported() { return false; }

bool platform_path_is_mountpoint(const std::string&) { return false; }

virtual_mount_factory platform_virtual_mount_factory() { return nullptr; }

}  // namespace molink

#endif
