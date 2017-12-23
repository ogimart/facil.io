/*
Copyright: Boaz Segev, 2017
License: MIT
*/
#if defined(__unix__) || defined(__APPLE__) || defined(__linux__) ||           \
    defined(__CYGWIN__) /* require POSIX */

/**
 * A dynamic type for reading / writing to a local file,  a temporary file or an
 * in-memory string.
 *
 * Supports basic reak, write, seek, puts and gets operations.
 *
 * Writing is always performed at the end of the stream / memory buffer,
 * ignoring the current seek position.
 */
#include "fiobj_internal.h"

#include "fiobj_io.h"

#include <sys/stat.h>
#include <unistd.h>

/* *****************************************************************************
Numbers Type
***************************************************************************** */

typedef struct {
  struct fiobj_vtable_s *vtable;
  uint8_t *buffer; /* reader buffer */
  union {
    void (*dealloc)(void *); /* buffer deallocation function */
    size_t fpos;             /* the file reader's position */
  };
  size_t capa; /* total buffer capacity */
  size_t len;  /* length of valid data in buffer */
  size_t pos;  /* position of reader */
  int fd;      /* file descriptor (-1 if invalid). */
} fiobj_io_s;

#define obj2io(o) ((fiobj_io_s *)(o))

/* *****************************************************************************
Object required VTable and functions
***************************************************************************** */

static struct fiobj_vtable_s FIOBJ_VTABLE_IO;

#define REQUIRE_MEM(mem)                                                       \
  do {                                                                         \
    if ((mem) == NULL) {                                                       \
      perror("FATAL ERROR: fiobj IO couldn't allocate memory");                \
      exit(errno);                                                             \
    }                                                                          \
  } while (0)

static void fiobj_io_copy_buffer(fiobj_s *o) {
  obj2io(o)->capa = (((obj2io(o)->len) >> 12) + 1) << 12;
  void *tmp = malloc(obj2io(o)->capa);
  REQUIRE_MEM(tmp);
  memcpy(tmp, obj2io(o)->buffer, obj2io(o)->len);
  if (obj2io(o)->dealloc)
    obj2io(o)->dealloc(obj2io(o)->buffer);
  obj2io(o)->dealloc = free;
  obj2io(o)->buffer = tmp;
}

static inline void fiobj_io_pre_write(fiobj_s *o, uintptr_t length) {
  if (obj2io(o)->fd == -1 && obj2io(o)->dealloc != free)
    fiobj_io_copy_buffer(o);
  if (obj2io(o)->capa <= obj2io(o)->len + length)
    return;
  /* add rounded pages (4096) to capacity */
  obj2io(o)->capa = (((obj2io(o)->len + length) >> 12) + 1) << 12;
  obj2io(o)->buffer = realloc(obj2io(o)->buffer, obj2io(o)->capa);
  REQUIRE_MEM(obj2io(o)->buffer);
}

static inline int64_t fiobj_io_get_fd_size(const fiobj_s *o) {
  struct stat stat;
retry:
  if (fstat(obj2io(o)->fd, &stat)) {
    if (errno == EINTR)
      goto retry;
    return -1;
  }
  return stat.st_size;
}

static fiobj_s *fiobj_io_alloc(void *buffer, int fd) {
  fiobj_s *o = fiobj_alloc(sizeof(fiobj_io_s));
  REQUIRE_MEM(o);
  obj2io(o)[0] =
      (fiobj_io_s){.vtable = &FIOBJ_VTABLE_IO, .buffer = buffer, .fd = fd};
  return o;
}

static void fiobj_io_dealloc(fiobj_s *o) {
  if (obj2io(o)->fd != -1) {
    close(obj2io(o)->fd);
    free(obj2io(o)->buffer);
  } else {
    if (obj2io(o)->dealloc && obj2io(o)->buffer)
      obj2io(o)->dealloc(obj2io(o)->buffer);
  }
  fiobj_dealloc(o);
}

static int64_t fiobj_io_i(const fiobj_s *o) {
  if (obj2io(o)->fd == -1) {
    return obj2io(o)->len;
  } else {
    return fiobj_io_get_fd_size(o);
  }
}

