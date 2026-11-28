# fstx format 1.2 (file format version 3)

Written in phase 1 (WS1) and finished by WS8 for the 1.0 release (15 Nov 2026); extended for fstx 1.2 (27 Nov
2026) with 64-bit integer and binary columns. Every rule here was checked against the code of fstx 1.2
(`src/core_reader.cpp`, `src/core_writer.cpp`, `src/enc_num.cpp`, `src/enc_str.cpp`, `src/kern_scalar.h`,
`src/codec_ext.cpp`, `src/core_filter.cpp`). `fstx-format-1.0.md` stays in the package unchanged: it is what
fstx 1.0 readers implement. The words "must" and "must not" are rules for every writer and reader; "fstx
checks" says what the fstx 1.2 reader does; anything else is description.
Someone who has never seen the code should be able to write a reader from this document alone.

fstx 1.0 reads and writes format 3 only. Files with format versions 1 and 2 (fstx 0.x development versions)
are refused with a message that says so.

fstx 1.2 also reads and writes format 3 and adds two column types: 64-bit integers (physical type 4) and binary
(physical type 5). A file that has a column of either type sets a required feature bit (section 5), so a reader
that does not know the type refuses the file by name ("Update fstx").

A reader must know all ten encoding tags to read ordinary files. The fstx writer often picks ALP (tag 8) or
ALPRD (tag 9) for double columns (decimal numbers, and dates and times stored as doubles) and FSST (tag 10)
for text, also for the dictionary page of dictionary-coded text. A reader without them can open every file
and read its integer, logical and factor columns, but not many of its double and text columns. Which tags a
file uses is shown only by the per-page chain (4.5).

## 1. File layout

```
offset 0      header, 32 bytes
offset 32     zero padding to 64
offset 64     pages, each starting on a 64-byte boundary, zero padding between them
...           footer (FlatBuffers), starting on a 64-byte boundary
end - 32      tail, 32 bytes
```

All integers are little-endian; floats are IEEE 754 binary64, little-endian. A file is read from the tail:
the tail gives the footer, the footer gives every page.

A freshly written or compacted file holds nothing but the parts above: padding is zero bytes. A file that
was appended to (section 9.3) also holds the footers and tails of its earlier versions, between the pages
of one append and the next. Readers never look at bytes that no page, footer or tail points to, and must
not depend on them; compaction drops them.

## 2. Header (32 bytes)

| bytes | type | meaning |
|---|---|---|
| 0-3 | char[4] | `FSTX` |
| 4-5 | u16 | format version: 3 |
| 6-7 | u16 | required feature bits 0-15 (section 5) |
| 8-11 | u32 | endian mark `0x01020304` (bytes `04 03 02 01`) |
| 12-15 | u32 | 0; readers ignore it |
| 16-23 | u64 | committed end while an append is running or was cut off, else 0 (section 9.3) |
| 24-31 | u64 | XXH3-64 (seed 0) of bytes 0-23 |

### 2.1 Opening a file

fstx checks, in this order (the first failure decides the message):

1. The file is at least 96 bytes (64 for header and padding, 32 for the tail); else "not an FSTX file".
2. Bytes 0-3 are `FSTX`; else "not an FSTX file".
3. The endian mark is `0x01020304`. `0x04030201` gives "written with the other byte order"; anything else is
   corrupt.
4. The header checksum matches; else corrupt.
5. Committed end (bytes 16-23): when not 0 it must be at least 96 and at most the file size (else corrupt),
   and the file is then treated as ending there.
