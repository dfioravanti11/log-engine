#include "io/real/real_network.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#if defined(__APPLE__) || defined(__FreeBSD__)
#define LOGENGINE_KQUEUE 1
#include <sys/event.h>
#include <sys/time.h>
#elif defined(__linux__)
#define LOGENGINE_EPOLL 1
#include <sys/epoll.h>
#else
#error "no supported readiness backend for this platform"
#endif

namespace io::real {
namespace {

constexpr int kMaxEventsPerPoll = 256;

base::ErrorCode errno_to_code() {
  switch (errno) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EINTR:
      return base::ErrorCode::kWouldBlock;
    case EPIPE:
    case ECONNRESET:
    case ENOTCONN:
      return base::ErrorCode::kClosed;
    default:
      return base::ErrorCode::kIoError;
  }
}

bool set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void set_socket_options(int fd) {
  int one = 1;
  // TCP_NODELAY matters more here than usual: this is a latency benchmark, and
  // Nagle would silently add up to 40 ms to a small request's p99.
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

int fd_of(ConnId conn) { return static_cast<int>(conn); }
ConnId conn_of(int fd) { return static_cast<ConnId>(fd); }

}  // namespace

RealNetwork::RealNetwork() {
  // A write to a closed peer must surface as EPIPE, not as a process-killing signal.
  ::signal(SIGPIPE, SIG_IGN);
#if LOGENGINE_KQUEUE
  poll_fd_ = ::kqueue();
#else
  poll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
#endif
}

RealNetwork::~RealNetwork() {
  for (std::size_t fd = 0; fd < entries_.size(); ++fd) {
    if (entries_[fd].active) ::close(static_cast<int>(fd));
  }
  if (poll_fd_ >= 0) ::close(poll_fd_);
}

void RealNetwork::ensure_slot(int fd) {
  const std::size_t needed = static_cast<std::size_t>(fd) + 1;
  if (entries_.size() < needed) entries_.resize(needed);
  if (closed_during_dispatch_.size() < needed) closed_during_dispatch_.resize(needed, false);
}

RealNetwork::Entry* RealNetwork::entry_for(ConnId conn) {
  const int fd = fd_of(conn);
  if (fd < 0 || static_cast<std::size_t>(fd) >= entries_.size()) return nullptr;
  Entry& e = entries_[static_cast<std::size_t>(fd)];
  return e.active ? &e : nullptr;
}

base::Status RealNetwork::register_fd(int fd) {
  ensure_slot(fd);
  Entry& e = entries_[static_cast<std::size_t>(fd)];
  e.active = true;
  e.interest = Interest::kNone;
  e.handler = nullptr;
  closed_during_dispatch_[static_cast<std::size_t>(fd)] = false;
  return {};
}

base::Result<ConnId> RealNetwork::listen(base::u16 port, int backlog) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return base::fail(base::ErrorCode::kIoError);

  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, backlog) != 0 || !set_nonblocking(fd)) {
    ::close(fd);
    return base::fail(base::ErrorCode::kIoError);
  }

  auto status = register_fd(fd);
  if (!status.ok()) {
    ::close(fd);
    return base::fail(status.error());
  }
  return conn_of(fd);
}

base::Result<base::u16> RealNetwork::local_port(ConnId listener) {
  if (entry_for(listener) == nullptr) return base::fail(base::ErrorCode::kNotFound);
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd_of(listener), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    return base::fail(base::ErrorCode::kIoError);
  }
  return static_cast<base::u16>(ntohs(addr.sin_port));
}

base::Result<ConnId> RealNetwork::accept(ConnId listener) {
  if (entry_for(listener) == nullptr) return base::fail(base::ErrorCode::kNotFound);

  const int fd = ::accept(fd_of(listener), nullptr, nullptr);
  if (fd < 0) return base::fail(errno_to_code());

  if (!set_nonblocking(fd)) {
    ::close(fd);
    return base::fail(base::ErrorCode::kIoError);
  }
  set_socket_options(fd);

  auto status = register_fd(fd);
  if (!status.ok()) {
    ::close(fd);
    return base::fail(status.error());
  }
  return conn_of(fd);
}