static fio_cstr_s fio_io2str(const fiobj_s *o) {
  if (obj2io(o)->fd == -1) {
    return (fio_cstr_s){.buffer = obj2io(o)->buffer, .len = obj2io(o)->len};
  }
  int64_t i = fiobj_io_get_fd_size(o);
  if (i <= 0)
    return (fio_cstr_s){.buffer = obj2io(o)->buffer, .len = obj2io(o)->len};
  obj2io(o)->len = 0;
  obj2io(o)->pos = 0;
  fiobj_io_pre_write((fiobj_s *)o, i + 1);
  if (pread(obj2io(o)->fd, obj2io(o)->buffer, i, 0) != i)
    return (fio_cstr_s){.buffer = NULL, .len = 0};
  obj2io(o)->buffer[i] = 0;
  return (fio_cstr_s){.buffer = obj2io(o)->buffer, .len = i};
}

static int fiobj_io_is_eq(const fiobj_s *self, const fiobj_s *other) {
  int64_t len;
  return (self == other || (self->type == other->type &&
                            (len = fiobj_io_i(self)) == fiobj_io_i(other) &&
                            !memcmp(fio_io2str(self).buffer,
                                    fio_io2str(other).buffer, (size_t)len)));
}

static struct fiobj_vtable_s FIOBJ_VTABLE_IO = {
    .name = "IO",
    .free = fiobj_io_dealloc,
    .to_i = fiobj_io_i,
    .to_f = fiobj_noop_f,
    .to_str = fio_io2str,
    .is_true = fiobj_noop_true,
    .is_eq = fiobj_io_is_eq,
    .count = fiobj_noop_count,
    .unwrap = fiobj_noop_unwrap,
    .each1 = fiobj_noop_each1,
};

/** The local IO abstraction type indentifier. */
const uintptr_t FIOBJ_T_IO = (uintptr_t)&FIOBJ_VTABLE_IO;

/* *****************************************************************************
Seeking for characters in a string
***************************************************************************** */

#if PREFER_MEMCHAR

/* a helper that seeks any char, converts it to NUL and returns 1 if found. */
inline static uint8_t swallow_ch(uint8_t **pos, uint8_t *const limit,
                                 uint8_t ch) {
  /* This is library based alternative that is sometimes slower  */
  if (*pos >= limit || **pos == ch) {
    return 0;
  }
  uint8_t *tmp = memchr(*pos, ch, limit - (*pos));
  if (tmp) {
    *pos = tmp;
    pos++;
    return 1;
  }
  *pos = limit;
  return 0;
}

#else

/* a helper that seeks any char, converts it to NUL and returns 1 if found. */
static inline uint8_t swallow_ch(uint8_t **buffer, const uint8_t *const limit,
                                 const uint8_t c) {
  /* this single char lookup is better when target is closer... */
  if (**buffer == c) {
    return 1;
  }

  uint64_t wanted = 0x0101010101010101ULL * c;
  uint64_t *lpos = (uint64_t *)*buffer;
  uint64_t *llimit = ((uint64_t *)limit) - 1;

  for (; lpos < llimit; lpos++) {
    const uint64_t eq = ~((*lpos) ^ wanted);
    const uint64_t t0 = (eq & 0x7f7f7f7f7f7f7f7fllu) + 0x0101010101010101llu;
    const uint64_t t1 = (eq & 0x8080808080808080llu);
    if ((t0 & t1)) {
      break;
    }
  }

  *buffer = (uint8_t *)lpos;
  while (*buffer < limit) {
    if (**buffer == c) {
      (*buffer)++;
      return 1;
    }
    (*buffer)++;
  }
  return 0;
}

#endif

/* *****************************************************************************
Creating the IO object
***************************************************************************** */

/** Creates a new local in-memory IO object */
fiobj_s *fiobj_io_newstr(void) {
  fiobj_s *o = fiobj_io_alloc(malloc(4096), -1);
  REQUIRE_MEM(obj2io(o)->buffer);
  obj2io(o)->capa = 4096;
  obj2io(o)->dealloc = free;
  return o;
}

/**
 * Creates a IO object from an existing buffer. The buffer will be deallocated
 * using the provided `dealloc` function pointer. Use a NULL `dealloc` function
 * pointer if the buffer is static and shouldn't be freed.
 */
fiobj_s *fiobj_io_newstr2(void *buffer, uintptr_t length,
                          void (*dealloc)(void *)) {
  fiobj_s *o = fiobj_io_alloc(buffer, -1);
  obj2io(o)->capa = length;
  obj2io(o)->len = length;
  obj2io(o)->dealloc = dealloc;
  return o;
}

