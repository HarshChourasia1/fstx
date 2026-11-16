# fstx format 1.0 (file format version 3)

Written in phase 1 (WS1) and finished by WS8 for the 1.0 release (15 Nov 2026). Every rule here was checked
against the code of fstx 1.0 (`src/core_reader.cpp`, `src/core_writer.cpp`, `src/enc_num.cpp`,
`src/enc_str.cpp`, `src/kern_scalar.h`, `src/codec_ext.cpp`). The words "must" and "must not" are rules for
every writer and reader; "fstx checks" says what the fstx 1.0 reader does; anything else is description.
Someone who has never seen the code should be able to write a reader from this document alone.

fstx 1.0 reads and writes format 3 only. Files with format versions 1 and 2 (fstx 0.x development versions)
are refused with a message that says so.

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
| 5 | table_attrs | [u8] | data-frame attributes, R serialization (rule 4, section 9); may be absent |
| 6 | created_by | string | `fstx <version>` of the writer (fstx 1.0.0 writes `fstx 1.0.0`). Informational: a reader must not use it to decide how to read the file; a file written by a development build before 1.0.0 may say `fstx 0.99.0` |
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
| 6 | attrs | [u8] | other R attributes, R serialization; may be absent |

Type combinations fstx accepts (anything else is corrupt):

| logical | phys | stored values |
|---|---|---|
| 0 none | any of 0-3 | as the physical type |
| 1 factor | 0 int32, with `levels` present | 1-based codes into `levels`; NA = missing. A code outside 1..number of levels is corrupt (checked on the rows read) |
| 2 Date | 0 int32 or 1 float64 | days since 1970-01-01 |
| 3 POSIXct | 1 float64 | seconds since 1970-01-01 00:00 UTC |

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
| 1 | dictionary | Page (struct) | string columns only: the chunk's dictionary page (kind 6, no missing values); its data pages are then kind 5 (codes). Absent otherwise |

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
when there is at least one; null_count counts only NA_real_ (section 6.2), so other NaNs are values. String,
codes and dictionary pages: no statistics (min = max = 0, flag clear). A reader may skip a page whose
min/max cannot match a filter only when the flag is set. fstx does not check min, max or flags against the
data; a writer must not set the flag with a range that leaves out a value.

null_count must equal the number of rows the page's missing-value block marks, and 0 when the mode is 0. A
writer must write mode 0 when no row is missing; a reader must still accept mode 1 or 2 with no row marked.
fstx does not compare null_count with the body. It trusts it the way it trusts min and max: to skip pages in
filters and to tell R and Arrow that a column has no missing values. A wrong count gives wrong answers
(missing values that are not reported as missing), not a crash.

Pages must lie wholly before the footer; fstx does not check that pages do not overlap one another.

fstx also refuses, as corrupt, a page whose raw_len the codec cannot reach: LZ4, raw_len above comp_len x
255 + 64; Zstandard, raw_len above comp_len x 32,768 + 131,072; and, for int, double, logical and codes pages
with any codec, raw_len above row_count x value size x 64 + 65,536 (value size 8 for double pages, else 4).

### 4.6 Numbers and their reserved ranges

| what | read and written | reserved | a reader that meets a reserved or unknown number |
|---|---|---|---|
| physical type | 0 int32, 1 float64, 2 logical (int32 0/1/NA), 3 string | 4 = 64-bit integer, 5 = binary, 6-255 | refuses the file: "uses ..., Update fstx" |
| logical type | 0-3 | 4-255 | refuses, "Update fstx" |
| codec | 0 none, 1 LZ4 block, 2 Zstandard | 3 = zstd with a dictionary, 4 = Pcodec, 5-127 later codecs, 128-255 experiments never written by a release | refuses, naming the codec when it has a name |
| page kind | 1 int, 2 double, 3 logical, 4 string, 5 dictionary codes, 6 dictionary | 0 corrupt; 7-15 reserved | refuses, "Update fstx" |
| missing-value mode | 0 none, 1 bitmap, 2 runs | 3-15 | refuses, "Update fstx" |
| encoding tag (chain bytes 57-59) | 1-10 (section 6) | 11-255 | refuses, "Update fstx" |

Refusals of reserved numbers happen when the file is opened, before any data is read, and before the
corruption checks of the same page entry, so a newer file gets "Update fstx", not "corrupt". Tags deeper
inside a page body than the chain shows are checked while decoding, and an unknown one there is "corrupt".
So a writer that uses a new encoding tag anywhere in a page must also set a required feature bit (rule 5).

The page kind follows from the column: phys 0 gives kind 1, phys 1 kind 2, phys 2 kind 3, phys 3 kind 4
(kind 5 for the data pages of a chunk with a dictionary, kind 6 for the dictionary page). Any other kind
in a known range is corrupt.

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
| 16 | int64 | 64-bit integer columns (physical type 4) |
| 17 | binary | binary columns (physical type 5) |
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
- fstx 1.0 knows no feature bits: every file it writes has both fields 0 and no names.

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
numeric or code page is missing, the writer stores 0 in every slot (the page is then CONST 0).

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

