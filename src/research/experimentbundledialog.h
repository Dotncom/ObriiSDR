#pragma once

#include <QDialog>
#include <QJsonObject>
#include <functional>
#include <memory>

class QCloseEvent;

class ExperimentBundleDialog final : public QDialog {
public:
    using Snapshot = std::function<QJsonObject()>;
    // Receives the full sanitized context. The owner must back up the current
    // workspace, require stopped RX, and restore layout only, never hardware.
    using ApplyWorkspace = std::function<bool(const QJsonObject &, QString *)>;
    using Translator = std::function<QString(const QString &, const QString &)>;

    ExperimentBundleDialog(bool importing, Snapshot snapshot, ApplyWorkspace applyWorkspace,
                           ApplyWorkspace applySession, Translator translator, QWidget *parent = nullptr);
    ~ExperimentBundleDialog() override;
    void reject() override;
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
