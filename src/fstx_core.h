/* FST-X core engine: C interface.
 * No R headers here or anywhere in src/core. Hosts (R today, Python later) talk to the engine
 * only through these functions and callbacks. Errors come back as a code plus a message.
 * MIT licence. */
#ifndef FSTX_CORE_H
#define FSTX_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- errors ---- */
enum {
  FSTX_OK = 0,
  FSTX_ERR_IO = 1,        /* cannot open / read / write / map */
  FSTX_ERR_FORMAT = 2,    /* not an FSTX file, unsupported version, wrong endianness */
  FSTX_ERR_CORRUPT = 3,   /* structure inconsistent, truncated, decoder failure */
  FSTX_ERR_CHECKSUM = 4,  /* xxh3 mismatch */
  FSTX_ERR_ARG = 5,       /* bad argument from the host */
  FSTX_ERR_MEMORY = 6,
  FSTX_ERR_HOST = 7,      /* a host callback reported failure */
  FSTX_ERR_TRANSLATE = 8, /* writer: a string column needs conversion to UTF-8 (err->column says which) */
  FSTX_ERR_INTERNAL = 9
};

typedef struct fstx_error {
  int code;
  int64_t column; /* column involved, or -1 */
  char message[1024];
} fstx_error;

/* ---- types ---- */
enum { FSTX_INT32 = 0, FSTX_FLOAT64 = 1, FSTX_LOGICAL = 2, FSTX_STRING = 3 };
enum { FSTX_LT_NONE = 0, FSTX_LT_FACTOR = 1, FSTX_LT_DATE = 2, FSTX_LT_POSIXCT = 3 };
enum { FSTX_CODEC_NONE = 0, FSTX_CODEC_LZ4 = 1, FSTX_CODEC_ZSTD = 2 };
enum { FSTX_PROFILE_FAST = 0, FSTX_PROFILE_BALANCED = 1, FSTX_PROFILE_SMALL = 2 };

/* ---- reading ---- */
typedef struct fstx_file fstx_file;

typedef struct fstx_file_info {
  uint64_t n_rows;
  uint32_t n_cols;
  uint64_t n_row_groups;
  uint64_t n_pages;
  uint64_t file_size;
  uint64_t row_group_size;
  uint32_t format_version;
  int profile;                /* FSTX_PROFILE_* the file was written with */
  const uint8_t* table_attrs; /* R-serialized blob, may be NULL */
  size_t table_attrs_len;
} fstx_file_info;

typedef struct fstx_column_info {
  const char* name;
  uint32_t name_len;
  int phys;
  int logical;
  int ordered;
  uint32_t n_levels;
  int has_tzone;
  const char* tzone;
  uint32_t tzone_len;
  const uint8_t* attrs; /* may be NULL */
  size_t attrs_len;
  uint64_t compressed_bytes; /* stored bytes of all its pages */
  uint64_t raw_bytes;        /* bytes after the codec is removed */
  uint64_t n_pages;
} fstx_column_info;

/* path is UTF-8. On Windows it is opened with CreateFileW. */
int fstx_open(const char* path_utf8, fstx_file** out, fstx_error* err);
void fstx_close(fstx_file* f);
void fstx_file_info_get(const fstx_file* f, fstx_file_info* out);
int fstx_column_info_get(const fstx_file* f, uint32_t col, fstx_column_info* out);
int fstx_level_get(const fstx_file* f, uint32_t col, uint32_t i, const char** s, uint32_t* len);
/* index of the column with this UTF-8 name, or -1 */
int64_t fstx_find_column(const fstx_file* f, const char* name, size_t len);

/* One stored page (data page or string dictionary page), for inspection. */
typedef struct fstx_page_info {
  uint32_t row_group;
  uint32_t column;
  int is_dictionary;
  uint64_t first_row;
  uint32_t row_count;
  uint32_t null_count;
  uint64_t offset;
  uint32_t comp_len;
  uint32_t raw_len;
  int codec;
  int kind;    /* page kind (see fstx_kind_name) */
  int na_mode; /* 0 none, 1 bitmap, 2 runs */
  int enc[3];  /* top encoding, then the top encodings of its first two child blocks (0 = none) */
} fstx_page_info;
/* Fills up to cap entries (out may be NULL); returns the number of pages in the file. */
int64_t fstx_pages_get(const fstx_file* f, fstx_page_info* out, uint64_t cap);
const char* fstx_encoding_name(int tag); /* "" for unknown tags */
const char* fstx_kind_name(int kind);