/** Creates a new local file IO object */
fiobj_s *fiobj_io_newfd(int fd) {
  fiobj_s *o = fiobj_io_alloc(malloc(4096), fd);
  REQUIRE_MEM(obj2io(o)->buffer);
  obj2io(o)->fpos = 0;
  return o;
}

/** Creates a new local tempfile IO object */
fiobj_s *fiobj_io_newtmpfile(void) {
// create a temporary file to contain the data.
#ifdef P_tmpdir
#if defined(__linux__) /* linux doesn't end with a divider */
  char template[] = P_tmpdir "/http_request_body_XXXXXXXX";
#else
  char template[] = P_tmpdir "http_request_body_XXXXXXXX";
#endif
#else
  char template[] = "/tmp/http_request_body_XXXXXXXX";
#endif
  int fd = mkstemp(template);
  if (fd == -1)
    return NULL;
  return fiobj_io_newfd(fd);
}

/* *****************************************************************************
Reading API
***************************************************************************** */

/**
 * Reads up to `length` bytes and returns a temporary(!) C string object.
 *
 * The C string object will be invalidate the next time a function call to the
 * IO object is made.
 */
fio_cstr_s fiobj_io_read(fiobj_s *io, intptr_t length) {
  if (!io || io->type != FIOBJ_T_IO) {
    errno = EFAULT;
    return (fio_cstr_s){.buffer = NULL, .len = 0};
  }
  errno = 0;
  if (obj2io(io)->fd == -1) {
    /* String code */

    if (obj2io(io)->pos == obj2io(io)->len) {
      /* EOF */
      return (fio_cstr_s){.buffer = NULL, .len = 0};
    }

    if (!length) {
      /* read to EOF */
      length = obj2io(io)->len - obj2io(io)->pos;
    } else if (length < 0) {
      /* read to EOF - length */
      length = (obj2io(io)->len - obj2io(io)->pos) + length + 1;
    }

    if (length <= 0) {
      /* We are at EOF - length or beyond */
      return (fio_cstr_s){.buffer = NULL, .len = 0};
    }

    /* reading length bytes */
    register size_t pos = obj2io(io)->pos;
    obj2io(io)->pos = pos + length;
    if (obj2io(io)->pos > obj2io(io)->len)
      obj2io(io)->pos = obj2io(io)->len;
    return (fio_cstr_s){
        .buffer = (obj2io(io)->buffer + pos), .length = (obj2io(io)->pos - pos),
    };
  }

  /* File code */
  uintptr_t fsize = fiobj_io_get_fd_size(io);

  if (!length) {
    /* read to EOF */
    length = fsize - obj2io(io)->fpos;

  } else if (length < 0) {
    /* read to EOF - length */

    length = (fsize - obj2io(io)->fpos) + length + 1;
  }
  if (length <= 0) {
    /* We are at EOF - length or beyond */
    errno = 0;
    return (fio_cstr_s){.buffer = NULL, .len = 0};
  }

  /* reading length bytes */
  if (length + obj2io(io)->pos <= obj2io(io)->len) {
    /* the data already exists in the buffer */
    fio_cstr_s data = {.buffer = (obj2io(io)->buffer + obj2io(io)->pos),
                       .length = (uintptr_t)length};
    obj2io(io)->pos += length;
    obj2io(io)->fpos += length;
    return data;
  } else {
    /* read the data into the buffer - internal counting gets invalidated */
    obj2io(io)->len = 0;
    obj2io(io)->pos = 0;
    fiobj_io_pre_write(io, length);
    ssize_t l;
  retry_int:
    l = pread(obj2io(io)->fd, obj2io(io)->buffer, length, obj2io(io)->fpos);
    if (l == -1 && errno == EINTR)
      goto retry_int;
    if (l == -1 || l == 0)
      return (fio_cstr_s){.buffer = NULL, .len = 0};
    obj2io(io)->fpos += l;
    return (fio_cstr_s){.buffer = obj2io(io)->buffer, .len = l};
  }
}

/**
 * Reads until the `token` byte is encountered or until the end of the stream.
 *
 * Returns a temporary(!) C string including the end of line marker.
 *
 * Careful when using this call on large file streams, as the whole file
 * stream might be loaded into the memory.
 *
 * The C string object will be invalidate the next time a function call to the
 * IO object is made.
 */
