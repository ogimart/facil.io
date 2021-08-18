/* *****************************************************************************
Event / IO Reactor Pattern
***************************************************************************** */
#ifndef H_FACIL_IO_H /* development sugar, ignore */
#include "999 dev.h" /* development sugar, ignore */
#endif               /* development sugar, ignore */

/* *****************************************************************************
The Reactor event scheduler
***************************************************************************** */
FIO_SFUNC void fio___schedule_events(void) {
  static int old = 0;
  /* make sure the user thread is active */
  if (fio_queue_count(FIO_QUEUE_USER))
    fio_user_thread_wake();
  /* schedule IO events */
  fio_io_wakeup_prep();
  /* make sure all system events were processed */
  fio_queue_perform_all(FIO_QUEUE_SYSTEM);
  int c = fio_monitor_review(fio_poll_timeout_calc());
  fio_data.tick = fio_time2milli(fio_time_real());
  /* schedule Signal events */
  c += fio_signal_review();
  /* review IO timeouts */
  fio___review_timeouts();
  /* schedule timer events */
  fio_timer_push2queue(FIO_QUEUE_USER, &fio_data.timers, fio_data.tick);
  /* schedule on_idle events */
  if (!c) {
    if (old) {
      fio_state_callback_force(FIO_CALL_ON_IDLE);
    }
    /* test that parent process is active (during idle cycle) */
#if FIO_OS_POSIX
    if (!fio_data.is_master && fio_data.running &&
        getppid() != fio_data.master) {
      fio_data.running = 0;
      FIO_LOG_FATAL("(%d) parent process (%d != %d) seems to have crashed",
                    getpid(),
                    (int)fio_data.master,
                    (int)getppid());
    }
#endif
  }
  old = c;
  /* make sure the user thread is active after all events were scheduled */
  if (fio_queue_count(FIO_QUEUE_USER))
    fio_user_thread_wake();
}

/* *****************************************************************************
Shutdown cycle
***************************************************************************** */

/* cycles until all existing IO objects were closed. */
static void fio___shutdown_cycle(void) {
  for (;;) {
    if (!fio_queue_perform(FIO_QUEUE_SYSTEM))
      continue;
    fio_signal_review();
    if (!fio_queue_perform(FIO_QUEUE_USER))
      continue;
    if (fio___review_timeouts())
      continue;
    fio_data.tick = fio_time2milli(fio_time_real());
    if (fio_monitor_review(FIO_POLL_SHUTDOWN_TICK))
      continue;
    if (fio___is_waiting_on_io())
      continue;
    break;
  }
}

/* *****************************************************************************
Event consumption and cycling according to state (move some `if`s out of loop).
***************************************************************************** */

/** Worker no-threads cycle */
static void fio___worker_nothread_cycle(void) {
  for (;;) {
    if (!fio_queue_perform(FIO_QUEUE_SYSTEM))
      continue;
    if (!fio_queue_perform(FIO_QUEUE_USER))
      continue;
    if (!fio_data.running)
      break;
    fio___schedule_events();
  }
}

/** Worker thread cycle */
static void *fio___user_thread_cycle(void *ignr) {
  (void)ignr;
  for (;;) {
    fio_queue_perform_all(FIO_QUEUE_USER);
    if (fio_data.running) {
      fio_user_thread_suspent();
      continue;
    }
    return NULL;
  }
}
/** Worker cycle */
static void fio___worker_cycle(void) {
  fio_queue_perform_all(FIO_QUEUE_SYSTEM);
  while (fio_data.running) {
    fio___schedule_events();
    fio_queue_perform_all(FIO_QUEUE_SYSTEM);
  }
}

/* *****************************************************************************
Worker Processes work cycle
***************************************************************************** */

/** Worker cycle */
static void fio___worker(void) {
  fio_data.is_worker = 1;
  fio_state_callback_force(FIO_CALL_ON_START);
  fio_thread_t *threads = NULL;
  if (fio_data.threads) {
    threads = calloc(sizeof(threads), fio_data.threads);
    FIO_ASSERT_ALLOC(threads);
    for (size_t i = 0; i < fio_data.threads; ++i) {
      FIO_ASSERT(!fio_thread_create(threads + i, fio___user_thread_cycle, NULL),
                 "thread creation failed in worker.");
    }
    fio___worker_cycle();
  } else {
    fio___worker_nothread_cycle();
  }
  if (threads) {
    fio_user_thread_wake_all();
    for (size_t i = 0; i < fio_data.threads; ++i) {
      fio_monitor_review(FIO_POLL_SHUTDOWN_TICK);
      fio_queue_perform_all(FIO_QUEUE_SYSTEM);
      if (fio_thread_join(threads[i]))
        FIO_LOG_ERROR("Couldn't join worker thread.");
    }
    free(threads);
  }
  fio___start_shutdown();
  fio___shutdown_cycle();
  if (fio_data.io_wake_io)
    fio_close_now_unsafe(fio_data.io_wake_io);
  fio_queue_perform_all(FIO_QUEUE_SYSTEM);
  fio_close_wakeup_pipes();
  if (!fio_data.is_master)
    FIO_LOG_INFO("(%d) worker shutdown complete.", (int)getpid());
  else {
#if FIO_OS_POSIX
    /*
     * Wait for some of the children, assuming at least one of them will be a
     * worker.
     */
    for (size_t i = 0; i < fio_data.workers; ++i) {
      int jnk = 0;
      waitpid(-1, &jnk, 0);
    }
#endif
    FIO_LOG_INFO("(%d) IO shutdown complete.", (int)fio_data.master);
  }
}

/* *****************************************************************************
Spawning Worker Processes
***************************************************************************** */

static void fio_spawn_worker(void *ignr_1, void *ignr_2);

/** Worker sentinel */
static void *fio_worker_sentinal(void *GIL) {
  fio_state_callback_force(FIO_CALL_BEFORE_FORK);
  pid_t pid = fork();
  FIO_ASSERT(pid != (pid_t)-1, "system call `fork` failed.");
  fio_state_callback_force(FIO_CALL_AFTER_FORK);
  fio_unlock(GIL);
  if (pid) {
    int status = 0;
    (void)status;
    if (waitpid(pid, &status, 0) != pid && fio_data.running)
      FIO_LOG_ERROR("waitpid failed, worker re-spawning might fail.");
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
      FIO_LOG_WARNING("abnormal worker exit detected");
      fio_state_callback_force(FIO_CALL_ON_CHILD_CRUSH);
    }
    if (fio_data.running) {
      FIO_ASSERT_DEBUG(
          0,
          "DEBUG mode prevents worker re-spawning, now crashing parent.");
      fio_queue_push(FIO_QUEUE_SYSTEM, .fn = fio_spawn_worker);
    }
    return NULL;
  }
  fio_data.is_master = 0;
  FIO_LOG_INFO("(%d) worker starting up.", (int)getpid());
  fio_state_callback_force(FIO_CALL_IN_CHILD);
  fio___worker();
  exit(0);
  return NULL;
}

static void fio_spawn_worker(void *ignr_1, void *ignr_2) {
  static fio_lock_i GIL = FIO_LOCK_INIT;
  fio_thread_t t;
  if (!fio_data.is_master)
    return;
  fio_lock(&GIL);
  if (fio_thread_create(&t, fio_worker_sentinal, (void *)&GIL)) {
    fio_unlock(&GIL);
    FIO_LOG_FATAL(
        "sentinel thread creation failed, no worker will be spawned.");
    fio_stop();
  }
  fio_thread_detach(t);
  fio_lock(&GIL);
  fio_unlock(&GIL);
  (void)ignr_1;
  (void)ignr_2;
}