typedef struct fstx_plan fstx_plan;
/* rows [from, to) counted from 0; cols are column indexes (any order, repeats allowed) */
int fstx_plan_create(const fstx_file* f, const uint32_t* cols, uint32_t n_cols, uint64_t from, uint64_t to,
                     fstx_plan** out, fstx_error* err);
/* rows are 0-based, in any order, repeats allowed; the read delivers them in the order given. Only the pages
 * that hold requested rows are read; pages without a codec decode only the 1,024-row vectors needed. */
int fstx_plan_create_rows(const fstx_file* f, const uint32_t* cols, uint32_t n_cols, const uint64_t* rows,
                          uint64_t n_rows, fstx_plan** out, fstx_error* err);
/* Rows plans with rows in ascending order (repeats allowed): output column out_index, a string column, delivers
 * its dictionary-coded pages as codes instead of strings: on_dict (the page's dictionary, when it differs from the
 * one sent before for this column), then on_codes for its rows. Pages without a dictionary still come as strings. */
int fstx_plan_rows_codes(fstx_plan* p, uint32_t out_index, fstx_error* err);
void fstx_plan_free(fstx_plan* p);
uint64_t fstx_plan_rows(const fstx_plan* p);

typedef struct fstx_read_host {
  void* ctx;
  /* Main thread, before any decoding starts. Return a pointer to n_rows values of the physical type
   * (int32 for INT32 and LOGICAL, double for FLOAT64). For STRING columns the return value is not used
   * (return any non-NULL value); the strings arrive through on_strings. NULL means failure. */
  void* (*alloc_column)(void* ctx, uint32_t out_index, uint32_t col, int phys, uint64_t n_rows);
  /* Main thread, called in row order for each string column: n strings for output rows
   * [dest_row, dest_row + n). lens[i] == -1 marks NA; other strings follow each other in bytes.
   * Return 0 on success. */
  int (*on_strings)(void* ctx, uint32_t out_index, uint64_t dest_row, uint32_t n, const int32_t* lens,
                    const char* bytes);
  /* Main thread. A string dictionary (n entries, never NA, laid out like on_strings) for the following
   * on_codes calls of this output column; it replaces the previous one. */
  int (*on_dict)(void* ctx, uint32_t out_index, uint32_t n, const int32_t* lens, const char* bytes);
  /* Main thread, in row order: output rows [dest_row, dest_row + n) are dictionary entries codes[i]
   * (0-based; -1 = NA). Codes are checked against the dictionary size before this call. */
  int (*on_codes)(void* ctx, uint32_t out_index, uint64_t dest_row, uint32_t n, const int32_t* codes);
  /* Main thread, rows plans whose rows are not ascending: string i goes to output row dest[i] (laid out like
   * on_strings). Rows plans call on_dict / on_codes only for columns set with fstx_plan_rows_codes. */
  int (*on_strings_at)(void* ctx, uint32_t out_index, uint32_t n, const uint64_t* dest, const int32_t* lens,
                       const char* bytes);
} fstx_read_host;

/* verify (fstx_read, fstx_colsrc_open, fstx_arrow_args): bit flags. FSTX_VERIFY_HASH checks every page's xxh3
 * (header and footer are always checked); FSTX_VERIFY_NULLS counts the missing values of every page decoded in
 * full and refuses the file as corrupt when the count differs from the page's null_count (rows plans then decode
 * whole pages; fstx_filter then decides no page from its null_count alone). */
enum { FSTX_VERIFY_HASH = 1, FSTX_VERIFY_NULLS = 2 };

/* threads <= 0 means all cores. */
int fstx_read(const fstx_plan* p, const fstx_read_host* host, int threads, int verify, fstx_error* err);

/* Plain (non-dictionary) string rows a plan delivers, and the raw bytes of their pages (heap pre-sizing). */
void fstx_plan_string_stats(const fstx_plan* p, uint64_t* plain_rows, uint64_t* plain_raw_bytes);

/* ---- column sources: one column over a row range, decoded page by page on demand (lazy columns) or all
 * pages at once in parallel (deferred strings, factor codes). The file must stay open while a source that
 * is not detached uses it. Not thread-safe: one caller at a time per source. */
typedef struct fstx_colsrc fstx_colsrc;
enum { FSTX_SRC_FIXED = 0, FSTX_SRC_STRINGS = 1, FSTX_SRC_CODES = 2 };
typedef struct fstx_colsrc_info {
  uint64_t rows;
  int phys, logical, mode;
  uint32_t n_pages, n_dicts;
  int all_dict;        /* every page is dictionary-coded (string columns) */
  uint64_t null_count; /* 0 means no NA for sure (never 0 under FSTX_VERIFY_NULLS: pages are checked on decode) */
  uint32_t pages_ready;
  uint64_t cached_bytes;
  int detached;
} fstx_colsrc_info;
/* own_bytes = 0 lets plain uncompressed string pages point into the mapped file. */
int fstx_colsrc_open(const fstx_file* f, uint32_t col, uint64_t from, uint64_t to, int verify, int own_bytes,
                     fstx_colsrc** out, fstx_error* err);
