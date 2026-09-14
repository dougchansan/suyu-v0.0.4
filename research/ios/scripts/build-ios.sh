#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
[[ $(uname -s) == Darwin ]] || { echo 'Run on the Mac with Xcode.' >&2; exit 2; }
HERE=$(cd "$(dirname "$0")/.." && pwd)
SDK=${1:-iphoneos}
case "$SDK" in iphoneos|iphonesimulator) ;; *) echo 'Expected iphoneos or iphonesimulator' >&2; exit 2;; esac
xcrun --sdk "$SDK" --show-sdk-path >/dev/null
BUILD=${SWITCH_AOT_BUILD_DIR:-"$HERE/build-$SDK"}
args=(-S "$HERE" -B "$BUILD" -G Xcode -DCMAKE_SYSTEM_NAME=iOS
      "-DCMAKE_OSX_SYSROOT=$SDK" -DCMAKE_OSX_ARCHITECTURES=arm64
      "-DCMAKE_OSX_DEPLOYMENT_TARGET=${SWITCH_AOT_MIN_IOS:-18.0}"
      -DSUYU_NO_JIT=ON -DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO)
args+=("-DSWITCH_AOT_EXEFS=${SWITCH_AOT_EXEFS:-}")
# Stable iHorizon identity; signing overrides remain local.
args+=("-DSWITCH_AOT_BUNDLE_ID=${SWITCH_AOT_BUNDLE_ID:-org.ihorizon.app}")
if [[ -n ${SWITCH_AOT_TEAM_ID:-} ]]; then
  args+=("-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=$SWITCH_AOT_TEAM_ID"
         -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=YES)
else
  args+=(-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO)
fi
cmake "${args[@]}"
cmake --build "$BUILD" --config Release --target iHorizon --parallel "${SWITCH_AOT_JOBS:-2}"
echo 'Diagnostic app built. This is NOT a completed Suyu runtime port.'
echo 'Use the generated Xcode project for local signing, paired-device selection and installation.'