A writer must not store the missing-value pattern of a page kind (`0x80000000` on int and logical pages,
`0x7FF00000000007A2` on double pages, -1 on code pages) on a row that the missing-value block does not mark.
If a file does, the value is still missing for an R reader, since R has no other meaning for that pattern,
but null_count and the missing-value block do not show it; fstx does not check for it. Readers for other
systems should treat such a value as missing too.

A logical page stores 1 for TRUE and 0 for FALSE; fstx does not check that other values are absent.

### 6.3 Encoding tags

Values are either 32-bit (int, logical and code pages; lengths, run lengths and codes inside other blocks)
or 64-bit (double pages, which the integer encodings work on as int64 bit patterns; ALP integers; ALPRD
right parts). "n" is the block's value count: the page's row count for the top block, the counts below for
children. All arithmetic on values (FOR base, DELTA sums) is on unsigned 32- or 64-bit integers and wraps.
A decoded 32-bit value is a two's-complement int32 (so code -1 is `0xFFFFFFFF`), a decoded 64-bit value on a
double page is the IEEE 754 bit pattern of the double.

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

Kind 4 (string) and kind 6 (dictionary) pages hold strings; kind 6 has no missing values. Strings are
UTF-8 and must not contain a NUL byte (a NUL is corrupt); a string is at most 2^31-1 bytes and the writer
refuses strings over 1 GB. Top block:

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
corrupt) and must not use a code at or above the symbol count other than 255. (The fstx 1.0 decoder writes
the 8 bytes `corrupt` and a zero byte for an unused code; see section 10.)

## 7. Legal combinations

| page kind | encodings (top tag) | missing-value modes |
|---|---|---|
| 1 int, 3 logical, 5 codes | 1-7 | 0, 1, 2 |
| 2 double | 1-9 | 0, 1, 2 |
| 4 string | 1, 10 | 0, 1, 2 |
| 6 dictionary | 1, 10 | 0 |

Each with codec 0, 1 or 2: 294 mixes in all (63 + 63 + 63 + 81 + 18 + 6). The package test
`test-compat-format.R` writes and reads every one; the golden files (`test-compat-golden.R`) hold every one
as written by fstx 1.0.

## 8. Limits (checked on read)

| what | limit |
|---|---|
| columns | 2^24 |
| rows | 2^62 |
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
4. Attribute blobs are R serialization: `serialize(x, NULL, xdr = FALSE)` (R's native binary format, which
   starts with `B\n`) of a named list. Column.attrs holds a column's attributes other than those the
   schema stores (class for factor, Date and POSIXct; levels; tzone). Footer.table_attrs holds the data
   frame's attributes without names, without automatic row names and without `.internal.selfref`. Readers
   that are not R may ignore both.
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
   and must not let one bad string change its neighbours. A NUL byte in a string value is corrupt.

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
`46 53 54 58 03 00 00 00 04 03 02 01 00 00 00 00 00 00 00 00 00 00 00 00` (every fstx 1.0 file that is not
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

Page-copy compaction (`fstx_compact()`) copies every page's stored bytes unchanged, checksum checked, into a
fresh file: per row group, per column, the dictionary page first, then the data pages.

## 10. Where this document and fstx 1.0 differ

Things a strict reading of the rules above would refuse that fstx 1.0 accepts (all on corrupt or hostile
input; a writer following this document never produces them):

- Chain bytes 58-59 are not compared with the body (6.5).
- Bytes 2-3 of the page header, the u32 + 4 padding bytes, padding inside payloads, and the a, b, c bytes of
  missing-value blocks and byte blocks are not checked.
- Values under missing rows of numeric and code pages are not checked (6.2); logical values other than
  0 and 1 are not refused.
- Dictionary page first_row, page min / max / flags, null_count against the missing-value block, and
  overlapping pages are not checked.
- Extra bytes at the end of a runs block payload are ignored.
- Bytes after the last child of an RLE, DICT, ALP or ALPRD payload, and after the packed vectors of a FOR or
  DELTA payload, are ignored.
- Anything after the bytes block of a string PLAIN payload, or after the lengths block of an FSST payload,
  is ignored.
- FSST: an unused code decodes to `corrupt` plus a zero byte and is not refused.

## Appendix A. Footer schema (`tools/fstx_footer.fbs`)

```
namespace fstx.fb;

file_identifier "FSTX";

enum PhysType : ubyte { Int32 = 0, Float64 = 1, Logical = 2, String = 3 }
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

The enums list only the numbers fstx 1.0 writes; a reader must accept any byte in an enum field and apply
4.6 to it (FlatBuffers verifiers do not check enum values).