6. Format version: 0 is corrupt; 1 and 2 are refused as written by fstx 0.x ("not supported by fstx 1.0 or
   later"); above 3 is refused with "Update fstx".
7. Header feature bits: a bit this reader does not know is refused by number (section 5).
8. Tail (section 3), footer checksum, the FlatBuffers verifier, then the footer checks of section 4.8.

A refusal of something newer says "Update fstx". A file of fstx 0.x says so. A file that is not an fstx file,
or has the other byte order, says so. Every other failure says the file is corrupt.

## 3. Tail (32 bytes, the last 32 bytes before the committed end)

| bytes | type | meaning |
|---|---|---|
| 0-7 | u64 | footer offset |
| 8-15 | u64 | footer length |
| 16-23 | u64 | XXH3-64 (seed 0) of the footer bytes |
| 24-27 | u32 | format version; must equal header bytes 4-5 |
| 28-31 | char[4] | `FSTX` |

The writer puts the footer on a 64-byte boundary, right before the tail. fstx checks: the end marker (else
"truncated or incomplete"); the tail version; footer offset at least 64 and a multiple of 8;
the footer ends exactly where the tail starts; footer length at least 8 and below 2^31-1; then the footer
checksum.

## 4. Footer

A FlatBuffers buffer (standard binary layout, little-endian) with file identifier `FSTX` at footer bytes
4-7 and root table `Footer`. The schema is appendix A; the same text is `tools/fstx_footer.fbs` in the
package sources (compiled with flatc 25.12.19). Readers must run a FlatBuffers verifier on the footer before
they use it. It must check, for the root table and everything reachable from it: the file identifier; that
every offset, vtable, table, string and vector lies inside the footer; that strings end in a zero byte; and
that every scalar, table and vector length sits on a multiple of its size counted from the first footer byte
(the footer itself starts on a multiple of 8 in the file, so this is also its alignment in the file). fstx
uses the FlatBuffers 25.12.19 verifier with alignment checks on and a nesting limit of 16 tables (the root
counts as 1; this format nests 3 deep: Footer, RowGroup, ColumnChunk). A file has 1 + columns + row groups x
(1 + columns) tables, which can pass the verifier's default limit of 1,000,000 tables in a legal file, so a
reader must raise that limit (fstx uses 2^31-1).

Field ids are the order of the fields in the schema, from 0. A table field that is absent reads as its
default (0, false, empty, or Balanced for `profile`).

### 4.1 Footer

| id | field | type | meaning |
|---|---|---|---|
| 0 | format_version | u16 | same as the header |
| 1 | row_count | u64 | rows in the file; at most 2^62 |
| 2 | row_group_size | u64 | most rows a row group may hold; 1 to 2^31 (the writer uses a multiple of 1,024; default 1,048,576) |
| 3 | columns | [Column] | schema, in column order; at most 2^24 columns |
| 4 | row_groups | [RowGroup] | in row order; empty for a table with no rows |
| 5 | table_attrs | [u8] | data-frame attributes, an attribute blob (rule 4, section 9; section 9.4); may be absent |
| 6 | created_by | string | `fstx <version>` of the writer (fstx 1.0.0 writes `fstx 1.0.0`, fstx 1.2.0 writes `fstx 1.2.0`). Informational: a reader must not use it to decide how to read the file; a file written by a development build before 1.0.0 may say `fstx 0.99.0` |
| 7 | profile | enum u8 | 0 fast, 1 balanced (default), 2 small; informational. 3-255 are read and reported as "unknown" |
| 8 | required_features | u64 | bits a reader must know (section 5) |
| 9 | optional_features | u64 | bits a reader may ignore |
| 10 | feature_names | [string] | one name per set bit, required bits first, then optional, each in ascending bit order |

### 4.2 Column

| id | field | type | meaning |
|---|---|---|---|
| 0 | name | string | UTF-8, no NUL byte; may repeat (a reader that looks columns up by name takes the first) |
| 1 | phys | enum u8 | physical type (4.6) |
| 2 | logical | enum u8 | 0 none, 1 factor, 2 Date, 3 POSIXct; 4-255 reserved |
| 3 | ordered | bool | ordered factor |
| 4 | levels | [string] | factor levels, UTF-8 |
| 5 | tzone | string | POSIXct time zone, UTF-8, no NUL; absent when the column has none |
| 6 | attrs | [u8] | other R attributes, an attribute blob (section 9.4); may be absent |

Type combinations fstx accepts (anything else is corrupt):

| logical | phys | stored values |
|---|---|---|
| 0 none | any of 0-5 | as the physical type |
| 0 none | 4 int64 | two's-complement 64-bit integers; the missing value is INT64_MIN (`0x8000000000000000`) |
| 0 none | 5 binary | byte strings: any bytes, NUL included, no text encoding |
| 1 factor | 0 int32, with `levels` present | 1-based codes into `levels`; NA = missing. A code outside 1..number of levels is corrupt (checked on the rows read) |
| 2 Date | 0 int32 or 1 float64 | days since 1970-01-01 |
| 3 POSIXct | 1 float64 | seconds since 1970-01-01 00:00 UTC |

Logical types 1-3 on physical type 4 or 5 are corrupt. A column of physical type 4 needs required feature bit 16
and one of type 5 needs bit 17; a column whose bit is not set is corrupt.

`levels` on a column that is not a factor, `ordered` on one that is not a factor and `tzone` on one that is
not POSIXct are ignored when the data is read (fstx still checks the time zone text, and `fstx_meta()` shows
what is stored). A writer must not write them.

### 4.3 RowGroup

| id | field | type | meaning |
|---|---|---|---|
| 0 | first_row | u64 | first row, counted from 0; equals the rows of all earlier row groups |
| 1 | row_count | u64 | 1 to row_group_size |
| 2 | columns | [ColumnChunk] | exactly one per column, in column order |

Row counts of all row groups add up to the footer's row_count.

### 4.4 ColumnChunk

| id | field | type | meaning |
|---|---|---|---|
| 0 | pages | [Page] | at least one; in row order; their row counts add up to the row group's |
| 1 | dictionary | Page (struct) | string and binary columns only: the chunk's dictionary page (kind 6, no missing values); its data pages are then kind 5 (codes). Absent otherwise |

Page i's first_row must equal the row group's first_row plus the rows of pages 0..i-1.

### 4.5 Page (FlatBuffers struct, 64 bytes, 8-byte aligned)

| bytes | field | type | meaning |
|---|---|---|---|
| 0-7 | offset | u64 | file offset of the stored bytes; at least 64; a multiple of 64 |
| 8-15 | first_row | u64 | first row, counted from the start of the file. Dictionary page: the chunk's first row (not checked) |
| 16-23 | hash | u64 | XXH3-64 (seed 0) of the stored bytes |
| 24-31 | min | f64 | smallest non-missing value (see below) |
| 32-39 | max | f64 | largest non-missing value |
| 40-43 | comp_len | u32 | stored length; at most 2^31-1; equals raw_len when codec = 0; offset + comp_len at most the footer offset |
| 44-47 | raw_len | u32 | page body length after the codec is removed; a multiple of 8, at least 8, at most 2^31-1 |
| 48-51 | row_count | u32 | rows (dictionary page: entries, at most 2^25); at least 1 |
| 52-55 | null_count | u32 | missing values; at most row_count; 0 for a dictionary page |
| 56-59 | chain | u32 | byte 56: page kind (bits 0-3) and missing-value mode (bits 4-7); byte 57: top encoding tag; bytes 58-59: top tags of the first and second child blocks (6.5) |
| 60 | codec | enum u8 | section 4.7 |
| 61 | flags | u8 | bit 0: min/max are valid; other bits 0 |
| 62-63 | padding | | zero |

Statistics (min, max, flags bit 0): int and logical pages: over the non-NA values, flag set when there is at
least one. Double pages: over values that are not NaN (NA_real_ and every other NaN are left out), flag set
when there is at least one; null_count counts only R's NA (see below), so other NaNs are values. int64 pages:
over the non-missing values, with min the largest double that is not above the smallest value and max the
smallest double that is not below the largest value (rounded outward: values from -2^53 to 2^53 are exact,
larger ones widen the range by less than one double step); flag set when there is at least one. A reader
compares int64 values with a bound or a filter value exactly, never through a rounded int64. String, binary,
codes and dictionary pages: no statistics (min = max = 0, flag clear). A reader may skip a page whose
min/max cannot match a filter only when the flag is set. fstx does not check min, max or flags against the
data; a writer must not set the flag with a range that leaves out a value.

null_count must equal the number of rows the page's missing-value block marks (0 when the mode is 0), plus, on
a double page, the unmarked values that are a NaN whose low 32 bits are 1954. Those are R's NA_real_ with the
quiet bit set (`0x7FF80000000007A2`, what arithmetic on NA_real_ gives): R reads them as NA, but the writer
marks only the exact pattern `0x7FF00000000007A2` (section 6.2) and stores them as values, so such a page can
have mode 0 and a null_count above 0. A writer must write mode 0 when the block would mark no row; a reader must
still accept mode 1 or 2 with no row marked.

By default fstx does not compare null_count with the body. It trusts it the way it trusts min and max: to skip
pages in filters and to tell R and Arrow that a column has no missing values. A wrong count gives wrong answers
(missing values that are not reported as missing), not a crash. With the R option `fstx.verify_nulls` set to
TRUE, fstx counts the missing values of every page it decodes in full (read_fstx, lazy columns, the Arrow
stream, and the pages a filter tests), refuses the file as corrupt when the count differs from null_count, and
no filter decision rests on null_count alone. It counts what decoding gives: the rows the missing-value block
marks plus any unmarked value that is the missing-value pattern (on double pages every NaN whose low 32 bits
are 1954, as above). Pages a filter decides without decoding (skipped by min / max or by the dictionary, proved
to match in full, or holding no candidate row) are not counted by the filter; a read of their rows decodes and
counts them.

Pages must lie wholly before the footer; fstx does not check that pages do not overlap one another.

fstx also refuses, as corrupt, a page whose raw_len the codec cannot reach: LZ4, raw_len above comp_len x
255 + 64; Zstandard, raw_len above comp_len x 32,768 + 131,072; and, for int, double, logical and codes pages
with any codec (and int64 pages), raw_len above row_count x value size x 64 + 65,536 (value size 8 for double
and int64 pages, else 4).

### 4.6 Numbers and their reserved ranges

| what | read and written | reserved | a reader that meets a reserved or unknown number |
|---|---|---|---|
| physical type | 0 int32, 1 float64, 2 logical (int32 0/1/NA), 3 string, 4 int64, 5 binary | 6-255 | refuses the file: "uses ..., Update fstx" |
| logical type | 0-3 | 4-255 | refuses, "Update fstx" |
| codec | 0 none, 1 LZ4 block, 2 Zstandard | 3 = zstd with a dictionary, 4 = Pcodec, 5-127 later codecs, 128-255 experiments never written by a release | refuses, naming the codec when it has a name |
| page kind | 1 int, 2 double, 3 logical, 4 string, 5 dictionary codes, 6 dictionary, 7 int64 | 0 corrupt; 8-15 reserved | refuses, "Update fstx" |
| missing-value mode | 0 none, 1 bitmap, 2 runs | 3-15 | refuses, "Update fstx" |
| encoding tag (chain bytes 57-59) | 1-10 (section 6) | 11-255 | refuses, "Update fstx" |

Refusals of reserved numbers happen when the file is opened, before any data is read, and before the
corruption checks of the same page entry, so a newer file gets "Update fstx", not "corrupt". Tags deeper
inside a page body than the chain shows are checked while decoding, and an unknown one there is "corrupt".
So a writer that uses a new encoding tag anywhere in a page must also set a required feature bit (rule 5).

The page kind follows from the column: phys 0 gives kind 1, phys 1 kind 2, phys 2 kind 3, phys 3 and phys 5
kind 4 (kind 5 for the data pages of a chunk with a dictionary, kind 6 for the dictionary page), phys 4 kind 7.
Any other kind in a known range is corrupt.

### 4.7 Codecs

The codec applies to the whole page body; the hash is over the stored (compressed) bytes.

- 1, LZ4: one block in the LZ4 block format ("LZ4 Block Format Description", `doc/lz4_Block_format.md` in the
  lz4 sources), with no frame and no size prefix. The writer follows that document's end-of-block rules (the
  last sequence is literals only, the last 5 bytes are literals, the last match starts at least 12 bytes
  before the end). fstx decodes with the safe decoder (`LZ4_decompress_safe`): the block must use exactly
  comp_len input bytes and give exactly raw_len bytes, else the page is corrupt.
