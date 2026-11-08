#include "main.h"
#include "appsettingsutils.h"
#include "core/fobosruntimeif.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QDebug>
#include <QSettings>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>

namespace {
const QString CustomEnabledKey = QStringLiteral("receiver/customIfRulesEnabled");
const QString CustomRulesKey = QStringLiteral("receiver/customIfRules");

struct IfRulesSnapshot {
    FobosRuntimeIf::Frame frame;
    bool custom = false;
};

int readIfRulesSnapshot(IfRulesSnapshot *output) {
    unsigned int before[16] = {}, after[16] = {};
    int result = getActiveFobosReceiverConfigSafely(before, sizeof(before));
    if (result != FOBOS_ERR_OK) return result;
    if (before[0] != OBRII_CONFIG_MAGIC || before[1] != 1 || before[2] < 7 ||
        !(before[3] & OBRII_CAP_TABLE)) return FOBOS_ERR_UNSUPPORTED;
    if (before[4] > 1 || before[5] > OBRII_MAX_IF_RULES) return FOBOS_ERR_CONTROL;
    FobosRuntimeIf::Frame frame(static_cast<int>(OBRII_TABLE_WORDS), 0);
    unsigned int bytes = OBRII_TABLE_WORDS * sizeof(quint32);
    result = getActiveFobosIfTableSafely(frame.data(), &bytes);
    if (result != FOBOS_ERR_OK) return result;
    result = getActiveFobosReceiverConfigSafely(after, sizeof(after));
    if (result != FOBOS_ERR_OK) return result;
    // Keep table data and its activation flag in the same verified generation.
    for (int index = 4; index <= 7; ++index)
        if (before[index] != after[index]) return FOBOS_ERR_CONTROL;
    if (!FobosRuntimeIf::validFrame(frame) || bytes != FobosRuntimeIf::frameBytes(frame) ||
        before[5] != frame[2] || before[6] != frame[3]) return FOBOS_ERR_CONTROL;
    output->frame = frame;
    output->custom = before[4] != 0;
    return FOBOS_ERR_OK;
}

bool snapshotMatches(const IfRulesSnapshot &snapshot, bool custom,
    const FobosRuntimeIf::Frame &frame) {
    return snapshot.custom == custom && (!custom || FobosRuntimeIf::sameRules(snapshot.frame, frame));
}

bool storedIfRules(QSettings &settings, FobosRuntimeIf::Frame *frame, QString *error = nullptr) {
    const QByteArray json = settings.value(CustomRulesKey).toByteArray();
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("The saved IF rules are not a JSON object.");
        return false;
    }
    return FobosRuntimeIf::frameFromJson(document.object(), frame, error);
}

int selectIfRules(bool custom, const FobosRuntimeIf::Frame &frame) {
    return custom ? setActiveFobosIfTableSafely(frame.constData(), FobosRuntimeIf::frameBytes(frame))
                  : resetActiveFobosIfTableSafely();
}
} // namespace

bool YourClassName::applyStoredFobosIfRules(bool *changed) {
    if (changed) *changed = false;
    IfRulesSnapshot current;
    int result = readIfRulesSnapshot(&current);
    if (result == FOBOS_ERR_UNSUPPORTED) {
        // Keep the preference for a compatible receiver without issuing an OUT
        // command or preventing a stock/older receiver session from starting.
        fobosActiveIfRulesFrame = FobosRuntimeIf::builtinFrame();
        fobosCustomIfRulesActive = false;
        return true;
    }
    if (result != FOBOS_ERR_OK) {
        qWarning() << "[Fobos IF rules] cannot read receiver table" << result;
        return false;
    }
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    bool custom = settings.value(CustomEnabledKey, false).toBool();
    FobosRuntimeIf::Frame requested = FobosRuntimeIf::builtinFrame();
    if (custom) {
        QString error;
        if (!storedIfRules(settings, &requested, &error)) {
            // A malformed saved file must not send unchecked frequency rules.
            // Preserve it for correction and start with the built-in table.
            custom = false;
            qWarning() << "[Fobos IF rules] invalid saved table; using built-in rules" << error;
        }
    }
    if (!snapshotMatches(current, custom, requested)) {
        result = selectIfRules(custom, requested);
        IfRulesSnapshot verified;
        if (result == FOBOS_ERR_OK) result = readIfRulesSnapshot(&verified);
        if (result != FOBOS_ERR_OK || !snapshotMatches(verified, custom, requested)) {
            fobosActiveIfRulesFrame.clear();
            fobosCustomIfRulesActive = false;
            qWarning() << "[Fobos IF rules] table application/readback failed" << result;
            return false;
        }
        current = verified;
        if (changed) *changed = true;
    }
    fobosActiveIfRulesFrame = current.frame;
    fobosCustomIfRulesActive = current.custom;
    return true;
}

