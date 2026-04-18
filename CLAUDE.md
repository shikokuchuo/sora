# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

mori — Shared Memory for R Objects. Uses POSIX shared memory (Linux, macOS) and Win32 file mappings (Windows) with R's ALTREP framework to let multiple processes on the same machine read the same physical memory pages. No external dependencies. Requires R >= 4.3.0 (for ALTLIST). API: `share()` → ALTREP shared object, `map_shared()` → open SHM by name, `shared_name()` → extract SHM name, `is_shared()` → test if shared. SHM lifetime is automatic — managed by R's garbage collector via chained external pointer finalizers. ALTREP serialization hooks serialize standalone shared objects as the SHM name (~30 bytes) and nested ALTLIST elements (including sub-lists) as `(parent_name, integer_path)`, enabling transparent use with mirai and any R serialization path. Nested VECSXP elements are zero-copy: each level is an inline self-describing MORL region inside the parent SHM.

## Development Commands

### Testing

```r
# Run the full testthat suite
devtools::test()
```

### Building and Checking

```bash
# Build package
R CMD build .

# Check package
R CMD check --no-manual mori_*.tar.gz
```

```r
# Generate documentation from roxygen2 comments
devtools::document()
```

## Key Architecture

### Storage Model

- **Zero-copy (SHM-backed)**: All atomic vectors (including character vectors and those with arbitrary attributes such as names, class, levels, dim) and data frame columns are written directly into SHM and backed by ALTREP on consumers. Attributes are serialized into a trailing section of the SHM region and restored via `SET_ATTRIB` on the consumer. For numeric types, `Dataptr_or_null` returns the SHM pointer for reads; `Dataptr(writable=TRUE)` materializes a private copy (COW). For character vectors, `Elt` lazily creates each CHARSXP via `Rf_mkCharLenCE` from the SHM data; `Dataptr_or_null` returns NULL to force element-by-element access.
- **Nested lists (zero-copy)**: VECSXP/LISTSXP elements of a shared list are written inline as a complete child MORL region at their `data_offset`. Each nested region is self-describing (magic bytes + header) and recursively contains its own directory, element data, and attributes. Consumers wrap each level in its own ALTLIST via a lightweight `mori_list_view` (pointer + bounds + index) rather than a full `mori_shm`. Sub-lists therefore stay zero-copy all the way down and report `is_shared(x) == TRUE`; `shared_name()` returns the root SHM name for root lists and `""` for sub-lists.
- **Pass-through**: All other R objects (environments, closures, language objects) are returned unchanged by `share()`. No SHM is created.

### share() Dispatch Logic (altrep.c: `mori_create`)

All R exported functions are single `.Call` wrappers. `share()` calls `mori_create` which dispatches on `TYPEOF(x)`:

0. If `x` is already a mori-backed ALTREP (detected via the `mori_owned_tag` on `R_altrep_data1(x)`), return `x` unchanged. This makes `share()` idempotent and also short-circuits re-sharing of sub-list views and element vectors extracted from an ALTLIST.
1. `NILSXP` → returned as-is (falls through all checks).
2. `VECSXP`/`LISTSXP` → `mori_shm_create_list_call` — ALTLIST with per-element directory. Writing is split into two passes: `mori_nested_size` computes the total SHM size (recursing into nested VECSXP/LISTSXP elements), and `mori_nested_write` produces a complete MORL region at the target base pointer, recursing into child VECSXP/LISTSXP elements by calling itself on `base + child_data_offset`. Each element is independently zero-copy (any atomic, with or without attributes), serialized as bytes, or a nested MORL region. Data frames and pairlists go through this path (pairlists are coerced to VECSXP via `Rf_coerceVector`, both at the top level and at each level of the recursion).
3. `STRSXP` → `mori_shm_create_string_call` — ALTSTRING backed by SHM with offset table + packed string data. Attributes (if any) are serialized after the string data.
4. Other SHM-eligible types (`REALSXP`, `INTSXP`, `LGLSXP`, `RAWSXP`, `CPLXSXP`) → `mori_shm_create_vector_call` — single ALTREP vector backed by SHM. Attributes (if any) are serialized after the vector data.
5. Everything else → returned as-is (pass-through).

Each creation path returns the ALTREP result via `mori_make_result`, which chains the host extptr (responsible for `shm_unlink`) into the ALTREP's external pointer hierarchy. SHM lifetime is thus tied to the R object — when the ALTREP is garbage collected, both munmap and unlink happen automatically.

### ALTREP Serialization Hooks

All ALTREP classes register `Serialized_state` and `Unserialize` methods.

