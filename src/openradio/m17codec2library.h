#pragma once

#include <QCoreApplication>
#include <QLibrary>
#include <QStringList>

#include <algorithm>
#include <initializer_list>

inline bool loadM17Codec2(QLibrary &library, std::initializer_list<const char *> symbols) {
    const QStringList names{
        QCoreApplication::applicationDirPath() + QStringLiteral("/libcodec2"),
        QStringLiteral("codec2"), QStringLiteral("libcodec2"),
        QStringLiteral("libcodec2.so.1.2"), QStringLiteral("libcodec2.so.1.0")};
    for (const auto &name : names) {
        library.setFileName(name);
        if (!library.load()) continue;
        if (std::all_of(symbols.begin(), symbols.end(), [&](const char *symbol) {
                return library.resolve(symbol) != nullptr;
            })) return true;
        library.unload();
    }
    return false;
}
