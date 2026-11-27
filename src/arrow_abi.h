// FST-X 1.0: Arrow C data and C stream interface (WS2).
// The three structs and the flag values are the Arrow C Data Interface and C Stream Interface ABI, copied from the
// Apache Arrow specification (https://arrow.apache.org/docs/format/CDataInterface.html), Apache License 2.0.
// The fstx_arrow_* declarations below are fstx's own (MIT). No R headers: the fuzz target builds this without R.
#ifndef FSTX_ARROW_ABI_H
#define FSTX_ARROW_ABI_H

#include <stdint.h>

#include "fstx_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

#define ARROW_FLAG_DICTIONARY_ORDERED 1
#define ARROW_FLAG_NULLABLE 2
#define ARROW_FLAG_MAP_KEYS_SORTED 4

struct ArrowSchema {
  const char* format;
  const char* name;
  const char* metadata;
  int64_t flags;
  int64_t n_children;
  struct ArrowSchema** children;
  struct ArrowSchema* dictionary;
  void (*release)(struct ArrowSchema*);
  void* private_data;
};

struct ArrowArray {
  int64_t length;
  int64_t null_count;
  int64_t offset;
  int64_t n_buffers;
  int64_t n_children;
  const void** buffers;
  struct ArrowArray** children;
  struct ArrowArray* dictionary;
  void (*release)(struct ArrowArray*);
  void* private_data;
};

#endif  // ARROW_C_DATA_INTERFACE

#ifndef ARROW_C_STREAM_INTERFACE
#define ARROW_C_STREAM_INTERFACE

struct ArrowArrayStream {
  int (*get_schema)(struct ArrowArrayStream*, struct ArrowSchema* out);
  int (*get_next)(struct ArrowArrayStream*, struct ArrowArray* out);
  const char* (*get_last_error)(struct ArrowArrayStream*);
  void (*release)(struct ArrowArrayStream*);
  void* private_data;
};

#endif  // ARROW_C_STREAM_INTERFACE

// ---- fstx: an Arrow stream straight from a file (r_arrow.cpp)
typedef struct fstx_arrow_args {
  const uint32_t* cols;      /* column indexes (any order, repeats allowed) */
  const char* const* names;  /* UTF-8, NUL-terminated: the field names, one per column */
  uint32_t n_cols;
  uint64_t from, to;         /* rows [from, to), 0-based; ignored when rows is given */
  const uint64_t* rows;      /* or NULL: 0-based rows, ascending (repeats allowed) */
  uint64_t n_rows;
  int threads;               /* <= 0: all cores */
  uint64_t batch_rows;       /* rows per batch at most; 0 = one batch per row group. Batches never span row groups */
  int verify;                /* FSTX_VERIFY_HASH / FSTX_VERIFY_NULLS bits (fstx_core.h) */
  int raw;                   /* != 0: Date and POSIXct columns come as stored (int32 / float64), not date32 / timestamp */
  int flags;                 /* FSTX_ARROW_* bits below (0: dictionary-coded columns as dictionaries, no UTF-8 check) */
  uint8_t* utf8_bad;         /* with FSTX_ARROW_CHECK_UTF8: n_cols bytes the stream sets after every batch it returns,
                                1 where that batch's column holds text that is not valid UTF-8, else 0 */
} fstx_arrow_args;

enum {
  FSTX_ARROW_PLAIN_STRINGS = 1, /* string columns always as utf8 / large_utf8 (dictionary pages laid out as text) */
  FSTX_ARROW_CHECK_UTF8 = 2     /* check the text of every string column (needs utf8_bad) */
};

/* Opens path and fills *out (which must be released, i.e. out->release == NULL). The stream owns the open file
 * until its release callback runs. Its callbacks never call R and may run on any thread (one at a time per
 * stream). Returns FSTX_OK or an error code with err filled in; *out is untouched on error. */