- 2, Zstandard: exactly one frame, whose header stores the content size, and that size must equal raw_len
  (checked before anything is decoded; a frame without a content size is corrupt). fstx then decodes with
  `ZSTD_decompressDCtx` and needs exactly raw_len bytes out.

The writer keeps a codec on a page only when it saves at least 10% (profiles balanced and small; profile
fast writes no codec).

### 4.8 Footer checks

After the verifier, fstx checks, in this order: footer version equals the header; low 16 bits of
required_features equal header bytes 6-7; no unknown required bit (section 5); row_group_size 1 to 2^31; at most 2^24 columns;
per column: name and time zone are valid UTF-8 without NUL (section 8), physical and logical type are known
(4.6), the type combination is allowed (4.2); row_count at most 2^62; per row group and column: the first
Page of every pages vector must sit on a multiple of 8 counted from the footer start (the FlatBuffers verifier
does not check this), and the rules of 4.3 to 4.5 and of 4.6 for each page entry. Factor levels are checked
only when used.

## 5. Feature bits

Bits 0-15 are for features that change how the header, tail or footer are read; they are also stored in
header bytes 6-7 and checked before the footer is read, so a reader cannot name them. Bits 16-63 are for
everything else. Named and reserved bits:

| bit | name | meaning |
|---|---|---|
| 16 | int64 | 64-bit integer columns (physical type 4); fstx 1.2 |
| 17 | binary | binary columns (physical type 5); fstx 1.2 |
| 18 | zstd_dict | pages with codec 3 |
| 19 | pcodec | pages with codec 4 |
| 63 | experimental | anything not in a release: codecs 128-255 or trial data; set by any build that writes them |

Rules:

- A reader must refuse a file with a required bit it does not know, naming the feature: from
  feature_names when the list has one entry per set bit, else from the table above, else by bit number.
