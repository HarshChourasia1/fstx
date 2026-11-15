# fstx format 1.0 (file format version 3)

Draft, written in phase 1 (WS1, 15 Nov 2026). WS8 finishes it for the 1.0 release. The words "must" and
"must not" are rules for every writer and reader; anything else is description.

fstx 1.0 writes format 3 and reads formats 2 and 3. Format 2 (fstx 0.2 to 0.7) is format 3 without the
header feature bits, the tail version check and the three footer feature fields. Format 1 (fstx 0.1) is
refused with a message that says so.

## 1. File layout

```
offset 0      header, 32 bytes
offset 32     zero padding to 64
offset 64     pages, each starting on a 64-byte boundary, zero padding between them
...           footer (FlatBuffers), starting on a 64-byte boundary
end - 32      tail, 32 bytes
```

All integers are little-endian. A file is read from the tail: the tail gives the footer, the footer gives
every page. Bytes outside the header, pages, footer and tail are padding and must be zero when written;
readers do not check them.

## 2. Header (32 bytes)

| bytes | type | meaning |
|---|---|---|
| 0-3 | char[4] | `FSTX` |
| 4-5 | u16 | format version (3; 2 for files kept in format 2, rule 6 in section 9) |
| 6-7 | u16 | required feature bits 0-15 (section 5); must be 0 in format 2 |
| 8-11 | u32 | endian mark `0x01020304` |
| 12-15 | u32 | 0; readers ignore it |
| 16-23 | u64 | committed end during an append, else 0 (section 9.3) |
| 24-31 | u64 | XXH3-64 (seed 0) of bytes 0-23 |

A reader checks magic, header checksum and endian mark, then the format version (below 2: refused as too
old; above 3: refused with "Update fstx"), then the header feature bits, before it reads the tail.

## 3. Tail (32 bytes, at end of file)

| bytes | type | meaning |
|---|---|---|
| 0-7 | u64 | footer offset (64-byte aligned) |
| 8-15 | u64 | footer length |
| 16-23 | u64 | XXH3-64 (seed 0) of the footer bytes |
| 24-27 | u32 | format version; in format 3 it must equal header bytes 4-5 |
| 28-31 | char[4] | `FSTX` |

## 4. Footer

A FlatBuffers buffer with file identifier `FSTX`, root table `Footer`. The schema is
`tools/fstx_footer.fbs` in the package sources (compiled with flatc 25.12.19). Readers must run the
FlatBuffers verifier on it before use.

### 4.1 Footer

| field | type | meaning |
|---|---|---|
| format_version | u16 | same as the header |
| row_count | u64 | rows in the file |
| row_group_size | u64 | most rows a row group may hold; 1 to 2^31 |
| columns | [Column] | schema, in column order |
| row_groups | [RowGroup] | in row order; their row counts add up to row_count |
| table_attrs | [u8] | data-frame attributes (class and others), R `serialize()` output; may be empty |
| created_by | string | `fstx <version>` of the writer |
| profile | enum u8 | 0 fast, 1 balanced (default), 2 small; informational; above 2 is corrupt |
| required_features | u64 | format 3: bits a reader must know (section 5) |
| optional_features | u64 | format 3: bits a reader may ignore |
| feature_names | [string] | format 3: one name per set bit, required bits first, then optional, each in ascending bit order |

### 4.2 Column

| field | type | meaning |
|---|---|---|
| name | string | UTF-8, no NUL byte |
| phys | enum u8 | physical type (4.6) |
| logical | enum u8 | 0 none, 1 factor, 2 Date, 3 POSIXct; 4-255 reserved |
| ordered | bool | ordered factor |
| levels | [string] | factor levels, UTF-8 |
| tzone | string | POSIXct time zone, UTF-8, no NUL; absent when the column has none |
| attrs | [u8] | other R attributes, R `serialize()` output; optional for readers that are not R |

A factor is `phys = 0` with 1-based codes into `levels` (NA = missing). Date and POSIXct keep R's storage:
days or seconds since 1970-01-01, as int32 or float64.

### 4.3 RowGroup

| field | type | meaning |
|---|---|---|
| first_row | u64 | first row, counted from 0 at the start of the file |
| row_count | u64 | 1 to row_group_size |
| columns | [ColumnChunk] | one per column, in column order |

### 4.4 ColumnChunk

| field | type | meaning |
|---|---|---|
| pages | [Page] | in row order; their row counts add up to the row group's |
| dictionary | Page | string columns only: the chunk's dictionary page (kind 6); its pages are then kind 5 |

### 4.5 Page (fixed struct, 64 bytes)