base::Result<ConnId> RealNetwork::connect(std::string_view host, base::u16 port) {
  // Deliberately blocking, and only correct because it is: connect() happens once
  // during setup, before the loop starts spinning. The moment a client has to
  // reconnect mid-flight (week 6, leader failover) this must become a non-blocking
  // connect that completes on writability — a blocking call inside an event loop
  // stalls every partition pinned to that core.
  const std::string host_str(host);
  const std::string port_str = std::to_string(port);

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* result = nullptr;
  if (::getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &result) != 0 ||
      result == nullptr) {
    return base::fail(base::ErrorCode::kNotFound);
  }

  const int fd = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (fd < 0) {
    ::freeaddrinfo(result);
    return base::fail(base::ErrorCode::kIoError);
  }

  const int rc = ::connect(fd, result->ai_addr, result->ai_addrlen);
  ::freeaddrinfo(result);
  if (rc != 0) {
    ::close(fd);
    return base::fail(base::ErrorCode::kIoError);
  }

  if (!set_nonblocking(fd)) {
    ::close(fd);
    return base::fail(base::ErrorCode::kIoError);
  }
  set_socket_options(fd);

  auto status = register_fd(fd);
  if (!status.ok()) {
    ::close(fd);
    return base::fail(status.error());
  }
  return conn_of(fd);
}

base::Result<std::size_t> RealNetwork::read(ConnId conn, base::MutSlice out) {
  if (entry_for(conn) == nullptr) return base::fail(base::ErrorCode::kNotFound);
  const ssize_t n = ::read(fd_of(conn), out.data(), out.size());
  if (n < 0) return base::fail(errno_to_code());
  return static_cast<std::size_t>(n);  // 0 == peer closed cleanly
}

base::Result<std::size_t> RealNetwork::write(ConnId conn, base::Slice data) {
  if (entry_for(conn) == nullptr) return base::fail(base::ErrorCode::kNotFound);
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  const ssize_t n = ::send(fd_of(conn), data.data(), data.size(), flags);
  if (n < 0) return base::fail(errno_to_code());
  return static_cast<std::size_t>(n);
}

void RealNetwork::close(ConnId conn) {
  Entry* e = entry_for(conn);
  if (e == nullptr) return;
  const int fd = fd_of(conn);
  apply_interest(fd, e->interest, Interest::kNone);
  e->active = false;
  e->interest = Interest::kNone;
  e->handler = nullptr;
  closed_during_dispatch_[static_cast<std::size_t>(fd)] = true;
  ::close(fd);
}

void RealNetwork::watch(ConnId conn, Interest interest, ConnHandler* handler) {
  Entry* e = entry_for(conn);
  if (e == nullptr) return;
  apply_interest(fd_of(conn), e->interest, interest);
  e->interest = interest;
  e->handler = handler;
}

void RealNetwork::apply_interest(int fd, Interest before, Interest after) {
  if (before == after) return;

#if LOGENGINE_KQUEUE
  struct kevent changes[2];
  int n = 0;
  if (wants_read(after) != wants_read(before)) {
    EV_SET(&changes[n++], static_cast<uintptr_t>(fd), EVFILT_READ,
           wants_read(after) ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, nullptr);
  }
  if (wants_write(after) != wants_write(before)) {
    EV_SET(&changes[n++], static_cast<uintptr_t>(fd), EVFILT_WRITE,
           wants_write(after) ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, nullptr);
  }
  if (n > 0) ::kevent(poll_fd_, changes, n, nullptr, 0, nullptr);
#else
  epoll_event ev{};
  ev.data.fd = fd;
  if (wants_read(after)) ev.events |= EPOLLIN;
  if (wants_write(after)) ev.events |= EPOLLOUT;

  const bool was_registered = before != Interest::kNone;
  const bool is_registered = after != Interest::kNone;
  if (!was_registered && is_registered) {
    ::epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &ev);
  } else if (was_registered && !is_registered) {
    ::epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  } else if (is_registered) {
    ::epoll_ctl(poll_fd_, EPOLL_CTL_MOD, fd, &ev);
  }
