/* *****************************************************************************
IO Polling
***************************************************************************** */
#ifndef H_FACIL_IO_H /* development sugar, ignore */
#include "999 dev.h" /* development sugar, ignore */
#endif               /* development sugar, ignore */

#ifndef FIO_POLL_TICK
#define FIO_POLL_TICK 993
#endif

#ifndef FIO_POLL_MAX_EVENTS
/* The number of events to collect with each call to epoll or kqueue. */
#define FIO_POLL_MAX_EVENTS 96
#endif

// #if FIO_ENGINE_POLL
/* fio_poll_remove might not work when polling from multiple threads */
#define FIO_POLL_EV_VALIDATE_UUID(uuid)                                        \
  do {                                                                         \
    fio_queue_perform_all(task_queues);                                        \
    if (!fio_uuid_is_valid(uuid)) {                                            \
      FIO_LOG_DEBUG("uuid validation failed for uuid %p", (void *)uuid);       \
      return;                                                                  \
    }                                                                          \
  } while (0);
// #else
// #define FIO_POLL_EV_VALIDATE_UUID(uuid)
// #endif

FIO_IFUNC void fio___poll_ev_wrap_data(int fd, void *uuid_) {
  fio_uuid_s *uuid = uuid_;
  // FIO_LOG_DEBUG("event on_data detected for uuid %p", uuid_);
  FIO_POLL_EV_VALIDATE_UUID(uuid);
  fio_queue_push(fio_queue_select(uuid->protocol->reserved.flags),
                 fio_ev_on_data,
                 fio_uuid_dup(uuid),
                 uuid->udata);
  (void)fd;
}
FIO_IFUNC void fio___poll_ev_wrap_ready(int fd, void *uuid_) {
  fio_uuid_s *uuid = uuid_;
  // FIO_LOG_DEBUG("event on_ready detected for uuid %p", uuid_);
  FIO_POLL_EV_VALIDATE_UUID(uuid);
  fio_queue_push(task_queues, fio_ev_on_ready, fio_uuid_dup(uuid), uuid->udata);
  (void)fd;
}

FIO_IFUNC void fio___poll_ev_wrap_close(int fd, void *uuid_) {
  fio_uuid_s *uuid = uuid_;
  // FIO_LOG_DEBUG("event on_close detected for uuid %p", uuid_);
  FIO_POLL_EV_VALIDATE_UUID(uuid);
  fio_uuid_close_unsafe(uuid);
  (void)fd;
}

FIO_IFUNC int fio_uuid_monitor_tick_len(void) {
  return (FIO_POLL_TICK * fio_data.running) | 7;
}

/* *****************************************************************************
EPoll
***************************************************************************** */
#if FIO_ENGINE_EPOLL
#include <sys/epoll.h>

/**
 * Returns a C string detailing the IO engine selected during compilation.
 *
 * Valid values are "kqueue", "epoll" and "poll".
 */
char const *fio_engine(void) { return "epoll"; }

/* epoll tester, in and out */
static int evio_fd[3] = {-1, -1, -1};

FIO_IFUNC void fio_uuid_monitor_close(void) {
  for (int i = 0; i < 3; ++i) {
    if (evio_fd[i] != -1) {
      close(evio_fd[i]);
      evio_fd[i] = -1;
    }
  }
}

FIO_IFUNC void fio_uuid_monitor_init(void) {
  fio_uuid_monitor_close();
  for (int i = 0; i < 3; ++i) {
    evio_fd[i] = epoll_create1(EPOLL_CLOEXEC);
    if (evio_fd[i] == -1)
      goto error;
  }
  for (int i = 1; i < 3; ++i) {
    struct epoll_event chevent = {
        .events = (EPOLLOUT | EPOLLIN),
        .data.fd = evio_fd[i],
    };
    if (epoll_ctl(evio_fd[0], EPOLL_CTL_ADD, evio_fd[i], &chevent) == -1)
      goto error;
  }
  return;
error:
  FIO_LOG_FATAL("couldn't initialize epoll.");
  fio_uuid_monitor_close();
  exit(errno);
  return;
}

FIO_IFUNC int fio___epoll_add2(fio_uuid_s *uuid, uint32_t events, int ep_fd) {
  int ret = -1;
  struct epoll_event chevent;
  int fd = uuid->fd;
  if (fd == -1)
    return ret;
  do {
    errno = 0;
    chevent = (struct epoll_event){
        .events = events,
        .data.fd = fd,
    };
    ret = epoll_ctl(ep_fd, EPOLL_CTL_MOD, fd, &chevent);
    if (ret == -1 && errno == ENOENT) {
      errno = 0;
      chevent = (struct epoll_event){
          .events = events,
          .data.fd = fd,
      };
      ret = epoll_ctl(ep_fd, EPOLL_CTL_ADD, fd, &chevent);
    }
  } while (errno == EINTR);

  return ret;
}