int fstx_arrow_stream_open(const char* path_utf8, const fstx_arrow_args* args, struct ArrowArrayStream* out,
                           fstx_error* err);

/* ---- engine helpers the stream uses (core_colsrc.cpp); not part of the public engine API */
typedef struct fstx_scratch fstx_scratch; /* one worker's decode state */
typedef struct fstx_scratchset fstx_scratchset; /* decode state for every worker, kept from one call to the next */
fstx_scratchset* fstx_scratchset_new(void); /* NULL when out of memory */
void fstx_scratchset_free(fstx_scratchset* ss);
typedef int (*fstx_task_fn)(void* ctx, int64_t i, fstx_scratch* w); /* 0 = done, else stop the other tasks */
/* Runs fn(ctx, i, w) for i in [0, n) on up to `threads` workers of the engine's pool (<= 0: all cores; on the
 * calling thread when the pool is busy). fn must not throw. ss: the workers' decode state (NULL: made for this
 * call); one call at a time per scratch set. */
int fstx_run_tasks(int64_t n, int threads, fstx_task_fn fn, void* ctx, fstx_scratchset* ss, fstx_error* err);
/* A buffer of at least `bytes` for the task's own use, kept with the worker's state (contents undefined); valid
 * until the next call on the same w. NULL when out of memory. */
void* fstx_scratch_buffer(fstx_scratch* w, size_t bytes);
/* fstx_colsrc_load on the calling thread, with a worker's decode state (inside a task of fstx_run_tasks). */
int fstx_colsrc_load_with(fstx_colsrc* s, void* dst, fstx_scratch* w, fstx_error* err);
/* Decodes page p of a string source (if not yet) and returns it as fstx_colsrc_str_page does. Thread-safe for
 * distinct pages of one source once its dictionaries are decoded (fstx_colsrc_dict); w may be NULL. */
int fstx_colsrc_load_page(fstx_colsrc* s, uint32_t page, fstx_scratch* w, const int32_t** lens, const char** bytes,
                          int32_t* dict, fstx_error* err);
/* Text straight into a caller's buffer (whole plain / FSST pages of a string source): fstx_colsrc_open_page reads
 * and checks page p and gives its bytes of text and its NA count without decoding the text; *direct = 0 when the
 * page must go through fstx_colsrc_load_page instead (dictionary pages, pages cut by the source's row range).
 * fstx_colsrc_page_text then writes the text to dst (room for text + slack bytes; writes stay below that) and gives
 * the rows' lengths (-1 = NA). Thread-safe for distinct pages; free with fstx_colsrc_drop_page. */
int fstx_colsrc_open_page(fstx_colsrc* s, uint32_t page, fstx_scratch* w, int* direct, uint64_t* text, uint32_t* nas,
                          fstx_error* err);
int fstx_colsrc_page_text(fstx_colsrc* s, uint32_t page, char* dst, size_t slack, const int32_t** lens,
                          fstx_error* err);
/* Frees a loaded page's buffers (as fstx_colsrc_drop); thread-safe for distinct pages of one source. */
void fstx_colsrc_drop_page(fstx_colsrc* s, uint32_t page);
/* A pool of string page buffers: a source given one (before any page is loaded) takes its pages' buffers from it
 * and gives them back when a page is dropped or the source freed. Thread-safe. The pool must outlive the source. */
typedef struct fstx_bufpool fstx_bufpool;
fstx_bufpool* fstx_bufpool_new(void); /* NULL when out of memory */
void fstx_bufpool_free(fstx_bufpool* p);
void fstx_colsrc_set_bufpool(fstx_colsrc* s, fstx_bufpool* p);
/* read != 0: the source copies its pages out of the file with positional reads (no page faults on the mapping,
 * which the OS serialises per process) instead of decoding them where they are mapped. Before any page loads. */
void fstx_colsrc_set_read(fstx_colsrc* s, int read);

#ifdef __cplusplus
}
#endif

#endif