- A reader must ignore optional bits it does not know when it reads. A writer must refuse to append to or
  compact (page copy) a file with an optional bit it does not know ("Update fstx to change this file; reading
  works"), since the change could make the data that bit describes wrong. A merging compaction reads the
  table and writes a fresh file (rule 6, section 9), which carries no bits it does not know.
- Anything a 1.0 reader cannot decode must set a required bit; an optional bit is only for data a reader can
  skip and still read the table correctly.
- Whenever a writer sets a bit it also writes the bit's name, so feature_names always has one entry per set
  bit. Names are ASCII letters, digits and `_`, at most 64 bytes. fstx does not check the names it reads;
  it only uses them in messages.
- The low 16 bits of required_features must equal header bytes 6-7.
- fstx 1.0 knows no feature bits. fstx 1.2 knows bits 16 and 17, both required bits: a writer sets bit 16 exactly
  when the file has at least one column of physical type 4 and bit 17 exactly when it has one of type 5, and names
  them `int64` and `binary` in feature_names. It sets no other bit. A 1.2 reader accepts bit 16 or 17 on a file
  without such a column (and reads it); it refuses as corrupt a type 4 or 5 column whose bit is not set.

## 6. Page bodies

A page body (after the codec is removed) is:

```
8 bytes   page header: byte 0 kind, byte 1 missing-value mode, bytes 2-3 zero, bytes 4-7 u32 row count
          missing-value block, only when the mode is not 0
          the top value block
```

Every part starts on a multiple of 8 counted from the start of the body. Kind and mode must equal chain
byte 56, the row count must equal the Page's row_count, the top value block's tag must equal chain byte 57,
and nothing may follow the top value block (all checked; bytes 2-3 are written as zero and not checked).
A dictionary page (kind 6) must have mode 0.

### 6.1 Blocks

Every block is an 8-byte header followed by its payload, padded with zeros to a multiple of 8:

```
byte 0     tag
bytes 1-3  a, b, c: tag-specific small parameters (0 when unused)
bytes 4-7  u32 payload length (without the padding)
```

The padding after a payload belongs to the block: the next block starts at header + 8 + payload length
rounded up to 8, and that much must fit in the parent. Inside a payload, "pad" means zero bytes up to the
next multiple of 8 counted from the payload start. "u32 + 4" means a u32 followed by 4 zero bytes. Child
blocks follow one another inside their parent's payload, right after the parent's own fields.

A child block is a value block: it holds a stated number of 32- or 64-bit values and takes part in the depth
rule. Byte blocks (6.8) are not child blocks.

A writer must not put anything after the last field or child of a payload. fstx ignores such bytes inside a
payload (section 10); only bytes after the top value block make a page corrupt.

Depth: the top value block is depth 0; the children of a block at depth d are at depth d + 1. A block at
depth 3 or more is corrupt, and RLE and DICT blocks are allowed only at depth 0 and 1. ALP and ALPRD blocks
are allowed only as the top value block of a double page.

### 6.2 Missing values

The writer stores missing values out of line and fills their slots in the value block, so the encodings
never see a missing value: numeric and code pages repeat the previous non-missing value (the first
non-missing value of the page for leading missing rows); string pages store length 0. When every row of a
numeric (int, logical, double or int64) or code page is missing, the writer stores 0 in every slot (the page is
then CONST 0). Binary pages are string pages here (kinds 4 / 5 / 6: missing = stored length 0, code -1).

The missing-value block is one of:

- Tag 0x81, bitmap: one bit per row, 1 = missing; row i is bit i % 64 of u64 word i / 64. Payload exactly
  8 x ceil(n / 64) bytes. Bits for rows n and above must be 0 (checked).
- Tag 0x82, runs: u32 run count + 4, then that many u32 run lengths, alternating not-missing / missing,
  starting with a not-missing run (which may be 0; others may be 0 too). The payload length is exactly
  8 + 4 x run count; the zero padding up to a multiple of 8 is outside it, as for every block. The lengths must
  add up to n (checked). fstx needs at least 8 + 4 x run count payload bytes and ignores any more.

Both blocks have a = b = c = 0 (written so, not checked).

The block's tag must match the mode in the page header (checked). The writer picks runs when that block is
smaller than the bitmap (8 + run count x 4, rounded up to 8, against 8 x ceil(n / 64)), else the bitmap.

What a missing value becomes after decoding:

| page kind | missing value | what the value block holds under it |
|---|---|---|
| 1 int, 3 logical | NA_integer_ (int32 `0x80000000`) | any value (not checked) |
| 2 double | NA_real_, bit pattern `0x7FF00000000007A2` | any value (not checked) |
| 4 string | NA_character_ | stored length 0 (checked) |
| 5 codes | code -1 | any value (not checked) |
| 7 int64 | INT64_MIN (`0x8000000000000000`) | any value (not checked); the writer repeats the previous value as on int pages |

A writer must not store the missing-value pattern of a page kind (`0x80000000` on int and logical pages,
`0x7FF00000000007A2` on double pages, `0x8000000000000000` on int64 pages, -1 on code pages) on a row that the missing-value block does not mark.
If a file does, the value is still missing for an R reader, since R has no other meaning for that pattern,
but null_count and the missing-value block do not show it; fstx does not check for it. Readers for other
systems should treat such a value as missing too. On a page whose null_count is 0, fstx's Arrow stream does not look for the pattern
and passes it on as a value, unless the null_count check of section 4.5 is on, which refuses such a page.

A logical page stores 1 for TRUE and 0 for FALSE; fstx does not check that other values are absent.

### 6.3 Encoding tags

Values are either 32-bit (int, logical and code pages; lengths, run lengths and codes inside other blocks)
or 64-bit (double pages, which the integer encodings work on as int64 bit patterns; int64 pages; ALP integers;
ALPRD right parts). "n" is the block's value count: the page's row count for the top block, the counts below for
children. All arithmetic on values (FOR base, DELTA sums) is on unsigned 32- or 64-bit integers and wraps.
A decoded 32-bit value is a two's-complement int32 (so code -1 is `0xFFFFFFFF`). A decoded 64-bit value on an
int64 page is a two's-complement int64; on a double page it is the IEEE 754 bit pattern of the double. The tags
8 and 9 are for double pages only and tag 10 for string and dictionary pages only, so an int64 page with tag 8,
9 or 10 is corrupt.

| tag | name | a, b | payload |
|---|---|---|---|
| 1 | PLAIN | | n values; payload length exactly n x value size |
| 2 | CONST | | one value; payload length exactly the value size; every row holds it |
| 3 | FOR | | bit-packed vectors (6.4) |
| 4 | DELTA | | bit-packed vectors of deltas (6.4) |
| 5 | RLE | | u32 run count r + 4; child block of r run values (same value size as the parent); child 32-bit block of r run lengths. r is 1..n, each length at least 1, lengths add up to n |
| 6 | DICT | | u32 dictionary size d + 4; child block of d values (same value size as the parent); child 32-bit block of n codes, each 0..d-1. d is 1..n. The writer sorts the values (as signed integers); readers must not rely on the order |
| 7 | SPLIT | a = value size, which must equal the value size of this block (4 for a 32-bit block, 8 for a 64-bit block; anything else is corrupt) | byte-split: byte 0 of every value, then byte 1 of every value, ...; payload length exactly n x a |
| 8 | ALP | a = e, b = f | double pages, top block only (6.6) |
| 9 | ALPRD | a = left bits, b = dictionary size | double pages, top block only (6.7) |
| 10 | FSST | | string and dictionary pages only (6.8) |

Child blocks may use any tag of 1-7 that suits their values (subject to the depth rule). A top block on a
string or dictionary page must be PLAIN or FSST; on other pages any tag but FSST, with ALP and ALPRD only on
double pages. An unknown tag inside a body, or one in the wrong place, is corrupt.

### 6.4 FOR and DELTA: bit-packing

Values are cut into vectors of 1,024: vector v holds values 1024 v to 1024 v + 1023, and there are
nv = ceil(n / 1024) vectors; the last one may be short. With B = value bits (32 or 64):

```
FOR payload                            DELTA payload
  nv x u8   bit width w[v] (pad)         nv x u8   bit width w[v] (pad)
                                         nv values first[v] (pad)
  nv values base[v] (pad)                nv values base[v] (pad)
  packed vector 0, 1, ..., nv-1          packed vector 0, 1, ..., nv-1
```

Each w[v] is 0..B (above B is corrupt); packed vector v is 128 x w[v] bytes (1,024 values of w bits). The
payload must hold at least all of this.

Packed layout (the same for every width). With L = 1024 / B lanes (32 for 32-bit values, 16 for 64-bit),
value i of a vector (0..1023) is in lane l = i mod L, row r = i div L. Each lane is its own bit stream of
B-bit words: row r occupies bits r x w .. r x w + w - 1 of the stream, low bits first, so it starts in word
k = (r x w) div B at bit (r x w) mod B and spills into word k + 1 when it does not fit. Word k of lane l is
stored as word k x L + l of the packed vector (words of B bits, little-endian). Width 0 stores nothing.

Decoding vector v: unpack the 1,024 stored numbers u[i], then

- FOR: value i = base[v] + u[i].
- DELTA: x[0] = first[v], and for i = 1..m-1 (m = values in this vector), x[i] = x[i-1] + base[v] + u[i].
  Slot 0's stored delta is ignored.

Stored numbers past the end of a short last vector are ignored (the writer stores 0). The writer takes base
as the smallest value (FOR) or the smallest delta (DELTA, slot 0 set to the next delta so it stays in range)
and w as the bit length of max - min; readers must not depend on that.

Worked example: an int page, FOR, no codec, n = 5, values 100, 103, 101, 107, 100.

- One vector. base = 100, numbers to pack 0, 3, 1, 7, 0 (and 1,019 zeros), w = 3 bits (7 = binary 111).
- B = 32, L = 32: values 0-4 are rows 0 of lanes 0-4. Row 0 starts at bit 0 of word 0 of each lane, and
  word 0 of lane l is packed word l. So packed u32 words 0-4 are 0, 3, 1, 7, 0 and words 5-95 are 0
  (128 x 3 = 384 bytes = 96 words).
- Payload: `03` + 7 zero bytes (widths, pad); `64 00 00 00` + 4 zero bytes (base 100, pad); the 384 packed
  bytes, starting `00 00 00 00  03 00 00 00  01 00 00 00  07 00 00 00`. Length 400 (`0x190`).
- Block header: `03 00 00 00 90 01 00 00`. Page body: `01 00 00 00 05 00 00 00` (kind 1, mode 0, 5 rows),
  then the block: 416 bytes, raw_len 416, chain `0x00000301`.
- Where a later row would go: value 37 is lane 5, row 1, so bits 3-5 of packed word 5. Value 330 is lane 10,
  row 10: it starts at bit 30, so its low 2 bits are bits 30-31 of packed word 10 and its top bit is bit 0
  of packed word 1 x 32 + 10 = 42.

The package test `test-compat-golden.R` writes this page and compares its bytes with this example.

### 6.5 Chain bytes 58-59

The writer sets them to the top tags of the top block's first and second child blocks, else 0:

| top block | byte 58 | byte 59 |
|---|---|---|
| RLE | run values | run lengths |
| DICT | dictionary values | codes |
| ALP | integers q | 0 |
| ALPRD | dictionary codes | right parts |
| string PLAIN | byte lengths | 1 (the bytes block) |
| FSST | compressed lengths (the third block in the payload, after the two byte blocks) | 0 |
| others | 0 | 0 |

For a string PLAIN page byte 59 is 1, the tag of the bytes block, although that block is a byte block; it is
the one place where a chain byte names a byte block.

They are informational (for `fstx_pages()`): fstx checks only that they are at most 10 (above 10 is refused
as an unknown encoding when the file is opened, 4.6) and does not compare them with the body.

### 6.6 ALP (tag 8): decimal doubles

```
a = e (0..18), b = f (0..e)          (others corrupt)
u32 exception count x + 4            (x at most n)
x u32 exception rows (pad)
x u64 exception bit patterns
child 64-bit block of n integers q
```

q[i] is a two's-complement signed 64-bit integer (negative values have the top bit set), converted to double
as a signed number. Value i = ((double) q[i] x 10^f) / 10^e, computed as two IEEE double operations (convert q to double,
multiply, then divide, each rounded to nearest; no fused multiply-add), with 10^k the exact double for 1e0
to 1e18. Then, for each exception j, the value at row `rows[j]` (which must be below n) is replaced by the
double with bit pattern `patterns[j]`; the q at that row is ignored. A row listed twice takes the later
pattern. The writer chooses e and f so that every non-exception value comes back with the same bits, and
puts the previous q (or the first good one) under exceptions.

### 6.7 ALPRD (tag 9): real doubles

```
a = left bits lb (1..16), b = dictionary size nd (1..8)    (others corrupt)
8 x u16 dictionary of left parts (entries nd..7 are 0 and ignored)
u32 exception count x + 4                                   (x at most n)
x u32 exception rows (pad)
x u16 exception left parts (pad)
child 32-bit block of n dictionary codes (each 0..nd-1)
child 64-bit block of n right parts
```

With rb = 64 - lb, the bit pattern of value i is `(dict[code[i]] << rb) | (right[i] & (2^rb - 1))`; only
the low rb bits of a right part are used, and bits of a left part above lb fall off the top. Then, for
each exception j, row `rows[j]` (below n) gets `(left[j] << rb) | (right[rows[j]] & (2^rb - 1))`.

### 6.8 String pages

Kind 4 (string) and kind 6 (dictionary) pages hold strings; kind 6 has no missing values. Values of a string
column (physical type 3) are UTF-8 and must not contain a NUL byte (a NUL is corrupt); a string is at most
2^31-1 bytes and the writer refuses strings over 1 GB.

Binary columns (physical type 5) use the same pages: kind 4, or kind 5 codes with a kind 6 dictionary, with PLAIN
or FSST top blocks exactly as below. Their values are bytes, not text: NUL bytes are allowed, the UTF-8 rule 7 of
section 9 does not apply, and a reader returns them unchanged. Every other rule of this section holds (lengths,
2^31-1 bytes per value, FSST only for pages of at most 64 MB, dictionary entries at most 2^25). The fstx writer
builds a binary column's dictionary by comparing the values' bytes; equal values get one entry.

Top block:

- PLAIN (tag 1): its payload is two blocks: a 32-bit child block (depth 1) of the n byte lengths (0 under
  missing rows), then a PLAIN block of all string bytes, one string after another. No length may be
  negative; the lengths must add up to the bytes block's payload length.
- FSST (tag 10): u64 total decoded bytes T; a PLAIN block holding the symbol table (6.9); a PLAIN block of
  the compressed strings, one after another; a 32-bit child block (depth 1) of the n compressed lengths.
  No compressed length may be negative; they must add up to the compressed block's payload length C, with
  T at most 8 x C and at most 2^31-1. String i is the next compressed-length bytes decoded on their own;
  the decoded lengths must add up to T. A missing row has compressed length 0.

The bytes block of a PLAIN string page, and the symbol-table and compressed-bytes blocks of an FSST page,
are "byte blocks": tag 1 (checked), a = b = c = 0 (written so, not checked), and a payload that is plain
bytes, with no value count of its own: its length is the sum of the lengths (PLAIN), the table length, or
the sum of the compressed lengths (FSST). They are not value blocks: the depth rule does not apply to them
and they have no children. The top PLAIN or FSST block of a string page also has a = b = c = 0.

The writer uses FSST only for pages whose strings add up to at most 64 MB. Kind 5 (codes) pages are 32-bit
pages of 0-based positions into the chunk's dictionary page, -1 = missing; any other code outside
0..entries-1 is corrupt (checked on the rows read).

### 6.9 FSST symbol table and decoding

The table is the `fsst_export()` format of the FSST library (cwida/fsst, version 20190218), which fstx
vendors in `src/vendor/fsst`:

| bytes | meaning |
|---|---|
| 0-7 | u64 version word: bits 32-63 = 20190218 (others refused); bits 24-31 suffix limit, 16-23 terminator, 8-15 symbol count, 0-7 = 1 (the decoder ignores bits 0-31) |
| 8 | bit 0: zero-terminated table. fstx never writes one and refuses it |
| 9-16 | symbol counts by length: byte 9 + k for k = 1..7 counts symbols of k + 1 bytes; byte 9 counts 1-byte symbols |
| 17 on | the symbols' bytes, one after another, with no separators |

fstx checks before it decodes: table length at least 17; byte 8 bit 0 clear; the eight counts add up to
at most 255; 17 + the symbols' total bytes is at most the table length. Bytes after the symbols are
ignored.

Codes are given in this order: first every 2-byte symbol (count at byte 10), then 3-byte, ..., 8-byte
(count at byte 16), then 1-byte symbols (count at byte 9); the first symbol gets code 0. A compressed
string is a sequence of codes: a code below 255 writes its symbol's bytes; code 255 (escape) writes the
next byte as it is. A writer must not end a compressed string with a lone escape (fstx checks this:
corrupt) and must not use a code at or above the symbol count other than 255. (The fstx decoder writes
the 8 bytes `corrupt` and a zero byte for an unused code; see section 10.)

## 7. Legal combinations

| page kind | encodings (top tag) | missing-value modes |
|---|---|---|
| 1 int, 3 logical, 5 codes | 1-7 | 0, 1, 2 |
| 2 double | 1-9 | 0, 1, 2 |
| 7 int64 | 1-7 | 0, 1, 2 |
| 4 string | 1, 10 | 0, 1, 2 |
| 6 dictionary | 1, 10 | 0 |

Each with codec 0, 1 or 2: 357 mixes in all (63 + 63 + 63 + 81 + 63 + 18 + 6). Binary columns use the 87
mixes of kinds 4, 5 and 6 a second time, so the package test `test-compat-format.R` writes and reads 444 column /
page mixes. The golden files hold the 294 mixes as written by fstx 1.0 (`golden-1.0`) and the new ones as written
by fstx 1.2 (`golden-1.2`).

## 8. Limits (checked on read)

| what | limit |
|---|---|
| columns | 2^24 |
| rows | 2^62 |
| row_group_size | 1 to 2^31; every row group's row_count at most row_group_size |
| page raw_len and comp_len | 2^31-1 |
| dictionary entries per page | 2^25 |
| string length | 2^31-1 bytes (writer: 1 GB) |
| binary value | 2^31-1 bytes (writer: 1 GB) |
| int64 value | any int64 but INT64_MIN, which is the missing value |
| names, time zones | valid UTF-8 (no overlongs, surrogates or values past U+10FFFF), no NUL |

Factor levels are not checked when a file is opened, to keep opening fast; they are checked when used.

## 9. Rules for writers and later versions

1. The Page struct never changes. New per-page facts go into new lists in ColumnChunk, one entry per page.
2. FlatBuffers fields are only ever added at the end of a table.
3. Page min / max of int64 pages are rounded outward to doubles (4.5), so page skipping stays correct.
4. Attribute blobs (Column.attrs, Footer.table_attrs) hold a named list of R attributes, in the FXA1
   encoding of section 9.4 when it can hold them, else as R serialization. Column.attrs holds a column's
   attributes other than those the schema stores (class for factor, Date and POSIXct; levels; tzone).
   Footer.table_attrs holds the data frame's attributes without names, without automatic row names and
   without `.internal.selfref`. Readers that are not R may ignore both.
5. A new encoding tag, codec, physical type or page kind needs a required feature bit, so an older reader
   names it instead of calling the file corrupt.
6. Compaction always writes format 3. Appends and page-copy compaction keep the file's profile (as stored,
   even one this build does not know) and feature fields. A merging compaction (`fstx_compact(merge =
   TRUE)`) reads the table and writes a fresh file, with no feature bits it does not know, and with profile
   balanced when the stored profile is unknown.