void YourClassName::openFobosIfEditor() {
    const QString objectName = QStringLiteral("fobosIfRulesEditorDialog");
    if (auto *existing = findChild<QDialog *>(objectName)) {
        existing->show(); existing->raise(); existing->activateWindow(); return;
    }
    const bool uk = uiLanguage == QStringLiteral("uk");
    const QString title = uk ? QStringLiteral("Правила автоматичного вибору ПЧ")
                             : QStringLiteral("Automatic IF rules");
    IfRulesSnapshot actual;
    const int probe = !hasActiveFobosDevice() || isNetworkClientMode()
        ? FOBOS_ERR_NOT_OPEN : readIfRulesSnapshot(&actual);
    if (probe != FOBOS_ERR_OK) {
        QMessageBox::information(this, title, uk
            ? QStringLiteral("Потрібен локальний Фобос із підтримкою завантажуваних правил ПЧ: Agile Obrii ревізії7 або відповідна стандартна бібліотека. Зупиніть приймання та відкрийте приймач.")
            : QStringLiteral("Use a local Fobos with uploadable IF rules: Obrii Agile revision7 or a compatible standard library. Stop reception and open the receiver."));
        return;
    }
    fobosActiveIfRulesFrame = actual.frame;
    fobosCustomIfRulesActive = actual.custom;
    QSettings saved(persistentSettingsFilePath(), QSettings::IniFormat);
    FobosRuntimeIf::Frame draft;
    const bool savedValid = storedIfRules(saved, &draft);
    if (!savedValid) draft = actual.frame;
    const bool draftCustom = saved.value(CustomEnabledKey, actual.custom).toBool();

    auto *dialog = new QDialog(this);
    dialog->setObjectName(objectName);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    dialog->setWindowTitle(title);
    dialog->resize(810, 540);
    dialog->setMinimumWidth(620);
    auto *layout = new QVBoxLayout(dialog);
    auto *hint = new QLabel(uk
        ? QStringLiteral("Правила діють лише в режимі «Автоматично», при Fs49–51 MS/s. Межі: [від; до), з точністю1 Гц. Не більше24 інтервалів без перекриттів, у межах50–6900 МГц; RF-обхід2300,5–2550,5 МГц не змінюється. Ручні2350/2450 мають пріоритет. Для застосування зупиніть приймання й сканування.")
        : QStringLiteral("Rules apply only in Automatic mode at49–51 MS/s. Intervals are [from; to), with1 Hz precision. Up to24 non-overlapping intervals within50–6900 MHz;2300.5–2550.5 MHz RF bypass remains unchanged. Manual2350/2450 takes priority. Stop reception and scanning before applying."), dialog);
    hint->setWordWrap(true); layout->addWidget(hint);
    auto *enabled = new QCheckBox(uk ? QStringLiteral("Використовувати власні правила")
                                   : QStringLiteral("Use custom rules"), dialog);
    enabled->setObjectName(QStringLiteral("fobosCustomIfRulesEnabled"));
    enabled->setChecked(draftCustom);
    layout->addWidget(enabled);
    auto *table = new QTableWidget(0, 3, dialog);
    table->setObjectName(QStringLiteral("fobosCustomIfRulesTable"));
    table->setHorizontalHeaderLabels(uk
        ? QStringList{QStringLiteral("Від, МГц"), QStringLiteral("До, МГц (не включно)"), QStringLiteral("ПЧ, МГц")}
        : QStringList{QStringLiteral("From, MHz"), QStringLiteral("To, MHz (exclusive)"), QStringLiteral("IF, MHz")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(table, 1);
    const auto appendRule = [table](const FobosRuntimeIf::Rule &rule) {
        const int row = table->rowCount(); table->insertRow(row);
        for (int column = 0; column < 2; ++column) {
            auto *frequency = new QDoubleSpinBox(table);
            frequency->setDecimals(6); frequency->setRange(50.0, 6900.000001);
            frequency->setSingleStep(1.0); frequency->setKeyboardTracking(false);
            frequency->setValue((column ? rule.toHz : rule.fromHz) / 1e6);
            table->setCellWidget(row, column, frequency);
        }
        auto *ifMhz = new QComboBox(table);
        ifMhz->addItem(QStringLiteral("2350"), 2350);
        ifMhz->addItem(QStringLiteral("2450"), 2450);
        ifMhz->setCurrentIndex(rule.ifMhz == 2450 ? 1 : 0);
        table->setCellWidget(row, 2, ifMhz);
    };
    const auto populate = [table, appendRule](const FobosRuntimeIf::Frame &frame) {
        table->setRowCount(0);
        for (const auto &rule : FobosRuntimeIf::rules(frame)) appendRule(rule);
    };
    populate(draft);
    auto *actions = new QHBoxLayout;
    auto *add = new QPushButton(uk ? QStringLiteral("Додати") : QStringLiteral("Add"), dialog);
    auto *remove = new QPushButton(uk ? QStringLiteral("Видалити рядок") : QStringLiteral("Delete row"), dialog);
    auto *defaults = new QPushButton(uk ? QStringLiteral("Вбудовані правила") : QStringLiteral("Built-in rules"), dialog);
    actions->addWidget(add); actions->addWidget(remove); actions->addWidget(defaults); actions->addStretch();
    layout->addLayout(actions);
    auto *status = new QLabel(dialog); status->setWordWrap(true);
    status->setObjectName(QStringLiteral("fobosCustomIfRulesStatus"));
    const auto describe = [uk](const IfRulesSnapshot &snapshot) {
        return (uk ? QStringLiteral("Застосовано: %1, правил %2. Правила зберігаються в Обрії та відновлюються при підключенні; перепрошивка не потрібна.")
                   : QStringLiteral("Applied: %1, %2 rules. Obrii saves and restores them on connection; no firmware update is needed."))
            .arg(snapshot.custom ? (uk ? QStringLiteral("власна таблиця") : QStringLiteral("custom table"))
                                 : (uk ? QStringLiteral("вбудована таблиця") : QStringLiteral("built-in table")))
            .arg(snapshot.frame[2]);
    };
    status->setText(describe(actual)); layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, dialog);
    auto *apply = buttons->button(QDialogButtonBox::Apply);
    apply->setObjectName(QStringLiteral("fobosApplyCustomIfRules"));
    apply->setText(uk ? QStringLiteral("Застосувати") : QStringLiteral("Apply"));
    buttons->button(QDialogButtonBox::Close)->setText(uk ? QStringLiteral("Закрити") : QStringLiteral("Close"));
    layout->addWidget(buttons);
    auto applying = std::make_shared<bool>(false);
    const QPointer<QDialog> editor(dialog);
    connect(add, &QPushButton::clicked, dialog, [table, appendRule] {
        if (table->rowCount() >= static_cast<int>(OBRII_MAX_IF_RULES)) return;
        appendRule({50000000ULL, 51000000ULL, 2350}); table->selectRow(table->rowCount() - 1);
    });
    connect(remove, &QPushButton::clicked, dialog, [table] {
        if (table->currentRow() >= 0) table->removeRow(table->currentRow());
    });
    connect(defaults, &QPushButton::clicked, dialog, [populate, enabled] {
        populate(FobosRuntimeIf::builtinFrame()); enabled->setChecked(false);
    });
    const auto collect = [table, uk](FobosRuntimeIf::Frame *frame, QString *error) {
        QVector<FobosRuntimeIf::Rule> rows;
        for (int index = 0; index < table->rowCount(); ++index) {
            const auto *from = qobject_cast<QDoubleSpinBox *>(table->cellWidget(index, 0));
            const auto *to = qobject_cast<QDoubleSpinBox *>(table->cellWidget(index, 1));
            const auto *mhz = qobject_cast<QComboBox *>(table->cellWidget(index, 2));
            if (!from || !to || !mhz) return false;
            rows.append({static_cast<quint64>(std::llround(from->value() * 1e6)),
                static_cast<quint64>(std::llround(to->value() * 1e6)), mhz->currentData().toUInt()});
        }
        std::sort(rows.begin(), rows.end(), [](const FobosRuntimeIf::Rule &a, const FobosRuntimeIf::Rule &b) {
            return a.fromHz < b.fromHz;
        });
        QString reason;
        if (!FobosRuntimeIf::frameForRules(rows, frame, &reason)) {
            *error = uk ? QStringLiteral("Перевірте межі: початок має бути менший за кінець, інтервали не повинні перекриватися або заходити в RF-обхід2300,5–2550,5 МГц. Допустима ПЧ2350 або2450, максимум24 правила.") : reason;
            return false;
        }
        return true;
    };
    connect(apply, &QPushButton::clicked, dialog,
        [this, editor, enabled, status, applying, collect, populate, describe, uk] {
        if (!editor || *applying || !isIdle() || !hasActiveFobosDevice() || isNetworkClientMode()) return;
        FobosRuntimeIf::Frame requested;
        QString validation;
        const bool custom = enabled->isChecked();
        const bool draftValid = collect(&requested, &validation);
        if (custom && !draftValid) { status->setText(validation); return; }
        IfRulesSnapshot previous;
        int result = readIfRulesSnapshot(&previous);
        if (result != FOBOS_ERR_OK) {
            status->setText(uk ? QStringLiteral("Не вдалося прочитати поточну таблицю. Нічого не застосовано.")
                              : QStringLiteral("Cannot read the current table. Nothing was applied.")); return;
        }
        QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
        const bool hadEnabled = settings.contains(CustomEnabledKey), hadRules = settings.contains(CustomRulesKey);
        const QVariant oldEnabled = settings.value(CustomEnabledKey), oldRules = settings.value(CustomRulesKey);
        *applying = true;
        if (!snapshotMatches(previous, custom, requested)) result = selectIfRules(custom, requested);
        IfRulesSnapshot verified;
        if (result == FOBOS_ERR_OK) result = readIfRulesSnapshot(&verified);
        if (result == FOBOS_ERR_OK && !snapshotMatches(verified, custom, requested)) result = FOBOS_ERR_CONTROL;
        if (result == FOBOS_ERR_OK) {
            settings.setValue(CustomEnabledKey, custom);
            if (draftValid) settings.setValue(CustomRulesKey,
                QJsonDocument(FobosRuntimeIf::frameToJson(requested)).toJson(QJsonDocument::Compact));
            settings.sync();
            if (settings.status() != QSettings::NoError) result = FOBOS_ERR_CONTROL;
        }
        if (result == FOBOS_ERR_OK) {
            fobosActiveIfRulesFrame = verified.frame;
            fobosCustomIfRulesActive = verified.custom;
            hardwareSettingsApplied = false;
            if (!prepareFobosSessionFromSettings(QStringLiteral("custom IF rules"))) result = FOBOS_ERR_CONTROL;
            if (result == FOBOS_ERR_OK) {
                IfRulesSnapshot afterTune;
                result = readIfRulesSnapshot(&afterTune);
                if (result == FOBOS_ERR_OK && !snapshotMatches(afterTune, custom, requested)) result = FOBOS_ERR_CONTROL;
                if (result == FOBOS_ERR_OK) verified = afterTune;
            }
        }
        if (result != FOBOS_ERR_OK) {
            if (hadEnabled) settings.setValue(CustomEnabledKey, oldEnabled); else settings.remove(CustomEnabledKey);
            if (hadRules) settings.setValue(CustomRulesKey, oldRules); else settings.remove(CustomRulesKey);
            settings.sync();
            const bool settingsRestored = settings.status() == QSettings::NoError;
            // A failed preparation may have closed the USB handle. Reopen with
            // the previous saved settings before restoring the exact snapshot.
            if (!hasActiveFobosDevice()) prepareFobosSessionFromSettings(QStringLiteral("custom IF rollback reopen"));
            bool restored = settingsRestored && hasActiveFobosDevice() && selectIfRules(previous.custom, previous.frame) == FOBOS_ERR_OK;
            IfRulesSnapshot rollback;
            restored = restored && readIfRulesSnapshot(&rollback) == FOBOS_ERR_OK &&
                snapshotMatches(rollback, previous.custom, previous.frame);
            double actualFrequency = pendingSettings.centerFrequency;
            restored = restored && setCalibratedActiveFrequencySafely(pendingSettings.centerFrequency, &actualFrequency) == FOBOS_ERR_OK;
            if (restored) {
                fobosActiveIfRulesFrame = rollback.frame;
                fobosCustomIfRulesActive = rollback.custom;
                pendingSettings.actualFrequency = actualFrequency;
            } else { fobosActiveIfRulesFrame.clear(); fobosCustomIfRulesActive = false; }
            hardwareSettingsApplied = false;
            qWarning() << "[Fobos IF rules] apply failed; snapshot rollback" << restored << "result" << result;
            if (editor) status->setText(restored
                ? (uk ? QStringLiteral("Не вдалося застосувати правила; попередню таблицю та налаштування відновлено.")
                      : QStringLiteral("Could not apply rules; the previous table and settings were restored."))
                : (uk ? QStringLiteral("Не вдалося застосувати або підтвердити відновлення таблиці. Приймання залишається зупиненим.")
                      : QStringLiteral("Could not apply rules or confirm table restoration. Reception remains stopped.")));
        } else {
            fobosActiveIfRulesFrame = verified.frame;
            fobosCustomIfRulesActive = verified.custom;
            if (editor) status->setText(describe(verified));
            if (editor && draftValid) populate(requested);
            updateSpurSuppressionStatus(); refreshFobosIfStatus();
        }
        *applying = false;
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    auto *poll = new QTimer(dialog); poll->setInterval(250);
    const auto updateButtons = [this, applying, table, add, remove, defaults, apply] {
        const bool idle = !*applying && isIdle();
        table->setEnabled(idle); add->setEnabled(idle && table->rowCount() < static_cast<int>(OBRII_MAX_IF_RULES));
        remove->setEnabled(idle && table->currentRow() >= 0); defaults->setEnabled(idle);
        apply->setEnabled(idle && hasActiveFobosDevice() && !isNetworkClientMode());
    };
    connect(poll, &QTimer::timeout, dialog, updateButtons);
    updateButtons(); poll->start(); dialog->show();
}
