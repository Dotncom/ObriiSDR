#include "meshrouteddiscoverydialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSpinBox>
#include <QTableWidget>
#include <cstdio>
#include <limits>

namespace {
int failures = 0;
int checks = 0;
void check(bool okay, const char *description)
{
    ++checks;
    std::printf("%s %s\n", okay ? "PASS" : "FAIL", description);
    std::fflush(stdout);
    if (!okay) ++failures;
}

QJsonObject endpoint(int index = 1, const QString &role = "seed")
{
    return {{"enabled", true}, {"role", role}, {"name", "Test endpoint"},
            {"host", QString("192.0.2.%1").arg(index)}, {"port", 45454}};
}

QJsonObject config(const QJsonArray &endpoints)
{
    return {{"version", 1}, {"endpoints", endpoints}};
}

bool rejected(const QJsonObject &object)
{
    QString error;
    return !Mesh::routedDiscoveryConfigurationValid(object, &error) && !error.isEmpty() &&
           !Mesh::routedDiscoveryConfigurationValid(object, nullptr);
}

void validationTests()
{
    const auto empty = Mesh::defaultRoutedDiscoveryConfiguration();
    QString error = "stale error";
    check(empty == config({}) && Mesh::routedDiscoveryConfigurationValid(empty, &error) && error.isEmpty(),
          "default is a valid empty v1 configuration and clears stale errors");
    check(rejected({}), "validator requires explicit v1 schema");
    const auto original = config({endpoint()});
    check(Mesh::routedDiscoveryConfigurationValid(original) &&
          QJsonDocument::fromJson(QJsonDocument(original).toJson()).object() == original,
          "valid configuration survives JSON serialization roundtrip");
    for (const auto &field : QStringList{"version", "endpoints"}) {
        auto broken = original;
        broken.remove(field);
        check(rejected(broken), qPrintable("missing root field rejected: " + field));
    }
    for (const auto &version : {QJsonValue("1"), QJsonValue(true), QJsonValue(1.5),
                                QJsonValue(2), QJsonValue(QJsonValue::Null)}) {
        auto broken = original;
        broken["version"] = version;
        check(rejected(broken), "version rejects wrong type, fractional and unsupported values");
    }
    for (const auto &value : {QJsonValue(QJsonObject{}), QJsonValue("[]"), QJsonValue(false),
                              QJsonValue(QJsonValue::Null)}) {
        auto broken = original;
        broken["endpoints"] = value;
        check(rejected(broken), "endpoints must be an array");
        check(rejected(config({value})), "each endpoint must be a complete object");
    }
    for (const auto &field : QStringList{"enabled", "role", "name", "host", "port"}) {
        auto broken = endpoint();
        broken.remove(field);
        check(rejected(config({broken})), qPrintable("missing endpoint field rejected: " + field));
    }
    const QStringList forbidden{"token", "password", "secret", "key", "pairingSecret", "credentials",
                                "privateKey", "directory", "path", "scan", "unexpected"};
    for (const auto &field : forbidden) {
        auto broken = original;
        broken[field] = "sensitive-value-never-export";
        auto entry = endpoint();
        entry[field] = "sensitive-value-never-export";
        check(rejected(broken) && rejected(config({entry})),
              qPrintable("unknown/private field rejected at both levels: " + field));
    }
    for (const auto &field : QStringList{"enabled", "role", "name", "host", "port"}) {
        const QList<QJsonValue> badTypes{QJsonValue::Null, QJsonArray{}, QJsonObject{}};
        bool allRejected = true;
        for (const auto &value : badTypes) {
            auto entry = endpoint();
            entry[field] = value;
            allRejected &= rejected(config({entry}));
        }
        check(allRejected, qPrintable("null and container types rejected: " + field));
    }
    for (const auto &value : {QJsonValue(1), QJsonValue("true")}) {
        auto entry = endpoint();
        entry["enabled"] = value;
        check(rejected(config({entry})), "enabled is strictly boolean");
    }
    for (const auto &role : QStringList{"", "SEED", "peer", "directory "}) {
        auto entry = endpoint();
        entry["role"] = role;
        check(rejected(config({entry})), "unknown and noncanonical roles rejected");
    }
    auto entry = endpoint();
    entry["name"] = QString(64, QChar(0x0406));
    check(Mesh::routedDiscoveryConfigurationValid(config({entry})), "64-character Unicode name accepted");
    entry["name"] = QString(65, 'x');
    check(rejected(config({entry})), "65-character name rejected");
    entry["name"] = 42;
    check(rejected(config({entry})), "numeric name rejected");
    entry["name"] = "";
    entry["enabled"] = false;
    check(Mesh::routedDiscoveryConfigurationValid(config({entry})), "empty name and disabled endpoint accepted");

    const QStringList badHosts{"", "0.0.0.0", "0.0.0.2", "224.0.0.1", "255.255.255.255", "example.org", "192.0.2.0/24", "192.0.2.*", "::1", "::ffff:192.0.2.1",
                               "192.0.2.256", "192.0.2.-1", "192.0.2", "192.0.2.1.2", "3221225985",
                               "192.000.2.1", "0xc0.0.2.1", "192.0.2.1 ", " 192.0.2.1", "192..2.1",
                               "http://192.0.2.1", "user@192.0.2.1", "192.0.2.1:80"};
    for (const auto &host : badHosts) {
        auto broken = endpoint();
        broken["host"] = host;
        check(rejected(config({broken})), qPrintable("DNS/CIDR/noncanonical IPv4 rejected: " + host));
    }
    entry = endpoint();
    entry["host"] = 3221225985.0;
    check(rejected(config({entry})), "host must be a string, not an integer");
    for (const auto &port : {QJsonValue(0), QJsonValue(-1), QJsonValue(65536), QJsonValue(1.5),
                             QJsonValue("45454"), QJsonValue(true),
                             QJsonValue(std::numeric_limits<double>::infinity())}) {
        entry = endpoint();
        entry["port"] = port;
        check(rejected(config({entry})), "port rejects range, fractional and type errors");
    }
    entry = endpoint();
    entry["port"] = 1;
    auto other = endpoint(2);
    other["port"] = 65535;
    check(Mesh::routedDiscoveryConfigurationValid(config({entry, other})), "port boundaries 1 and 65535 accepted");
    auto duplicate = endpoint();
    duplicate["enabled"] = false;
    duplicate["name"] = "Different name";
    check(rejected(config({endpoint(), duplicate})), "duplicate role/host/port rejected even when disabled or renamed");
    auto directory = endpoint(1, "directory");
    other = endpoint();
    other["port"] = 45455;
    check(Mesh::routedDiscoveryConfigurationValid(config({endpoint(), directory, other})),
          "same host with distinct role or port is not a duplicate");
    QJsonArray entries;
    for (int i = 1; i <= 48; ++i) entries.append(endpoint(i));
    check(Mesh::routedDiscoveryConfigurationValid(config(entries)), "48 total entries accepted");
    entries.append(endpoint(49));
    check(rejected(config(entries)), "49 total entries rejected");
    entries = {};
    for (int i = 1; i <= 8; ++i) entries.append(endpoint(i, "directory"));
    check(Mesh::routedDiscoveryConfigurationValid(config(entries)), "8 directory entries accepted");
    directory = endpoint(9, "directory");
    directory["enabled"] = false;
    entries.append(directory);
    check(rejected(config(entries)), "9 directory entries rejected, including disabled entries");
}

template<typename T> T *cell(QTableWidget *table, int row, int column)
{
    return qobject_cast<T *>(table->cellWidget(row, column));
}

void edit(QLineEdit *field, const QString &text)
{
    field->setFocus();
    field->selectAll();
    QKeyEvent key(QEvent::KeyPress, 0, Qt::NoModifier, text);
    QApplication::sendEvent(field, &key);
    QApplication::processEvents();
}

void screenshot(Mesh::RoutedDiscoveryDialog &dialog, const QString &directory, const QString &name)
{
    if (directory.isEmpty()) return;
    QApplication::processEvents();
    const auto image = dialog.grab().toImage();
    QSet<QRgb> colors;
    for (int y = 0; y < image.height(); y += 5)
        for (int x = 0; x < image.width(); x += 5) colors.insert(image.pixel(x, y));
    check(!image.isNull() && colors.size() > 8 && image.save(QDir(directory).filePath(name)),
          qPrintable("nonblank offscreen screenshot: " + name));
}

void uiTests(const QString &screenshots)
{
    const auto original = config({endpoint(), endpoint(2, "directory")});
    Mesh::RoutedDiscoveryDialog dialog(original);
    dialog.show();
    QApplication::processEvents();
    auto table = dialog.findChild<QTableWidget *>("routedDiscoveryEndpoints");
    auto add = dialog.findChild<QPushButton *>("routedDiscoveryAdd");
    auto remove = dialog.findChild<QPushButton *>("routedDiscoveryRemove");
    auto save = dialog.findChild<QPushButton *>("routedDiscoverySave");
    auto cancel = dialog.findChild<QPushButton *>("routedDiscoveryCancel");
    auto error = dialog.findChild<QLabel *>("routedDiscoveryError");
    check(table && add && remove && save && cancel && error, "production controls have discoverable object names");
    if (!table || !add || !remove || !save || !cancel || !error) return;
    check(table->rowCount() == 2 && dialog.configuration() == original && save->isEnabled(),
          "production editor exactly roundtrips valid input");
    check(dialog.maximumWidth() <= 960 && !add->icon().isNull() && !remove->icon().isNull(),
          "bounded dialog width and native icon-only add/remove controls");
    check(cell<QComboBox>(table, 0, 2)->toolTip().contains("UDP/TCP") &&
          cell<QLineEdit>(table, 0, 3)->placeholderText() == "192.0.2.1",
          "role tooltip explains direct transports and host hint is RFC5737");
    screenshot(dialog, screenshots, "routed-desktop.png");

    edit(cell<QLineEdit>(table, 0, 1), "Edited seed");
    cell<QCheckBox>(table, 0, 0)->click();
    cell<QComboBox>(table, 0, 2)->setCurrentIndex(1);
    edit(cell<QLineEdit>(table, 0, 3), "198.51.100.7");
    cell<QSpinBox>(table, 0, 4)->setValue(65535);
    const auto edited = dialog.configuration().value("endpoints").toArray().at(0).toObject();
    check(edited.value("name") == "Edited seed" && edited.value("enabled") == false &&
          edited.value("role") == "directory" && edited.value("host") == "198.51.100.7" &&
          edited.value("port") == 65535, "production cell edits update all five configuration fields");
    edit(cell<QLineEdit>(table, 0, 1), QString(70, 'n'));
    check(cell<QLineEdit>(table, 0, 1)->text().size() == 64 &&
          cell<QSpinBox>(table, 0, 4)->minimum() == 1 && cell<QSpinBox>(table, 0, 4)->maximum() == 65535,
          "production editors enforce name and port bounds");
    add->click();
    check(table->rowCount() == 3 && cell<QLineEdit>(table, 2, 3)->text().isEmpty() &&
          !save->isEnabled() && dialog.configuration().isEmpty() && !error->text().isEmpty(),
          "add creates an unfilled endpoint, never a default IP, and blocks Save");
    dialog.accept();
    check(dialog.isVisible() && dialog.result() != QDialog::Accepted, "accept cannot bypass invalid configuration");
    edit(cell<QLineEdit>(table, 2, 3), "203.0.113.9");
    check(save->isEnabled(), "valid added endpoint enables Save");
    cell<QComboBox>(table, 2, 2)->setCurrentIndex(1);
    cell<QSpinBox>(table, 2, 4)->setValue(65535);
    edit(cell<QLineEdit>(table, 2, 3), "198.51.100.7");
    check(!save->isEnabled() && dialog.configuration().isEmpty(), "UI rejects duplicate role/host/port");
    edit(cell<QLineEdit>(table, 2, 3), "192.0.2.0/24");
    check(!save->isEnabled(), "UI rejects CIDR input");
    dialog.resize(360, 360);
    QApplication::processEvents();
    check(dialog.width() == 360 && table->horizontalScrollBar()->maximum() > 0 &&
          dialog.rect().contains(save->mapTo(&dialog, save->rect().bottomRight())),
          "narrow layout scrolls horizontally and retains visible Save button");
    screenshot(dialog, screenshots, "routed-narrow-error.png");
    table->selectRow(0);
    cell<QLineEdit>(table, 2, 1)->setFocus();
    QApplication::processEvents();
    check(table->currentRow() == 2, "focusing an embedded editor selects its row for Remove");
    remove->click();
    check(table->rowCount() == 2 && save->isEnabled(), "remove deletes selected invalid endpoint and restores validity");
    const auto saved = dialog.configuration();
    save->click();
    check(dialog.result() == QDialog::Accepted, "production Save accepts a valid configuration");
    Mesh::RoutedDiscoveryDialog reopened(QJsonDocument::fromJson(QJsonDocument(saved).toJson()).object());
    check(reopened.configuration() == saved, "edited saved configuration reopens without loss");
    reopened.show();
    reopened.findChild<QPushButton *>("routedDiscoveryCancel")->click();
    check(reopened.result() == QDialog::Rejected && !reopened.isVisible(), "production Cancel rejects the dialog");
    check(original == config({endpoint(), endpoint(2, "directory")}), "editing never mutates caller's JSON");

    Mesh::RoutedDiscoveryDialog empty({});
    check(empty.configuration() == Mesh::defaultRoutedDiscoveryConfiguration(), "empty constructor input uses empty default");
    auto broken = original;
    broken["token"] = "sensitive-value-never-export";
    Mesh::RoutedDiscoveryDialog invalid(broken);
    auto invalidTable = invalid.findChild<QTableWidget *>("routedDiscoveryEndpoints");
    check(invalidTable->rowCount() == 0 && invalid.configuration().isEmpty() &&
          !invalid.findChild<QPushButton *>("routedDiscoverySave")->isEnabled() &&
          !invalid.findChild<QLabel *>("routedDiscoveryError")->text().contains("sensitive-value"),
          "malformed saved input is not partially imported, exposed or silently made saveable");
    invalid.findChild<QPushButton *>("routedDiscoveryAdd")->click();
    invalid.findChild<QPushButton *>("routedDiscoveryRemove")->click();
    check(invalid.configuration() == Mesh::defaultRoutedDiscoveryConfiguration(),
          "explicit add/remove permits replacing malformed saved input with empty configuration");

    QJsonArray bounded;
    for (int i = 1; i <= 48; ++i) bounded.append(endpoint(i, i <= 8 ? "directory" : "seed"));
    Mesh::RoutedDiscoveryDialog full(config(bounded));
    full.show();
    auto fullTable = full.findChild<QTableWidget *>("routedDiscoveryEndpoints");
    auto fullAdd = full.findChild<QPushButton *>("routedDiscoveryAdd");
    check(full.configuration() == config(bounded) && !fullAdd->isEnabled(), "UI loads 48 entries and disables Add at bound");
    fullAdd->click();
    check(fullTable->rowCount() == 48, "disabled Add cannot exceed total bound");
    cell<QComboBox>(fullTable, 8, 2)->setCurrentIndex(1);
    check(full.configuration().isEmpty() && !full.findChild<QPushButton *>("routedDiscoverySave")->isEnabled(),
          "UI rejects ninth directory entry");
    cell<QComboBox>(fullTable, 8, 2)->setCurrentIndex(0);
    QApplication::processEvents();
    check(fullTable->verticalScrollBar()->maximum() > 0, "48-entry editor scrolls vertically");
    screenshot(full, screenshots, "routed-full.png");
    fullTable->selectRow(47);
    full.findChild<QPushButton *>("routedDiscoveryRemove")->click();
    check(fullTable->rowCount() == 47 && fullAdd->isEnabled(), "remove re-enables Add below total bound");
    while (fullTable->rowCount()) {
        fullTable->selectRow(fullTable->rowCount() - 1);
        full.findChild<QPushButton *>("routedDiscoveryRemove")->click();
    }
    check(full.configuration() == Mesh::defaultRoutedDiscoveryConfiguration() &&
          !full.findChild<QPushButton *>("routedDiscoveryRemove")->isEnabled(),
          "repeated row removal, including the final row, safely restores empty default");
}
}

// Usage: selftest [screenshot-directory]. Offscreen by default, no QtTest dependency.
int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setApplicationName("Routed discovery self-test");
#ifdef Q_OS_WIN
    // The Windows offscreen plugin does not enumerate system fonts.
    if (QApplication::platformName() == "offscreen") {
        const auto fontPath = QDir(qEnvironmentVariable("WINDIR", "C:/Windows"))
                                  .filePath("Fonts/segoeui.ttf");
        const int fontId = QFontDatabase::addApplicationFont(fontPath);
        const auto families = QFontDatabase::applicationFontFamilies(fontId);
        check(fontId >= 0 && !families.isEmpty(), "offscreen system font loaded for readable screenshots");
        if (!families.isEmpty()) QApplication::setFont(QFont(families.first(), 9));
    }
#endif
    check(QFontMetrics(QApplication::font()).horizontalAdvance("IPv4") > 0,
          "UI font has nonzero text metrics");
    const QString screenshots = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString{};
    if (!screenshots.isEmpty()) check(QDir().mkpath(screenshots), "screenshot directory available");
    validationTests();
    uiTests(screenshots);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
