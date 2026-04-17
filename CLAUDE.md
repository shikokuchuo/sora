# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working
with code in this repository.

## Overview

sora — Shared Objects for R Applications. Uses POSIX shared memory
(Linux, macOS) and Win32 file mappings (Windows) with R’s ALTREP
framework to let multiple processes on the same machine read the same
physical memory pages. No external dependencies. Requires R \>= 4.3.0
(for ALTLIST). API:
[`share()`](https://shikokuchuo.net/sora/reference/share.md) → ALTREP
shared object,
[`map_shared()`](https://shikokuchuo.net/sora/reference/map_shared.md) →
open SHM by name,
[`shared_name()`](https://shikokuchuo.net/sora/reference/shared_name.md)
→ extract SHM name,
[`is_shared()`](https://shikokuchuo.net/sora/reference/is_shared.md) →
test if shared. SHM lifetime is automatic — managed by R’s garbage
collector via chained external pointer finalizers. ALTREP serialization
hooks serialize standalone shared objects as the SHM name (~30 bytes)
and ALTLIST element vectors as `(parent_name, index)`, enabling
transparent use with mirai and any R serialization path.

## Development Commands

### Testing

``` r
# Run unit tests (single file, minitest framework — not testthat)
source("tests/tests.R")
```

Set `NOT_CRAN=true` environment variable to run integration tests that
require mirai daemons.

### Building and Checking

``` bash
# Build package
R CMD build .

# Check package
R CMD check --no-manual sora_*.tar.gz
```

``` r
# Generate documentation from roxygen2 comments
devtools::document()
```

## Key Architecture

### Two-Tier Design

- **Tier 2 (zero-copy)**: All atomic vectors (including character
  vectors and those with arbitrary attributes such as names, class,
  levels, dim) and data frame columns are written directly into SHM and
  backed by ALTREP on consumers. Attributes are serialized into a
  trailing section of the SHM region and restored via `SET_ATTRIB` on
  the consumer. For numeric types, `Dataptr_or_null` returns the SHM
  pointer for reads; `Dataptr(writable=TRUE)` materializes a private
  copy (COW). For character vectors, `Elt` lazily creates each CHARSXP
  via `Rf_mkCharLenCE` from the SHM data; `Dataptr_or_null` returns NULL
  to force element-by-element access.
- **Tier 1 (pass-through)**: All other R objects (environments,
  closures, language objects) are returned unchanged by
  [`share()`](https://shikokuchuo.net/sora/reference/share.md). No SHM
  is created.

### share() Dispatch Logic (altrep.c: `sora_create`)

All R exported functions are single `.Call` wrappers.
[`share()`](https://shikokuchuo.net/sora/reference/share.md) calls
`sora_create` which dispatches on `TYPEOF(x)`:

1.  `NILSXP` → returned as-is (falls through all checks).
2.  `VECSXP`/`LISTSXP` → `sora_shm_create_list_call` — ALTLIST with
    per-element directory. Each element is independently Tier 2 (any
    atomic, with or without attributes) or Tier 1 (serialized). Data
    frames and pairlists go through this path (pairlists are coerced to
    VECSXP via `Rf_coerceVector` at the C level).
3.  `STRSXP` → `sora_shm_create_string_call` — ALTSTRING backed by SHM
    with offset table + packed string data. Attributes (if any) are
    serialized after the string data.
4.  Other Tier 2 eligible types (`REALSXP`, `INTSXP`, `LGLSXP`,
    `RAWSXP`, `CPLXSXP`) → `sora_shm_create_vector_call` — single ALTREP
    vector backed by SHM. Attributes (if any) are serialized after the
    vector data.
5.  Everything else → returned as-is (Tier 1 pass-through).

Each creation path returns the ALTREP result via `sora_make_result`,
which chains the host extptr (responsible for `shm_unlink`) into the
ALTREP’s external pointer hierarchy. SHM lifetime is thus tied to the R
object — when the ALTREP is garbage collected, both munmap and unlink
happen automatically.

### ALTREP Serialization Hooks

All ALTREP classes register `Serialized_state` and `Unserialize`
methods.

- **Standalone shared objects** (created by
  [`share()`](https://shikokuchuo.net/sora/reference/share.md) or
  [`map_shared()`](https://shikokuchuo.net/sora/reference/map_shared.md))
  serialize as just the SHM name string (~30 bytes). On unserialize,
  `sora_Unserialize` validates the name via `sora_is_shm_name()` (checks
  `/sora_` prefix on POSIX, `Local\sora_` on Windows), then
  `sora_shm_open_and_wrap` opens the SHM and creates a fresh ALTREP
  wrapper. R’s ALTREP serialization framework separately serializes and
  restores the object’s attributes.
- **Element vectors from ALTLIST** serialize as
  `list(parent_name, index)`. On unserialize, `sora_Unserialize`
  validates element types (first element is STRSXP, second is INTSXP)
  before treating as an element reference, then `sora_open_element`
  opens the parent SHM and extracts the element by index (including
  restoring per-element attributes from the directory’s `attrs_size`
  field). The element’s `sora_vec`/`sora_str` struct stores an `index`
  field (int32_t, -1 for standalone, \>= 0 for ALTLIST elements).

Fallback to full materialization when: - COW-materialized vectors (data2
is set) - SHM mapping already closed (extptr cleared)

### SHM Magic Bytes

The first 4 bytes of each SHM region identify the layout: - `0x534F524C`
(“SORL”): ALTLIST — contains element directory + per-element data -
`0x534F5248` (“SORH”): ALTREP vector — 64-byte header + data + optional
serialized attributes - `0x534F5253` (“SORS”): ALTSTRING — 24-byte
header + offset table + packed strings + optional serialized attributes

### SHM Region Layouts

**Atomic vector (SORH):** 64-byte header, data starts at byte 64
(64-byte aligned for SIMD). Serialized attributes (if any) follow the
vector data.

| Offset               | Size | Field                                                        |
|----------------------|------|--------------------------------------------------------------|
| 0                    | 4    | magic (`0x534F5248`)                                         |
| 4                    | 4    | sexptype (REALSXP, INTSXP, etc.)                             |
| 8                    | 8    | length (int64)                                               |
| 16                   | 8    | attrs_size (int64, 0 if no attributes)                       |
| 24                   | 40   | reserved (zero)                                              |
| 64+                  |      | raw vector data                                              |
| 64 + length×elt_size |      | serialized attributes (attrs_size bytes, if attrs_size \> 0) |

All attributes (names, dim, class, levels, tzone, etc.) are serialized
as a pairlist into the trailing attrs section. On the consumer,
`sora_restore_attrs` unserializes them and sets via `SET_ATTRIB`.

**ALTLIST (SORL):** Header + element directory + per-element data
regions.

| Offset | Size | Field                                           |
|--------|------|-------------------------------------------------|
| 0      | 4    | magic (`0x534F524C`)                            |
| 4      | 4    | n_elements (int32)                              |
| 8      | 8    | attrs_offset (int64)                            |
| 16     | 8    | attrs_size (int64)                              |
| 24     | 32×n | element directory                               |
| varies |      | element data (64-byte aligned)                  |
| varies |      | serialized attributes (names, class, row.names) |

Each element directory entry (32 bytes):
`data_offset(8) + data_size(8) + sexptype(4) + attrs_size(4) + length(8)`.
`sexptype != 0` means Tier 2 (raw data or string); `sexptype == 0` means
Tier 1 (serialized bytes). `sexptype == STRSXP` indicates a string
element whose data region contains an offset table + packed strings.
When `attrs_size > 0`, the element’s serialized attributes are appended
after the raw data within the data region (at offset
`data_offset + data_size - attrs_size`). On the consumer,
`sora_restore_attrs` unserializes and applies them.

**String vector (SORS):** 24-byte header + offset table + packed string
data + optional serialized attributes.

| Offset             | Size | Field                                                              |
|--------------------|------|--------------------------------------------------------------------|
| 0                  | 4    | magic (`0x534F5253`)                                               |
| 4                  | 4    | attrs_size (int32, 0 if no attributes)                             |
| 8                  | 8    | n_strings (int64)                                                  |
| 16                 | 8    | str_data_size (int64, total size of offset table + packed strings) |
| 24                 | 16×n | offset table                                                       |
| 64-aligned         |      | packed string bytes                                                |
| 24 + str_data_size |      | serialized attributes (attrs_size bytes, if attrs_size \> 0)       |

Each offset table entry (16 bytes):
`str_offset(int64) + str_length(int32) + str_encoding(int32)`.
`str_offset` is relative to the start of the packed string area.
`str_length < 0` means `NA_STRING`. `str_encoding` is a `cetype_t` value
(0=native, 1=UTF-8, 2=Latin-1, 3=bytes). The same layout is used for
string elements within ALTLIST regions (offset table starts at the
element’s `data_offset`; element-level attrs use the directory entry’s
`attrs_size` field).

### ALTREP Classes (registered in `sora_altrep_init`)

| Class          | R type           | Backing                                                                                                    |
|----------------|------------------|------------------------------------------------------------------------------------------------------------|
| `sora_list`    | ALTLIST (VECSXP) | SHM with element directory; lazy per-element access via `Elt`                                              |
| `sora_real`    | REALSXP          | SHM data pointer via `sora_vec`                                                                            |
| `sora_integer` | INTSXP           | Same pattern                                                                                               |
| `sora_logical` | LGLSXP           | Same pattern                                                                                               |
| `sora_raw`     | RAWSXP           | Same pattern                                                                                               |
| `sora_complex` | CPLXSXP          | Same pattern                                                                                               |
| `sora_string`  | STRSXP           | SHM offset table + packed strings via `sora_str`; lazy per-element access via `Elt` using `Rf_mkCharLenCE` |

## Internal State

- **`sora_tag`** (C global, `altrep.c`): Interned symbol
  `Rf_install("sora")`. Serves two roles: (1) tag on every SHM extptr
  for
  [`is_shared()`](https://shikokuchuo.net/sora/reference/is_shared.md)
  identification, (2) ALTLIST cache sentinel to distinguish “not yet
  accessed” from a cached `NULL` element.

### GC and Lifetime

SHM lifetime is fully automatic, managed by chaining the host extptr
(responsible for `shm_unlink`/`CloseHandle`) into the ALTREP object’s
external pointer hierarchy.

- **Host side (`sora`)**: `sora_shm_create` allocates the SHM region and
  mmaps it; on POSIX, the fd is closed immediately after mmap (the
  mapping stays valid; the `sora_shm` struct does not retain the fd).
  `sora_make_result` splits ownership: the ALTREP wrapper gets the
  mapping (`addr`, `size`) via the daemon-side SHM extptr
  (`sora_shm_finalizer` → munmap), and the host extptr gets the handle
  (`name` for POSIX unlink, `HANDLE` for Windows) via
  `sora_host_finalizer`. The host extptr is chained as the protected
  value of the ALTREP’s SHM extptr. When the ALTREP is garbage
  collected, both finalizers run: munmap + unlink.
  `R_RegisterCFinalizerEx(..., TRUE)` ensures cleanup on session exit.
- **Consumer side (`map_shared`/unserialize)**: `sora_shm_open` maps the
  region read-only (size discovered via `fstat`/`VirtualQuery`); on
  POSIX, the fd is closed immediately after mmap. Each ALTREP wrapper
  holds an external pointer (`sora_shm_finalizer`) that calls `munmap`
  (never unlink). ALTLIST element vectors hold a `sora_vec` or
  `sora_str` extptr whose protected value references the parent SHM
  extptr, keeping the mapping alive. Element vectors store their index
  (`int32_t`) in the `sora_vec`/`sora_str` struct for compact
  serialization.
- **Element lifetime**: Element vectors extracted from an ALTLIST keep
  the parent SHM alive through the extptr chain (element → parent SHM
  extptr → host extptr). Even if the parent ALTLIST is garbage
  collected, the SHM survives as long as any element is referenced.

## Code Organization

### src/ Directory

- **sora.h**: Types (`sora_shm`, `sora_buf`, `sora_vec` with `index`
  field), function declarations, `SORA_ALIGN64` macro
- **shm.c**: Platform-abstracted SHM create/open/close. On Linux, uses
  `open("/dev/shm/...")` directly to avoid `-lrt` link dependency. On
  macOS, uses `shm_open`/`shm_unlink` (in libc). Finalizers for both
  host and daemon side.
- **serialize.c**: Counting pass (`sora_serialize_count`), fixed-buffer
  write (`sora_serialize_into`), unserialize-from-buffer
  (`sora_unserialize_from`), `sora_sizeof_elt`
- **altrep.c**: All ALTREP class definitions and methods,
  `sora_make_vector`/`sora_make_string` helpers, `sora_unwrap_element`
  helper (shared element extraction for `sora_list_Elt` and
  `sora_open_element`), `sora_restore_attrs` helper, `sora_is_shm_name`
  validator, unified creation dispatcher (`sora_create`), static
  per-type creation functions, consumer-side open+wrap dispatch
  (`sora_shm_open_and_wrap`), element open (`sora_open_element`),
  identity check (`sora_is_shared`), name extraction (`sora_shm_name`),
  serialization hooks (`Serialized_state`/`Unserialize`),
  `sora_altrep_init`
- **init.c**: `R_init_sora`, `.Call` registration table (4 entry points:
  `sora_create`, `sora_shm_open_and_wrap`, `sora_is_shared`,
  `sora_shm_name`)

`.Call` names match their C function names. All entry points take a
single `SEXP` argument.

### R/ Directory

- **sora-package.R**: Package docs
- **sora.R**: All four exported functions are single `.Call` wrappers —
  dispatch and error handling are at the C level.
  [`share()`](https://shikokuchuo.net/sora/reference/share.md) →
  `sora_create`,
  [`map_shared()`](https://shikokuchuo.net/sora/reference/map_shared.md)
  → `sora_shm_open_and_wrap`,
  [`shared_name()`](https://shikokuchuo.net/sora/reference/shared_name.md)
  → `sora_shm_name`,
  [`is_shared()`](https://shikokuchuo.net/sora/reference/is_shared.md) →
  `sora_is_shared`.

## Testing

Uses **minitest**, a minimal framework defined at the top of
`tests/tests.R`: - `test_true()`, `test_false()`, `test_null()`,
`test_equal()`, `test_identical()`, `test_class()`, `test_error()`

All tests run sequentially in a single file. Integration tests (with
mirai daemons) are gated behind `NOT_CRAN=true`.

## Platform Notes

- **Linux**: SHM lives in `/dev/shm/` (tmpfs). Accessed via
  [`open()`](https://rdrr.io/r/base/connections.html)/[`unlink()`](https://rdrr.io/r/base/unlink.html)
  directly, avoiding the `-lrt` link dependency that `shm_open`
  requires. `MAP_POPULATE` pre-faults pages.
- **macOS**: Kernel-backed SHM via `shm_open`/`shm_unlink` (in libc, no
  extra link flags). `MAP_POPULATE` is a no-op (defined to 0).
- **Windows**: Page-file-backed via
  `CreateFileMappingA`/`MapViewOfFile`. `kernel32` is always available.
  Host must keep the mapping handle alive until consumers have opened it
  (the GC-chained host extptr handles this automatically).

## Package Conventions

- roxygen2 with markdown support; NAMESPACE is auto-generated
- MIT license
- `CLAUDE.md` and `.claude/` are excluded from package builds (via
  `.Rbuildignore`)
