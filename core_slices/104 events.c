/* *****************************************************************************
Internal helpers
***************************************************************************** */
#ifndef H_FACIL_IO_H /* development sugar, ignore */
#include "999 dev.h" /* development sugar, ignore */
#endif               /* development sugar, ignore */

/* *****************************************************************************
On Ready (User Callback Handling)
***************************************************************************** */

static void fio_ev_on_ready_user(void *io_, void *udata) {
  fio_s *const io = io_;
  if ((io->state & FIO_IO_OPEN) && !fio_stream_any(&io->stream)) {
    if (fio_trylock(&io->lock))
      goto reschedule;
    io->protocol->on_ready(io);
    fio_unlock(&io->lock);
  }
  fio_undup(io);
  return;

reschedule:
  fio_queue_push_urgent(fio_queue_select(io->protocol->reserved.flags),
                        .fn = fio_ev_on_ready_user,
                        .udata1 = io,
                        .udata2 = udata);
}

/* *****************************************************************************
On Ready (System Callback Handling)
***************************************************************************** */

static void fio_ev_on_ready(void *io_, void *udata) {
  fio_s *const io = io_;
  if ((io->state & FIO_IO_OPEN)) {
    char buf_mem[FIO_SOCKET_BUFFER_PER_WRITE];
    size_t total = 0;
    for (;;) {
      size_t len = FIO_SOCKET_BUFFER_PER_WRITE;
      char *buf = buf_mem;
      fio_stream_read(&io->stream, &buf, &len);
      if (!len)
        break;
      ssize_t r = fio_sock_write(io->fd, buf, len);
      if (r > 0) {
        total += r;
        fio_stream_advance(&io->stream, len);
        continue;
      } else if (r == -1 || errno == EWOULDBLOCK || errno == EAGAIN ||
                 errno == EINTR) {
        break;
      } else {
        fio_close_now_unsafe(io);
        goto finish;
      }
    }
    if (total)
      fio_touch___unsafe(io, NULL);
    if (!fio_stream_any(&io->stream)) {
      if ((io->state & FIO_IO_CLOSED_BIT)) {
        fio_close_now_unsafe(io);
      } else {
        fio_queue_push_urgent(fio_queue_select(io->protocol->reserved.flags),
                              .fn = fio_ev_on_ready_user,
                              .udata1 = io,
                              .udata2 = udata);
        return; /* fio_undup will be called after the user's on_ready event */
      }
    } else {
      fio_monitor_write(io);
    }
  }
finish:
  fio_free2(io);
}

/* *****************************************************************************
On Data
***************************************************************************** */

static void fio_ev_on_data(void *io_, void *udata) {
  fio_s *const io = io_;
  if (!(io->state & FIO_IO_CLOSED)) {
    if (fio_trylock(&io->lock))
      goto reschedule;
    io->protocol->on_data(io);
    fio_unlock(&io->lock);
    if (!(io->state & FIO_IO_CLOSED)) {
      /* this also tests for the suspended flag (0x02) */
      fio_monitor_read(io);
    }
  } else if ((io->state & FIO_IO_OPEN)) {
    fio_monitor_write(io);
    FIO_LOG_DEBUG("skipping on_data callback for IO %p (fd %d)", io_, io->fd);
  }
  fio_undup(io);
  return;

reschedule:
  FIO_LOG_DEBUG("rescheduling on_data for IO %p (fd %d)", io_, io->fd);
  fio_queue_push(fio_queue_select(io->protocol->reserved.flags),
                 .fn = fio_ev_on_data,
                 .udata1 = io,
                 .udata2 = udata);
}

/* *****************************************************************************
On Timeout
***************************************************************************** */

static void fio_ev_on_timeout(void *io_, void *udata) {
  fio_s *const io = io_;
  if ((io->state & FIO_IO_OPEN)) {
    io->protocol->on_timeout(io);
  } else {
    FIO_LOG_WARNING("timeout event on a non-open IO %p (fd %d)", io_, io->fd);
  }
  fio_undup(io);
  return;
  (void)udata;
}

/* *****************************************************************************
On Shutdown
***************************************************************************** */

static void fio_ev_on_shutdown(void *io_, void *udata) {
  fio_s *const io = io_;
  if (fio_trylock(&io->lock))
    goto reschedule;
  io->protocol->on_shutdown(io);
  fio_close(io);
  fio_unlock(&io->lock);
  fio_undup(io);
  return;

reschedule:
  fio_queue_push(fio_queue_select(io->protocol->reserved.flags),
                 .fn = fio_ev_on_shutdown,
                 .udata1 = io,
                 .udata2 = udata);
}

/* *****************************************************************************
On Close
***************************************************************************** */

static void fio_ev_on_close(void *io, void *udata) {
  (void)udata;
  fio_close_now_unsafe(io);
  fio_free2(io);
}