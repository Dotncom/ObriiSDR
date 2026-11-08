#include "dmrbackendpaths.h"

QString defaultDsdNeoProgramPath() {
#if defined(_WIN32)
    return QStringLiteral("dsd-neo/dsd-neo.exe");
#else
    return QStringLiteral("dsd-neo");
#endif
}

bool isLegacyDsdNeoProgramName(const QString &program) {
    const QString trimmed = program.trimmed();
    return trimmed.compare(QStringLiteral("dsd-neo.exe"), Qt::CaseInsensitive) == 0 ||
           trimmed.compare(QStringLiteral("dsd-neo"), Qt::CaseInsensitive) == 0 ||
           trimmed.compare(defaultDsdNeoProgramPath(), Qt::CaseInsensitive) == 0;
}

QString defaultGopherTrunkProgramPath() {
#if defined(_WIN32)
    return QStringLiteral("gophertrunk/fobos-dmr-virtual.exe");
#else
    return QStringLiteral("gophertrunk/fobos-dmr-virtual");
#endif
}