fio_cstr_s fiobj_io_read2ch(fiobj_s *io, uint8_t token) {
  if (!io || io->type != FIOBJ_T_IO) {
    errno = EFAULT;
    return (fio_cstr_s){.buffer = NULL, .len = 0};
  }
  if (obj2io(io)->fd == -1) {
    /* String code */
    if (obj2io(io)->pos == obj2io(io)->len) /* EOF */
      return (fio_cstr_s){.buffer = NULL, .len = 0};

    uint8_t *pos = obj2io(io)->buffer + obj2io(io)->pos;
    uint8_t *lim = obj2io(io)->buffer + obj2io(io)->len;
    swallow_ch(&pos, lim, token);
    fio_cstr_s ret = (fio_cstr_s){
        .buffer = obj2io(io)->buffer + obj2io(io)->pos,
        .length = (uintptr_t)(pos - obj2io(io)->buffer) - obj2io(io)->pos,
    };
    obj2io(io)->pos = (uintptr_t)(pos - obj2io(io)->buffer);
    return ret;

  } else {
    /* File */
    uint8_t *pos = obj2io(io)->buffer + obj2io(io)->pos;
    uint8_t *lim = obj2io(io)->buffer + obj2io(io)->len;
    if (pos != lim && swallow_ch(&pos, lim, token)) {
      /* newline found in existing buffer */
      const uintptr_t delta =
          (uintptr_t)(pos - (obj2io(io)->buffer + obj2io(io)->pos));
      obj2io(io)->pos += delta;
      obj2io(io)->fpos += delta;
      return (fio_cstr_s){
          .buffer =
              (delta ? ((obj2io(io)->buffer + obj2io(io)->pos) - delta) : NULL),
          .length = delta,
      };
    }

    obj2io(io)->pos = 0;
    obj2io(io)->len = 0;

    while (1) {
      ssize_t tmp;
      fiobj_io_pre_write(io, 4096); /* read a page at a time */
    retry_int:
      tmp = pread(obj2io(io)->fd, obj2io(io)->buffer + obj2io(io)->len, 4096,
                  obj2io(io)->fpos + obj2io(io)->len);
      if (tmp < 0 && errno == EINTR)
        goto retry_int;
      if (tmp < 0 || (tmp == 0 && obj2io(io)->len == 0)) {
        return (fio_cstr_s){.buffer = NULL, .len = 0};
      }
      if (tmp == 0) {
        obj2io(io)->fpos += obj2io(io)->len;
        return (fio_cstr_s){.buffer = obj2io(io)->buffer,
                            .len = obj2io(io)->len};
      }
      obj2io(io)->len += tmp;
      pos = obj2io(io)->buffer;
      lim = obj2io(io)->buffer + obj2io(io)->len;
      if (swallow_ch(&pos, lim, token)) {
        const uintptr_t delta =
            (uintptr_t)(pos - (obj2io(io)->buffer + obj2io(io)->pos));
        obj2io(io)->pos = delta;
        obj2io(io)->fpos += delta;
        return (fio_cstr_s){
            .buffer = obj2io(io)->buffer, .length = delta,
        };
      }
    }
  }
}

/**
 * Returns the current reading position.
 */
intptr_t fiobj_io_pos(fiobj_s *io) {
  if (!io || io->type != FIOBJ_T_IO)
    return -1;
  if (obj2io(io)->fd == -1)
    return obj2io(io)->pos;
  return obj2io(io)->fpos;
}

/**
 * Moves the reading position to the requested position.
 */
void fiobj_io_seek(fiobj_s *io, intptr_t position) {
  if (!io || io->type != FIOBJ_T_IO)
    return;
  if (obj2io(io)->fd == -1) {
    /* String code */

    if (position == 0) {
      obj2io(io)->pos = 0;
      return;
    }
    if (position > 0) {
      if ((uintptr_t)position > obj2io(io)->len)
        position = obj2io(io)->len;
      obj2io(io)->pos = position;
      return;
    }
    position = (0 - position) + 1;
    if ((uintptr_t)position > obj2io(io)->len)
      position = 0;
    else
      position = obj2io(io)->len - position;
    obj2io(io)->pos = position;
    return;

  } else {
    /* File code */
    obj2io(io)->pos = 0;
    obj2io(io)->len = 0;

    if (position == 0) {
      obj2io(io)->fpos = 0;
      return;
    }
    int64_t len = fiobj_io_get_fd_size(io);
    if (len < 0)
      len = 0;
    if (position > 0) {
      if (position > len)
        position = len;

      obj2io(io)->fpos = position;
      return;
    }
    position = (0 - position) + 1;
    if (position > len)
      position = 0;
    else
      position = len - position;
    obj2io(io)->fpos = position;
    return;
  }
}

