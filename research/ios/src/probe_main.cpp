// SPDX-License-Identifier: GPL-2.0-or-later
#include "research_host.h"
#include <iostream>
int main() {
    const int result = switch_aot_self_test();
    std::cout << switch_aot_report() << '\n';
    // Private mode is a successful LINK/DESCRIPTOR probe, not game execution.
    return result == 2 ? 0 : result;
}
