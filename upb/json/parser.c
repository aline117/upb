
#line 1 "upb/json/parser.rl"
/*
** upb::json::Parser (upb_json_parser)
**
** A parser that uses the Ragel State Machine Compiler to generate
** the finite automata.
**
** Ragel only natively handles regular languages, but we can manually
** program it a bit to handle context-free languages like JSON, by using
** the "fcall" and "fret" constructs.
**
** This parser can handle the basics, but needs several things to be fleshed
** out:
**
** - handling of unicode escape sequences (including high surrogate pairs).
** - properly check and report errors for unknown fields, stack overflow,
**   improper array nesting (or lack of nesting).
** - handling of base64 sequences with padding characters.
** - handling of push-back (non-success returns from sink functions).
** - handling of keys/escape-sequences/etc that span input buffers.
*/

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Need to define __USE_XOPEN before including time.h to make strptime work. */
#ifndef __USE_XOPEN
#define __USE_XOPEN
#endif
#include <time.h>

#include "upb/json/parser.h"

#define UPB_JSON_MAX_DEPTH 64

static const char *kDoubleValueFullMessageName = "google.protobuf.DoubleValue";
static const char *kFloatValueFullMessageName = "google.protobuf.FloatValue";
static const char *kInt64ValueFullMessageName = "google.protobuf.Int64Value";
static const char *kUInt64ValueFullMessageName = "google.protobuf.UInt64Value";
static const char *kInt32ValueFullMessageName = "google.protobuf.Int32Value";
static const char *kUInt32ValueFullMessageName = "google.protobuf.UInt32Value";
static const char *kBoolValueFullMessageName = "google.protobuf.BoolValue";
static const char *kStringValueFullMessageName = "google.protobuf.StringValue";
static const char *kBytesValueFullMessageName = "google.protobuf.BytesValue";

/* Type of value message */
enum {
  VALUE_NULLVALUE   = 0,
  VALUE_NUMBERVALUE = 1,
  VALUE_STRINGVALUE = 2,
  VALUE_BOOLVALUE   = 3,
  VALUE_STRUCTVALUE = 4,
  VALUE_LISTVALUE   = 5
};

/* Forward declare */
static bool is_top_level(upb_json_parser *p);

static bool is_number_wrapper_object(upb_json_parser *p);
static bool does_number_wrapper_start(upb_json_parser *p);
static bool does_number_wrapper_end(upb_json_parser *p);

static bool is_string_wrapper_object(upb_json_parser *p);
static bool does_string_wrapper_start(upb_json_parser *p);
static bool does_string_wrapper_end(upb_json_parser *p);

static bool is_boolean_wrapper_object(upb_json_parser *p);
static bool does_boolean_wrapper_start(upb_json_parser *p);
static bool does_boolean_wrapper_end(upb_json_parser *p);

static bool is_duration_object(upb_json_parser *p);
static bool does_duration_start(upb_json_parser *p);
static bool does_duration_end(upb_json_parser *p);

static bool is_timestamp_object(upb_json_parser *p);
static bool does_timestamp_start(upb_json_parser *p);
static bool does_timestamp_end(upb_json_parser *p);

static bool is_value_object(upb_json_parser *p);
static bool does_value_start(upb_json_parser *p);
static bool does_value_end(upb_json_parser *p);

static bool is_listvalue_object(upb_json_parser *p);
static bool does_listvalue_start(upb_json_parser *p);
static bool does_listvalue_end(upb_json_parser *p);

static bool is_structvalue_object(upb_json_parser *p);
static bool does_structvalue_start(upb_json_parser *p);
static bool does_structvalue_end(upb_json_parser *p);

static void start_wrapper_object(upb_json_parser *p);
static void end_wrapper_object(upb_json_parser *p);

static void start_value_object(upb_json_parser *p, int value_type);
static void end_value_object(upb_json_parser *p);

static void start_listvalue_object(upb_json_parser *p);
static void end_listvalue_object(upb_json_parser *p);

static void start_structvalue_object(upb_json_parser *p);
static void end_structvalue_object(upb_json_parser *p);

static void start_object(upb_json_parser *p);
static void end_object(upb_json_parser *p);

static bool start_subobject(upb_json_parser *p);
static void end_subobject(upb_json_parser *p);

static void start_member(upb_json_parser *p);
static void end_member(upb_json_parser *p);
static bool end_membername(upb_json_parser *p);

static const char eof_ch = 'e';

typedef struct {
  upb_sink sink;

  /* The current message in which we're parsing, and the field whose value we're
   * expecting next. */
  const upb_msgdef *m;
  const upb_fielddef *f;

  /* The table mapping json name to fielddef for this message. */
  upb_strtable *name_table;

  /* We are in a repeated-field context, ready to emit mapentries as
   * submessages. This flag alters the start-of-object (open-brace) behavior to
   * begin a sequence of mapentry messages rather than a single submessage. */
  bool is_map;

  /* We are in a map-entry message context. This flag is set when parsing the
   * value field of a single map entry and indicates to all value-field parsers
   * (subobjects, strings, numbers, and bools) that the map-entry submessage
   * should end as soon as the value is parsed. */
  bool is_mapentry;

  /* If |is_map| or |is_mapentry| is true, |mapfield| refers to the parent
   * message's map field that we're currently parsing. This differs from |f|
   * because |f| is the field in the *current* message (i.e., the map-entry
   * message itself), not the parent's field that leads to this map. */
  const upb_fielddef *mapfield;
} upb_jsonparser_frame;

struct upb_json_parser {
  upb_env *env;
  const upb_json_parsermethod *method;
  upb_bytessink input_;

  /* Stack to track the JSON scopes we are in. */
  upb_jsonparser_frame stack[UPB_JSON_MAX_DEPTH];
  upb_jsonparser_frame *top;
  upb_jsonparser_frame *limit;

  upb_status status;

  /* Ragel's internal parsing stack for the parsing state machine. */
  int current_state;
  int parser_stack[UPB_JSON_MAX_DEPTH];
  int parser_top;

  /* The handle for the current buffer. */
  const upb_bufhandle *handle;

  /* Accumulate buffer.  See details in parser.rl. */
  const char *accumulated;
  size_t accumulated_len;
  char *accumulate_buf;
  size_t accumulate_buf_size;

  /* Multi-part text data.  See details in parser.rl. */
  int multipart_state;
  upb_selector_t string_selector;

  /* Input capture.  See details in parser.rl. */
  const char *capture;

  /* Intermediate result of parsing a unicode escape sequence. */
  uint32_t digit;

  /* Whether to proceed if unknown field is met. */
  bool ignore_json_unknown;

  /* Cache for parsing timestamp due to base and zone are handled in different
   * handlers. */
  struct tm tm;
};

struct upb_json_parsermethod {
  upb_refcounted base;

  upb_byteshandler input_handler_;

  /* Mainly for the purposes of refcounting, so all the fielddefs we point
   * to stay alive. */
  const upb_msgdef *msg;

  /* Keys are upb_msgdef*, values are upb_strtable (json_name -> fielddef) */
  upb_inttable name_tables;
};

#define PARSER_CHECK_RETURN(x) if (!(x)) return false

/* Used to signal that a capture has been suspended. */
static char suspend_capture;

static upb_selector_t getsel_for_handlertype(upb_json_parser *p,
                                             upb_handlertype_t type) {
  upb_selector_t sel;
  bool ok = upb_handlers_getselector(p->top->f, type, &sel);
  UPB_ASSERT(ok);
  return sel;
}

static upb_selector_t parser_getsel(upb_json_parser *p) {
  return getsel_for_handlertype(
      p, upb_handlers_getprimitivehandlertype(p->top->f));
}

static bool check_stack(upb_json_parser *p) {
  if ((p->top + 1) == p->limit) {
    upb_status_seterrmsg(&p->status, "Nesting too deep");
    upb_env_reporterror(p->env, &p->status);
    return false;
  }

  return true;
}

static void set_name_table(upb_json_parser *p, upb_jsonparser_frame *frame) {
  upb_value v;
  bool ok = upb_inttable_lookupptr(&p->method->name_tables, frame->m, &v);
  UPB_ASSERT(ok);
  frame->name_table = upb_value_getptr(v);
}

/* There are GCC/Clang built-ins for overflow checking which we could start
 * using if there was any performance benefit to it. */

static bool checked_add(size_t a, size_t b, size_t *c) {
  if (SIZE_MAX - a < b) return false;
  *c = a + b;
  return true;
}

static size_t saturating_multiply(size_t a, size_t b) {
  /* size_t is unsigned, so this is defined behavior even on overflow. */
  size_t ret = a * b;
  if (b != 0 && ret / b != a) {
    ret = SIZE_MAX;
  }
  return ret;
}


/* Base64 decoding ************************************************************/

/* TODO(haberman): make this streaming. */

static const signed char b64table[] = {
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      62/*+*/, -1,      -1,      -1,      63/*/ */,
  52/*0*/, 53/*1*/, 54/*2*/, 55/*3*/, 56/*4*/, 57/*5*/, 58/*6*/, 59/*7*/,
  60/*8*/, 61/*9*/, -1,      -1,      -1,      -1,      -1,      -1,
  -1,       0/*A*/,  1/*B*/,  2/*C*/,  3/*D*/,  4/*E*/,  5/*F*/,  6/*G*/,
  07/*H*/,  8/*I*/,  9/*J*/, 10/*K*/, 11/*L*/, 12/*M*/, 13/*N*/, 14/*O*/,
  15/*P*/, 16/*Q*/, 17/*R*/, 18/*S*/, 19/*T*/, 20/*U*/, 21/*V*/, 22/*W*/,
  23/*X*/, 24/*Y*/, 25/*Z*/, -1,      -1,      -1,      -1,      -1,
  -1,      26/*a*/, 27/*b*/, 28/*c*/, 29/*d*/, 30/*e*/, 31/*f*/, 32/*g*/,
  33/*h*/, 34/*i*/, 35/*j*/, 36/*k*/, 37/*l*/, 38/*m*/, 39/*n*/, 40/*o*/,
  41/*p*/, 42/*q*/, 43/*r*/, 44/*s*/, 45/*t*/, 46/*u*/, 47/*v*/, 48/*w*/,
  49/*x*/, 50/*y*/, 51/*z*/, -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1,
  -1,      -1,      -1,      -1,      -1,      -1,      -1,      -1
};

/* Returns the table value sign-extended to 32 bits.  Knowing that the upper
 * bits will be 1 for unrecognized characters makes it easier to check for
 * this error condition later (see below). */
int32_t b64lookup(unsigned char ch) { return b64table[ch]; }

/* Returns true if the given character is not a valid base64 character or
 * padding. */
bool nonbase64(unsigned char ch) { return b64lookup(ch) == -1 && ch != '='; }