/**
 * Reads up to `length` bytes starting at `start_at` position and returns a
 * temporary(!) C string object. The reading position is ignored and
 * unchanged.
 *
 * The C string object will be invalidate the next time a function call to the
 * IO object is made.
 */
fio_cstr_s fiobj_io_pread(fiobj_s *io, intptr_t start_at, uintptr_t length) {
  if (!io || io->type != FIOBJ_T_IO) {
    errno = EFAULT;
    return (fio_cstr_s){
        .buffer = NULL, .length = 0,
    };
  }

  errno = 0;

  if (obj2io(io)->fd == -1) {
    /* String Code */
    if (start_at < 0)
      start_at = obj2io(io)->len + start_at + 1;
    if (start_at < 0)
      start_at = 0;
    if (length + start_at > obj2io(io)->len)
      length = obj2io(io)->len - start_at;
    if (length == 0)
      return (fio_cstr_s){
          .buffer = NULL, .length = 0,
      };
    return (fio_cstr_s){
        .buffer = obj2io(io)->buffer + start_at, .length = length,
    };
  }
  /* File Code */

  const int64_t size = fiobj_io_get_fd_size(io);
  if (start_at < 0)
    start_at = size + start_at + 1;
  if (start_at < 0)
    start_at = 0;
  if (length + start_at > (uint64_t)size)
    length = size - start_at;
  if (length == 0)
    return (fio_cstr_s){
        .buffer = NULL, .length = 0,
    };
  obj2io(io)->len = 0;
  obj2io(io)->pos = 0;
  fiobj_io_pre_write(io, length + 1);
  ssize_t tmp = pread(obj2io(io)->fd, obj2io(io)->buffer, length, start_at);
  if (tmp <= 0)
    return (fio_cstr_s){
        .buffer = NULL, .length = 0,
    };
  obj2io(io)->buffer[tmp] = 0;
  return (fio_cstr_s){
      .buffer = obj2io(io)->buffer, .length = tmp,
  };
}

/* *****************************************************************************
Writing API
***************************************************************************** */

/**
 * Makes sure the IO object isn't attached to a static or external string.
 *
 * If the IO object is attached to a static or external string, the data will be
 * copied to a new memory block.
 */
void fiobj_io_assert_dynamic(fiobj_s *io) {
  if (!io) {
    errno = ENFILE;
    return;
  }
  assert(io->type == FIOBJ_T_IO);
  fiobj_io_pre_write(io, 0);
  return;
}

/**
 * Writes `length` bytes at the end of the IO stream, ignoring the reading
 * position.
 *
 * Behaves and returns the same value as the system call `write`.
 */
intptr_t fiobj_io_write(fiobj_s *io, void *buffer, uintptr_t length) {
  if (!io || io->type != FIOBJ_T_IO || (!buffer && length)) {
    errno = EFAULT;
    return -1;
  }
  errno = 0;
  if (obj2io(io)->fd == -1) {
    /* String Code */
    fiobj_io_pre_write(io, length + 1);
    memcpy(obj2io(io)->buffer + obj2io(io)->len, buffer, length);
    obj2io(io)->len = obj2io(io)->len + length;
    obj2io(io)->buffer[obj2io(io)->len] = 0;
    return length;
  }

  /* File Code */
  return pwrite(obj2io(io)->fd, buffer, length, fiobj_io_get_fd_size(io));
}

/**
 * Writes `length` bytes at the end of the IO stream, ignoring the reading
 * position, adding an EOL marker ("\r\n") to the end of the stream.
 *
 * Behaves and returns the same value as the system call `write`.
 */
intptr_t fiobj_io_puts(fiobj_s *io, void *buffer, uintptr_t length) {
  if (!io || io->type != FIOBJ_T_IO || (!buffer && length)) {
    errno = EFAULT;
    return -1;
  }
  obj2io(io)->pos = 0;
  if (obj2io(io)->fd == -1) {
    /* String Code */
    fiobj_io_pre_write(io, length + 2);
    if (length) {
      memcpy(obj2io(io)->buffer + obj2io(io)->len, buffer, length);
    }
    obj2io(io)->len = obj2io(io)->len + length + 2;
    obj2io(io)->buffer[obj2io(io)->len - 2] = '\r';
    obj2io(io)->buffer[obj2io(io)->len - 1] = '\n';
    return length + 2;
  }
  /* File Code */
  uintptr_t end = fiobj_io_get_fd_size(io);
  ssize_t t1 = 0, t2 = 0;

  if (length) {
    ssize_t t1 = pwrite(obj2io(io)->fd, buffer, length, end);
    if (t1 < 0)
      return t1;
    end += t1;
  }
  t2 = pwrite(obj2io(io)->fd, buffer, length, end);
  if (t2 < 0)
    return t1;
  return t1 + t2;
}

