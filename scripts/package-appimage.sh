#!/bin/bash
set -ex

# 1. Install build targets into AppDir
DESTDIR="$PWD/AppDir" ninja -C build install
rm -f AppDir/usr/bin/suyu-cmd AppDir/usr/bin/suyu-tester 2>/dev/null || true

# 2. Setup AppDir root metadata
cp dist/dev.suyu_emu.suyu.desktop AppDir/
cp dist/suyu.svg AppDir/dev.suyu_emu.suyu.svg
cp dist/suyu.svg AppDir/suyu.svg
cp dist/suyu.svg AppDir/.DirIcon

# 3. Download official AppImage packaging tools
curl -sLO https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
curl -sLO https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
curl -sLO https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
chmod +x *.AppImage

# Extract them to run without FUSE in CI
./linuxdeploy-x86_64.AppImage --appimage-extract
mv squashfs-root linuxdeploy-bin
./linuxdeploy-plugin-qt-x86_64.AppImage --appimage-extract
mv squashfs-root linuxdeploy-qt-bin
./appimagetool-x86_64.AppImage --appimage-extract
mv squashfs-root appimagetool-bin

export PATH="$PWD/linuxdeploy-bin/usr/bin:$PWD/linuxdeploy-qt-bin/usr/bin:$PWD/appimagetool-bin/usr/bin:$PATH"
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE=/usr/lib/qt6/bin/qmake
export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so"

# Run linuxdeploy to bundle Qt and generate AppImage
linuxdeploy \
  --appdir AppDir \
  -d dist/dev.suyu_emu.suyu.desktop \
  -i dist/suyu.svg \
  --plugin qt \
  --output appimage || true

AI_OUTPUT=$(find . -maxdepth 1 -name "*.AppImage" ! -name "linuxdeploy*" ! -name "appimagetool*" | head -n 1)
if [ -n "$AI_OUTPUT" ]; then
  cp "$AI_OUTPUT" suyu-x86_64.AppImage
else
  # Fallback: create AppRun and bundle via appimagetool
  cat > AppDir/AppRun << 'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "${0}")")"
export PATH="${HERE}/usr/bin:${PATH}"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${HERE}/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="${HERE}/usr/plugins:${QT_PLUGIN_PATH}"
export QML2_IMPORT_PATH="${HERE}/usr/qml:${QML2_IMPORT_PATH}"
exec "${HERE}/usr/bin/suyu" "$@"
EOF
  chmod +x AppDir/AppRun
  appimagetool-bin/AppRun AppDir suyu-x86_64.AppImage
fi

ls -lh *.AppImage
