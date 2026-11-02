#pragma once

#include <QJsonObject>

// QtCore + QtNetwork, synchronous and bounded by fixed candidate/array limits.
// Default is passive. Explicit checkPorts=true probes only supplied tcpPorts by
// IPv4 loopback bind/close; never listen/connect. No hardware, processes, profiles
// or library loading. The sole filesystem write requires
// checkWritableDirectory=true and an explicit absolute directory.
// See docs/cli_doctor.md for the versioned request/report contract.
QJsonObject runCliDoctor(const QJsonObject &request = QJsonObject());