#endif
}

std::size_t RealNetwork::poll(base::Nanos timeout) {
  std::fill(closed_during_dispatch_.begin(), closed_during_dispatch_.end(), false);

#if LOGENGINE_KQUEUE
  struct kevent events[kMaxEventsPerPoll];
  struct timespec ts {};
  struct timespec* tsp = nullptr;
  if (timeout >= 0) {
    ts.tv_sec = static_cast<time_t>(timeout / base::kNanosPerSecond);
    ts.tv_nsec = static_cast<long>(timeout % base::kNanosPerSecond);
    tsp = &ts;
  }

  const int n = ::kevent(poll_fd_, nullptr, 0, events, kMaxEventsPerPoll, tsp);
  if (n <= 0) return 0;

  std::size_t dispatched = 0;
  for (int i = 0; i < n; ++i) {
    const int fd = static_cast<int>(events[i].ident);
    if (static_cast<std::size_t>(fd) >= entries_.size()) continue;
    if (closed_during_dispatch_[static_cast<std::size_t>(fd)]) continue;

    Entry& e = entries_[static_cast<std::size_t>(fd)];
    if (!e.active || e.handler == nullptr) continue;

    ++dispatched;
    if ((events[i].flags & EV_ERROR) != 0) {
      e.handler->on_hangup(conn_of(fd));
    } else if (events[i].filter == EVFILT_READ) {
      // EV_EOF may also be set; deliver readable anyway so the handler drains
      // whatever is still buffered and then observes read() == 0.
      e.handler->on_readable(conn_of(fd));
    } else if (events[i].filter == EVFILT_WRITE) {
      e.handler->on_writable(conn_of(fd));
    }
  }
  return dispatched;
#else
  epoll_event events[kMaxEventsPerPoll];
  int timeout_ms = -1;
  if (timeout >= 0) {
    // Round up: truncating a sub-millisecond timeout to 0 turns a timed wait into
    // a spin loop.
    timeout_ms = static_cast<int>((timeout + base::kNanosPerMilli - 1) / base::kNanosPerMilli);
  }

  const int n = ::epoll_wait(poll_fd_, events, kMaxEventsPerPoll, timeout_ms);
  if (n <= 0) return 0;

  std::size_t dispatched = 0;
  for (int i = 0; i < n; ++i) {
    const int fd = events[i].data.fd;
    if (fd < 0 || static_cast<std::size_t>(fd) >= entries_.size()) continue;
    if (closed_during_dispatch_[static_cast<std::size_t>(fd)]) continue;

    Entry& e = entries_[static_cast<std::size_t>(fd)];
    if (!e.active || e.handler == nullptr) continue;

    ++dispatched;
    const uint32_t mask = events[i].events;
    if ((mask & EPOLLERR) != 0) {
      e.handler->on_hangup(conn_of(fd));
      continue;
    }
    if ((mask & (EPOLLIN | EPOLLHUP)) != 0) {
      e.handler->on_readable(conn_of(fd));
      if (closed_during_dispatch_[static_cast<std::size_t>(fd)]) continue;
    }
    if ((mask & EPOLLOUT) != 0 && e.active) {
      e.handler->on_writable(conn_of(fd));
    }
  }
  return dispatched;
#endif
}

std::size_t RealNetwork::open_connection_count() const {
  std::size_t count = 0;
  for (const Entry& e : entries_) {
    if (e.active) ++count;
  }
  return count;
}

}  // namespace io::real
