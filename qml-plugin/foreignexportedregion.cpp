// Copyright (c) 2026 LuneOS project
// SPDX-License-Identifier: Apache-2.0

#include "foreignexportedregion.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <qpa/qplatformnativeinterface.h>

#include <wayland-client.h>
#include <wayland-webos-foreign-client-protocol.h>

void handleRegistryGlobal(void *data, wl_registry *registry, uint32_t name,
                          const char *interface, uint32_t)
{
    auto *self = static_cast<ForeignExportedRegion *>(data);
    if (qstrcmp(interface, "wl_compositor") == 0 && !self->m_compositor)
        self->m_compositor = static_cast<wl_compositor *>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 1));
    else if (qstrcmp(interface, "wl_webos_foreign") == 0 && !self->m_foreign)
        self->m_foreign = static_cast<wl_webos_foreign *>(
            wl_registry_bind(registry, name, &wl_webos_foreign_interface, 1));
}

static void handleRegistryGlobalRemove(void *, wl_registry *, uint32_t) {}

static const wl_registry_listener registryListener = {
    handleRegistryGlobal,
    handleRegistryGlobalRemove,
};

void handleWindowIdAssigned(void *data, wl_webos_exported *,
                            const char *windowId, uint32_t)
{
    auto *self       = static_cast<ForeignExportedRegion *>(data);
    self->m_windowId = QString::fromUtf8(windowId ? windowId : "");
}

static const wl_webos_exported_listener exportedListener = {
    handleWindowIdAssigned,
};

ForeignExportedRegion::ForeignExportedRegion(QQuickItem *parent)
    : QQuickItem(parent)
{
    m_retryTimer.setInterval(100);
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this,
            &ForeignExportedRegion::tryExport);
}

ForeignExportedRegion::~ForeignExportedRegion() { teardown(); }

void ForeignExportedRegion::itemChange(ItemChange change,
                                       const ItemChangeData &data)
{
    QQuickItem::itemChange(change, data);
    if (change == ItemSceneChange)
    {
        if (data.window)
            tryExport();
        else
            teardown();
    }
}

void ForeignExportedRegion::geometryChange(const QRectF &newGeometry,
                                           const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (m_exported)
        updateRegion();
}

void ForeignExportedRegion::tryExport()
{
    if (m_exported || !window())
        return;

    QPlatformNativeInterface *native =
        QGuiApplication::platformNativeInterface();
    if (!native)
        return;

    /* The platform surface only exists once the window is created and
     * shown; poll until it appears. */
    auto *display = static_cast<wl_display *>(
        native->nativeResourceForIntegration("wl_display"));
    auto *surface = static_cast<wl_surface *>(
        native->nativeResourceForWindow("surface", window()));
    if (!display || !surface)
    {
        if (m_retriesLeft-- > 0)
            m_retryTimer.start();
        else
            qWarning("ForeignExportedRegion: no wayland surface for window");
        return;
    }

    m_display = display;
    m_queue   = wl_display_create_queue(m_display);

    /* Bind our globals through a display wrapper on a private queue so
     * their events never race Qt's own dispatch of the default queue. */
    m_displayWrapper = wl_proxy_create_wrapper(m_display);
    wl_proxy_set_queue(static_cast<wl_proxy *>(m_displayWrapper), m_queue);
    m_registry = wl_display_get_registry(
        static_cast<wl_display *>(m_displayWrapper));
    wl_registry_add_listener(m_registry, &registryListener, this);
    wl_display_roundtrip_queue(m_display, m_queue);

    if (!m_foreign || !m_compositor)
    {
        qWarning("ForeignExportedRegion: compositor lacks wl_webos_foreign");
        teardown();
        return;
    }

    m_exported = wl_webos_foreign_export_element(
        m_foreign, surface, WL_WEBOS_FOREIGN_WEBOS_EXPORTED_TYPE_VIDEO_OBJECT);
    wl_webos_exported_add_listener(m_exported, &exportedListener, this);
    wl_display_roundtrip_queue(m_display, m_queue);

    if (m_windowId.isEmpty())
    {
        qWarning("ForeignExportedRegion: no window ID assigned");
        teardown();
        return;
    }

    updateRegion();
    emit windowIdChanged();
}

void ForeignExportedRegion::updateRegion()
{
    if (!m_exported || !m_compositor || !window())
        return;

    const qreal dpr = window()->effectiveDevicePixelRatio();
    const QRectF rect =
        mapRectToScene(QRectF(0, 0, width(), height()));

    /* Source region is a crop of the incoming video; full frame here. */
    wl_region *source = wl_compositor_create_region(m_compositor);
    wl_region_add(source, 0, 0, 8192, 8192);
    wl_region *destination = wl_compositor_create_region(m_compositor);
    wl_region_add(destination, qRound(rect.x() * dpr), qRound(rect.y() * dpr),
                  qRound(rect.width() * dpr), qRound(rect.height() * dpr));

    wl_webos_exported_set_exported_window(m_exported, source, destination);
    wl_region_destroy(source);
    wl_region_destroy(destination);
    wl_display_flush(m_display);
}

void ForeignExportedRegion::teardown()
{
    m_retryTimer.stop();
    if (m_exported)
    {
        wl_webos_exported_destroy(m_exported);
        m_exported = nullptr;
    }
    if (m_foreign)
    {
        wl_webos_foreign_destroy(m_foreign);
        m_foreign = nullptr;
    }
    if (m_compositor)
    {
        wl_compositor_destroy(m_compositor);
        m_compositor = nullptr;
    }
    if (m_registry)
    {
        wl_registry_destroy(m_registry);
        m_registry = nullptr;
    }
    if (m_displayWrapper)
    {
        wl_proxy_wrapper_destroy(m_displayWrapper);
        m_displayWrapper = nullptr;
    }
    if (m_display && m_queue)
        wl_display_flush(m_display);
    if (m_queue)
    {
        wl_event_queue_destroy(m_queue);
        m_queue = nullptr;
    }
    m_display = nullptr;
    if (!m_windowId.isEmpty())
    {
        m_windowId.clear();
        emit windowIdChanged();
    }
}