7. Writers write all text (names, levels, time zones, strings) as UTF-8. Readers must not rely on it. Names
   and time zones that are not valid UTF-8 are corrupt (section 8). A string value that is not valid UTF-8 is
   not corrupt: fstx returns its bytes unchanged, marked as UTF-8, without checking them (so does its Arrow
   stream). Another reader may do the same, replace the bad bytes, or refuse the value, but it must not crash
   and must not let one bad string change its neighbours. A NUL byte in a string value is corrupt. Binary
   values are not text: no UTF-8 rule, NUL allowed (6.8).

### 9.1 Same bytes

The same data, settings and fstx version give the same file bytes on every thread count and every SIMD
setting (no fused multiply-add anywhere).

### 9.2 Checksums

Header, footer and every page carry a checksum: XXH3, 64-bit variant, as `XXH3_64bits()` of xxHash 0.8.0 or
later computes it (default secret, seed 0). The 64-bit result is stored as a little-endian u64; this is not
the big-endian "canonical" byte order that the xxHash tools print. A header is 24 bytes; footers and stored
pages can be any length from 8 bytes up, so a reader needs the whole algorithm, not only the long-input path.
Pages are checked when read unless the reader turns checking off; page-copy compaction always checks them.

Test vectors: the empty input gives `0x2D06800538D394C2`. The 24 header bytes
`46 53 54 58 03 00 00 00 04 03 02 01 00 00 00 00 00 00 00 00 00 00 00 00` (every fstx 1.0 or 1.2 file that is not
in the middle of an append starts with them) give `0x435A43570CB4FC1E`, stored at offset 24 of any such file
as the 8 bytes `1E FC B4 0C 57 43 5A 43`.

