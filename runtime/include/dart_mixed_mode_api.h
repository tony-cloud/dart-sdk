/*
 * Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
 * for details. All rights reserved. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file.
 */

#ifndef RUNTIME_INCLUDE_DART_MIXED_MODE_API_H_
#define RUNTIME_INCLUDE_DART_MIXED_MODE_API_H_

/*
 * Mixed-mode AOT patches (runtime built with GN dart_mixed_mode = true; see
 * runtime/vm/mixed_mode.h and docs/ios-linker/RUNTIME.md). A patch is a
 * complete AOT snapshot whose text is mapped without execute permission:
 * entries linked to the release run from the release's text, the rest runs
 * in the ARM64 simulator.
 *
 * Embedder sequence, all before the isolate group is created:
 *   1. Map the release normally and the patch ELF with read-only text.
 *   2. Dart_MixedModeRegisterImages(patch isolate text, release isolate text,
 *      link records from the vmcode container).
 *   3. Dart_MixedModeLinkSnapshot(patch isolate data, patch isolate text).
 *   4. On any failure, call Dart_MixedModeDeactivate and run the release.
 * Only the isolate snapshot comes from the patch; the VM snapshot stays the
 * release's.
 */

#include "dart_api.h" /* NOLINT */

/* One vmcode v1 link record (docs/ios-linker/COMPILER_LINKER.md, section 6):
 * offsets relative to each instructions image, size of the payload. */
typedef struct {
  uint32_t patch_offset;
  uint32_t release_offset;
  uint32_t size;
  uint32_t flags; /* Must be 0. */
} Dart_MixedLink;

/* Registers the patch and release instructions images and the link records
 * (link_records may be NULL to link by identity, for testing). Validates the
 * records structurally. Returns false and sets *error (static) on failure. */
DART_EXPORT bool Dart_MixedModeRegisterImages(const uint8_t* patch_text,
                                              const uint8_t* release_text,
                                              const Dart_MixedLink* links,
                                              intptr_t link_count,
                                              const char** error);

/* Links the registered patch: maps every record to a patch code entry and
 * verifies it against the release code, and checks the gate budget. Needs
 * no isolate. Returns false and sets *error (static) on failure; the
 * patch is then deregistered. */
DART_EXPORT bool Dart_MixedModeLinkSnapshot(const uint8_t* snapshot_data,
                                            const uint8_t* snapshot_instructions,
                                            const char** error);

/* Forgets the patch (before any isolate used it). */
DART_EXPORT void Dart_MixedModeDeactivate(void);

/* Prints transition statistics when DART_MIXED_VERBOSE is set. */
DART_EXPORT void Dart_MixedModePrintStats(void);

#endif /* RUNTIME_INCLUDE_DART_MIXED_MODE_API_H_ */