- **Standalone shared objects** (created by `share()` or `map_shared()`) serialize as just the SHM name string (~30 bytes). On unserialize, `mori_Unserialize` validates the name via `mori_is_shm_name()` (checks `/mori_` prefix on POSIX, `Local\mori_` on Windows), then `mori_shm_open_and_wrap` opens the SHM and creates a fresh ALTREP wrapper. R's ALTREP serialization framework separately serializes and restores the object's attributes.
- **Element vectors and sub-lists from ALTLIST** serialize as `list(parent_name, int_path)` where `int_path` is an INTSXP of length >= 1 giving top-down indices from the root region to the leaf. Length 1 is wire-identical to the previous scalar `(name, index)` form, so existing serialized blobs remain readable. On unserialize, `mori_Unserialize` validates element types (first element is STRSXP, second is INTSXP of length >= 1) and that the name looks like a mori SHM name before calling `mori_open_path`, which opens the root SHM, walks each intermediate VECSXP directory entry via `mori_make_list_view` (bounds-checking the child region against its parent), and finally calls `mori_unwrap_element` at the leaf. The element's `mori_vec`/`mori_str` struct stores an `index` field (int32_t, -1 for standalone, >= 0 for ALTLIST elements), and each `mori_list_view` likewise stores an `index` (-1 for a root view, >= 0 for a sub-list) — these indices are what `mori_build_path` collects while walking the keeper chain up through view extptrs at serialize time.

Fallback to full materialization when:
- COW-materialized vectors (data2 is set) — returns the materialized copy
- Nesting depth exceeds `MORI_MAX_PATH` (64) in `mori_build_path`

### SHM Magic Bytes

The first 4 bytes of each SHM region identify the layout:
- `0x4D4F524C` ("MORL"): ALTLIST — contains element directory + per-element data
- `0x4D4F5248` ("MORH"): ALTREP vector — 64-byte header + data + optional serialized attributes
- `0x4D4F5253` ("MORS"): ALTSTRING — 24-byte header + offset table + packed strings + optional serialized attributes

### SHM Region Layouts

**Atomic vector (MORH):** 64-byte header, data starts at byte 64 (64-byte aligned for SIMD). Serialized attributes (if any) follow the vector data.

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic (`0x4D4F5248`) |
| 4 | 4 | sexptype (REALSXP, INTSXP, etc.) |
| 8 | 8 | length (int64) |
| 16 | 8 | attrs_size (int64, 0 if no attributes) |
| 24 | 40 | reserved (zero) |
| 64+ | | raw vector data |
| 64 + length×elt_size | | serialized attributes (attrs_size bytes, if attrs_size > 0) |

All attributes (names, dim, class, levels, tzone, etc.) are serialized as a pairlist into the trailing attrs section. On the consumer, `mori_restore_attrs` unserializes them and sets via `SET_ATTRIB`.

**ALTLIST (MORL):** Header + element directory + per-element data regions.

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic (`0x4D4F524C`) |
| 4 | 4 | n_elements (int32) |
| 8 | 8 | attrs_offset (int64) |
| 16 | 8 | attrs_size (int64) |
| 24 | 32×n | element directory |
| varies | | element data (64-byte aligned) |
| varies | | serialized attributes (names, class, row.names) |