static bool base64_push(upb_json_parser *p, upb_selector_t sel, const char *ptr,
                        size_t len) {
  const char *limit = ptr + len;
  for (; ptr < limit; ptr += 4) {
    uint32_t val;
    char output[3];

    if (limit - ptr < 4) {
      upb_status_seterrf(&p->status,
                         "Base64 input for bytes field not a multiple of 4: %s",
                         upb_fielddef_name(p->top->f));
      upb_env_reporterror(p->env, &p->status);
      return false;
    }

    val = b64lookup(ptr[0]) << 18 |
          b64lookup(ptr[1]) << 12 |
          b64lookup(ptr[2]) << 6  |
          b64lookup(ptr[3]);

    /* Test the upper bit; returns true if any of the characters returned -1. */
    if (val & 0x80000000) {
      goto otherchar;
    }

    output[0] = val >> 16;
    output[1] = (val >> 8) & 0xff;
    output[2] = val & 0xff;
    upb_sink_putstring(&p->top->sink, sel, output, 3, NULL);
  }
  return true;

otherchar:
  if (nonbase64(ptr[0]) || nonbase64(ptr[1]) || nonbase64(ptr[2]) ||
      nonbase64(ptr[3]) ) {
    upb_status_seterrf(&p->status,
                       "Non-base64 characters in bytes field: %s",
                       upb_fielddef_name(p->top->f));
    upb_env_reporterror(p->env, &p->status);
    return false;
  } if (ptr[2] == '=') {
    uint32_t val;
    char output;

    /* Last group contains only two input bytes, one output byte. */
    if (ptr[0] == '=' || ptr[1] == '=' || ptr[3] != '=') {
      goto badpadding;
    }

    val = b64lookup(ptr[0]) << 18 |
          b64lookup(ptr[1]) << 12;

    UPB_ASSERT(!(val & 0x80000000));
    output = val >> 16;
    upb_sink_putstring(&p->top->sink, sel, &output, 1, NULL);
    return true;
  } else {
    uint32_t val;
    char output[2];

    /* Last group contains only three input bytes, two output bytes. */
    if (ptr[0] == '=' || ptr[1] == '=' || ptr[2] == '=') {
      goto badpadding;
    }

    val = b64lookup(ptr[0]) << 18 |
          b64lookup(ptr[1]) << 12 |
          b64lookup(ptr[2]) << 6;

    output[0] = val >> 16;
    output[1] = (val >> 8) & 0xff;
    upb_sink_putstring(&p->top->sink, sel, output, 2, NULL);
    return true;
  }

badpadding:
  upb_status_seterrf(&p->status,
                     "Incorrect base64 padding for field: %s (%.*s)",
                     upb_fielddef_name(p->top->f),
                     4, ptr);
  upb_env_reporterror(p->env, &p->status);
  return false;
}


/* Accumulate buffer **********************************************************/

/* Functionality for accumulating a buffer.
 *
 * Some parts of the parser need an entire value as a contiguous string.  For
 * example, to look up a member name in a hash table, or to turn a string into
 * a number, the relevant library routines need the input string to be in
 * contiguous memory, even if the value spanned two or more buffers in the
 * input.  These routines handle that.
 *
 * In the common case we can just point to the input buffer to get this
 * contiguous string and avoid any actual copy.  So we optimistically begin
 * this way.  But there are a few cases where we must instead copy into a
 * separate buffer:
 *
 *   1. The string was not contiguous in the input (it spanned buffers).
 *
 *   2. The string included escape sequences that need to be interpreted to get
 *      the true value in a contiguous buffer. */

static void assert_accumulate_empty(upb_json_parser *p) {
  UPB_ASSERT(p->accumulated == NULL);
  UPB_ASSERT(p->accumulated_len == 0);
}

static void accumulate_clear(upb_json_parser *p) {
  p->accumulated = NULL;
  p->accumulated_len = 0;
}

/* Used internally by accumulate_append(). */
static bool accumulate_realloc(upb_json_parser *p, size_t need) {
  void *mem;
  size_t old_size = p->accumulate_buf_size;
  size_t new_size = UPB_MAX(old_size, 128);
  while (new_size < need) {
    new_size = saturating_multiply(new_size, 2);
  }

  mem = upb_env_realloc(p->env, p->accumulate_buf, old_size, new_size);
  if (!mem) {
    upb_status_seterrmsg(&p->status, "Out of memory allocating buffer.");
    upb_env_reporterror(p->env, &p->status);
    return false;
  }

  p->accumulate_buf = mem;
  p->accumulate_buf_size = new_size;
  return true;
}

/* Logically appends the given data to the append buffer.
 * If "can_alias" is true, we will try to avoid actually copying, but the buffer
 * must be valid until the next accumulate_append() call (if any). */
static bool accumulate_append(upb_json_parser *p, const char *buf, size_t len,
                              bool can_alias) {
  size_t need;

  if (!p->accumulated && can_alias) {
    p->accumulated = buf;
    p->accumulated_len = len;
    return true;
  }

  if (!checked_add(p->accumulated_len, len, &need)) {
    upb_status_seterrmsg(&p->status, "Integer overflow.");
    upb_env_reporterror(p->env, &p->status);
    return false;
  }

  if (need > p->accumulate_buf_size && !accumulate_realloc(p, need)) {
    return false;
  }

  if (p->accumulated != p->accumulate_buf) {
    memcpy(p->accumulate_buf, p->accumulated, p->accumulated_len);
    p->accumulated = p->accumulate_buf;
  }

  memcpy(p->accumulate_buf + p->accumulated_len, buf, len);
  p->accumulated_len += len;
  return true;
}

/* Returns a pointer to the data accumulated since the last accumulate_clear()
 * call, and writes the length to *len.  This with point either to the input
 * buffer or a temporary accumulate buffer. */
static const char *accumulate_getptr(upb_json_parser *p, size_t *len) {
  UPB_ASSERT(p->accumulated);
  *len = p->accumulated_len;
  return p->accumulated;
}


/* Mult-part text data ********************************************************/

/* When we have text data in the input, it can often come in multiple segments.
 * For example, there may be some raw string data followed by an escape
 * sequence.  The two segments are processed with different logic.  Also buffer
 * seams in the input can cause multiple segments.
 *
 * As we see segments, there are two main cases for how we want to process them:
 *
 *  1. we want to push the captured input directly to string handlers.
 *
 *  2. we need to accumulate all the parts into a contiguous buffer for further
 *     processing (field name lookup, string->number conversion, etc). */

/* This is the set of states for p->multipart_state. */
enum {
  /* We are not currently processing multipart data. */
  MULTIPART_INACTIVE = 0,

  /* We are processing multipart data by accumulating it into a contiguous
   * buffer. */
  MULTIPART_ACCUMULATE = 1,

  /* We are processing multipart data by pushing each part directly to the
   * current string handlers. */
  MULTIPART_PUSHEAGERLY = 2
};

/* Start a multi-part text value where we accumulate the data for processing at
 * the end. */
static void multipart_startaccum(upb_json_parser *p) {
  assert_accumulate_empty(p);
  UPB_ASSERT(p->multipart_state == MULTIPART_INACTIVE);
  p->multipart_state = MULTIPART_ACCUMULATE;
}

/* Start a multi-part text value where we immediately push text data to a string
 * value with the given selector. */
static void multipart_start(upb_json_parser *p, upb_selector_t sel) {
  assert_accumulate_empty(p);
  UPB_ASSERT(p->multipart_state == MULTIPART_INACTIVE);
  p->multipart_state = MULTIPART_PUSHEAGERLY;
  p->string_selector = sel;
}

static bool multipart_text(upb_json_parser *p, const char *buf, size_t len,
                           bool can_alias) {
  switch (p->multipart_state) {
    case MULTIPART_INACTIVE:
      upb_status_seterrmsg(
          &p->status, "Internal error: unexpected state MULTIPART_INACTIVE");
      upb_env_reporterror(p->env, &p->status);
      return false;

    case MULTIPART_ACCUMULATE:
      if (!accumulate_append(p, buf, len, can_alias)) {
        return false;
      }
      break;

    case MULTIPART_PUSHEAGERLY: {
      const upb_bufhandle *handle = can_alias ? p->handle : NULL;
      upb_sink_putstring(&p->top->sink, p->string_selector, buf, len, handle);
      break;
    }
  }

  return true;
}

/* Note: this invalidates the accumulate buffer!  Call only after reading its
 * contents. */
static void multipart_end(upb_json_parser *p) {
  UPB_ASSERT(p->multipart_state != MULTIPART_INACTIVE);
  p->multipart_state = MULTIPART_INACTIVE;
  accumulate_clear(p);
}


/* Input capture **************************************************************/

/* Functionality for capturing a region of the input as text.  Gracefully
 * handles the case where a buffer seam occurs in the middle of the captured
 * region. */

static void capture_begin(upb_json_parser *p, const char *ptr) {
  UPB_ASSERT(p->multipart_state != MULTIPART_INACTIVE);
  UPB_ASSERT(p->capture == NULL);
  p->capture = ptr;
}

static bool capture_end(upb_json_parser *p, const char *ptr) {
  UPB_ASSERT(p->capture);
  if (multipart_text(p, p->capture, ptr - p->capture, true)) {
    p->capture = NULL;
    return true;
  } else {
    return false;
  }
}

/* This is called at the end of each input buffer (ie. when we have hit a
 * buffer seam).  If we are in the middle of capturing the input, this
 * processes the unprocessed capture region. */
static void capture_suspend(upb_json_parser *p, const char **ptr) {
  if (!p->capture) return;

  if (multipart_text(p, p->capture, *ptr - p->capture, false)) {
    /* We use this as a signal that we were in the middle of capturing, and
     * that capturing should resume at the beginning of the next buffer.
     * 
     * We can't use *ptr here, because we have no guarantee that this pointer
     * will be valid when we resume (if the underlying memory is freed, then
     * using the pointer at all, even to compare to NULL, is likely undefined
     * behavior). */
    p->capture = &suspend_capture;
  } else {
    /* Need to back up the pointer to the beginning of the capture, since
     * we were not able to actually preserve it. */
    *ptr = p->capture;
  }
}

static void capture_resume(upb_json_parser *p, const char *ptr) {
  if (p->capture) {
    UPB_ASSERT(p->capture == &suspend_capture);
    p->capture = ptr;
  }
}


/* Callbacks from the parser **************************************************/

/* These are the functions called directly from the parser itself.
 * We define these in the same order as their declarations in the parser. */

static char escape_char(char in) {
  switch (in) {
    case 'r': return '\r';
    case 't': return '\t';
    case 'n': return '\n';
    case 'f': return '\f';
    case 'b': return '\b';
    case '/': return '/';
    case '"': return '"';
    case '\\': return '\\';
    default:
      UPB_ASSERT(0);
      return 'x';
  }
}

static bool escape(upb_json_parser *p, const char *ptr) {
  char ch = escape_char(*ptr);
  return multipart_text(p, &ch, 1, false);
}

static void start_hex(upb_json_parser *p) {
  p->digit = 0;
}

static void hexdigit(upb_json_parser *p, const char *ptr) {
  char ch = *ptr;

  p->digit <<= 4;

  if (ch >= '0' && ch <= '9') {
    p->digit += (ch - '0');
  } else if (ch >= 'a' && ch <= 'f') {
    p->digit += ((ch - 'a') + 10);
  } else {
    UPB_ASSERT(ch >= 'A' && ch <= 'F');
    p->digit += ((ch - 'A') + 10);
  }
}

static bool end_hex(upb_json_parser *p) {
  uint32_t codepoint = p->digit;

  /* emit the codepoint as UTF-8. */
  char utf8[3]; /* support \u0000 -- \uFFFF -- need only three bytes. */
  int length = 0;
  if (codepoint <= 0x7F) {
    utf8[0] = codepoint;
    length = 1;
  } else if (codepoint <= 0x07FF) {
    utf8[1] = (codepoint & 0x3F) | 0x80;
    codepoint >>= 6;
    utf8[0] = (codepoint & 0x1F) | 0xC0;
    length = 2;
  } else /* codepoint <= 0xFFFF */ {
    utf8[2] = (codepoint & 0x3F) | 0x80;
    codepoint >>= 6;
    utf8[1] = (codepoint & 0x3F) | 0x80;
    codepoint >>= 6;
    utf8[0] = (codepoint & 0x0F) | 0xE0;
    length = 3;
  }
  /* TODO(haberman): Handle high surrogates: if codepoint is a high surrogate
   * we have to wait for the next escape to get the full code point). */

  return multipart_text(p, utf8, length, false);
}