### 9.3 Appends

The writer first writes the header with bytes 16-23 set to the current committed end (the file size, or
the committed end of an append that was cut off) and flushes it to disk; it then cuts the file back to that
end, writes the new pages, a new footer (describing old and new row groups) and a new tail after it, flushes
them, and only then rewrites the header with 0 in bytes 16-23. A reader that finds a committed end treats
the file as ending there (its tail is the 32 bytes before it) and ignores anything after it. The old footer
and tail stay in the file, unreferenced, until compaction.

The pages of an append start on the next 64-byte boundary at or after the committed end, with zero bytes in
between, and follow the layout rules of section 1 from there.

A new factor level may be added by an append only after the file's levels (the file's levels must be the
first levels of the new rows' factor).

The new footer keeps the file's table attributes (section 9.4), except that stored row names are removed and,
when the append adds at least one row, so are a data.table key (`sorted`) and `index`, which the new rows may
not follow. When anything is removed the blob is encoded again (FXA1 when it can hold it); otherwise its bytes
are kept.

Page-copy compaction (`fstx_compact()`) copies every page's stored bytes unchanged, checksum checked, into a
fresh file: per row group, per column, the dictionary page first, then the data pages.

An append keeps every column's physical type, so it never adds or removes a feature bit. Page-copy compaction
keeps the feature fields; a merging compaction keeps each column's physical type.

