#pragma once

#include "fpgalogicblock.h"

#include <QDialog>
#include <QHash>
#include <QVector>

class QCloseEvent;
class QComboBox;
class QGraphicsPathItem;
class QGraphicsRectItem;
class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QLineEdit;
class QSpinBox;

class FpgaLogicEditor final : public QDialog {
public:
    explicit FpgaLogicEditor(const QString &projectRoot, QWidget *parent = nullptr);
    QString requestedSynthesisRtl() const { return requestedSynthesisRtl_; }

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

private:
    struct Connection {
        QGraphicsPathItem *path = nullptr;
        int source = 0;
        int target = 0;
        int port = 0;
    };

    bool confirmClose();
    bool load(const QString &path);
    bool save();
    bool exportRtl(const QString &path);
    void draw();
    void updateConnections();
    void updateInspector();
    void updateStatus();
    void capturePositions();
    void markDirty();
    int nextId() const;
    FpgaLogicNode *selectedNode();

    QString projectRoot_;
    QString filePath_;
    QString requestedSynthesisRtl_;
    FpgaLogicBlock block_;
    bool dirty_ = false;
    bool drawing_ = false;
    int selectedId_ = -1;
    QGraphicsScene *scene_ = nullptr;
    QGraphicsView *view_ = nullptr;
    QHash<int, QGraphicsRectItem *> nodeItems_;
    QVector<Connection> connections_;
    QComboBox *primitive_ = nullptr;
    QComboBox *inputChoices_[4] = {nullptr, nullptr, nullptr, nullptr};
    QComboBox *outputIChoice_ = nullptr;
    QComboBox *outputQChoice_ = nullptr;
    QLineEdit *nameEdit_ = nullptr;
    QLineEdit *truthEdit_ = nullptr;
    QSpinBox *constantEdit_ = nullptr;
    QLabel *status_ = nullptr;
};
