#include "meshrouteddiscoverydialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScopedValueRollback>
#include <QSet>
#include <QSpinBox>
#include <QStyle>
#include <QTableWidget>
#include <QVBoxLayout>
#include <cmath>

namespace Mesh {
namespace {
constexpr int MaximumEndpoints = 48;
constexpr int MaximumDirectories = 8;
QString u(const char *text) { return QString::fromUtf8(text); }

bool integer(const QJsonValue &value, int minimum, int maximum)
{
    const double number = value.toDouble();
    return value.isDouble() && std::isfinite(number) && number >= minimum &&
           number <= maximum && std::floor(number) == number;
}

bool numericIpv4(const QString &host)
{
    if (host.size() < 7 || host.size() > 15) return false;
    const auto octets = host.split('.');
    if (octets.size() != 4) return false;
    for (const auto &octet : octets) {
        if (octet.isEmpty() || octet.size() > 3 ||
            (octet.size() > 1 && octet.startsWith('0'))) return false;
        for (const auto c : octet)
            if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
        if (octet.toInt() > 255) return false;
    }
    return octets.first().toInt()>0&&octets.first().toInt()<224;
}

bool exactFields(const QJsonObject &object, const QSet<QString> &fields)
{
    if (object.size() != fields.size()) return false;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        if (!fields.contains(it.key())) return false;
    return true;
}

QString roleTip()
{
    return u("Seed: прямий UDP/TCP до вузла. Directory: каталог адрес вузлів.");
}
}

QJsonObject defaultRoutedDiscoveryConfiguration()
{
    return {{"version", 1}, {"endpoints", QJsonArray{}}};
}

bool routedDiscoveryConfigurationValid(const QJsonObject &configuration, QString *error)
{
    if (error) error->clear();
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!exactFields(configuration, {"version", "endpoints"}) ||
        !integer(configuration.value("version"), 1, 1) ||
        !configuration.value("endpoints").isArray())
        return fail(u("Очікується конфігурація v1: лише version та endpoints."));

    const auto endpoints = configuration.value("endpoints").toArray();
    if (endpoints.size() > MaximumEndpoints)
        return fail(u("Не більше 48 адрес."));
    int directories = 0;
    QSet<QString> seen;
    for (int index = 0; index < endpoints.size(); ++index) {
        const auto value = endpoints.at(index);
        const auto endpoint = value.toObject();
        const auto prefix = u("Рядок %1: ").arg(index + 1);
        if (!value.isObject() || !exactFields(endpoint, {"enabled", "role", "name", "host", "port"}))
            return fail(prefix + u("дозволені лише enabled, role, name, host, port."));
        if (!endpoint.value("enabled").isBool() || !endpoint.value("name").isString() ||
            endpoint.value("name").toString().size() > 64)
            return fail(prefix + u("enabled має бути bool, назва — до 64 символів."));
        const auto role = endpoint.value("role");
        if (!role.isString() || (role != "seed" && role != "directory"))
            return fail(prefix + u("роль має бути seed або directory."));
        const auto host = endpoint.value("host");
        if (!host.isString() || !numericIpv4(host.toString()))
            return fail(prefix + u("потрібна числова IPv4-адреса без DNS або CIDR."));
        if (!integer(endpoint.value("port"), 1, 65535))
            return fail(prefix + u("порт має бути цілим числом 1–65535."));
        if (role == "directory" && ++directories > MaximumDirectories)
            return fail(u("Не більше 8 записів directory, включно з вимкненими."));
        const auto key = role.toString() + '/' + host.toString() + '/' +
                         QString::number(endpoint.value("port").toInt());
        if (seen.contains(key))
            return fail(prefix + u("повтор ролі, адреси та порту."));
        seen.insert(key);
    }
    return true;
}

