// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Single caller, on a background thread. Read the report only after completion.
// Returns zero only after construction, initialization and destruction pass.
int ihorizon_core_initialize_probe(const char* progress_path);
const char* ihorizon_core_report(void);

#ifdef __cplusplus
}
#endif
