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
} fstx_arrow_args;

/* Opens path and fills *out (which must be released, i.e. out->release == NULL). The stream owns the open file
 * until its release callback runs. Its callbacks never call R and may run on any thread (one at a time per
 * stream). Returns FSTX_OK or an error code with err filled in; *out is untouched on error. */
int fstx_arrow_stream_open(const char* path_utf8, const fstx_arrow_args* args, struct ArrowArrayStream* out,
                           fstx_error* err);

#ifdef __cplusplus
}
#endif

#endif