RoutedDiscoveryDialog::RoutedDiscoveryDialog(const QJsonObject &configuration, QWidget *parent)
    : QDialog(parent)
{
    setObjectName("routedDiscoveryDialog");
    setWindowTitle(u("Адреси маршрутизованого виявлення"));
    setMaximumWidth(960);
    setMinimumSize(320, 240);
    const auto screen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    const auto available = screen ? screen->availableGeometry().size() : QSize(760, 460);
    resize(qMin(760, available.width()), qMin(460, available.height()));

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    auto commands = new QHBoxLayout;
    addButton = new QPushButton(style()->standardIcon(QStyle::SP_FileDialogNewFolder), {}, this);
    addButton->setObjectName("routedDiscoveryAdd");
    addButton->setAutoDefault(false);
    addButton->setToolTip(u("Додати адресу"));
    addButton->setAccessibleName(addButton->toolTip());
    removeButton = new QPushButton(style()->standardIcon(QStyle::SP_TrashIcon), {}, this);
    removeButton->setObjectName("routedDiscoveryRemove");
    removeButton->setAutoDefault(false);
    removeButton->setToolTip(u("Видалити вибрану адресу"));
    removeButton->setAccessibleName(removeButton->toolTip());
    commands->addWidget(addButton);
    commands->addWidget(removeButton);
    commands->addStretch();
    layout->addLayout(commands);

    table = new QTableWidget(0, 5, this);
    table->setObjectName("routedDiscoveryEndpoints");
    table->setHorizontalHeaderLabels({u("Увімкнено"), u("Назва"), u("Роль"), "IPv4", u("Порт")});
    table->horizontalHeaderItem(2)->setToolTip(roleTip());
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setMinimumSize(0, 0);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setMinimumSectionSize(72);
    const int widths[] = {92, 180, 120, 160, 96};
    for (int column = 0; column < 5; ++column) table->setColumnWidth(column, widths[column]);
    table->verticalHeader()->setDefaultSectionSize(32);
    layout->addWidget(table, 1);

    errorLabel = new QLabel(this);
    errorLabel->setObjectName("routedDiscoveryError");
    errorLabel->setTextFormat(Qt::PlainText);
    errorLabel->setWordWrap(true);
    errorLabel->setMinimumWidth(0);
    layout->addWidget(errorLabel);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->setObjectName("routedDiscoveryButtons");
    saveButton = buttons->button(QDialogButtonBox::Save);
    saveButton->setObjectName("routedDiscoverySave");
    saveButton->setDefault(true);
    saveButton->setText(u("Зберегти"));
    saveButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    buttons->button(QDialogButtonBox::Cancel)->setObjectName("routedDiscoveryCancel");
    buttons->button(QDialogButtonBox::Cancel)->setText(u("Скасувати"));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &RoutedDiscoveryDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(addButton, &QPushButton::clicked, this, [this] {
        if (table->rowCount() >= MaximumEndpoints) return;
        // Malformed saved data is never partially imported or silently saved as empty.
        loadError.clear();
        appendEndpoint({{"enabled", true}, {"name", ""}, {"role", "seed"}, {"host", ""}, {"port", 1}});
        table->selectRow(table->rowCount() - 1);
        table->scrollToBottom();
        table->cellWidget(table->rowCount() - 1, 3)->setFocus();
        refresh();
    });
    connect(removeButton, &QPushButton::clicked, this, [this] {
        const int row = table->currentRow();
        if (row < 0) return;
        {
            // Qt can emit selection/focus changes after removing cell widgets,
            // but before updating rowCount(). Do not read that intermediate state.
            QScopedValueRollback<bool> guard(changingRows, true);
            table->removeRow(row);
        }
        if (table->rowCount()) table->selectRow(qMin(row, table->rowCount() - 1));
        refresh();
    });
    connect(table, &QTableWidget::itemSelectionChanged, this, &RoutedDiscoveryDialog::refresh);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *focused) {
        if (!focused || changingRows) return;
        for (int row = 0; row < table->rowCount(); ++row)
            for (int column = 0; column < table->columnCount(); ++column) {
                auto editor = table->cellWidget(row, column);
                if (editor && (editor == focused || editor->isAncestorOf(focused))) {
                    table->setCurrentCell(row, column);
                    return;
                }
            }
    });

    const auto initial = configuration.isEmpty() ? defaultRoutedDiscoveryConfiguration() : configuration;
    if (routedDiscoveryConfigurationValid(initial, &loadError)) {
        for (const auto &value : initial.value("endpoints").toArray()) appendEndpoint(value.toObject());
    }
    refresh();
}

