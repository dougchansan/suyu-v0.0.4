// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QGuiApplication>
#include <QStringLiteral>
#include <QWindow>
#include "common/logging/log.h"
#include "core/frontend/emu_window.h"
#include "suyu/qt_common.h"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <qpa/qplatformnativeinterface.h>
#elif defined(__APPLE__)
#include <objc/message.h>
#endif

#if defined(__APPLE__)
namespace {
id FindMetalLayerInTree(id layer, Class metal_layer_class) {
    // Qt may expose a QContainerLayer here; MoltenVK needs the actual CAMetalLayer.
    if (!layer || !metal_layer_class) {
        return nullptr;
    }

    const SEL is_kind_of_class = sel_registerName("isKindOfClass:");
    if (reinterpret_cast<bool (*)(id, SEL, Class)>(objc_msgSend)(
            layer, is_kind_of_class, metal_layer_class)) {
        return layer;
    }

    // Search descendants because the Metal layer may be nested below Qt's wrappers.
    const SEL sublayers_selector = sel_registerName("sublayers");
    id sublayers =
        reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(layer, sublayers_selector);
    if (!sublayers) {
        return nullptr;
    }

    const SEL count_selector = sel_registerName("count");
    const SEL object_at_index_selector = sel_registerName("objectAtIndex:");
    const auto count =
        reinterpret_cast<unsigned long (*)(id, SEL)>(objc_msgSend)(sublayers, count_selector);
    for (unsigned long i = 0; i < count; ++i) {
        id metal_layer = FindMetalLayerInTree(
            reinterpret_cast<id (*)(id, SEL, unsigned long)>(objc_msgSend)(
                sublayers, object_at_index_selector, i),
            metal_layer_class);
        if (metal_layer) {
            return metal_layer;
        }
    }

    return nullptr;
}
} // namespace
#endif

namespace QtCommon {
Core::Frontend::WindowSystemType GetWindowSystemType() {
    // Determine WSI type based on Qt platform.
    QString platform_name = QGuiApplication::platformName();
    if (platform_name == QStringLiteral("windows"))
        return Core::Frontend::WindowSystemType::Windows;
    else if (platform_name == QStringLiteral("xcb"))
        return Core::Frontend::WindowSystemType::X11;
    else if (platform_name == QStringLiteral("wayland"))
        return Core::Frontend::WindowSystemType::Wayland;
    else if (platform_name == QStringLiteral("wayland-egl"))
        return Core::Frontend::WindowSystemType::Wayland;
    else if (platform_name == QStringLiteral("cocoa"))
        return Core::Frontend::WindowSystemType::Cocoa;
    else if (platform_name == QStringLiteral("android"))
        return Core::Frontend::WindowSystemType::Android;

    LOG_CRITICAL(Frontend, "Unknown Qt platform {}!", platform_name.toStdString());
    return Core::Frontend::WindowSystemType::Windows;
} // namespace Core::Frontend::WindowSystemType

Core::Frontend::EmuWindow::WindowSystemInfo GetWindowSystemInfo(QWindow* window) {
    Core::Frontend::EmuWindow::WindowSystemInfo wsi;
    wsi.type = GetWindowSystemType();

#if defined(_WIN32)
    // Our Win32 Qt external doesn't have the private API.
    wsi.render_surface = reinterpret_cast<void*>(window->winId());
#elif defined(__APPLE__)
    id layer = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(
        reinterpret_cast<id>(window->winId()), sel_registerName("layer"));

    // In Qt 6, the NSView layer might be a QContainerLayer. Search the entire
    // layer tree for the CAMetalLayer required by VK_EXT_metal_surface.
    id metal_layer = FindMetalLayerInTree(layer, objc_getClass("CAMetalLayer"));
    wsi.render_surface = reinterpret_cast<void*>(metal_layer);
#else
    QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
    wsi.display_connection = pni->nativeResourceForWindow("display", window);
    if (wsi.type == Core::Frontend::WindowSystemType::Wayland)
        wsi.render_surface = window ? pni->nativeResourceForWindow("surface", window) : nullptr;
    else
        wsi.render_surface = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
#endif
    wsi.render_surface_scale = window ? static_cast<float>(window->devicePixelRatio()) : 1.0f;

    return wsi;
}
} // namespace QtCommon
