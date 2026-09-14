// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace SwitchAOT {
bool InstallStaticImages();
bool FinalizeStaticImages();
const char* StaticImageError();
void ClearStaticImages();
}