### 9.4 Attribute blobs

An attribute blob is one of two encodings, told apart by its first bytes:

- **FXA1** (written whenever it can hold the list): the 4 bytes `46 58 41 01` ("FXA", version 1), then
  one attribute list, then nothing.
- **R serialization** (the fallback): `serialize(x, NULL, xdr = FALSE)` of the named list, R's native
  binary format, which starts with `B\n`. An R serialization always starts with `A\n`, `B\n` or `X\n`, so
  it never starts with "FXA". fstx writes it only for attributes FXA1 cannot hold, which is exactly
  when one of these holds anywhere in the list: a value that is not a logical, integer, double or character
  vector (lists, environments, functions, complex or raw values, ...); attributes nested deeper than the
  three levels below; a list whose names are missing, empty or repeated; a vector of 2^31-1 or more values;
  strings marked "bytes"; a name or string value that `enc2utf8()` cannot convert or that is not valid UTF-8
  after it; the strings of one vector totalling 2^31-1 or more bytes. Files written by fstx 1.0 builds
  before FXA1 hold only this encoding.

An attribute list, all integers i32 little-endian:

| field | bytes | |
|---|---|---|
| count | 4 | number of attributes |
| per attribute: name_len | 4 | 1 or more |
| name | name_len | UTF-8, no NUL; the names in one list are distinct |
| type | 4 | 1 logical, 2 integer, 3 double, 4 character |
| length | 4 | number of values, 0 to 2^31-2 |
| values | | logical and integer: length x i32 (NA = -2^31; logical only 0, 1 or NA). double: length x 8 bytes, the IEEE 754 bits (NA and NaN payloads kept). character: length x i32 byte lengths (-1 = NA), then the bytes of the non-NA strings back to back (UTF-8 as written; no NUL) |
| attrs | | the value's own attribute list (count 0 for none) |

