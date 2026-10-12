#ifndef AUDIOFILTERCHAINWIDGET_H
#define AUDIOFILTERCHAINWIDGET_H

#include "audiofilterchain.h"

#include <QHash>
#include <QPointer>
#include <QWidget>

class QMenu;
class QTableWidget;
class QToolButton;

class AudioFilterChainWidget : public QWidget {
    Q_OBJECT

public:
    explicit AudioFilterChainWidget(QWidget *parent = nullptr);

    QString configurationJson() const;
    void setConfigurationJson(const QString &json);
    void setLanguage(bool ukrainian);
    void ensureStage(const QString &id, AudioFilterKind kind, bool initiallyEnabled = true);
    bool removeStageById(const QString &id);
    bool openStageEditor(const QString &id);

signals:
    void configurationChanged(const QString &json);

private:
    void rebuildAddMenu();
    void rebuildTable(int selectedRow = -1);
    void addStage(AudioFilterKind kind);
    void removeSelectedStage();
    void moveSelectedStage(int delta);
    void editStage(int row);
    void showStageTypeMenu(const QPoint &position);
    void replaceStageType(int row, AudioFilterKind kind);
    void emitConfigurationChanged();
    void ensureStageIds();
    int rowForStageId(const QString &id) const;
    void closeEditorForStage(const QString &id);
    void positionEditor(QWidget *editor);
    QString stageDisplayName(const AudioFilterStage &stage) const;

    QVector<AudioFilterStage> stages;
    QTableWidget *table = nullptr;
    QToolButton *addButton = nullptr;
    QToolButton *removeButton = nullptr;
    QToolButton *moveUpButton = nullptr;
    QToolButton *moveDownButton = nullptr;
    QMenu *addMenu = nullptr;
    QHash<QString, QPointer<QWidget>> openEditors;
    bool ukrainianLanguage = false;
    bool rebuilding = false;
};

#endif // AUDIOFILTERCHAINWIDGET_H
