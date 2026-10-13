#pragma once

#include <QJsonObject>
#include <QWidget>

class DspBranchSettingsWidget : public QWidget {
    Q_OBJECT

public:
    explicit DspBranchSettingsWidget(const QString &type, const QJsonObject &settings,
                                     bool ukrainian, QWidget *parent = nullptr);

    static bool supportsType(const QString &type);
    static QString blockTitle(const QString &type, bool ukrainian);

signals:
    // Only edited keys are emitted, preserving other block and workspace settings.
    void settingsEdited(const QJsonObject &changes);
};