/* A detached string source over n strings laid out like on_strings (for unserialising). */
int fstx_colsrc_from_strings(uint64_t n, const int32_t* lens, const char* bytes, fstx_colsrc** out, fstx_error* err);
void fstx_colsrc_free(fstx_colsrc* s);
void fstx_colsrc_info_get(const fstx_colsrc* s, fstx_colsrc_info* out);
uint32_t fstx_colsrc_page_of(const fstx_colsrc* s, uint64_t row);
void fstx_colsrc_page_span(const fstx_colsrc* s, uint32_t page, uint64_t* first, uint32_t* n);
/* Dictionary d's n entries (decoded on first use). */
int fstx_colsrc_dict(fstx_colsrc* s, uint32_t d, uint32_t* n, const int32_t** lens, const char** bytes, fstx_error* err);
/* Turns an all-dictionary string source into factor codes: remap[d][k] is the 1-based level of entry k. */
int fstx_colsrc_set_codes(fstx_colsrc* s, const int32_t* const* remap, fstx_error* err);
/* Decoded values of a fixed / codes page (cached; the oldest pages are dropped past 64 MB per source). */
int fstx_colsrc_fixed_page(fstx_colsrc* s, uint32_t page, const void** vals, fstx_error* err);
/* A string page: lens (-1 = NA) with bytes back to back, or, when *dict >= 0, codes into dictionary *dict. */
int fstx_colsrc_str_page(fstx_colsrc* s, uint32_t page, const int32_t** lens, const char** bytes, int32_t* dict,
                         fstx_error* err);
void fstx_colsrc_drop(fstx_colsrc* s, uint32_t page); /* UINT32_MAX drops every page */
/* Workers for read-ahead when a lazy fixed column is scanned page after page (1 = none; default 1). */
void fstx_colsrc_set_threads(fstx_colsrc* s, int threads);
/* Decodes every page on up to `threads` threads: into dst (rows x width) for fixed / codes sources, or
 * into the source's own buffers for string sources (dst = NULL). */
int fstx_colsrc_load(fstx_colsrc* s, void* dst, int threads, fstx_error* err);
/* A fully loaded string source that owns its bytes stops using the file. */
int fstx_colsrc_detach(fstx_colsrc* s, fstx_error* err);

/* ---- filters: skip pages by their min / max / NA count, then test the remaining rows exactly ---- */
enum {
  FSTX_OP_NONE = 0, /* matches no row (a comparison with NA) */
  FSTX_OP_EQ, FSTX_OP_NE, FSTX_OP_LT, FSTX_OP_LE, FSTX_OP_GT, FSTX_OP_GE,
  FSTX_OP_IN, FSTX_OP_ISNA, FSTX_OP_NOTNA
};
typedef struct fstx_pred {
  uint32_t col;
  int op;
  /* numeric, logical, date and factor-code columns: EQ..GE compare with value (R semantics: NA and NaN rows
   * never match); IN tests set[0..n_set) plus NA rows when set_na, NaN rows (doubles) when set_nan */
  double value;
  const double* set;
  uint64_t n_set;
  int set_na, set_nan;
  /* string columns: EQ / NE use strs[0], IN all n_strs (exact byte compare, UTF-8) */
  const char* const* strs;
  const int32_t* str_lens;
  uint64_t n_strs;
} fstx_pred;
typedef struct fstx_filter_stats {
  uint64_t pages_total;     /* data pages of the filter columns that overlap the row window */
  uint64_t pages_skipped;   /* not read: their min / max / NA count (or dictionary) rule them out */
  uint64_t pages_pruned;    /* not read: other conditions already ruled out all their rows */
  uint64_t pages_all;       /* not read: their statistics prove every row matches */
  uint64_t pages_read;
  uint64_t rows_window;     /* rows in [from, to) */
  uint64_t rows_candidate;  /* rows left after page skipping */
  uint64_t rows_matched;
} fstx_filter_stats;
typedef struct fstx_selection fstx_selection;
/* Rows of [from, to) that match every predicate (AND), ascending and 0-based. plan_only != 0 only skips
 * pages (no decoding): the selection is then empty and rows_matched is 0. */