static void start_text(upb_json_parser *p, const char *ptr) {
  capture_begin(p, ptr);
}

static bool end_text(upb_json_parser *p, const char *ptr) {
  return capture_end(p, ptr);
}

static bool start_number(upb_json_parser *p, const char *ptr) {
  if (is_top_level(p)) {
    if (is_number_wrapper_object(p)) {
      start_wrapper_object(p);
    } else if (is_value_object(p)) {
      start_value_object(p, VALUE_NUMBERVALUE);
    } else {
      return false;
    }
  } else if (does_number_wrapper_start(p)) {
    if (!start_subobject(p)) {
      return false;
    }
    start_wrapper_object(p);
  } else if (does_value_start(p)) {
    if (!start_subobject(p)) {
      return false;
    }
    start_value_object(p, VALUE_NUMBERVALUE);
  }

  multipart_startaccum(p);
  capture_begin(p, ptr);
  return true;
}

static bool parse_number(upb_json_parser *p, bool is_quoted);

static bool end_number_nontop(upb_json_parser *p, const char *ptr) {
  if (!capture_end(p, ptr)) {
    return false;
  }

  if (p->top->f == NULL) {
    multipart_end(p);
    return true;
  }

  return parse_number(p, false);
}

static bool end_number(upb_json_parser *p, const char *ptr) {
  if (!end_number_nontop(p, ptr)) {
    return false;
  }

  if (does_number_wrapper_end(p)) {
    end_wrapper_object(p);
    if (!is_top_level(p)) {
      end_subobject(p);
    }
    return true;
  }

  if (does_value_end(p)) {
    end_value_object(p);
    if (!is_top_level(p)) {
      end_subobject(p);
    }
    return true;
  }

  return true;
}

/* |buf| is NULL-terminated. |buf| itself will never include quotes;
 * |is_quoted| tells us whether this text originally appeared inside quotes. */
static bool parse_number_from_buffer(upb_json_parser *p, const char *buf,
                                     bool is_quoted) {
  size_t len = strlen(buf);
  const char *bufend = buf + len;
  char *end;
  upb_fieldtype_t type = upb_fielddef_type(p->top->f);
  double val;
  double dummy;
  double inf = 1.0 / 0.0;  /* C89 does not have an INFINITY macro. */

  errno = 0;

  if (len == 0 || buf[0] == ' ') {
    return false;
  }

  /* For integer types, first try parsing with integer-specific routines.
   * If these succeed, they will be more accurate for int64/uint64 than
   * strtod().
   */
  switch (type) {
    case UPB_TYPE_ENUM:
    case UPB_TYPE_INT32: {
      long val = strtol(buf, &end, 0);
      if (errno == ERANGE || end != bufend) {
        break;
      } else if (val > INT32_MAX || val < INT32_MIN) {
        return false;
      } else {
        upb_sink_putint32(&p->top->sink, parser_getsel(p), val);
        return true;
      }
    }
    case UPB_TYPE_UINT32: {
      unsigned long val = strtoul(buf, &end, 0);
      if (end != bufend) {
        break;
      } else if (val > UINT32_MAX || errno == ERANGE) {
        return false;
      } else {
        upb_sink_putuint32(&p->top->sink, parser_getsel(p), val);
        return true;
      }
    }
    /* XXX: We can't handle [u]int64 properly on 32-bit machines because
     * strto[u]ll isn't in C89. */
    case UPB_TYPE_INT64: {
      long val = strtol(buf, &end, 0);
      if (errno == ERANGE || end != bufend) {
        break;
      } else {
        upb_sink_putint64(&p->top->sink, parser_getsel(p), val);
        return true;
      }
    }
    case UPB_TYPE_UINT64: {
      unsigned long val = strtoul(p->accumulated, &end, 0);
      if (end != bufend) {
        break;
      } else if (errno == ERANGE) {
        return false;
      } else {
        upb_sink_putuint64(&p->top->sink, parser_getsel(p), val);
        return true;
      }
    }
    default:
      break;
  }

  if (type != UPB_TYPE_DOUBLE && type != UPB_TYPE_FLOAT && is_quoted) {
    /* Quoted numbers for integer types are not allowed to be in double form. */
    return false;
  }

  if (len == strlen("Infinity") && strcmp(buf, "Infinity") == 0) {
    /* C89 does not have an INFINITY macro. */
    val = inf;
  } else if (len == strlen("-Infinity") && strcmp(buf, "-Infinity") == 0) {
    val = -inf;
  } else {
    val = strtod(buf, &end);
    if (errno == ERANGE || end != bufend) {
      return false;
    }
  }

  switch (type) {
#define CASE(capitaltype, smalltype, ctype, min, max)                     \
    case UPB_TYPE_ ## capitaltype: {                                      \
      if (modf(val, &dummy) != 0 || val > max || val < min) {             \
        return false;                                                     \
      } else {                                                            \
        upb_sink_put ## smalltype(&p->top->sink, parser_getsel(p),        \
                                  (ctype)val);                            \
        return true;                                                      \
      }                                                                   \
      break;                                                              \
    }
    case UPB_TYPE_ENUM:
    CASE(INT32, int32, int32_t, INT32_MIN, INT32_MAX);
    CASE(INT64, int64, int64_t, INT64_MIN, INT64_MAX);
    CASE(UINT32, uint32, uint32_t, 0, UINT32_MAX);
    CASE(UINT64, uint64, uint64_t, 0, UINT64_MAX);
#undef CASE

    case UPB_TYPE_DOUBLE:
      upb_sink_putdouble(&p->top->sink, parser_getsel(p), val);
      return true;
    case UPB_TYPE_FLOAT:
      if ((val > FLT_MAX || val < -FLT_MAX) && val != inf && val != -inf) {
        return false;
      } else {
        upb_sink_putfloat(&p->top->sink, parser_getsel(p), val);
        return true;
      }
    default:
      return false;
  }
}

static bool parse_number(upb_json_parser *p, bool is_quoted) {
  size_t len;
  const char *buf;

  /* strtol() and friends unfortunately do not support specifying the length of
   * the input string, so we need to force a copy into a NULL-terminated buffer. */
  if (!multipart_text(p, "\0", 1, false)) {
    return false;
  }

  buf = accumulate_getptr(p, &len);

  if (parse_number_from_buffer(p, buf, is_quoted)) {
    multipart_end(p);
    return true;
  } else {
    upb_status_seterrf(&p->status, "error parsing number: %s", buf);
    upb_env_reporterror(p->env, &p->status);
    multipart_end(p);
    return false;
  }
}

static bool parser_putbool(upb_json_parser *p, bool val) {
  bool ok;

  if (p->top->f == NULL) {
    return true;
  }

  if (upb_fielddef_type(p->top->f) != UPB_TYPE_BOOL) {
    upb_status_seterrf(&p->status,
                       "Boolean value specified for non-bool field: %s",
                       upb_fielddef_name(p->top->f));
    upb_env_reporterror(p->env, &p->status);
    return false;
  }

  ok = upb_sink_putbool(&p->top->sink, parser_getsel(p), val);
  UPB_ASSERT(ok);

  return true;
}

static bool end_bool(upb_json_parser *p, bool val) {
  if (is_top_level(p)) {
    if (is_boolean_wrapper_object(p)) {
      start_wrapper_object(p);
    } else if (is_value_object(p)) {
      start_value_object(p, VALUE_BOOLVALUE);
    } else {
      return false;
    }
  } else if (does_boolean_wrapper_start(p)) {
    if (!start_subobject(p)) {
      return false;
    }
    start_wrapper_object(p);
  } else if (does_value_start(p)) {
    if (!start_subobject(p)) {
      return false;
    }
    start_value_object(p, VALUE_BOOLVALUE);
  }

  if (!parser_putbool(p, val)) {
    return false;
  }

  if (does_boolean_wrapper_end(p)) {
    end_wrapper_object(p);
    if (!is_top_level(p)) {
      end_subobject(p);
    }
    return true;
  }

  if (does_value_end(p)) {
    end_value_object(p);
    if (!is_top_level(p)) {
      end_subobject(p);
    }
    return true;
  }

  return true;
}

static bool end_null(upb_json_parser *p) {
  const char *zero_ptr = "0";

  if (is_top_level(p)) {
    if (is_value_object(p)) {
      start_value_object(p, VALUE_NULLVALUE);
    } else {
      return true;
    }
  } else if (does_value_start(p)) {
    if (!start_subobject(p)) {
      return false;
    }
    start_value_object(p, VALUE_NULLVALUE);
  } else {
    return true;
  }

  /* Fill null_value field. */
  multipart_startaccum(p);
  capture_begin(p, zero_ptr);
  capture_end(p, zero_ptr + 1);
  parse_number(p, false);

  end_value_object(p);
  if (!is_top_level(p)) {
    end_subobject(p);
  }

  return true;
}

static bool start_stringval(upb_json_parser *p) {
  if (is_top_level(p)) {
    if (is_string_wrapper_object(p)) {
      start_wrapper_object(p);
    } else if (is_timestamp_object(p) || is_duration_object(p)) {
      start_object(p);
    } else if (is_value_object(p)) {
      start_value_object(p, VALUE_STRINGVALUE);
    } else {
      return false;
    }
  } else if (does_string_wrapper_start(p)) {
    if (!start_subobject(p)) {
      return false;
    }
    start_wrapper_object(p);
  } else if (does_timestamp_start(p) || does_duration_start(p)) {
    if (!start_subobject(p)) {
      return false;
    }
    start_object(p);
  } else if (does_value_start(p)) {
    if (!start_subobject(p)) {
      return false;
    }
    start_value_object(p, VALUE_STRINGVALUE);
  }

  if (p->top->f == NULL) {
    multipart_startaccum(p);
    return true;
  }

  if (upb_fielddef_isstring(p->top->f)) {
    upb_jsonparser_frame *inner;
    upb_selector_t sel;

    if (!check_stack(p)) return false;

    /* Start a new parser frame: parser frames correspond one-to-one with
     * handler frames, and string events occur in a sub-frame. */
    inner = p->top + 1;
    sel = getsel_for_handlertype(p, UPB_HANDLER_STARTSTR);
    upb_sink_startstr(&p->top->sink, sel, 0, &inner->sink);
    inner->m = p->top->m;
    inner->f = p->top->f;
    inner->name_table = NULL;
    inner->is_map = false;
    inner->is_mapentry = false;
    p->top = inner;

    if (upb_fielddef_type(p->top->f) == UPB_TYPE_STRING) {
      /* For STRING fields we push data directly to the handlers as it is
       * parsed.  We don't do this yet for BYTES fields, because our base64
       * decoder is not streaming.
       *
       * TODO(haberman): make base64 decoding streaming also. */
      multipart_start(p, getsel_for_handlertype(p, UPB_HANDLER_STRING));
      return true;
    } else {
      multipart_startaccum(p);
      return true;
    }
  } else if (upb_fielddef_type(p->top->f) != UPB_TYPE_BOOL &&
             upb_fielddef_type(p->top->f) != UPB_TYPE_MESSAGE) {
    /* No need to push a frame -- numeric values in quotes remain in the
     * current parser frame.  These values must accmulate so we can convert
     * them all at once at the end. */
    multipart_startaccum(p);
    return true;
  } else {
    upb_status_seterrf(&p->status,
                       "String specified for bool or submessage field: %s",
                       upb_fielddef_name(p->top->f));
    upb_env_reporterror(p->env, &p->status);
    return false;
  }
}

