// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Returns zero only for a passing synthetic test. A private export is never
// executed by this diagnostic host; it returns 2 (integration pending).
int switch_aot_self_test(void);
const char* switch_aot_report(void);
#ifdef __cplusplus
}
#endif
