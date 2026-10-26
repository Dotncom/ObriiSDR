#ifndef SPECTRUMMARKERPAIRSDIALOG_H
#define SPECTRUMMARKERPAIRSDIALOG_H

#include "spectrumoverlaytypes.h"

#include <QDialog>
#include <QJsonArray>
#include <QVector>

class QComboBox;
class QPushButton;
class QTableWidget;

class SpectrumMarkerPairsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit SpectrumMarkerPairsDialog(bool ukrainian, QWidget *parent = nullptr);

    void setMarkers(const QVector<SpectrumScienceMarker> &markers);
    void setPairConfiguration(const QJsonArray &configuration);
    QJsonArray pairConfiguration() const;

signals:
    void primaryPairChanged(int firstMarker, int secondMarker);
    void configurationChanged();

private:
    void addPair(int firstMarker, int secondMarker);
    void rebuildDefaults();
    void refreshRows();
    void emitConfigurationChanged();
    QString frequencyText(double hz) const;

    bool ukrainian = false;
    bool rebuilding = false;
    QVector<SpectrumScienceMarker> currentMarkers;
    QTableWidget *table = nullptr;
    QPushButton *addButton = nullptr;
    QPushButton *removeButton = nullptr;
};

#endif