void RoutedDiscoveryDialog::appendEndpoint(const QJsonObject &endpoint)
{
    QScopedValueRollback<bool> guard(changingRows, true);
    const int row = table->rowCount();
    table->insertRow(row);
    auto enabled = new QCheckBox(table);
    enabled->setObjectName("routedDiscoveryEnabled");
    enabled->setAccessibleName(u("Увімкнено"));
    enabled->setChecked(endpoint.value("enabled").toBool());
    auto name = new QLineEdit(endpoint.value("name").toString(), table);
    name->setObjectName("routedDiscoveryName");
    name->setMaxLength(64);
    name->setAccessibleName(u("Назва"));
    auto role = new QComboBox(table);
    role->setObjectName("routedDiscoveryRole");
    role->addItem("Seed", "seed");
    role->addItem("Directory", "directory");
    role->setCurrentIndex(role->findData(endpoint.value("role").toString()));
    role->setToolTip(roleTip());
    role->setAccessibleName(u("Роль"));
    auto host = new QLineEdit(endpoint.value("host").toString(), table);
    host->setObjectName("routedDiscoveryHost");
    host->setMaxLength(15);
    host->setPlaceholderText("192.0.2.1");
    host->setToolTip(u("Числова IPv4-адреса; без DNS і сканування підмереж."));
    host->setAccessibleName("IPv4");
    auto port = new QSpinBox(table);
    port->setObjectName("routedDiscoveryPort");
    port->setRange(1, 65535);
    port->setValue(endpoint.value("port").toInt());
    port->setAccessibleName(u("Порт"));
    table->setCellWidget(row, 0, enabled);
    table->setCellWidget(row, 1, name);
    table->setCellWidget(row, 2, role);
    table->setCellWidget(row, 3, host);
    table->setCellWidget(row, 4, port);
    connect(enabled, &QCheckBox::toggled, this, &RoutedDiscoveryDialog::refresh);
    connect(name, &QLineEdit::textChanged, this, &RoutedDiscoveryDialog::refresh);
    connect(role, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &RoutedDiscoveryDialog::refresh);
    connect(host, &QLineEdit::textChanged, this, &RoutedDiscoveryDialog::refresh);
    connect(port, QOverload<int>::of(&QSpinBox::valueChanged), this, &RoutedDiscoveryDialog::refresh);
}

QJsonObject RoutedDiscoveryDialog::editedConfiguration() const
{
    QJsonArray endpoints;
    for (int row = 0; row < table->rowCount(); ++row) {
        endpoints.append(QJsonObject{
            {"enabled", static_cast<QCheckBox *>(table->cellWidget(row, 0))->isChecked()},
            {"name", static_cast<QLineEdit *>(table->cellWidget(row, 1))->text()},
            {"role", static_cast<QComboBox *>(table->cellWidget(row, 2))->currentData().toString()},
            {"host", static_cast<QLineEdit *>(table->cellWidget(row, 3))->text()},
            {"port", static_cast<QSpinBox *>(table->cellWidget(row, 4))->value()}});
    }
    return {{"version", 1}, {"endpoints", endpoints}};
}

QJsonObject RoutedDiscoveryDialog::configuration() const
{
    const auto result = editedConfiguration();
    return loadError.isEmpty() && routedDiscoveryConfigurationValid(result) ? result : QJsonObject{};
}

void RoutedDiscoveryDialog::refresh()
{
    if (changingRows) return;
    QString error = loadError;
    const bool valid = error.isEmpty() && routedDiscoveryConfigurationValid(editedConfiguration(), &error);
    errorLabel->setText(error);
    errorLabel->setVisible(!error.isEmpty());
    saveButton->setEnabled(valid);
    addButton->setEnabled(table->rowCount() < MaximumEndpoints);
    removeButton->setEnabled(table->currentRow() >= 0);
}

void RoutedDiscoveryDialog::accept()
{
    refresh();
    if (saveButton->isEnabled()) QDialog::accept();
}

}