#if DEBUG

#include "fiobj.h"

void fiobj_io_test(char *filename) {
  fiobj_s *text;
  fio_cstr_s s1, s2;
  fprintf(stderr, "*** testing fiobj_io ***\n");
  if (filename)
    text = fiobj_str_readfile(filename, 0, 0);
  else
    text = fiobj_str_static("Line 1\r\nLine 2\nLine 3 unended", 29);
  fiobj_s *strio = fiobj_io_newstr();
  fprintf(stderr, "* `newstr` passed.\n");
  fiobj_s *fdio = fiobj_io_newtmpfile();
  fprintf(stderr, "* `newtmpfile` passed.\n");
  fiobj_io_write(fdio, fiobj_obj2cstr(text).buffer,
                 fiobj_obj2cstr(text).length);
  fiobj_io_write(strio, fiobj_obj2cstr(text).buffer,
                 fiobj_obj2cstr(text).length);
  if (fiobj_obj2cstr(strio).length != fiobj_obj2cstr(text).length ||
      fiobj_obj2cstr(fdio).length != fiobj_obj2cstr(text).length)
    fprintf(stderr, "* `write` operation FAILED!\n");
  else
    fprintf(stderr, "* `write` operation (probably) passed.\n");
  s1 = fiobj_io_gets(strio);
  s2 = fiobj_io_gets(fdio);
  fprintf(stderr, "str(%d): %.*s", (int)s1.len, (int)s1.len, s1.data);
  fprintf(stderr, "fd(%d): %.*s", (int)s2.len, (int)s2.len, s2.data);
  if (s1.length != s2.length || memcmp(s1.data, s2.data, s1.length))
    fprintf(stderr,
            "* `gets` operation FAILED! (non equal data):\n"
            "%d bytes vs. %d bytes\n"
            "%.*s vs %.*s\n",
            (int)s1.len, (int)s2.len, (int)s1.len, s1.data, (int)s2.len,
            s2.data);
  else
    fprintf(stderr, "* `gets` operation passed (equal data).\n");
  s1 = fiobj_io_read(strio, 3);
  s2 = fiobj_io_read(fdio, 3);
  if (s1.length != s2.length || memcmp(s1.data, s2.data, s1.length))
    fprintf(stderr,
            "* `read` operation FAILED! (non equal data):\n"
            "%d bytes vs. %d bytes\n"
            "%.*s vs %.*s\n",
            (int)s1.len, (int)s2.len, (int)s1.len, s1.data, (int)s2.len,
            s2.data);
  else
    fprintf(stderr, "* `read` operation passed (equal data).\n");
  if (!filename) {
    s1 = fiobj_io_gets(strio);
    s2 = fiobj_io_gets(fdio);
    s1 = fiobj_io_gets(strio);
    s2 = fiobj_io_gets(fdio);
    if (s1.length != s2.length || memcmp(s1.data, s2.data, s1.length))
      fprintf(stderr,
              "* EOF `gets` operation FAILED! (non equal data):\n"
              "%d bytes vs. %d bytes\n"
              "%.*s vs %.*s\n",
              (int)s1.len, (int)s2.len, (int)s1.len, s1.data, (int)s2.len,
              s2.data);
    else
      fprintf(stderr, "* EOF `gets` operation passed (equal data).\n");
    s1 = fiobj_io_gets(strio);
    s2 = fiobj_io_gets(fdio);
    if (s1.data || s2.data)
      fprintf(stderr,
              "* EOF `gets` was not EOF?!\n"
              "str(%d): %.*s\n"
              "fd(%d): %.*s\n",
              (int)s1.len, (int)s1.len, s1.data, (int)s2.len, (int)s2.len,
              s2.data);
  }
  fiobj_free(text);
  fiobj_free(strio);
  fiobj_free(fdio);
}

#endif

#endif /* require POSIX */