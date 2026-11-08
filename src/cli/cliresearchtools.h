#pragma once

#include <QJsonObject>

#include <memory>

// Optional synchronous Qt Core helper. Serialize all calls on one thread.
// No subscriptions, scheduled timers, receiver control, settings, or implicit I/O.
class CliResearchTools {
public:
    static constexpr int MaximumIqSamples = 65536;
    static constexpr int MaximumSpectrumBins = 65536;
    static constexpr int MaximumPageRows = 1024;

    CliResearchTools();
    ~CliResearchTools();
    CliResearchTools(const CliResearchTools &) = delete;
    CliResearchTools &operator=(const CliResearchTools &) = delete;

    // See docs/cli_research_tools.md for operations, metadata and hard limits.
    // Failed requests never replace a successful snapshot.
    QJsonObject command(const QJsonObject &request);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