A count of 0 is valid at every level (at level 1 it is an empty list: no attributes). fstx never writes
it at level 1, as it stores no blob at all for no attributes; as a table blob it is corrupt (no class,
below).

Nesting: the blob's list is level 1, a value's own list is level 2 and the list of a level-2 value is level
3; the values of a level-3 list have count 0. This holds class vectors, row names, data.table `sorted` and
`index` (an `integer(0)` whose order vectors carry `starts`, `anyna` and the like), named vectors, factor- or
Date-valued attributes and labels. Nothing is aligned; a list has no padding.

Worked example, the table blob of a plain data.frame, `list(class = "data.frame")`, 43 bytes:
`46 58 41 01`, count `01 00 00 00`, name_len `05 00 00 00`, `class`, type `04 00 00 00`, length
`01 00 00 00`, byte length `0A 00 00 00`, `data.frame`, attrs count `00 00 00 00`.

A reader decodes FXA1 with every count and length checked against the bytes left. Any mismatch, an unknown
type, a logical value other than 0 / 1 / NA, an empty, non-UTF-8 or repeated name, a NUL in a name or string,
deeper nesting, bytes after the end, a version byte other than 01 after "FXA", or attributes R refuses to set
(for example a `dim` that does not fit the column) make the file corrupt. String values that are not valid
UTF-8 follow rule 7 of section 9. fstx restores R serialization with `unserialize()` (section 10), or skips it
with `read_fstx(attributes = FALSE)`.

Table attributes, in either encoding, are checked before they are set (by `read_fstx()`, `fstx_meta()` and
appends). Corrupt: a `class` that is not a character vector containing "data.frame" (including none), a
`names`, `dim` or `dimnames` attribute, and `row.names` that are not an integer or character vector with one
value per row of the file. Row names in R's compact form, an integer `c(NA, n)` with |n| the row count, are
accepted and rebuilt as automatic row names; any other n is corrupt. Dropped (the rest is kept): a data.table
`sorted` that is not a character vector of the file's column names; an `index` that is not an `integer(0)`;
each of its order vectors that is not an integer vector of length 0 or a permutation of 1 to the row count,
or whose `starts` are not integers in 1 to the row count, strictly increasing; the `index` when no order
vector is left. Whether a key or an index matches the data is not checked (section 10).

## 10. Where this document and fstx 1.2 differ

Things a strict reading of the rules above would refuse that fstx 1.2 accepts, as fstx 1.0 does (all on
corrupt or hostile input; a writer following this document never produces them):

- Chain bytes 58-59 are not compared with the body (6.5).
- Bytes 2-3 of the page header, the u32 + 4 padding bytes, padding inside payloads, and the a, b, c bytes of
  missing-value blocks and byte blocks are not checked.
- Values under missing rows of numeric and code pages are not checked (6.2); logical values other than
  0 and 1 are not refused.
- Dictionary page first_row, page min / max / flags, null_count against the missing-value block (unless
  the opt-in null_count check is on, section 4.5), and overlapping pages are not checked.
- Extra bytes at the end of a runs block payload are ignored.
- Bytes after the last child of an RLE, DICT, ALP or ALPRD payload, and after the packed vectors of a FOR or
  DELTA payload, are ignored.
- Anything after the bytes block of a string PLAIN payload, or after the lengths block of an FSST payload,
  is ignored.
- FSST: an unused code decodes to `corrupt` plus a zero byte and is not refused.
- Attribute blobs in R serialization (section 9.4: attributes FXA1 cannot hold, and files from fstx 1.0
  before FXA1) are not checked by fstx: they are restored with `unserialize()`, as `readRDS()` does. Like an
  .rds file, only read such files from sources you trust, or read them with `read_fstx(attributes = FALSE)`,
  which skips those blobs: a crafted blob can crash R when it is restored. The checksum catches accidental
  damage, not deliberate tampering. FXA1 blobs are checked as section 9.4 says and never run code, but a
  data.table key or index is not compared with the data, so a crafted file can give wrong data.table
  results or an R error.

## Appendix A. Footer schema (`tools/fstx_footer.fbs`)

```
namespace fstx.fb;

file_identifier "FSTX";

enum PhysType : ubyte { Int32 = 0, Float64 = 1, Logical = 2, String = 3, Int64 = 4, Binary = 5 }
enum LogicalType : ubyte { None = 0, Factor = 1, Date = 2, POSIXct = 3 }
enum Codec : ubyte { None = 0, LZ4 = 1, ZSTD = 2 }
enum Profile : ubyte { Fast = 0, Balanced = 1, Small = 2 }

struct Page {
  offset: ulong;
  first_row: ulong;
  hash: ulong;
  min: double;
  max: double;
  comp_len: uint;
  raw_len: uint;
  row_count: uint;
  null_count: uint;
  chain: uint;
  codec: Codec;
  flags: ubyte;
}

table ColumnChunk {
  pages: [Page];
  dictionary: Page;
}

table RowGroup {
  first_row: ulong;
  row_count: ulong;
  columns: [ColumnChunk];
}

table Column {
  name: string;
  phys: PhysType;
  logical: LogicalType;
  ordered: bool;
  levels: [string];
  tzone: string;
  attrs: [ubyte];
}

table Footer {
  format_version: ushort;
  row_count: ulong;
  row_group_size: ulong;
  columns: [Column];
  row_groups: [RowGroup];
  table_attrs: [ubyte];
  created_by: string;
  profile: Profile = Balanced;
  required_features: ulong;   // format 3
  optional_features: ulong;   // format 3
  feature_names: [string];    // format 3
}

root_type Footer;
```

The enums list only the numbers fstx 1.2 writes; a reader must accept any byte in an enum field and apply
4.6 to it (FlatBuffers verifiers do not check enum values).
