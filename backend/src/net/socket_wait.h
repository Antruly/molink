#ifndef MOLINK_NET_SOCKET_WAIT_H
#define MOLINK_NET_SOCKET_WAIT_H

// poll()-based socket readiness, used instead of select()/FD_SET.
//
// FD_SET aborts the process once a descriptor reaches FD_SETSIZE (1024 with
// glibc's _FORTIFY_SOURCE): __GI___fdelt_chk -> __GI___fortify_fail ->
// SIGABRT. A long-running host can easily hold more than 1024 descriptors
// (one socket per forwarded port connection), so any select() over those
// descriptors is a crash waiting to happen. poll() has no such limit.
//
// Windows has no POSIX poll(); Winsock's equivalent is WSAPoll(). Both take a
// milliseconds timeout and report readiness through revents, so the two
// implementations below keep the same semantics.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <cerrno>
#include <poll.h>
#endif

namespace molink {

#if defined(_WIN32)
using wait_socket_t = SOCKET;
constexpr short kPollRead = POLLRDNORM;
constexpr short kPollWrite = POLLWRNORM;
#else
using wait_socket_t = int;
constexpr short kPollRead = POLLIN;
constexpr short kPollWrite = POLLOUT;
#endif

/// Wait up to @p timeout_ms for @p fd to become ready for @p events.
///
/// Returns true when poll reports any actionable event for the descriptor,
/// including POLLERR/POLLHUP/POLLNVAL: a failed non-blocking connect shows up
/// as an error/hangup, and the caller is expected to read SO_ERROR afterwards.
/// Returns false on timeout or an unrecoverable poll error. EINTR is retried
/// internally so the timeout stays an upper bound in practice.
inline bool wait_socket(wait_socket_t fd, short events, int timeout_ms) {
  for (;;) {
#if defined(_WIN32)
    WSAPOLLFD pfd{};
    pfd.fd = fd;
    pfd.events = events;
    const int ready = ::WSAPoll(&pfd, 1, timeout_ms);
    if (ready > 0) {
      return (pfd.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0;
    }
    if (ready == 0) return false;
    if (::WSAGetLastError() == WSAEINTR) continue;
    return false;
#else
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = events;
    const int ready = ::poll(&pfd, 1, timeout_ms);
    if (ready > 0) {
      return (pfd.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0;
    }
    if (ready == 0) return false;
    if (errno == EINTR) continue;
    return false;
#endif
  }
}

/// poll() equivalent of select() with only the write set: true when a
/// non-blocking connect has a result to report (success, error or hangup).
inline bool wait_writable(wait_socket_t fd, int timeout_ms) {
  return wait_socket(fd, kPollWrite, timeout_ms);
}

/// poll() equivalent of select() with only the read set.
inline bool wait_readable(wait_socket_t fd, int timeout_ms) {
  return wait_socket(fd, kPollRead, timeout_ms);
}

}  // namespace molink

#endif  // MOLINK_NET_SOCKET_WAIT_H
