/*
Copyright: Boaz segev, 2016-2017
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/
#ifndef H_RESP_PARSER_H
/**
 * This single file library is a RESP parser for Redis connections.
 *
 * To use this file, the `.c` file in which this file is included MUST define a
 * number of callbacks, as later inticated.
 *
 * When feeding the parser, the parser will inform of any trailing bytes (bytes
 * at the end of the buffer that could not be parsed). These bytes should be
 * resent to the parser along with more data. Zero is a valid return value.
 */
#define H_RESP_PARSER_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* *****************************************************************************
The Parser
***************************************************************************** */

typedef struct resp_parser_s {
  intptr_t obj_countdown;
  intptr_t expecting;
} resp_parser_s;

static size_t resp_parse(resp_parser_s *parser, const void *buffer,
                         size_t length);

/* *****************************************************************************
Required Parser Callbacks (to be defined by the including file)
***************************************************************************** */

static int resp_on_number(resp_parser_s *parser, int64_t num);
static int resp_on_okay(resp_parser_s *parser);
static int resp_on_null(resp_parser_s *parser);

static int resp_on_start_string(resp_parser_s *parser, size_t str_len);
static int resp_on_string_chunk(resp_parser_s *parser, void *data, size_t len);
static int resp_on_end_string(resp_parser_s *parser);

static int resp_on_err_msg(resp_parser_s *parser, void *data, size_t len);

static int resp_on_start_array(resp_parser_s *parser, size_t array_len);

static int resp_on_message(resp_parser_s *parser);

static int resp_on_parser_error(resp_parser_s *parser);

/* *****************************************************************************
Seeking the new line...
***************************************************************************** */

#if PREFER_MEMCHAR

/* a helper that seeks any char, converts it to NUL and returns 1 if found. */
inline static uint8_t seek2ch(uint8_t **pos, uint8_t *const limit, uint8_t ch) {
  /* This is library based alternative that is sometimes slower  */
  if (*pos >= limit || **pos == ch) {
    return 0;
  }
  uint8_t *tmp = memchr(*pos, ch, limit - (*pos));
  if (tmp) {
    *pos = tmp;
    return 1;
  }
  *pos = limit;
  return 0;
}

#else

/* a helper that seeks any char, converts it to NUL and returns 1 if found. */
static inline uint8_t seek2ch(uint8_t **buffer, const uint8_t *const limit,
                              const uint8_t c) {
  /* this single char lookup is better when the target is closer... */
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
      return 1;
    }
    (*buffer)++;
  }
  return 0;
}

#endif

/* *****************************************************************************
Parsing RESP requests
***************************************************************************** */

/**
 * Parses raw RESP data into facil.io objects (`fiobj_s *`).
 *
 * Partial unparsed data will be moved to the begining of the buffer.
 *
 * Returns the number of bytes to be resent. i.e., for a return value 5, the
 * last 5 bytes in the buffer need to be resent to the parser.
 */
static size_t resp_parse(resp_parser_s *parser, const void *buffer,
                         size_t length) {
  uint8_t *pos = (uint8_t *)buffer;
  const uint8_t *stop = pos + length;
  while (pos < stop) {
    uint8_t *eol;
    if (parser->expecting) {
      if (pos + parser->expecting + 2 > stop) {
        /* read, but make sure the buffer includes the new line markers */
        size_t tmp = (size_t)((uintptr_t)stop - (uintptr_t)pos);
        if ((intptr_t)tmp >= parser->expecting)
          tmp = parser->expecting - 1;
        resp_on_string_chunk(parser, (void *)pos, tmp);
        parser->expecting -= tmp;
        return 0;
      } else {
        resp_on_string_chunk(parser, (void *)pos, parser->expecting);
        resp_on_end_string(parser);
        pos += parser->expecting;
        if (pos[0] == '\r')
          ++pos;
        if (pos[0] == '\n')
          ++pos;
        parser->expecting = 0;
        --parser->obj_countdown;
        if (parser->obj_countdown <= 0) {
          parser->obj_countdown = 0;
          resp_on_message(parser);
        }
        continue;
      }
    }
    eol = pos;
    if (seek2ch(&eol, stop, '\n') == 0)
      break;
    switch (*pos) {
    case '+':
      if (pos[1] == 'O' && pos[2] == 'K' && pos[3] == '\r' && pos[4] == 'n') {
        resp_on_okay(parser);
        --parser->obj_countdown;
        break;
      }
      resp_on_start_string(parser,
                           (size_t)((uintptr_t)eol - (uintptr_t)pos - 3));
      resp_on_string_chunk(parser, (void *)(pos + 1),
                           (size_t)((uintptr_t)eol - (uintptr_t)pos - 3));
      resp_on_end_string(parser);
      --parser->obj_countdown;
      break;
    case '-':
      resp_on_err_msg(parser, pos,
                      (size_t)((uintptr_t)eol - (uintptr_t)pos - 2));
      --parser->obj_countdown;
      break;
    case '*': /* fallthrough */
    case '$': /* fallthrough */
    case ':': {
      uint8_t id = *pos;
      uint8_t inv = 0;
      int64_t i = 0;
      ++pos;
      if (pos[0] == '-') {
        inv = 1;
        ++pos;
      }
      while ((size_t)(pos[0] - (uint8_t)'0') <= 9) {
        i = (i * 10) + (pos[0] - ((uint8_t)'0'));
        ++pos;
      }
      if (inv)
        i = i * -1;

      switch (id) {
      case ':':
        resp_on_number(parser, i);
        --parser->obj_countdown;
        break;
      case '$':
        if (i < 0) {
          resp_on_null(parser);
          --parser->obj_countdown;
        } else if (i == 0) {
          resp_on_start_string(parser, 0);
          resp_on_end_string(parser);
          --parser->obj_countdown;
        } else {
          resp_on_start_string(parser, i);
          parser->expecting = i;
        }
        break;
      case '*':
        if (i < 0) {
          resp_on_null(parser);
          --parser->obj_countdown;
        } else {
          resp_on_start_array(parser, i);
          parser->obj_countdown += i;
        }
        break;
      }
    } break;
    default:
      resp_on_parser_error(parser);
      return (size_t)((uintptr_t)stop - (uintptr_t)pos);
    }
    pos = eol + 1;
    if (parser->obj_countdown <= 0 && !parser->expecting) {
      parser->obj_countdown = 0;
      resp_on_message(parser);
    }
  }
  return (size_t)((uintptr_t)stop - (uintptr_t)pos);
}

#endif /* H_RESP_PARSER_H */