FIO_IFUNC void fio_uuid_monitor_add_read(fio_uuid_s *uuid) {
  fio___epoll_add2(uuid,
                   (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLONESHOT),
                   evio_fd[1]);
  return;
}

FIO_IFUNC void fio_uuid_monitor_add_write(fio_uuid_s *uuid) {
  fio___epoll_add2(uuid,
                   (EPOLLOUT | EPOLLRDHUP | EPOLLHUP | EPOLLONESHOT),
                   evio_fd[2]);
  return;
}

FIO_IFUNC void fio_uuid_monitor_add(fio_uuid_s *uuid) {
  if (fio___epoll_add2(uuid,
                       (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLONESHOT),
                       evio_fd[1]) == -1)
    return;
  fio___epoll_add2(uuid,
                   (EPOLLOUT | EPOLLRDHUP | EPOLLHUP | EPOLLONESHOT),
                   evio_fd[2]);
  return;
}

FIO_IFUNC void fio_uuid_monitor_remove(fio_uuid_s *uuid) {
  struct epoll_event chevent = {.events = (EPOLLOUT | EPOLLIN),
                                .data.ptr = uuid};
  epoll_ctl(evio_fd[1], EPOLL_CTL_DEL, uuid->fd, &chevent);
  epoll_ctl(evio_fd[2], EPOLL_CTL_DEL, uuid->fd, &chevent);
}

FIO_SFUNC size_t fio_uuid_monitor_review(void) {
  int timeout_millisec = fio_uuid_monitor_tick_len();
  struct epoll_event internal[2];
  struct epoll_event events[FIO_POLL_MAX_EVENTS];
  int total = 0;
  /* wait for events and handle them */
  int internal_count = epoll_wait(evio_fd[0], internal, 2, timeout_millisec);
  if (internal_count == 0)
    return internal_count;
  for (int j = 0; j < internal_count; ++j) {
    int active_count =
        epoll_wait(internal[j].data.fd, events, FIO_POLL_MAX_EVENTS, 0);
    if (active_count > 0) {
      for (int i = 0; i < active_count; i++) {
        if (events[i].events & (~(EPOLLIN | EPOLLOUT))) {
          // errors are hendled as disconnections (on_close)
          fio___poll_ev_wrap_close(0, events[i].data.ptr);
        } else {
          // no error, then it's an active event(s)
          if (events[i].events & EPOLLOUT) {
            fio___poll_ev_wrap_ready(0, events[i].data.ptr);
          }
          if (events[i].events & EPOLLIN)
            fio___poll_ev_wrap_data(0, events[i].data.ptr);
        }
      } // end for loop
      total += active_count;
    }
  }
  return total;
}

/* *****************************************************************************
KQueue
***************************************************************************** */
#elif FIO_ENGINE_KQUEUE
#include <sys/event.h>

/**
 * Returns a C string detailing the IO engine selected during compilation.
 *
 * Valid values are "kqueue", "epoll" and "poll".
 */
char const *fio_engine(void) { return "kqueue"; }

static int evio_fd = -1;

FIO_IFUNC void fio_uuid_monitor_close(void) {
  if (evio_fd != -1)
    close(evio_fd);
}

FIO_IFUNC void fio_uuid_monitor_init(void) {
  fio_uuid_monitor_close();
  evio_fd = kqueue();
  if (evio_fd == -1) {
    FIO_LOG_FATAL("couldn't open kqueue.\n");
    exit(errno);
  }
}

FIO_IFUNC void fio_uuid_monitor_add_read(fio_uuid_s *uuid) {
  struct kevent chevent[1];
  EV_SET(chevent,
         uuid->fd,
         EVFILT_READ,
         EV_ADD | EV_ENABLE | EV_CLEAR | EV_ONESHOT,
         0,
         0,
         ((void *)uuid));
  do {
    errno = 0;
    kevent(evio_fd, chevent, 1, NULL, 0, NULL);
  } while (errno == EINTR);
  return;
}

FIO_IFUNC void fio_uuid_monitor_add_write(fio_uuid_s *uuid) {
  struct kevent chevent[1];
  EV_SET(chevent,
         uuid->fd,
         EVFILT_WRITE,
         EV_ADD | EV_ENABLE | EV_CLEAR | EV_ONESHOT,
         0,
         0,
         ((void *)uuid));
  do {
    errno = 0;
    kevent(evio_fd, chevent, 1, NULL, 0, NULL);
  } while (errno == EINTR);
  return;
}

FIO_IFUNC void fio_uuid_monitor_add(fio_uuid_s *uuid) {
  struct kevent chevent[2];
  EV_SET(chevent,
         uuid->fd,
         EVFILT_READ,
         EV_ADD | EV_ENABLE | EV_CLEAR | EV_ONESHOT,
         0,
         0,
         ((void *)uuid));
  EV_SET(chevent + 1,
         uuid->fd,
         EVFILT_WRITE,
         EV_ADD | EV_ENABLE | EV_CLEAR | EV_ONESHOT,
         0,
         0,
         ((void *)uuid));
  do {
    errno = 0;
  } while (kevent(evio_fd, chevent, 2, NULL, 0, NULL) == -1 && errno == EINTR);
  return;
}

