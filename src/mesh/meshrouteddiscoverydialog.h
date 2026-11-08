#pragma once

#include <QDialog>
#include <QJsonObject>

class QLabel;
class QPushButton;
class QTableWidget;

namespace Mesh {

QJsonObject defaultRoutedDiscoveryConfiguration();
bool routedDiscoveryConfigurationValid(const QJsonObject &configuration,
                                       QString *error = nullptr);

class RoutedDiscoveryDialog : public QDialog {
public:
    // An empty object opens the empty v1 default; other invalid input blocks Save.
    explicit RoutedDiscoveryDialog(const QJsonObject &configuration,
                                   QWidget *parent = nullptr);

    // Returns an empty object while the editor contains invalid input.
    QJsonObject configuration() const;
    void accept() override;

private:
    void appendEndpoint(const QJsonObject &endpoint);
    QJsonObject editedConfiguration() const;
    void refresh();

    QTableWidget *table = nullptr;
    QPushButton *addButton = nullptr;
    QPushButton *removeButton = nullptr;
    QPushButton *saveButton = nullptr;
    QLabel *errorLabel = nullptr;
    QString loadError;
    bool changingRows = false;
};

}