static bool end_stringval_nontop(upb_json_parser *p) {
  bool ok = true;

  if (is_timestamp_object(p) || is_duration_object(p)) {
    multipart_end(p);
    return true;
  }

  if (p->top->f == NULL) {
    multipart_end(p);
    return true;
  }

  switch (upb_fielddef_type(p->top->f)) {
    case UPB_TYPE_BYTES:
      if (!base64_push(p, getsel_for_handlertype(p, UPB_HANDLER_STRING),
                       p->accumulated, p->accumulated_len)) {
        return false;
      }
      /* Fall through. */

    case UPB_TYPE_STRING: {
      upb_selector_t sel = getsel_for_handlertype(p, UPB_HANDLER_ENDSTR);
      p->top--;
      upb_sink_endstr(&p->top->sink, sel);
      break;
    }

    case UPB_TYPE_ENUM: {
      /* Resolve enum symbolic name to integer value. */
      const upb_enumdef *enumdef =
          (const upb_enumdef*)upb_fielddef_subdef(p->top->f);

      size_t len;
      const char *buf = accumulate_getptr(p, &len);

      int32_t int_val = 0;
      ok = upb_enumdef_ntoi(enumdef, buf, len, &int_val);

      if (ok) {
        upb_selector_t sel = parser_getsel(p);
        upb_sink_putint32(&p->top->sink, sel, int_val);
      } else {
        upb_status_seterrf(&p->status, "Enum value unknown: '%.*s'", len, buf);
        upb_env_reporterror(p->env, &p->status);
      }

      break;
    }

    case UPB_TYPE_INT32:
    case UPB_TYPE_INT64:
    case UPB_TYPE_UINT32:
    case UPB_TYPE_UINT64:
    case UPB_TYPE_DOUBLE:
    case UPB_TYPE_FLOAT:
      ok = parse_number(p, true);
      break;

    default:
      UPB_ASSERT(false);
      upb_status_seterrmsg(&p->status, "Internal error in JSON decoder");
      upb_env_reporterror(p->env, &p->status);
      ok = false;
      break;
  }

  multipart_end(p);

  return ok;
}

static bool end_stringval(upb_json_parser *p) {
  if (!end_stringval_nontop(p)) {
    return false;
  }

  if (does_string_wrapper_end(p)) {
    end_wrapper_object(p);
    if (!is_top_level(p)) {
      end_subobject(p);
    }
    return true;
  }

  if (does_value_end(p)) {
    end_value_object(p);
    if (!is_top_level(p)) {
      end_subobject(p);
    }
    return true;
  }

  if (does_timestamp_end(p) || does_duration_end(p)) {
    end_object(p);
    if (!is_top_level(p)) {
      end_subobject(p);
    }
    return true;
  }

  return true;
}

static void start_duration_base(upb_json_parser *p, const char *ptr) {
  capture_begin(p, ptr);
}

static bool end_duration_base(upb_json_parser *p, const char *ptr) {
  size_t len;
  const char *buf;
  char seconds_buf[14];
  char nanos_buf[12];
  char *end;
  int64_t seconds = 0;
  int32_t nanos = 0;
  double val = 0.0;
  const char *seconds_membername = "seconds";
  const char *nanos_membername = "nanos";
  size_t fraction_start;

  if (!capture_end(p, ptr)) {
    return false;
  }

  buf = accumulate_getptr(p, &len);

  memset(seconds_buf, 0, 14);
  memset(nanos_buf, 0, 12);

  /* Find out base end. The maximus duration is 315576000000, which cannot be
   * represented by double without losing precision. Thus, we need to handle
   * fraction and base separately. */
  for (fraction_start = 0; fraction_start < len && buf[fraction_start] != '.';
       fraction_start++);

  /* Parse base */
  memcpy(seconds_buf, buf, fraction_start);
  seconds = strtol(seconds_buf, &end, 10);
  if (errno == ERANGE || end != seconds_buf + fraction_start) {
    upb_status_seterrf(&p->status, "error parsing duration: %s",
                       seconds_buf);
    upb_env_reporterror(p->env, &p->status);
    return false;
  }

  if (seconds > 315576000000) {
    upb_status_seterrf(&p->status, "error parsing duration: "
                                   "maximum acceptable value is "
                                   "315576000000");
    upb_env_reporterror(p->env, &p->status);
    return false;
  }

  if (seconds < -315576000000) {
    upb_status_seterrf(&p->status, "error parsing duration: "
                                   "minimum acceptable value is "
                                   "-315576000000");
    upb_env_reporterror(p->env, &p->status);
    return false;
  }

  /* Parse fraction */
  nanos_buf[0] = '0';
  memcpy(nanos_buf + 1, buf + fraction_start, len - fraction_start);
  val = strtod(nanos_buf, &end);
  if (errno == ERANGE || end != nanos_buf + len - fraction_start + 1) {
    upb_status_seterrf(&p->status, "error parsing duration: %s",
                       nanos_buf);
    upb_env_reporterror(p->env, &p->status);
    return false;
  }

  nanos = val * 1000000000;
  if (seconds < 0) nanos = -nanos;

  /* Clean up buffer */
  multipart_end(p);

  /* Set seconds */
  start_member(p);
  capture_begin(p, seconds_membername);
  capture_end(p, seconds_membername + 7);
  end_membername(p);
  upb_sink_putint64(&p->top->sink, parser_getsel(p), seconds);
  end_member(p);

  /* Set nanos */
  start_member(p);
  capture_begin(p, nanos_membername);
  capture_end(p, nanos_membername + 5);
  end_membername(p);
  upb_sink_putint32(&p->top->sink, parser_getsel(p), nanos);
  end_member(p);

  /* Continue previous environment */
  multipart_startaccum(p);

  return true;
}

static void start_timestamp_base(upb_json_parser *p, const char *ptr) {
  capture_begin(p, ptr);
}

static bool end_timestamp_base(upb_json_parser *p, const char *ptr) {
  size_t len;
  const char *buf;

  if (!capture_end(p, ptr)) {
    return false;
  }

  buf = accumulate_getptr(p, &len);

  /* Parse seconds */
  if (strptime(buf, "%FT%H:%M:%S", &p->tm) == NULL) {
    upb_status_seterrf(&p->status, "error parsing timestamp: %s", buf);
    upb_env_reporterror(p->env, &p->status);
    return false;
  }

  /* Clean up buffer */
  multipart_end(p);
  multipart_startaccum(p);

  return true;
}

static void start_timestamp_fraction(upb_json_parser *p, const char *ptr) {
  capture_begin(p, ptr);
}

static bool end_timestamp_fraction(upb_json_parser *p, const char *ptr) {
  size_t len;
  const char *buf;
  char nanos_buf[12];
  char *end;
  double val = 0.0;
  int32_t nanos;
  const char *nanos_membername = "nanos";

  memset(nanos_buf, 0, 12);

  if (!capture_end(p, ptr)) {
    return false;
  }

  buf = accumulate_getptr(p, &len);

  if (len > 10) {
    upb_status_seterrf(&p->status,
        "error parsing timestamp: at most 9-digit fraction.");
    upb_env_reporterror(p->env, &p->status);
    return false;
  }

  /* Parse nanos */
  nanos_buf[0] = '0';
  memcpy(nanos_buf + 1, buf, len);
  val = strtod(nanos_buf, &end);

  if (errno == ERANGE || end != nanos_buf + len + 1) {
    upb_status_seterrf(&p->status, "error parsing timestamp nanos: %s",
                       nanos_buf);
    upb_env_reporterror(p->env, &p->status);
    return false;
  }

  nanos = val * 1000000000;

  /* Clean up previous environment */
  multipart_end(p);

  /* Set nanos */
  start_member(p);
  capture_begin(p, nanos_membername);
  capture_end(p, nanos_membername + 5);
  end_membername(p);
  upb_sink_putint32(&p->top->sink, parser_getsel(p), nanos);
  end_member(p);

  /* Continue previous environment */
  multipart_startaccum(p);

  return true;
}

static void start_timestamp_zone(upb_json_parser *p, const char *ptr) {
  capture_begin(p, ptr);
}

static bool end_timestamp_zone(upb_json_parser *p, const char *ptr) {
  size_t len;
  const char *buf;
  int hours;
  int64_t seconds;
  const char *seconds_membername = "seconds";

  if (!capture_end(p, ptr)) {
    return false;
  }

  buf = accumulate_getptr(p, &len);

  if (buf[0] != 'Z') {
    if (sscanf(buf + 1, "%2d:00", &hours) != 1) {
      upb_status_seterrf(&p->status, "error parsing timestamp offset");
      upb_env_reporterror(p->env, &p->status);
      return false;
    }

    if (buf[0] == '+') {
      hours = -hours;
    }

    p->tm.tm_hour += hours;
  }

  /* Normalize tm */
  seconds = mktime(&p->tm);

  /* Check timestamp boundary */
  if (seconds < -62135596800) {
    upb_status_seterrf(&p->status, "error parsing timestamp: "
                                   "minimum acceptable value is "
                                   "0001-01-01T00:00:00Z");
    upb_env_reporterror(p->env, &p->status);
    return false;
  }

  /* Clean up previous environment */
  multipart_end(p);

  /* Set seconds */
  start_member(p);
  capture_begin(p, seconds_membername);
  capture_end(p, seconds_membername + 7);
  end_membername(p);
  upb_sink_putint64(&p->top->sink, parser_getsel(p), seconds);
  end_member(p);

  /* Continue previous environment */
  multipart_startaccum(p);

  return true;
}

static void start_member(upb_json_parser *p) {
  UPB_ASSERT(!p->top->f);
  multipart_startaccum(p);
}

/* Helper: invoked during parse_mapentry() to emit the mapentry message's key
 * field based on the current contents of the accumulate buffer. */
static bool parse_mapentry_key(upb_json_parser *p) {

  size_t len;
  const char *buf = accumulate_getptr(p, &len);

  /* Emit the key field. We do a bit of ad-hoc parsing here because the
   * parser state machine has already decided that this is a string field
   * name, and we are reinterpreting it as some arbitrary key type. In
   * particular, integer and bool keys are quoted, so we need to parse the
   * quoted string contents here. */

  p->top->f = upb_msgdef_itof(p->top->m, UPB_MAPENTRY_KEY);
  if (p->top->f == NULL) {
    upb_status_seterrmsg(&p->status, "mapentry message has no key");
    upb_env_reporterror(p->env, &p->status);
    return false;
  }
  switch (upb_fielddef_type(p->top->f)) {
    case UPB_TYPE_INT32:
    case UPB_TYPE_INT64:
    case UPB_TYPE_UINT32:
    case UPB_TYPE_UINT64:
      /* Invoke end_number. The accum buffer has the number's text already. */
      if (!parse_number(p, true)) {
        return false;
      }
      break;
    case UPB_TYPE_BOOL:
      if (len == 4 && !strncmp(buf, "true", 4)) {
        if (!parser_putbool(p, true)) {
          return false;
        }
      } else if (len == 5 && !strncmp(buf, "false", 5)) {
        if (!parser_putbool(p, false)) {
          return false;
        }
      } else {
        upb_status_seterrmsg(&p->status,
                             "Map bool key not 'true' or 'false'");
        upb_env_reporterror(p->env, &p->status);
        return false;
      }
      multipart_end(p);
      break;
    case UPB_TYPE_STRING:
    case UPB_TYPE_BYTES: {
      upb_sink subsink;
      upb_selector_t sel = getsel_for_handlertype(p, UPB_HANDLER_STARTSTR);
      upb_sink_startstr(&p->top->sink, sel, len, &subsink);
      sel = getsel_for_handlertype(p, UPB_HANDLER_STRING);
      upb_sink_putstring(&subsink, sel, buf, len, NULL);
      sel = getsel_for_handlertype(p, UPB_HANDLER_ENDSTR);
      upb_sink_endstr(&p->top->sink, sel);
      multipart_end(p);
      break;
    }
    default:
      upb_status_seterrmsg(&p->status, "Invalid field type for map key");
      upb_env_reporterror(p->env, &p->status);
      return false;
  }

  return true;
}