int fstx_filter(const fstx_file* f, const fstx_pred* preds, uint32_t n_preds, uint64_t from, uint64_t to,
                int threads, int verify, int plan_only, fstx_selection** out, fstx_filter_stats* stats,
                fstx_error* err);
const uint64_t* fstx_selection_rows(const fstx_selection* s, uint64_t* n);
void fstx_selection_free(fstx_selection* s);

/* ---- writing ---- */
typedef struct fstx_writer fstx_writer;

typedef struct fstx_write_column {
  const char* name;
  uint32_t name_len;
  int phys;
  int logical;
  int ordered;
  uint32_t n_levels;
  const char* const* levels; /* UTF-8 */
  const uint32_t* level_lens;
  int has_tzone;
  const char* tzone;
  uint32_t tzone_len;
  const uint8_t* attrs; /* R-serialized blob or NULL */
  size_t attrs_len;
} fstx_write_column;

typedef struct fstx_write_options {
  int codec;               /* FSTX_CODEC_* */
  int level;               /* ZSTD level; ignored for LZ4 / NONE */
  int threads;             /* <= 0: all cores */
  uint64_t row_group_size; /* 0: default 1,048,576; must be a multiple of 1024 */
  uint32_t page_bytes;     /* 0: default 262,144 */
  int profile;             /* FSTX_PROFILE_*: recorded in the footer (the codec and level come from the host) */
  int keep_pct;            /* keep the codec on a page only if it saves at least this percent (0..90) */
  int force_encoding;      /* 0 = the chooser decides; else an encoding tag (tests only) */
  int test_crash;          /* tests only: 1 = an append stops after its tail as if the process died */
} fstx_write_options;

typedef struct fstx_column_data {
  /* INT32 / LOGICAL: const int32_t*; FLOAT64: const double*; NULL for STRING. Read in place, never copied. */
  const void* data;
  /* STRING only. Called from worker threads: must only read memory, never allocate or call into the host
   * runtime. Fill ptrs/lens for rows [row, row + n); lens[i] = -1 marks NA.
   * Return 0, or 1 if a string is not UTF-8 / ASCII (the writer then fails with FSTX_ERR_TRANSLATE before
   * writing any page), or 2 for strings that can never be converted (e.g. R "bytes" encoding).
   * The pointers must stay valid from one get_strings call to the next within one fstx_writer_append call (the
   * writer keeps them); it never uses them after fstx_writer_append returns. */
  int (*get_strings)(void* ctx, uint64_t row, uint32_t n, const char** ptrs, int32_t* lens);
  void* ctx;
} fstx_column_data;

int fstx_writer_open(const char* path_utf8, const fstx_write_column* cols, uint32_t n_cols,
                     const uint8_t* table_attrs, size_t table_attrs_len, const fstx_write_options* opt,
                     fstx_writer** out, fstx_error* err);
/* Appends n_rows rows as one or more row groups. */
int fstx_writer_append(fstx_writer* w, const fstx_column_data* cols, uint64_t n_rows, fstx_error* err);
/* Writes footer and tail, closes and frees the writer (also on failure). */
int fstx_writer_close(fstx_writer* w, fstx_error* err);
/* Closes, deletes the partial file and frees the writer. */
void fstx_writer_abort(fstx_writer* w);

/* Appending to an existing file. cols must match the file's columns in number, names and types; factor
 * levels may only grow (the file's levels first, in order); the file's own column definitions are replaced
 * by cols. opt->row_group_size and opt->profile are ignored (the file's are kept). Crash safe: until close
 * succeeds, header bytes 16..23 hold the file's committed end, so readers see the old table. Abort (or a
 * failed close) truncates the file back to it. */
int fstx_writer_open_append(const char* path_utf8, const fstx_write_column* cols, uint32_t n_cols,
                            const uint8_t* table_attrs, size_t table_attrs_len, const fstx_write_options* opt,
                            fstx_writer** out, fstx_error* err);
/* Rewrites the file's pages, in place order, into tmp_path (no re-encoding) with a fresh footer, so space
 * left behind by appends (old footers, an interrupted append) is dropped. The caller renames tmp_path over
 * the file. */
int fstx_compact_copy(const char* path_utf8, const char* tmp_path_utf8, fstx_error* err);
/* Committed size, physical size and header committed-end field (0 = no append in progress). */
int fstx_file_sizes(const char* path_utf8, uint64_t* committed, uint64_t* physical, uint64_t* valid_end,
                    fstx_error* err);

unsigned fstx_hardware_threads(void);
/* Stops the engine's worker pool (call before unloading the library). */
void fstx_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