| field | type | meaning |
|---|---|---|
| offset | u64 | file offset of the stored bytes; at least 64; 64-byte aligned |
| first_row | u64 | first row, counted from the start of the file (dictionary page: the chunk's first row) |
| hash | u64 | XXH3-64 (seed 0) of the stored bytes |
| min, max | f64 | statistics over non-missing values, numeric columns only |
| comp_len | u32 | stored length; at most 2^31-1; equals raw_len when codec = 0 |
| raw_len | u32 | page body length after the codec is removed; multiple of 8, at least 8, at most 2^31-1 |
| row_count | u32 | rows (dictionary page: entries, at most 2^25); at least 1 |
| null_count | u32 | missing values; at most row_count; 0 for a dictionary page |
| chain | u32 | byte 0 = page kind (bits 0-3) and missing-value mode (bits 4-7); byte 1 = top encoding tag; bytes 2-3 = top tags of the first and second child blocks (0 when none) |
| codec | enum u8 | section 4.7 |
| flags | u8 | bit 0: min/max are valid; other bits 0 |

The page body repeats the kind, missing-value mode and top tag; a reader checks that both agree.

### 4.6 Numbers and their reserved ranges

| what | read and written | reserved | a reader that meets a reserved or unknown number |
|---|---|---|---|
| physical type | 0 int32, 1 float64, 2 logical (int32 0/1/NA), 3 string | 4 = 64-bit integer, 5 = binary, 6-255 | refuses the file: "uses ..., Update fstx" |
| logical type | 0-3 | 4-255 | refuses, "Update fstx" |
| codec | 0 none, 1 LZ4 block, 2 Zstandard frame | 3 = zstd with a dictionary, 4 = Pcodec, 5-127 later codecs, 128-255 experiments never written by a release | refuses, naming the codec when it has a name |
| page kind | 1 int, 2 double, 3 logical, 4 string, 5 dictionary codes, 6 dictionary | 0 corrupt; 7-15 reserved | refuses, "Update fstx" |
| missing-value mode | 0 none, 1 bitmap, 2 runs | 3-15 | refuses, "Update fstx" |
| encoding tag (chain bytes 1-3) | 1-10 (section 6) | 11-255 | refuses, "Update fstx" |

Refusals of reserved numbers happen when the file is opened, before any data is read. Tags deeper inside a
page body than the chain shows are checked while decoding, and an unknown one there is "corrupt". So a
writer that uses a new encoding tag anywhere in a page must also set a required feature bit.

### 4.7 Codecs

The codec applies to the whole page body. LZ4: one raw LZ4 block (no frame), decoded with a safe decoder to
exactly raw_len bytes. Zstandard: one frame, decoded to exactly raw_len bytes. The writer keeps a codec only
when it saves at least 10% (profile balanced or small).

## 5. Feature bits (format 3)

Bits 0-15 are for features that change how the header, tail or footer are read; they are also stored in
header bytes 6-7 and checked before the footer is read, so a reader cannot name them. Bits 16-63 are for
everything else. Named and reserved bits:

| bit | name | meaning |
|---|---|---|
| 16 | int64 | 64-bit integer columns (physical type 4) |
| 17 | binary | binary columns (physical type 5) |
| 18 | zstd_dict | pages with codec 3 |
| 19 | pcodec | pages with codec 4 |

Rules:

- A reader must refuse a file with a required bit it does not know, naming the feature: from
  feature_names when the list has one entry per set bit, else from the table above, else by bit number.
- A reader must ignore optional bits it does not know.
- The low 16 bits of required_features must equal header bytes 6-7.
- In format 2, a header with feature bits, or a footer with any of the three feature fields, is corrupt.
- fstx 1.0 knows no feature bits: every file it writes has both fields 0 and no names.

## 6. Page bodies

A page body (after the codec is removed) is:

```
8 bytes   page header: byte 0 kind, byte 1 missing-value mode, bytes 2-3 zero, bytes 4-7 u32 row count
          missing-value block, only when the mode is not 0
          the top value block
```

### 6.1 Blocks

Every block is an 8-byte header followed by its payload, padded with zeros to a multiple of 8:

```
byte 0     tag
bytes 1-3  a, b, c: tag-specific small parameters (0 when unused)
bytes 4-7  u32 payload length (without the padding)
```

Inside a payload, "pad" means zero bytes up to the next multiple of 8 counted from the payload start. A
"u32 + 4" field is a u32 followed by 4 zero bytes. Child blocks follow one another inside their parent's
payload. Blocks nest at most 3 deep (RLE and DICT only at depth 0 and 1).

### 6.2 Missing values

The writer stores missing values out of line and fills their slots with the previous value (the first
non-missing value at the start of the page), so the value block sees no NA.

- Tag 0x81, bitmap: one bit per row, 1 = missing, bit i in u64 word i/64 at bit i%64; payload
  8 x ceil(n/64) bytes.
- Tag 0x82, runs: u32 run count + 4, then u32 run lengths alternating not-missing / missing, starting with
  a not-missing run (which may be 0). Lengths add up to n.

int and codes pages use NA_integer_ (int32 min); code pages use -1; double pages use R's NA_real_ bit
pattern `0x7FF00000000007A2` (other NaNs are values); string pages use a length of -1.

### 6.3 Encoding tags

Values are int32 (int, logical, codes pages; lengths and codes inside other encodings) or 64-bit (double
pages, worked on as their bit patterns, and ALP integers). n is the value count of the block.

| tag | name | a, b | payload |
|---|---|---|---|
| 1 | PLAIN | | n values |
| 2 | CONST | | one value, repeated n times |
| 3 | FOR | | per 1,024-value vector: u8 bit width w (pad); base per vector (pad); then for each vector 128 x w bytes of packed (value - base) |
| 4 | DELTA | | widths (pad); first value per vector (pad); FOR base per vector (pad); packed deltas; value i = value i-1 + delta i, wrapping |
| 5 | RLE | | u32 run count + 4; child block of run values; child int32 block of run lengths (each at least 1) |
| 6 | DICT | | u32 dictionary size + 4; child block of distinct values (sorted); child int32 block of 0-based codes |
| 7 | SPLIT | a = 4 or 8 | byte-split: byte 0 of every value, then byte 1, ... (n x a bytes) |
| 8 | ALP | a = e, b = f | u32 exception count + 4; u32 exception rows (pad); u64 exception bit patterns; child int64 block q; value = (double)q x 10^f / 10^e unless an exception |
| 9 | ALPRD | a = left bits, b = dictionary size (at most 8) | u16[8] left-part dictionary; u32 exception count + 4; u32 exception rows (pad); u16 exception left parts (pad); child int32 block of dictionary codes; child int64 block of right parts; value bits = left << (64 - left bits) or right |
| 10 | FSST | | string pages only (6.4) |

Bit packing (FOR, DELTA): a vector holds 1,024 values; the last vector of a block is padded with zeros to
1,024. With B = value bits (32 or 64) and L = 1024 / B lanes, value i sits in lane i % L, row i / L. Row r of
every lane is written at bit r x w of that lane's bit stream, and word k of lane l is stored at
word k x L + l. Width 0 stores nothing (every value equals base).

### 6.4 String pages

Kind 4 (string) and kind 6 (dictionary) pages hold strings; kind 6 has no missing values.

- PLAIN (tag 1): a PLAIN block whose payload is a child int32 block of byte lengths (missing = 0) followed
  by a PLAIN block of all string bytes, one after another. chain bytes 2-3 are the lengths' tag and 1.
- FSST (tag 10): u64 total decoded bytes; a PLAIN block holding the FSST symbol table (fsst_export format,
  never zero-terminated); a PLAIN block of the compressed bytes, one string after another; a child int32
  block of compressed lengths.

Strings are UTF-8 and must not contain NUL. A string is at most 2^31-1 bytes; the writer refuses strings
over 1 GB.

Kind 5 (codes) pages are int32 pages of 0-based positions into the chunk's dictionary page, -1 = missing.

## 7. Legal combinations

| page kind | encodings (top tag) | missing-value modes |
|---|---|---|
| 1 int, 3 logical, 5 codes | 1-7 | 0, 1, 2 |
| 2 double | 1-9 | 0, 1, 2 |
| 4 string | 1, 10 | 0, 1, 2 |
| 6 dictionary | 1, 10 | 0 |

Any of these with codec 0, 1 or 2. The package test `test-compat-format.R` writes and reads every one.

## 8. Limits (checked on read)

| what | limit |
|---|---|
| columns | 2^24 |
| row_group_size | 1 to 2^31; every row group's row_count at most row_group_size |
| page raw_len and comp_len | 2^31-1 |
| dictionary entries per page | 2^25 |
| string length | 2^31-1 bytes (writer: 1 GB) |
| names, time zones | valid UTF-8 (no overlongs, surrogates or values past U+10FFFF), no NUL |

Factor levels are not checked when a file is opened, to keep opening fast; they are checked when used.

## 9. Rules for writers and later versions

1. The Page struct never changes. New per-page facts go into new lists in ColumnChunk, one entry per page.
2. FlatBuffers fields are only ever added at the end of a table.
3. Page min/max for 64-bit integer columns (when they come) are rounded outward to the nearest doubles, so
   page skipping stays correct.
4. Attribute blobs (Column.attrs, Footer.table_attrs) are R `serialize()` output. Readers that are not R
   may ignore them.
5. A new encoding tag, codec, physical type or page kind needs a required feature bit, so an older reader
   names it instead of calling the file corrupt.
6. Appends and compaction keep the file's own format version and feature fields: a format 2 file stays
   format 2, so the fstx that wrote it can still read it.

### 9.1 Same bytes

The same data, settings and fstx version give the same file bytes on every thread count and every SIMD
setting (no fused multiply-add anywhere).

### 9.2 Checksums

Header, footer and every page carry XXH3-64 checksums (seed 0). Pages are checked when read unless the
reader turns checking off.

### 9.3 Appends

During an append the header's bytes 16-23 hold the old committed end, flushed to disk before any new byte
is written; new pages and a new footer and tail go after it, and only then is the header rewritten with 0
there. A reader that finds a committed end treats the file as ending there (its tail is the 32 bytes before
it) and ignores anything after it.