/* Helper: emit one map entry (as a submessage in the map field sequence). This
 * is invoked from end_membername(), at the end of the map entry's key string,
 * with the map key in the accumulate buffer. It parses the key from that
 * buffer, emits the handler calls to start the mapentry submessage (setting up
 * its subframe in the process), and sets up state in the subframe so that the
 * value parser (invoked next) will emit the mapentry's value field and then
 * end the mapentry message. */

static bool handle_mapentry(upb_json_parser *p) {
  const upb_fielddef *mapfield;
  const upb_msgdef *mapentrymsg;
  upb_jsonparser_frame *inner;
  upb_selector_t sel;

  /* Map entry: p->top->sink is the seq frame, so we need to start a frame
   * for the mapentry itself, and then set |f| in that frame so that the map
   * value field is parsed, and also set a flag to end the frame after the
   * map-entry value is parsed. */
  if (!check_stack(p)) return false;

  mapfield = p->top->mapfield;
  mapentrymsg = upb_fielddef_msgsubdef(mapfield);

  inner = p->top + 1;
  p->top->f = mapfield;
  sel = getsel_for_handlertype(p, UPB_HANDLER_STARTSUBMSG);
  upb_sink_startsubmsg(&p->top->sink, sel, &inner->sink);
  inner->m = mapentrymsg;
  inner->name_table = NULL;
  inner->mapfield = mapfield;
  inner->is_map = false;

  /* Don't set this to true *yet* -- we reuse parsing handlers below to push
   * the key field value to the sink, and these handlers will pop the frame
   * if they see is_mapentry (when invoked by the parser state machine, they
   * would have just seen the map-entry value, not key). */
  inner->is_mapentry = false;
  p->top = inner;

  /* send STARTMSG in submsg frame. */
  upb_sink_startmsg(&p->top->sink);

  parse_mapentry_key(p);

  /* Set up the value field to receive the map-entry value. */
  p->top->f = upb_msgdef_itof(p->top->m, UPB_MAPENTRY_VALUE);
  p->top->is_mapentry = true;  /* set up to pop frame after value is parsed. */
  p->top->mapfield = mapfield;
  if (p->top->f == NULL) {
    upb_status_seterrmsg(&p->status, "mapentry message has no value");
    upb_env_reporterror(p->env, &p->status);
    return false;
  }

  return true;
}

static bool end_membername(upb_json_parser *p) {
  UPB_ASSERT(!p->top->f);

  if (!p->top->m) {
    return true;
  }

  if (p->top->is_map) {
    return handle_mapentry(p);
  } else {
    size_t len;
    const char *buf = accumulate_getptr(p, &len);
    upb_value v;

    if (upb_strtable_lookup2(p->top->name_table, buf, len, &v)) {
      p->top->f = upb_value_getconstptr(v);
      multipart_end(p);

      return true;
    } else if (p->ignore_json_unknown) {
      multipart_end(p);
      return true;
    } else {
      upb_status_seterrf(&p->status, "No such field: %.*s\n", (int)len, buf);
      upb_env_reporterror(p->env, &p->status);
      return false;
    }
  }
}

static void end_member(upb_json_parser *p) {
  /* If we just parsed a map-entry value, end that frame too. */
  if (p->top->is_mapentry) {
    upb_status s = UPB_STATUS_INIT;
    upb_selector_t sel;
    bool ok;
    const upb_fielddef *mapfield;

    UPB_ASSERT(p->top > p->stack);
    /* send ENDMSG on submsg. */
    upb_sink_endmsg(&p->top->sink, &s);
    mapfield = p->top->mapfield;

    /* send ENDSUBMSG in repeated-field-of-mapentries frame. */
    p->top--;
    ok = upb_handlers_getselector(mapfield, UPB_HANDLER_ENDSUBMSG, &sel);
    UPB_ASSERT(ok);
    upb_sink_endsubmsg(&p->top->sink, sel);
  }

  p->top->f = NULL;
}

static bool start_subobject(upb_json_parser *p) {
  if (p->top->f == NULL) {
    upb_jsonparser_frame *inner;
    if (!check_stack(p)) return false;

    inner = p->top + 1;
    inner->m = NULL;
    inner->f = NULL;
    inner->is_map = false;
    inner->is_mapentry = false;
    p->top = inner;
    return true;
  }

  if (upb_fielddef_ismap(p->top->f)) {
    upb_jsonparser_frame *inner;
    upb_selector_t sel;

    /* Beginning of a map. Start a new parser frame in a repeated-field
     * context. */
    if (!check_stack(p)) return false;

    inner = p->top + 1;
    sel = getsel_for_handlertype(p, UPB_HANDLER_STARTSEQ);
    upb_sink_startseq(&p->top->sink, sel, &inner->sink);
    inner->m = upb_fielddef_msgsubdef(p->top->f);
    inner->name_table = NULL;
    inner->mapfield = p->top->f;
    inner->f = NULL;
    inner->is_map = true;
    inner->is_mapentry = false;
    p->top = inner;

    return true;
  } else if (upb_fielddef_issubmsg(p->top->f)) {
    upb_jsonparser_frame *inner;
    upb_selector_t sel;

    /* Beginning of a subobject. Start a new parser frame in the submsg
     * context. */
    if (!check_stack(p)) return false;

    inner = p->top + 1;

    sel = getsel_for_handlertype(p, UPB_HANDLER_STARTSUBMSG);
    upb_sink_startsubmsg(&p->top->sink, sel, &inner->sink);
    inner->m = upb_fielddef_msgsubdef(p->top->f);
    set_name_table(p, inner);
    inner->f = NULL;
    inner->is_map = false;
    inner->is_mapentry = false;
    p->top = inner;

    return true;
  } else {
    upb_status_seterrf(&p->status,
                       "Object specified for non-message/group field: %s",
                       upb_fielddef_name(p->top->f));
    upb_env_reporterror(p->env, &p->status);
    return false;
  }
}

static bool start_subobject_full(upb_json_parser *p) {
  if (is_top_level(p)) {
    if (is_value_object(p)) {
      start_value_object(p, VALUE_STRUCTVALUE);
      if (!start_subobject(p)) return false;
      start_structvalue_object(p);
    } else if (is_structvalue_object(p)) {
      start_structvalue_object(p);
    } else {
      return true;
    }
  } else if (does_structvalue_start(p)) {
    if (!start_subobject(p)) return false;
    start_structvalue_object(p);
  } else if (does_value_start(p)) {
    if (!start_subobject(p)) return false;
    start_value_object(p, VALUE_STRUCTVALUE);
    if (!start_subobject(p)) return false;
    start_structvalue_object(p);
  }

  return start_subobject(p);
}

static void end_subobject(upb_json_parser *p) {
  if (is_top_level(p)) {
    return;
  }

  if (p->top->is_map) {
    upb_selector_t sel;
    p->top--;
    sel = getsel_for_handlertype(p, UPB_HANDLER_ENDSEQ);
    upb_sink_endseq(&p->top->sink, sel);
  } else {
    upb_selector_t sel;
    bool is_unknown = p->top->m == NULL;
    p->top--;
    if (!is_unknown) {
      sel = getsel_for_handlertype(p, UPB_HANDLER_ENDSUBMSG);
      upb_sink_endsubmsg(&p->top->sink, sel);
    }
  }
}

static void end_subobject_full(upb_json_parser *p) {
  end_subobject(p);

  if (does_structvalue_end(p)) {
    end_structvalue_object(p);
    if (!is_top_level(p)) {
      end_subobject(p);
    }
  }

  if (does_value_end(p)) {
    end_value_object(p);
    if (!is_top_level(p)) {
      end_subobject(p);
    }
  }
}

static bool start_array(upb_json_parser *p) {
  upb_jsonparser_frame *inner;
  upb_selector_t sel;

  if (is_top_level(p)) {
    if (is_value_object(p)) {
      start_value_object(p, VALUE_LISTVALUE);
      if (!start_subobject(p)) return false;
      start_listvalue_object(p);
    } else if (is_listvalue_object(p)) {
      start_listvalue_object(p);
    } else {
      return false;
    }
  } else if (does_listvalue_start(p)) {
    if (!start_subobject(p)) return false;
    start_listvalue_object(p);
  } else if (does_value_start(p)) {
    if (!start_subobject(p)) return false;
    start_value_object(p, VALUE_LISTVALUE);
    if (!start_subobject(p)) return false;
    start_listvalue_object(p);
  }

  UPB_ASSERT(p->top->f);

  if (!upb_fielddef_isseq(p->top->f)) {
    upb_status_seterrf(&p->status,
                       "Array specified for non-repeated field: %s",
                       upb_fielddef_name(p->top->f));
    upb_env_reporterror(p->env, &p->status);
    return false;
  }

  if (!check_stack(p)) return false;

  inner = p->top + 1;
  sel = getsel_for_handlertype(p, UPB_HANDLER_STARTSEQ);
  upb_sink_startseq(&p->top->sink, sel, &inner->sink);
  inner->m = p->top->m;
  inner->name_table = NULL;
  inner->f = p->top->f;
  inner->is_map = false;
  inner->is_mapentry = false;
  p->top = inner;

  return true;
}

static void end_array(upb_json_parser *p) {
  upb_selector_t sel;

  UPB_ASSERT(p->top > p->stack);

  p->top--;
  sel = getsel_for_handlertype(p, UPB_HANDLER_ENDSEQ);
  upb_sink_endseq(&p->top->sink, sel);

  if (does_listvalue_end(p)) {
    end_listvalue_object(p);
    if (!is_top_level(p)) {
      end_subobject(p);
    }
  }

  if (does_value_end(p)) {
    end_value_object(p);
    if (!is_top_level(p)) {
      end_subobject(p);
    }
  }
}

static void start_object(upb_json_parser *p) {
  if (!p->top->is_map) {
    upb_sink_startmsg(&p->top->sink);
  }
}

static void end_object(upb_json_parser *p) {
  if (!p->top->is_map) {
    upb_status status;
    upb_status_clear(&status);
    upb_sink_endmsg(&p->top->sink, &status);
    if (!upb_ok(&status)) {
      upb_env_reporterror(p->env, &status);
    }
  }
}

static bool is_double_value(const upb_msgdef *m) {
  return strcmp(upb_msgdef_fullname(m), kDoubleValueFullMessageName) == 0;
}

static bool is_float_value(const upb_msgdef *m) {
  return strcmp(upb_msgdef_fullname(m), kFloatValueFullMessageName) == 0;
}

static bool is_int64_value(const upb_msgdef *m) {
  return strcmp(upb_msgdef_fullname(m), kInt64ValueFullMessageName) == 0;
}

static bool is_uint64_value(const upb_msgdef *m) {
  return strcmp(upb_msgdef_fullname(m), kUInt64ValueFullMessageName) == 0;
}

static bool is_int32_value(const upb_msgdef *m) {
  return strcmp(upb_msgdef_fullname(m), kInt32ValueFullMessageName) == 0;
}

static bool is_uint32_value(const upb_msgdef *m) {
  return strcmp(upb_msgdef_fullname(m), kUInt32ValueFullMessageName) == 0;
}

static bool is_bool_value(const upb_msgdef *m) {
  return strcmp(upb_msgdef_fullname(m), kBoolValueFullMessageName) == 0;
}

static bool is_string_value(const upb_msgdef *m) {
  return strcmp(upb_msgdef_fullname(m), kStringValueFullMessageName) == 0;
}

