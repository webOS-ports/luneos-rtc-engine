// Copyright (c) 2026 LuneOS project
// SPDX-License-Identifier: Apache-2.0

#ifndef FOREIGNEXPORTEDREGION_H
#define FOREIGNEXPORTEDREGION_H

#include <QQuickItem>
#include <QTimer>

struct wl_display;
struct wl_event_queue;
struct wl_registry;
struct wl_compositor;
struct wl_webos_foreign;
struct wl_webos_exported;

/**
 * Exports the item's on-screen rectangle through wl_webos_foreign so an
 * external video pipeline (for example luneos-rtc-engine) can render
 * into it via compositor punch-through. The assigned window ID appears
 * in the windowId property; pass it to the pipeline.
 *
 * All wayland traffic runs on a dedicated event queue so Qt's own
 * wayland event dispatch is never disturbed.
 */
class ForeignExportedRegion : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QString windowId READ windowId NOTIFY windowIdChanged)
    Q_PROPERTY(bool exported READ isExported NOTIFY windowIdChanged)

public:
    explicit ForeignExportedRegion(QQuickItem *parent = nullptr);
    ~ForeignExportedRegion() override;

    QString windowId() const { return m_windowId; }
    bool isExported() const { return !m_windowId.isEmpty(); }

    Q_INVOKABLE void updateRegion();

signals:
    void windowIdChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData &data) override;
    void geometryChange(const QRectF &newGeometry,
                        const QRectF &oldGeometry) override;

private slots:
    void tryExport();

private:
    void teardown();

    friend void handleRegistryGlobal(void *, wl_registry *, uint32_t,
                                     const char *, uint32_t);
    friend void handleWindowIdAssigned(void *, wl_webos_exported *,
                                       const char *, uint32_t);

    wl_display *m_display          = nullptr;
    wl_event_queue *m_queue        = nullptr;
    void *m_displayWrapper         = nullptr;
    wl_registry *m_registry        = nullptr;
    wl_compositor *m_compositor    = nullptr;
    wl_webos_foreign *m_foreign    = nullptr;
    wl_webos_exported *m_exported  = nullptr;

    QString m_windowId;
    QTimer m_retryTimer;
    int m_retriesLeft = 50;
};

#endif // FOREIGNEXPORTEDREGION_H
