#ifdef _WIN32
// Several hacks for cross-platform support on Windows...
#define O_CLOEXEC 0 //hopefully safe, since OR'd with other values

// From https://cohost.org/dcoles/post/4680394-emulating-pread-p
static int64_t pread(int fd, void* buf, uint64_t size, uint64_t offset) {
  HANDLE file = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (file == INVALID_HANDLE_VALUE) {
    return -1;
  }

  OVERLAPPED overlapped = {0};
  overlapped.Offset = offset & 0xFFFFFFFF;
  overlapped.OffsetHigh = (offset >> 32) & 0xFFFFFFFF;

  DWORD bytes_read;
  if (!ReadFile(file, buf, size, &bytes_read, &overlapped)) {
    if (GetLastError() != ERROR_HANDLE_EOF) {
      return -1;
    }
  }

  return bytes_read;
}

static int64_t pwrite(int fd, const void* buf, uint64_t size, uint64_t offset) {
  HANDLE file = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (file == INVALID_HANDLE_VALUE) {
    return -1;
  }

  OVERLAPPED overlapped = {0};
  overlapped.Offset = offset & 0xFFFFFFFF;
  overlapped.OffsetHigh = (offset >> 32) & 0xFFFFFFFF;

  DWORD bytes_written;
  if (!WriteFile(file, buf, size, &bytes_written, &overlapped)) {
    if (GetLastError() != ERROR_HANDLE_EOF) {
      return -1;
    }
  }

  return bytes_written;
}
#endif