static bool is_bytes_value(const upb_msgdef *m) {
  return strcmp(upb_msgdef_fullname(m), kBytesValueFullMessageName) == 0;
}

static bool is_number_wrapper(const upb_msgdef *m) {
  return is_double_value(m) ||
         is_float_value(m) ||
         is_int64_value(m) ||
         is_uint64_value(m) ||
         is_int32_value(m) ||
         is_uint32_value(m);
}

static bool is_string_wrapper(const upb_msgdef *m) {
  return is_string_value(m) ||
         is_bytes_value(m);
}

static void start_wrapper_object(upb_json_parser *p) {
  const char *membername = "value";

  start_object(p);

  /* Set up context for parsing value */
  start_member(p);
  capture_begin(p, membername);
  capture_end(p, membername + 5);
  end_membername(p);
}

static void end_wrapper_object(upb_json_parser *p) {
  end_member(p);
  end_object(p);
}

static void start_value_object(upb_json_parser *p, int value_type) {
  const char *nullmember = "null_value";
  const char *numbermember = "number_value";
  const char *stringmember = "string_value";
  const char *boolmember = "bool_value";
  const char *structmember = "struct_value";
  const char *listmember = "list_value";
  const char *membername = "";

  switch (value_type) {
    case VALUE_NULLVALUE:
      membername = nullmember;
      break;
    case VALUE_NUMBERVALUE:
      membername = numbermember;
      break;
    case VALUE_STRINGVALUE:
      membername = stringmember;
      break;
    case VALUE_BOOLVALUE:
      membername = boolmember;
      break;
    case VALUE_STRUCTVALUE:
      membername = structmember;
      break;
    case VALUE_LISTVALUE:
      membername = listmember;
      break;
  }

  start_object(p);

  /* Set up context for parsing value */
  start_member(p);
  capture_begin(p, membername);
  capture_end(p, membername + strlen(membername));
  end_membername(p);
}

static void end_value_object(upb_json_parser *p) {
  end_member(p);
  end_object(p);
}

static void start_listvalue_object(upb_json_parser *p) {
  const char *membername = "values";

  start_object(p);

  /* Set up context for parsing value */
  start_member(p);
  capture_begin(p, membername);
  capture_end(p, membername + strlen(membername));
  end_membername(p);
}

static void end_listvalue_object(upb_json_parser *p) {
  end_member(p);
  end_object(p);
}

static void start_structvalue_object(upb_json_parser *p) {
  const char *membername = "fields";

  start_object(p);

  /* Set up context for parsing value */
  start_member(p);
  capture_begin(p, membername);
  capture_end(p, membername + strlen(membername));
  end_membername(p);
}

static void end_structvalue_object(upb_json_parser *p) {
  end_member(p);
  end_object(p);
}

static bool is_top_level(upb_json_parser *p) {
  return p->top == p->stack && p->top->f == NULL;
}

static bool does_number_wrapper_start(upb_json_parser *p) {
  return p->top->f != NULL &&
         upb_fielddef_issubmsg(p->top->f) &&
         is_number_wrapper(upb_fielddef_msgsubdef(p->top->f));
}

static bool does_number_wrapper_end(upb_json_parser *p) {
  return p->top->m != NULL && is_number_wrapper(p->top->m);
}

static bool is_number_wrapper_object(upb_json_parser *p) {
  return p->top->m != NULL && is_number_wrapper(p->top->m);
}

static bool does_string_wrapper_start(upb_json_parser *p) {
  return p->top->f != NULL &&
         upb_fielddef_issubmsg(p->top->f) &&
         is_string_wrapper(upb_fielddef_msgsubdef(p->top->f));
}

static bool does_string_wrapper_end(upb_json_parser *p) {
  return p->top->m != NULL && is_string_wrapper(p->top->m);
}

static bool is_string_wrapper_object(upb_json_parser *p) {
  return p->top->m != NULL && is_string_wrapper(p->top->m);
}

static bool does_boolean_wrapper_start(upb_json_parser *p) {
  return p->top->f != NULL &&
         upb_fielddef_issubmsg(p->top->f) &&
         is_bool_value(upb_fielddef_msgsubdef(p->top->f));
}

static bool does_boolean_wrapper_end(upb_json_parser *p) {
  return p->top->m != NULL && is_bool_value(p->top->m);
}

static bool is_boolean_wrapper_object(upb_json_parser *p) {
  return p->top->m != NULL && is_bool_value(p->top->m);
}

static bool does_duration_start(upb_json_parser *p) {
  return p->top->f != NULL &&
         upb_fielddef_issubmsg(p->top->f) &&
         upb_msgdef_duration(upb_fielddef_msgsubdef(p->top->f));
}

static bool does_duration_end(upb_json_parser *p) {
  return p->top->m != NULL && upb_msgdef_duration(p->top->m);
}

static bool is_duration_object(upb_json_parser *p) {
  return p->top->m != NULL && upb_msgdef_duration(p->top->m);
}

static bool does_timestamp_start(upb_json_parser *p) {
  return p->top->f != NULL &&
         upb_fielddef_issubmsg(p->top->f) &&
         upb_msgdef_timestamp(upb_fielddef_msgsubdef(p->top->f));
}

static bool does_timestamp_end(upb_json_parser *p) {
  return p->top->m != NULL && upb_msgdef_timestamp(p->top->m);
}

static bool is_timestamp_object(upb_json_parser *p) {
  return p->top->m != NULL && upb_msgdef_timestamp(p->top->m);
}

static bool does_value_start(upb_json_parser *p) {
  return p->top->f != NULL &&
         upb_fielddef_issubmsg(p->top->f) &&
         upb_msgdef_value(upb_fielddef_msgsubdef(p->top->f));
}

static bool does_value_end(upb_json_parser *p) {
  return p->top->m != NULL && upb_msgdef_value(p->top->m);
}

static bool is_value_object(upb_json_parser *p) {
  return p->top->m != NULL && upb_msgdef_value(p->top->m);
}

static bool does_listvalue_start(upb_json_parser *p) {
  return p->top->f != NULL &&
         upb_fielddef_issubmsg(p->top->f) &&
         upb_msgdef_listvalue(upb_fielddef_msgsubdef(p->top->f));
}

static bool does_listvalue_end(upb_json_parser *p) {
  return p->top->m != NULL && upb_msgdef_listvalue(p->top->m);
}

static bool is_listvalue_object(upb_json_parser *p) {
  return p->top->m != NULL && upb_msgdef_listvalue(p->top->m);
}

static bool does_structvalue_start(upb_json_parser *p) {
  return p->top->f != NULL &&
         upb_fielddef_issubmsg(p->top->f) &&
         upb_msgdef_structvalue(upb_fielddef_msgsubdef(p->top->f));
}

static bool does_structvalue_end(upb_json_parser *p) {
  /* return p->top != p->stack && upb_msgdef_structvalue((p->top - 1)->m); */
  return p->top->m != NULL && upb_msgdef_structvalue(p->top->m);
}

static bool is_structvalue_object(upb_json_parser *p) {
  return p->top->m != NULL && upb_msgdef_structvalue(p->top->m);
}

#define CHECK_RETURN_TOP(x) if (!(x)) goto error


/* The actual parser **********************************************************/

/* What follows is the Ragel parser itself.  The language is specified in Ragel
 * and the actions call our C functions above.
 *
 * Ragel has an extensive set of functionality, and we use only a small part of
 * it.  There are many action types but we only use a few:
 *
 *   ">" -- transition into a machine
 *   "%" -- transition out of a machine
 *   "@" -- transition into a final state of a machine.
 *
 * "@" transitions are tricky because a machine can transition into a final
 * state repeatedly.  But in some cases we know this can't happen, for example
 * a string which is delimited by a final '"' can only transition into its
 * final state once, when the closing '"' is seen. */


#line 2252 "upb/json/parser.rl"



#line 2121 "upb/json/parser.c"
static const char _json_actions[] = {
	0, 1, 0, 1, 1, 1, 3, 1, 
	4, 1, 6, 1, 7, 1, 8, 1, 
	9, 1, 10, 1, 11, 1, 12, 1, 
	13, 1, 21, 1, 23, 1, 24, 1, 
	25, 1, 27, 1, 28, 1, 30, 1, 
	32, 1, 33, 1, 34, 1, 35, 1, 
	36, 1, 38, 2, 4, 9, 2, 5, 
	6, 2, 7, 3, 2, 7, 9, 2, 
	14, 15, 2, 16, 17, 2, 18, 19, 
	2, 22, 20, 2, 26, 37, 2, 29, 
	2, 2, 30, 38, 2, 31, 20, 2, 
	33, 38, 2, 34, 38, 2, 35, 38, 
	3, 25, 22, 20, 3, 26, 37, 38, 
	4, 14, 15, 16, 17
};

static const short _json_key_offsets[] = {
	0, 0, 12, 13, 18, 23, 28, 29, 
	30, 31, 32, 33, 34, 35, 36, 37, 
	38, 43, 48, 49, 53, 58, 63, 68, 
	72, 76, 79, 82, 84, 88, 92, 94, 
	96, 101, 103, 105, 114, 120, 126, 132, 
	138, 140, 144, 147, 149, 151, 154, 155, 
	159, 161, 163, 165, 167, 168, 170, 172, 
	173, 175, 177, 178, 180, 182, 183, 185, 
	187, 188, 190, 192, 196, 198, 200, 201, 
	202, 203, 204, 206, 211, 220, 221, 221, 
	221, 226, 231, 236, 237, 238, 239, 240, 
	240, 241, 242, 243, 243, 244, 245, 246, 
	246, 251, 256, 257, 261, 266, 271, 276, 
	280, 280, 283, 286, 289, 292, 295, 298, 
	298, 298, 298, 298
};

static const char _json_trans_keys[] = {
	32, 34, 45, 91, 102, 110, 116, 123, 
	9, 13, 48, 57, 34, 32, 93, 125, 
	9, 13, 32, 44, 93, 9, 13, 32, 
	93, 125, 9, 13, 97, 108, 115, 101, 
	117, 108, 108, 114, 117, 101, 32, 34, 
	125, 9, 13, 32, 34, 125, 9, 13, 
	34, 32, 58, 9, 13, 32, 93, 125, 
	9, 13, 32, 44, 125, 9, 13, 32, 
	44, 125, 9, 13, 32, 34, 9, 13, 
	45, 48, 49, 57, 48, 49, 57, 46, 
	69, 101, 48, 57, 69, 101, 48, 57, 
	43, 45, 48, 57, 48, 57, 48, 57, 
	46, 69, 101, 48, 57, 34, 92, 34, 
	92, 34, 47, 92, 98, 102, 110, 114, 
	116, 117, 48, 57, 65, 70, 97, 102, 
	48, 57, 65, 70, 97, 102, 48, 57, 
	65, 70, 97, 102, 48, 57, 65, 70, 
	97, 102, 34, 92, 45, 48, 49, 57, 
	48, 49, 57, 46, 115, 48, 57, 115, 
	48, 57, 34, 46, 115, 48, 57, 48, 
	57, 48, 57, 48, 57, 48, 57, 45, 
	48, 57, 48, 57, 45, 48, 57, 48, 
	57, 84, 48, 57, 48, 57, 58, 48, 
	57, 48, 57, 58, 48, 57, 48, 57, 
	43, 45, 46, 90, 48, 57, 48, 57, 
	58, 48, 48, 34, 48, 57, 43, 45, 
	90, 48, 57, 34, 45, 91, 102, 110, 
	116, 123, 48, 57, 34, 32, 93, 125, 
	9, 13, 32, 44, 93, 9, 13, 32, 
	93, 125, 9, 13, 97, 108, 115, 101, 
	117, 108, 108, 114, 117, 101, 32, 34, 
	125, 9, 13, 32, 34, 125, 9, 13, 
	34, 32, 58, 9, 13, 32, 93, 125, 
	9, 13, 32, 44, 125, 9, 13, 32, 
	44, 125, 9, 13, 32, 34, 9, 13, 
	32, 9, 13, 32, 9, 13, 32, 9, 
	13, 32, 9, 13, 32, 9, 13, 32, 
	9, 13, 0
};