Each element directory entry (32 bytes): `data_offset(8) + data_size(8) + sexptype(4) + attrs_size(4) + length(8)`. `sexptype != 0` means zero-copy (raw data, string, or nested list); `sexptype == 0` means serialized bytes. `sexptype == STRSXP` indicates a string element whose data region contains an offset table + packed strings. `sexptype == VECSXP` indicates a nested MORL region inline at `data_offset` of size `data_size` (the child's own header, directory, elements, and attrs all live inside that region); the parent directory entry's `attrs_size` is always 0 for VECSXP children — any attributes are part of the child region's own trailing attrs. For non-nested zero-copy entries with `attrs_size > 0`, the element's serialized attributes are appended after the raw data within the data region (at offset `data_offset + data_size - attrs_size`). On the consumer, `mori_restore_attrs` unserializes and applies them.

**String vector (MORS):** 24-byte header + offset table + packed string data + optional serialized attributes.

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic (`0x4D4F5253`) |
| 4 | 4 | attrs_size (int32, 0 if no attributes) |
| 8 | 8 | n_strings (int64) |
| 16 | 8 | str_data_size (int64, total size of offset table + packed strings) |
| 24 | 16×n | offset table |
| 64-aligned | | packed string bytes |
| 24 + str_data_size | | serialized attributes (attrs_size bytes, if attrs_size > 0) |

Each offset table entry (16 bytes): `str_offset(int64) + str_length(int32) + str_encoding(int32)`. `str_offset` is relative to the start of the packed string area. `str_length < 0` means `NA_STRING`. `str_encoding` is a `cetype_t` value (0=native, 1=UTF-8, 2=Latin-1, 3=bytes). The same layout is used for string elements within ALTLIST regions (offset table starts at the element's `data_offset`; element-level attrs use the directory entry's `attrs_size` field).

### ALTREP Classes (registered in `mori_altrep_init`)

| Class | R type | Backing |
|-------|--------|---------|
| `mori_list` | ALTLIST (VECSXP) | SHM with element directory; lazy per-element access via `Elt` |
| `mori_real` | REALSXP | SHM data pointer via `mori_vec` |
| `mori_integer` | INTSXP | Same pattern |
| `mori_logical` | LGLSXP | Same pattern |
| `mori_raw` | RAWSXP | Same pattern |
| `mori_complex` | CPLXSXP | Same pattern |
| `mori_string` | STRSXP | SHM offset table + packed strings via `mori_str`; lazy per-element access via `Elt` using `Rf_mkCharLenCE` |

## Internal State

- **`mori_tag`** (C global, `altrep.c`): Interned symbol `Rf_install("mori")`. Role: ALTLIST cache sentinel only (a VECSXP element marker, never an extptr tag) — distinguishes "not yet accessed" from a cached `NULL` element.
- **`mori_shm_tag`** (C global, `altrep.c`): Interned symbol `Rf_install("mori_shm")`. Tag on daemon-side SHM mapping extptrs; addr is a `mori_shm *`; finalizer calls `munmap`.
- **`mori_host_tag`** (C global, `altrep.c`): Interned symbol `Rf_install("mori_host")`. Tag on host-only unlink extptrs (created by `mori_make_result`); addr is a `mori_shm *`; finalizer calls `shm_unlink`/`CloseHandle`. Sits above the shm terminus in the keeper chain.
- **`mori_owned_tag`** (C global, `altrep.c`): Interned symbol `Rf_install("mori_owned")`. Tag on every mori ALTREP's `data1` extptr (view, vec, str — root / sub-list / standalone / element). The addr type is recovered from `TYPEOF(x)` at the ALTREP boundary: `VECSXP → mori_list_view *`, `STRSXP → mori_str *`, else `mori_vec *`. Standalone-vs-element is carried in each struct's `index` field (`-1` standalone/root, `>= 0` element/sub-list) and redundantly recoverable from the tag on `data1.prot` (`shm_tag → standalone`, `owned_tag → element/sub-list`). `is_shared()` is a single pointer comparison against this tag; `mori_shm_name()` adds one `prot` hop and returns the name only when `prot`'s tag is `shm_tag`.

### GC and Lifetime

SHM lifetime is fully automatic, managed by chaining the host extptr (responsible for `shm_unlink`/`CloseHandle`) into the ALTREP object's external pointer hierarchy.

- **Host side (`share`)**: `mori_shm_create` allocates the SHM region and mmaps it; on POSIX, the fd is closed immediately after mmap (the mapping stays valid; the `mori_shm` struct does not retain the fd). `mori_make_result` splits ownership: the ALTREP wrapper gets the mapping (`addr`, `size`) via the daemon-side SHM extptr (tag `mori_shm_tag`, finalizer `mori_shm_finalizer` → munmap), and the host extptr gets the handle (tag `mori_host_tag`, addr is a `mori_shm *` holding `name` for POSIX unlink, `HANDLE` for Windows) via `mori_host_finalizer`. The host extptr is chained as the protected value of the ALTREP's SHM extptr. When the ALTREP is garbage collected, both finalizers run: munmap + unlink. `R_RegisterCFinalizerEx(..., TRUE)` ensures cleanup on session exit.
- **Consumer side (`map_shared`/unserialize)**: `mori_shm_open` maps the region read-only (size discovered via `fstat`/`VirtualQuery`); on POSIX, the fd is closed immediately after mmap. The shm extptr (tag `mori_shm_tag`) holds the `mori_shm *` and its `mori_shm_finalizer` calls `munmap` (never unlink). For lists, the shm extptr's immediate child is a root view extptr (tag `mori_owned_tag`, addr `mori_list_view *`, index=-1) that becomes ALTREP data1; sub-list views (index >= 0) chain through their parent view extptr in the same way. ALTLIST element vectors hold a `mori_vec` or `mori_str` extptr (also tagged `mori_owned_tag`) whose protected value references the immediate parent view extptr (or the shm extptr, for vec/str at the root of the MORH/MORS regions). The SHM name is stored only at `shm->name`: `shared_name()` reads it via `data1.prot → shm_ptr.addr->name` when `data1.prot`'s tag is `mori_shm_tag`, and returns `""` otherwise (element / sub-list).
- **Element lifetime**: Element vectors and sub-lists extracted from an ALTLIST keep the root SHM alive through the keeper chain (leaf element → parent view(s) → root shm extptr → host extptr). Every hop in the chain carries one of `mori_owned_tag` (views/vec/str leaves), `mori_shm_tag` (terminus), or `mori_host_tag` (above the terminus, host side only). Even if the enclosing ALTLIST(s) are garbage collected, every `R_MakeExternalPtr(addr, tag, prot)` keeps its `prot` slot alive, so the chain survives as long as any descendant is referenced. `mori_build_path` is the only keeper-chain walker: it walks `mori_owned_tag` hops (always `mori_list_view *` since vec/str extptrs are only ever leaves), collecting view indices (skipping the root view's -1) until it reaches the shm extptr and reads the name.

## Code Organization

### src/ Directory

- **mori.h**: Types (`mori_shm`, `mori_buf`, `mori_vec` with `index` field, `mori_list_view`), function declarations, `MORI_ALIGN64` macro
- **shm.c**: Platform-abstracted SHM create/open/close. On Linux, uses `open("/dev/shm/...")` directly to avoid `-lrt` link dependency. On macOS, uses `shm_open`/`shm_unlink` (in libc). Finalizers for both host and daemon side.
- **serialize.c**: Counting pass (`mori_serialize_count`), fixed-buffer write (`mori_serialize_into`), unserialize-from-buffer (`mori_unserialize_from`), `mori_sizeof_elt`
- **altrep.c**: All ALTREP class definitions and methods, `mori_make_vector`/`mori_make_string`/`mori_make_list_view` helpers, `mori_unwrap_element` helper (shared element extraction for `mori_list_Elt` and `mori_open_path`; validates dir bounds against the enclosing region), `mori_restore_attrs` helper, `mori_is_shm_name` validator, unified creation dispatcher (`mori_create`), recursive writer helpers `mori_nested_size` / `mori_nested_write`, consumer-side open+wrap dispatch (`mori_shm_open_and_wrap`), path open (`mori_open_path`), identity check (`mori_is_shared`), name extraction (`mori_shm_name`), serialization hooks (`Serialized_state`/`Unserialize`), `mori_build_path` (keeper-chain walker), `mori_altrep_init`
- **init.c**: `R_init_mori`, `.Call` registration table (4 entry points: `mori_create`, `mori_shm_open_and_wrap`, `mori_is_shared`, `mori_shm_name`)

`.Call` names match their C function names. All entry points take a single `SEXP` argument.

### R/ Directory

- **mori-package.R**: Package docs
- **share.R**: All four exported functions are single `.Call` wrappers — dispatch and error handling are at the C level. `share()` → `mori_create`, `map_shared()` → `mori_shm_open_and_wrap`, `shared_name()` → `mori_shm_name`, `is_shared()` → `mori_is_shared`.

## Testing

Uses **testthat** (edition 3). The entry point is `tests/testthat.R`; individual tests live in `tests/testthat/test-*.R`, grouped by topic:

- `test-passthrough.R` — pass-through (formulas, NULL, closures/environments inside lists)
- `test-vectors.R` — atomic vector round-trip (double, integer, logical, raw, complex, matrix, empty)
- `test-strings.R` — ALTSTRING behaviour (basic, NA, empty, UTF-8, large, data frame column, list element, `Dataptr` materialization via `make.unique`/`duplicated`)
- `test-list.R` — ALTLIST (data frames, mixed lists, NULL elements, duplicate, pairlist)
- `test-nested.R` — nested VECSXP elements (sub-lists, list-columns, deep structures, path-based serialization, sub-list COW, GC with nested references)
- `test-attributes.R` — attribute preservation (names, factor, Date, POSIXct, matrix)
- `test-cow.R` — copy-on-write for numeric and string vectors, `Dataptr_or_null` on materialized vec
- `test-serialization.R` — ALTREP `Serialized_state`/`Unserialize` hooks (standalone, element, compact size, COW fallback, attributes)
- `test-gc.R` — automatic SHM cleanup by garbage collector
- `test-api.R` — `share()`/`map_shared()`/`shared_name()`/`is_shared()` behaviour including invalid inputs

All tests run unconditionally; nothing is gated behind `skip_on_cran` or environment variables.

## Platform Notes

- **Linux**: SHM lives in `/dev/shm/` (tmpfs). Accessed via `open()`/`unlink()` directly, avoiding the `-lrt` link dependency that `shm_open` requires. `MAP_POPULATE` pre-faults pages.
- **macOS**: Kernel-backed SHM via `shm_open`/`shm_unlink` (in libc, no extra link flags). `MAP_POPULATE` is a no-op (defined to 0).
- **Windows**: Page-file-backed via `CreateFileMappingA`/`MapViewOfFile`. `kernel32` is always available. Host must keep the mapping handle alive until consumers have opened it (the GC-chained host extptr handles this automatically).

## Package Conventions

- roxygen2 with markdown support; NAMESPACE is auto-generated
- MIT license
- `CLAUDE.md` and `.claude/` are excluded from package builds (via `.Rbuildignore`)
