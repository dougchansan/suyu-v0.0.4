// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace SwitchAOT {
bool InstallStaticImages();
// Reports the loader-owned pre-publication barrier result; never binds lazily.
bool FinalizeStaticImages();
const char* StaticImageError();
void ClearStaticImages();
}