static const char _json_single_lengths[] = {
	0, 8, 1, 3, 3, 3, 1, 1, 
	1, 1, 1, 1, 1, 1, 1, 1, 
	3, 3, 1, 2, 3, 3, 3, 2, 
	2, 1, 3, 0, 2, 2, 0, 0, 
	3, 2, 2, 9, 0, 0, 0, 0, 
	2, 2, 1, 2, 0, 1, 1, 2, 
	0, 0, 0, 0, 1, 0, 0, 1, 
	0, 0, 1, 0, 0, 1, 0, 0, 
	1, 0, 0, 4, 0, 0, 1, 1, 
	1, 1, 0, 3, 7, 1, 0, 0, 
	3, 3, 3, 1, 1, 1, 1, 0, 
	1, 1, 1, 0, 1, 1, 1, 0, 
	3, 3, 1, 2, 3, 3, 3, 2, 
	0, 1, 1, 1, 1, 1, 1, 0, 
	0, 0, 0, 0
};

static const char _json_range_lengths[] = {
	0, 2, 0, 1, 1, 1, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	1, 1, 0, 1, 1, 1, 1, 1, 
	1, 1, 0, 1, 1, 1, 1, 1, 
	1, 0, 0, 0, 3, 3, 3, 3, 
	0, 1, 1, 0, 1, 1, 0, 1, 
	1, 1, 1, 1, 0, 1, 1, 0, 
	1, 1, 0, 1, 1, 0, 1, 1, 
	0, 1, 1, 0, 1, 1, 0, 0, 
	0, 0, 1, 1, 1, 0, 0, 0, 
	1, 1, 1, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	1, 1, 0, 1, 1, 1, 1, 1, 
	0, 1, 1, 1, 1, 1, 1, 0, 
	0, 0, 0, 0
};

static const short _json_index_offsets[] = {
	0, 0, 11, 13, 18, 23, 28, 30, 
	32, 34, 36, 38, 40, 42, 44, 46, 
	48, 53, 58, 60, 64, 69, 74, 79, 
	83, 87, 90, 94, 96, 100, 104, 106, 
	108, 113, 116, 119, 129, 133, 137, 141, 
	145, 148, 152, 155, 158, 160, 163, 165, 
	169, 171, 173, 175, 177, 179, 181, 183, 
	185, 187, 189, 191, 193, 195, 197, 199, 
	201, 203, 205, 207, 212, 214, 216, 218, 
	220, 222, 224, 226, 231, 240, 242, 243, 
	244, 249, 254, 259, 261, 263, 265, 267, 
	268, 270, 272, 274, 275, 277, 279, 281, 
	282, 287, 292, 294, 298, 303, 308, 313, 
	317, 318, 321, 324, 327, 330, 333, 336, 
	337, 338, 339, 340
};

static const unsigned char _json_indicies[] = {
	0, 2, 3, 4, 5, 6, 7, 8, 
	0, 3, 1, 9, 1, 11, 12, 1, 
	11, 10, 13, 14, 12, 13, 1, 14, 
	1, 1, 14, 10, 15, 1, 16, 1, 
	17, 1, 18, 1, 19, 1, 20, 1, 
	21, 1, 22, 1, 23, 1, 24, 1, 
	25, 26, 27, 25, 1, 28, 29, 30, 
	28, 1, 31, 1, 32, 33, 32, 1, 
	33, 1, 1, 33, 34, 35, 36, 37, 
	35, 1, 38, 39, 30, 38, 1, 39, 
	29, 39, 1, 40, 41, 42, 1, 41, 
	42, 1, 44, 45, 45, 43, 46, 1, 
	45, 45, 46, 43, 47, 47, 48, 1, 
	48, 1, 48, 43, 44, 45, 45, 42, 
	43, 50, 51, 49, 53, 54, 52, 55, 
	55, 55, 55, 55, 55, 55, 55, 56, 
	1, 57, 57, 57, 1, 58, 58, 58, 
	1, 59, 59, 59, 1, 60, 60, 60, 
	1, 62, 63, 61, 64, 65, 66, 1, 
	67, 68, 1, 69, 70, 1, 71, 1, 
	70, 71, 1, 72, 1, 69, 70, 68, 
	1, 73, 1, 74, 1, 75, 1, 76, 
	1, 77, 1, 78, 1, 79, 1, 80, 
	1, 81, 1, 82, 1, 83, 1, 84, 
	1, 85, 1, 86, 1, 87, 1, 88, 
	1, 89, 1, 90, 1, 91, 1, 92, 
	92, 93, 94, 1, 95, 1, 96, 1, 
	97, 1, 98, 1, 99, 1, 100, 1, 
	101, 1, 102, 102, 103, 101, 1, 104, 
	105, 106, 107, 108, 109, 110, 105, 1, 
	111, 1, 112, 113, 115, 116, 1, 115, 
	114, 117, 118, 116, 117, 1, 118, 1, 
	1, 118, 114, 119, 1, 120, 1, 121, 
	1, 122, 1, 123, 124, 1, 125, 1, 
	126, 1, 127, 128, 1, 129, 1, 130, 
	1, 131, 132, 133, 134, 132, 1, 135, 
	136, 137, 135, 1, 138, 1, 139, 140, 
	139, 1, 140, 1, 1, 140, 141, 142, 
	143, 144, 142, 1, 145, 146, 137, 145, 
	1, 146, 136, 146, 1, 147, 148, 148, 
	1, 149, 149, 1, 150, 150, 1, 151, 
	151, 1, 152, 152, 1, 153, 153, 1, 
	1, 1, 1, 1, 1, 0
};

static const char _json_trans_targs[] = {
	1, 0, 2, 106, 3, 6, 10, 13, 
	16, 105, 4, 3, 105, 4, 5, 7, 
	8, 9, 107, 11, 12, 108, 14, 15, 
	109, 17, 18, 110, 17, 18, 110, 19, 
	19, 20, 21, 22, 23, 110, 22, 23, 
	25, 26, 32, 111, 27, 29, 28, 30, 
	31, 34, 112, 35, 34, 112, 35, 33, 
	36, 37, 38, 39, 40, 34, 112, 35, 
	42, 43, 47, 43, 47, 44, 46, 45, 
	113, 49, 50, 51, 52, 53, 54, 55, 
	56, 57, 58, 59, 60, 61, 62, 63, 
	64, 65, 66, 67, 68, 74, 73, 69, 
	70, 71, 72, 73, 114, 75, 68, 73, 
	77, 79, 80, 83, 88, 92, 96, 78, 
	115, 115, 81, 80, 78, 81, 82, 84, 
	85, 86, 87, 115, 89, 90, 91, 115, 
	93, 94, 95, 115, 97, 98, 104, 97, 
	98, 104, 99, 99, 100, 101, 102, 103, 
	104, 102, 103, 115, 105, 105, 105, 105, 
	105, 105
};

static const char _json_trans_actions[] = {
	0, 0, 84, 78, 33, 0, 0, 0, 
	47, 39, 25, 0, 35, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 31, 96, 31, 0, 72, 0, 27, 
	0, 0, 25, 29, 29, 29, 0, 0, 
	0, 0, 0, 3, 0, 0, 0, 0, 
	0, 5, 15, 0, 0, 51, 7, 13, 
	0, 54, 9, 9, 9, 57, 60, 11, 
	17, 17, 17, 0, 0, 0, 19, 0, 
	21, 23, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 104, 63, 104, 0, 
	0, 0, 0, 0, 69, 0, 66, 66, 
	84, 78, 33, 0, 0, 0, 47, 39, 
	49, 81, 25, 0, 35, 0, 0, 0, 
	0, 0, 0, 90, 0, 0, 0, 93, 
	0, 0, 0, 87, 31, 96, 31, 0, 
	72, 0, 27, 0, 0, 25, 29, 29, 
	29, 0, 0, 100, 0, 37, 43, 45, 
	41, 75
};

static const char _json_eof_actions[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 1, 0, 1, 0, 0, 1, 
	1, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 37, 43, 45, 41, 75, 0, 
	0, 0, 0, 0
};

static const int json_start = 1;

static const int json_en_number_machine = 24;
static const int json_en_string_machine = 33;
static const int json_en_duration_machine = 41;
static const int json_en_timestamp_machine = 48;
static const int json_en_value_machine = 76;
static const int json_en_main = 1;


#line 2255 "upb/json/parser.rl"

