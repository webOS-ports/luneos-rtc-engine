// Copyright (c) 2026 LuneOS project
// SPDX-License-Identifier: Apache-2.0

#include <QQmlExtensionPlugin>
#include <qqml.h>

#include "foreignexportedregion.h"

class LuneOSForeignPlugin : public QQmlExtensionPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void registerTypes(const char *uri) override
    {
        qmlRegisterType<ForeignExportedRegion>(uri, 1, 0,
                                               "ForeignExportedRegion");
    }
};

#include "plugin.moc"
