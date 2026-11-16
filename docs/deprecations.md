---
title: Deprecations
parent: Reference
nav_order: 14
lede: >-
  Every name deprecated in 2.x, what replaces it, and when it goes.
permalink: /reference/deprecations/
---
libxtc 2.2 renames part of its API so that one verb has one meaning
(the conventions in [ABI stability]({{ '/reference/abi-stability/' |
relative_url }})).  Each old name below still works through the rest of
2.x and is **removed in 3.0**.  Moving a program to the new names is a
search and replace, except where the table says the arguments changed.

How an old name keeps working:

- **A function** stays an exported symbol in its original version node
  (`XTC_2.0`), so a binary built against 2.1 links and runs against 2.2
  with no rebuild.  It is a thin wrapper that calls the new function,
  and its declaration carries `XTC_DEPRECATED("use xtc_new_name")`, so a
  rebuild warns (`-Wdeprecated-declarations` on gcc and clang, C4996 on
  MSVC).  The new name is a new symbol in `XTC_2.2`.
- **A type name** is a `typedef` of the new one, marked deprecated where
  the compiler can mark a typedef (gcc, clang, MSVC).
- **An enumerator** is an alias of the new one with the same value,
  marked `XTC_DEPRECATED_ENUM` (gcc and clang; MSVC accepts it silently).
- **A macro** is a `#define` of the new one, marked
  `XTC_DEPRECATED_MACRO` (clang only: no other compiler can mark a
  macro).
- **Frozen** names (the lock layer: `docs/abi-stability.md`, "Frozen
  surfaces") get the new spelling as an alias and keep the old one with
  no warning.  3.0 makes the alias the name.

`XTC_NO_DEPRECATION_WARNINGS`, defined before the first libxtc include,
silences every one of these marks while a large codebase migrates.
`test/abi/test_deprecated_bridge.c` calls each old name once to prove it
still works.

**Destructors.**  A new `*_destroy` returns `void`, and `NULL` is a
no-op: a destructor cannot fail, so there is nothing to check.  The old
`int`-returning `*_fini` keeps its contract exactly, including
`XTC_E_INVAL` for `NULL` and `XTC_OK` otherwise, so code that tests its
result keeps working until it moves to the new name.

## Renamed in 2.2 (removed in 3.0)

<!-- renames:begin -->
| Old (deprecated in 2.2) | New (2.2) | Kind | Notes |
|---|---|---|---|
<!-- renames:end -->

## Planned for 3.0 (not done in 2.x)

- `xtc_arwlock_*` / `xtc_arwlock_t` (the fiber-parking reader/writer
  lock) becomes `xtc_rwlock_*` / `xtc_rwlock_t`, reusing the name the
  blocking OS-thread lock gave up in 2.2 (it is `xtc_thrd_rwlock_*`
  now).  3.0 removes the deprecated `xtc_rwlock_*` bridges first, so a
  2.x program that still calls them fails to build against 3.0 instead
  of silently changing from a thread lock to a fiber lock.
- The frozen lock-layer aliases (`XTC_LWLOCK_*`, `xtc_lwlock_try_acquire`)
  become the only names.