size_t parse(void *closure, const void *hd, const char *buf, size_t size,
             const upb_bufhandle *handle) {
  upb_json_parser *parser = closure;

  /* Variables used by Ragel's generated code. */
  int cs = parser->current_state;
  int *stack = parser->parser_stack;
  int top = parser->parser_top;

  const char *p = buf;
  const char *pe = buf + size;
  const char *eof = &eof_ch;

  parser->handle = handle;

  UPB_UNUSED(hd);
  UPB_UNUSED(handle);

  capture_resume(parser, buf);

  
#line 2395 "upb/json/parser.c"
	{
	int _klen;
	unsigned int _trans;
	const char *_acts;
	unsigned int _nacts;
	const char *_keys;

	if ( p == pe )
		goto _test_eof;
	if ( cs == 0 )
		goto _out;
_resume:
	_keys = _json_trans_keys + _json_key_offsets[cs];
	_trans = _json_index_offsets[cs];

	_klen = _json_single_lengths[cs];
	if ( _klen > 0 ) {
		const char *_lower = _keys;
		const char *_mid;
		const char *_upper = _keys + _klen - 1;
		while (1) {
			if ( _upper < _lower )
				break;

			_mid = _lower + ((_upper-_lower) >> 1);
			if ( (*p) < *_mid )
				_upper = _mid - 1;
			else if ( (*p) > *_mid )
				_lower = _mid + 1;
			else {
				_trans += (unsigned int)(_mid - _keys);
				goto _match;
			}
		}
		_keys += _klen;
		_trans += _klen;
	}

	_klen = _json_range_lengths[cs];
	if ( _klen > 0 ) {
		const char *_lower = _keys;
		const char *_mid;
		const char *_upper = _keys + (_klen<<1) - 2;
		while (1) {
			if ( _upper < _lower )
				break;

			_mid = _lower + (((_upper-_lower) >> 1) & ~1);
			if ( (*p) < _mid[0] )
				_upper = _mid - 2;
			else if ( (*p) > _mid[1] )
				_lower = _mid + 2;
			else {
				_trans += (unsigned int)((_mid - _keys)>>1);
				goto _match;
			}
		}
		_trans += _klen;
	}

_match:
	_trans = _json_indicies[_trans];
	cs = _json_trans_targs[_trans];

	if ( _json_trans_actions[_trans] == 0 )
		goto _again;

	_acts = _json_actions + _json_trans_actions[_trans];
	_nacts = (unsigned int) *_acts++;
	while ( _nacts-- > 0 )
	{
		switch ( *_acts++ )
		{
	case 1:
#line 2126 "upb/json/parser.rl"
	{ p--; {cs = stack[--top]; goto _again;} }
	break;
	case 2:
#line 2128 "upb/json/parser.rl"
	{ p--; {stack[top++] = cs; cs = 24; goto _again;} }
	break;
	case 3:
#line 2132 "upb/json/parser.rl"
	{ start_text(parser, p); }
	break;
	case 4:
#line 2133 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_text(parser, p)); }
	break;
	case 5:
#line 2139 "upb/json/parser.rl"
	{ start_hex(parser); }
	break;
	case 6:
#line 2140 "upb/json/parser.rl"
	{ hexdigit(parser, p); }
	break;
	case 7:
#line 2141 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_hex(parser)); }
	break;
	case 8:
#line 2147 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(escape(parser, p)); }
	break;
	case 9:
#line 2153 "upb/json/parser.rl"
	{ p--; {cs = stack[--top]; goto _again;} }
	break;
	case 10:
#line 2165 "upb/json/parser.rl"
	{ start_duration_base(parser, p); }
	break;
	case 11:
#line 2166 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_duration_base(parser, p)); }
	break;
	case 12:
#line 2168 "upb/json/parser.rl"
	{ p--; {cs = stack[--top]; goto _again;} }
	break;
	case 13:
#line 2173 "upb/json/parser.rl"
	{ start_timestamp_base(parser, p); }
	break;
	case 14:
#line 2174 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_timestamp_base(parser, p)); }
	break;
	case 15:
#line 2176 "upb/json/parser.rl"
	{ start_timestamp_fraction(parser, p); }
	break;
	case 16:
#line 2177 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_timestamp_fraction(parser, p)); }
	break;
	case 17:
#line 2179 "upb/json/parser.rl"
	{ start_timestamp_zone(parser, p); }
	break;
	case 18:
#line 2180 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_timestamp_zone(parser, p)); }
	break;
	case 19:
#line 2182 "upb/json/parser.rl"
	{ p--; {cs = stack[--top]; goto _again;} }
	break;
	case 20:
#line 2187 "upb/json/parser.rl"
	{
        if (is_timestamp_object(parser)) {
          {stack[top++] = cs; cs = 48; goto _again;}
        } else if (is_duration_object(parser)) {
          {stack[top++] = cs; cs = 41; goto _again;}
        } else {
          {stack[top++] = cs; cs = 33; goto _again;}
        }
      }
	break;
	case 21:
#line 2198 "upb/json/parser.rl"
	{ p--; {stack[top++] = cs; cs = 76; goto _again;} }
	break;
	case 22:
#line 2203 "upb/json/parser.rl"
	{ start_member(parser); }
	break;
	case 23:
#line 2204 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_membername(parser)); }
	break;
	case 24:
#line 2207 "upb/json/parser.rl"
	{ end_member(parser); }
	break;
	case 25:
#line 2213 "upb/json/parser.rl"
	{ start_object(parser); }
	break;
	case 26:
#line 2216 "upb/json/parser.rl"
	{ end_object(parser); }
	break;
	case 27:
#line 2222 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(start_array(parser)); }
	break;
	case 28:
#line 2226 "upb/json/parser.rl"
	{ end_array(parser); }
	break;
	case 29:
#line 2231 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(start_number(parser, p)); }
	break;
	case 30:
#line 2232 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_number(parser, p)); }
	break;
	case 31:
#line 2234 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(start_stringval(parser)); }
	break;
	case 32:
#line 2235 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_stringval(parser)); }
	break;
	case 33:
#line 2237 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_bool(parser, true)); }
	break;
	case 34:
#line 2239 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_bool(parser, false)); }
	break;
	case 35:
#line 2241 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_null(parser)); }
	break;
	case 36:
#line 2243 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(start_subobject_full(parser)); }
	break;
	case 37:
#line 2244 "upb/json/parser.rl"
	{ end_subobject_full(parser); }
	break;
	case 38:
#line 2249 "upb/json/parser.rl"
	{ p--; {cs = stack[--top]; goto _again;} }
	break;
#line 2629 "upb/json/parser.c"
		}
	}

_again:
	if ( cs == 0 )
		goto _out;
	if ( ++p != pe )
		goto _resume;
	_test_eof: {}
	if ( p == eof )
	{
	const char *__acts = _json_actions + _json_eof_actions[cs];
	unsigned int __nacts = (unsigned int) *__acts++;
	while ( __nacts-- > 0 ) {
		switch ( *__acts++ ) {
	case 0:
#line 2124 "upb/json/parser.rl"
	{ p--; {cs = stack[--top]; goto _again;} }
	break;
	case 26:
#line 2216 "upb/json/parser.rl"
	{ end_object(parser); }
	break;
	case 30:
#line 2232 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_number(parser, p)); }
	break;
	case 33:
#line 2237 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_bool(parser, true)); }
	break;
	case 34:
#line 2239 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_bool(parser, false)); }
	break;
	case 35:
#line 2241 "upb/json/parser.rl"
	{ CHECK_RETURN_TOP(end_null(parser)); }
	break;
	case 37:
#line 2244 "upb/json/parser.rl"
	{ end_subobject_full(parser); }
	break;
#line 2673 "upb/json/parser.c"
		}
	}
	}

	_out: {}
	}

#line 2277 "upb/json/parser.rl"

  if (p != pe) {
    upb_status_seterrf(&parser->status, "Parse error at '%.*s'\n", pe - p, p);
    upb_env_reporterror(parser->env, &parser->status);
  } else {
    capture_suspend(parser, &p);
  }

error:
  /* Save parsing state back to parser. */
  parser->current_state = cs;
  parser->parser_top = top;

  return p - buf;
}

bool end(void *closure, const void *hd) {
  upb_json_parser *parser = closure;

  /* Prevent compile warning on unused static constants. */
  UPB_UNUSED(json_start);
  UPB_UNUSED(json_en_duration_machine);
  UPB_UNUSED(json_en_number_machine);
  UPB_UNUSED(json_en_string_machine);
  UPB_UNUSED(json_en_timestamp_machine);
  UPB_UNUSED(json_en_value_machine);
  UPB_UNUSED(json_en_main);

  parse(parser, hd, &eof_ch, 0, NULL);

  return parser->current_state >= 
#line 2713 "upb/json/parser.c"
105
#line 2307 "upb/json/parser.rl"
;
}

static void json_parser_reset(upb_json_parser *p) {
  int cs;
  int top;

  p->top = p->stack;
  p->top->f = NULL;
  p->top->is_map = false;
  p->top->is_mapentry = false;

  /* Emit Ragel initialization of the parser. */
  
#line 2730 "upb/json/parser.c"
	{
	cs = json_start;
	top = 0;
	}

#line 2321 "upb/json/parser.rl"
  p->current_state = cs;
  p->parser_top = top;
  accumulate_clear(p);
  p->multipart_state = MULTIPART_INACTIVE;
  p->capture = NULL;
  p->accumulated = NULL;
  upb_status_clear(&p->status);
}

static void visit_json_parsermethod(const upb_refcounted *r,
                                    upb_refcounted_visit *visit,
                                    void *closure) {
  const upb_json_parsermethod *method = (upb_json_parsermethod*)r;
  visit(r, upb_msgdef_upcast2(method->msg), closure);
}

static void free_json_parsermethod(upb_refcounted *r) {
  upb_json_parsermethod *method = (upb_json_parsermethod*)r;

  upb_inttable_iter i;
  upb_inttable_begin(&i, &method->name_tables);
  for(; !upb_inttable_done(&i); upb_inttable_next(&i)) {
    upb_value val = upb_inttable_iter_value(&i);
    upb_strtable *t = upb_value_getptr(val);
    upb_strtable_uninit(t);
    upb_gfree(t);
  }

  upb_inttable_uninit(&method->name_tables);

  upb_gfree(r);
}

static void add_jsonname_table(upb_json_parsermethod *m, const upb_msgdef* md) {
  upb_msg_field_iter i;
  upb_strtable *t;

  /* It would be nice to stack-allocate this, but protobufs do not limit the
   * length of fields to any reasonable limit. */
  char *buf = NULL;
  size_t len = 0;

  if (upb_inttable_lookupptr(&m->name_tables, md, NULL)) {
    return;
  }

  /* TODO(haberman): handle malloc failure. */
  t = upb_gmalloc(sizeof(*t));
  upb_strtable_init(t, UPB_CTYPE_CONSTPTR);
  upb_inttable_insertptr(&m->name_tables, md, upb_value_ptr(t));

  for(upb_msg_field_begin(&i, md);
      !upb_msg_field_done(&i);
      upb_msg_field_next(&i)) {
    const upb_fielddef *f = upb_msg_iter_field(&i);

    /* Add an entry for the JSON name. */
    size_t field_len = upb_fielddef_getjsonname(f, buf, len);
    if (field_len > len) {
      size_t len2;
      buf = upb_grealloc(buf, 0, field_len);
      len = field_len;
      len2 = upb_fielddef_getjsonname(f, buf, len);
      UPB_ASSERT(len == len2);
    }
    upb_strtable_insert(t, buf, upb_value_constptr(f));

    if (strcmp(buf, upb_fielddef_name(f)) != 0) {
      /* Since the JSON name is different from the regular field name, add an
       * entry for the raw name (compliant proto3 JSON parsers must accept
       * both). */
      upb_strtable_insert(t, upb_fielddef_name(f), upb_value_constptr(f));
    }

    if (upb_fielddef_issubmsg(f)) {
      add_jsonname_table(m, upb_fielddef_msgsubdef(f));
    }
  }

  upb_gfree(buf);
}

/* Public API *****************************************************************/

upb_json_parser *upb_json_parser_create(upb_env *env,
                                        const upb_json_parsermethod *method,
                                        upb_sink *output,
                                        bool ignore_json_unknown) {
#ifndef NDEBUG
  const size_t size_before = upb_env_bytesallocated(env);
#endif
  upb_json_parser *p = upb_env_malloc(env, sizeof(upb_json_parser));
  if (!p) return false;

  p->env = env;
  p->method = method;
  p->limit = p->stack + UPB_JSON_MAX_DEPTH;
  p->accumulate_buf = NULL;
  p->accumulate_buf_size = 0;
  upb_bytessink_reset(&p->input_, &method->input_handler_, p);

  json_parser_reset(p);
  upb_sink_reset(&p->top->sink, output->handlers, output->closure);
  p->top->m = upb_handlers_msgdef(output->handlers);
  set_name_table(p, p->top);

  p->ignore_json_unknown = ignore_json_unknown;

  /* If this fails, uncomment and increase the value in parser.h. */
  /* fprintf(stderr, "%zd\n", upb_env_bytesallocated(env) - size_before); */
  UPB_ASSERT_DEBUGVAR(upb_env_bytesallocated(env) - size_before <=
                      UPB_JSON_PARSER_SIZE);
  return p;
}

upb_bytessink *upb_json_parser_input(upb_json_parser *p) {
  return &p->input_;
}

upb_json_parsermethod *upb_json_parsermethod_new(const upb_msgdef* md,
                                                 const void* owner) {
  static const struct upb_refcounted_vtbl vtbl = {visit_json_parsermethod,
                                                  free_json_parsermethod};
  upb_json_parsermethod *ret = upb_gmalloc(sizeof(*ret));
  upb_refcounted_init(upb_json_parsermethod_upcast_mutable(ret), &vtbl, owner);

  ret->msg = md;
  upb_ref2(md, ret);

  upb_byteshandler_init(&ret->input_handler_);
  upb_byteshandler_setstring(&ret->input_handler_, parse, ret);
  upb_byteshandler_setendstr(&ret->input_handler_, end, ret);

  upb_inttable_init(&ret->name_tables, UPB_CTYPE_PTR);

  add_jsonname_table(ret, md);

  return ret;
}

const upb_byteshandler *upb_json_parsermethod_inputhandler(
    const upb_json_parsermethod *m) {
  return &m->input_handler_;
}