FIO_IFUNC void fio_uuid_monitor_remove(fio_uuid_s *uuid) {
  if (evio_fd < 0)
    return;
  struct kevent chevent[2];
  EV_SET(chevent, uuid->fd, EVFILT_READ, EV_DELETE, 0, 0, (void *)uuid);
  EV_SET(chevent + 1, uuid->fd, EVFILT_WRITE, EV_DELETE, 0, 0, (void *)uuid);
  do {
    errno = 0;
    kevent(evio_fd, chevent, 2, NULL, 0, NULL);
  } while (errno == EINTR);
}

FIO_SFUNC size_t fio_uuid_monitor_review(void) {
  if (evio_fd < 0)
    return -1;
  int timeout_millisec = fio_uuid_monitor_tick_len();
  struct kevent events[FIO_POLL_MAX_EVENTS] = {{0}};

  const struct timespec timeout = {
      .tv_sec = (timeout_millisec / 1024),
      .tv_nsec = ((timeout_millisec & (1023UL)) * 1000000)};
  /* wait for events and handle them */
  int active_count =
      kevent(evio_fd, NULL, 0, events, FIO_POLL_MAX_EVENTS, &timeout);

  if (active_count > 0) {
    for (int i = 0; i < active_count; i++) {
      // test for event(s) type
      if (events[i].filter == EVFILT_WRITE) {
        fio___poll_ev_wrap_ready(0, events[i].udata);
      } else if (events[i].filter == EVFILT_READ) {
        fio___poll_ev_wrap_data(0, events[i].udata);
      }
      if (events[i].flags & (EV_EOF | EV_ERROR)) {
        fio___poll_ev_wrap_close(0, events[i].udata);
      }
    }
  } else if (active_count < 0) {
    if (errno == EINTR)
      return 0;
    return -1;
  }
  return active_count;
}

/* *****************************************************************************
Poll
***************************************************************************** */
#elif FIO_ENGINE_POLL

#define FIO_POLL
#include "fio-stl.h"

/**
 * Returns a C string detailing the IO engine selected during compilation.
 *
 * Valid values are "kqueue", "epoll" and "poll".
 */
char const *fio_engine(void) { return "poll"; }

static fio_poll_s fio___poll_data = FIO_POLL_INIT(fio___poll_ev_wrap_data,
                                                  fio___poll_ev_wrap_ready,
                                                  fio___poll_ev_wrap_close);
FIO_IFUNC void fio_uuid_monitor_close(void) {
  fio_poll_destroy(&fio___poll_data);
}

FIO_IFUNC void fio_uuid_monitor_init(void) {
  fio___poll_data = (fio_poll_s)FIO_POLL_INIT(fio___poll_ev_wrap_data,
                                              fio___poll_ev_wrap_ready,
                                              fio___poll_ev_wrap_close);
}

FIO_IFUNC void fio_uuid_monitor_remove(fio_uuid_s *uuid) {
  // FIO_LOG_DEBUG("IO monitor removing %p", uuid);
  fio_io_thread_wake();
  fio_poll_forget(&fio___poll_data, uuid->fd);
}

FIO_IFUNC void fio_uuid_monitor_add_read(fio_uuid_s *uuid) {
  // FIO_LOG_DEBUG("IO monitor added read for %p (%d)", uuid, uuid->fd);
  fio_poll_monitor(&fio___poll_data, uuid->fd, uuid, POLLIN);
  fio_io_thread_wake();
}

FIO_IFUNC void fio_uuid_monitor_add_write(fio_uuid_s *uuid) {
  fio_poll_monitor(&fio___poll_data, uuid->fd, uuid, POLLOUT);
  fio_io_thread_wake();
}

FIO_IFUNC void fio_uuid_monitor_add(fio_uuid_s *uuid) {
  // FIO_LOG_DEBUG("IO monitor adding %p (%d)", uuid, uuid->fd);
  fio_poll_monitor(&fio___poll_data, uuid->fd, uuid, POLLIN | POLLOUT);
  fio_io_thread_wake();
}

/** returns non-zero if events were scheduled, 0 if idle */
FIO_SFUNC size_t fio_uuid_monitor_review(void) {
  // FIO_LOG_DEBUG("IO monitor reviewing events with %zu sockets",
  //               fio___poll_fds_count(&fio___poll_data.fds));
  fio_io_thread_wake_clear();
  return fio_poll_review(&fio___poll_data, fio_uuid_monitor_tick_len());
}

#endif /* FIO_ENGINE_POLL */
