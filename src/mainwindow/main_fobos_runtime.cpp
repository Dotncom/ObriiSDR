#include "main.h"
#include "appsettingsutils.h"
#include "../core/fobosruntimecaps.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QStandardItemModel>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

void YourClassName::openFobosDetailsDialog() {
    if (auto *existing = findChild<QDialog *>(QStringLiteral("fobosDetailsDialog"))) {
        existing->show(); existing->raise(); existing->activateWindow(); return;
    }

    const bool uk = normalizedUiLanguage(uiLanguage) == QStringLiteral("uk");
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("fobosDetailsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(uk ? QStringLiteral("Fobos — деталі й налаштування")
                              : QStringLiteral("Fobos — details and settings"));
    // Keep this a compact companion to the main window.  The scroll area
    // retains access to the less common controls on small displays.
    dialog->resize(560, 500);
    dialog->setMinimumSize(440, 390);
    auto *outer = new QVBoxLayout(dialog);
    outer->setContentsMargins(7, 7, 7, 7);
    outer->setSpacing(5);
    auto *scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget(scroll);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setSpacing(5);
    scroll->setWidget(content);
    outer->addWidget(scroll);

    auto *deviceBox = new QGroupBox(uk ? QStringLiteral("Приймач і прошивка")
                                      : QStringLiteral("Receiver and firmware"), content);
    auto *deviceLayout = new QVBoxLayout(deviceBox);
    auto *deviceInfo = new QLabel(deviceBox);
    deviceInfo->setWordWrap(true);
    auto *showFullReport = new QPushButton(uk ? QStringLiteral("Повний звіт про Fobos…")
                                              : QStringLiteral("Full Fobos report…"), deviceBox);
    deviceLayout->addWidget(deviceInfo);
    deviceLayout->addWidget(showFullReport);
    layout->addWidget(deviceBox);

    auto *ifBox = new QGroupBox(uk ? QStringLiteral("Проміжна частота")
                                  : QStringLiteral("Intermediate frequency"), content);
    auto *ifLayout = new QFormLayout(ifBox);
    ifLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto *corrected = new QCheckBox(uk ? QStringLiteral("Використовувати коригований план")
                                     : QStringLiteral("Use corrected plan"), ifBox);
    auto *plan = new QComboBox(ifBox);
    plan->addItem(uk ? QStringLiteral("Початковий / коригований план") : QStringLiteral("Base / corrected plan"), 0);
    plan->addItem(QStringLiteral("2350 MHz"), 1);
    plan->addItem(QStringLiteral("2450 MHz"), 2);
    plan->addItem(uk ? QStringLiteral("Автоматична таблиця ПЧ (50 MS/s)")
                     : QStringLiteral("Automatic IF table (50 MS/s)"), 3);
    plan->setCurrentIndex((std::max)(0, plan->findData(fobosIfTestMode)));
    auto *ifStatus = new QLabel(ifBox);
    ifStatus->setWordWrap(true);
    auto *ifRules = new QPushButton(uk ? QStringLiteral("Налаштувати правила ПЧ…")
                                      : QStringLiteral("Edit IF rules…"), ifBox);
    ifLayout->addRow(corrected);
    ifLayout->addRow(uk ? QStringLiteral("Режим ПЧ") : QStringLiteral("IF mode"), plan);
    ifLayout->addRow(ifStatus);
    ifLayout->addRow(ifRules);
    layout->addWidget(ifBox);

    auto *libraryBox = new QGroupBox(uk ? QStringLiteral("Бібліотека Fobos Dual")
                                       : QStringLiteral("Fobos Dual library"), content);
    auto *libraryLayout = new QVBoxLayout(libraryBox);
    auto *library = new QComboBox(libraryBox);
    library->addItems(uk ? QStringList{QStringLiteral("Agile Обрію · AUTO DMA"), QStringLiteral("Штатна Agile"),
        QStringLiteral("Standard Обрію"), QStringLiteral("Standard RigExpert · виправлений Stop")}
        : QStringList{QStringLiteral("Obrii Agile · AUTO DMA"), QStringLiteral("Stock Agile"),
        QStringLiteral("Obrii Standard"), QStringLiteral("RigExpert Standard · Stop fix")});
    auto *libraryStatus = new QLabel(libraryBox);
    libraryStatus->setWordWrap(true);
    libraryLayout->addWidget(library);
    libraryLayout->addWidget(libraryStatus);
    layout->addWidget(libraryBox);

    layout->addWidget(createFobosRuntimeControls(content));
    layout->addStretch(1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(showFullReport, &QPushButton::clicked, this, &YourClassName::listFobosDevices);
    connect(ifRules, &QPushButton::clicked, this, &YourClassName::openFobosIfEditor);

    const auto refresh = [this, deviceInfo, corrected, plan, ifStatus, ifRules, libraryBox, library, libraryStatus, uk] {
        const auto info = selectedFobosDeviceInfo();
        const bool idle = isIdle() && !(processor && processor->isRunning()) && info.present && !isNetworkClientMode();
        unsigned int config[16] = {};
        const int configResult = idle
            ? getActiveFobosReceiverConfigSafely(config, sizeof(config))
            : FOBOS_ERR_BUSY;
        const bool configAvailable = configResult == FOBOS_ERR_OK;
        deviceInfo->setText(info.present
            ? QStringLiteral("%1\n%2: %3\n%4: %5\n%6: %7%8")
                  .arg(info.label,
                       uk ? QStringLiteral("Виробник") : QStringLiteral("Manufacturer"), info.manufacturer,
                       uk ? QStringLiteral("Апаратна ревізія") : QStringLiteral("Hardware revision"), info.hardwareRevision,
                       uk ? QStringLiteral("Прошивка") : QStringLiteral("Firmware"), info.firmwareVersion,
                       configAvailable
                           ? (uk ? QStringLiteral("\nПротокол: r%1; можливості: 0x%2")
                                    .arg(config[2]).arg(config[3], 0, 16)
                                 : QStringLiteral("\nProtocol: r%1; capabilities: 0x%2")
                                    .arg(config[2]).arg(config[3], 0, 16))
                           : QString())
            : (uk ? QStringLiteral("Fobos не вибраний або недоступний.") : QStringLiteral("No local Fobos is selected or available.")));
        unsigned int mode = 0, state = 0, mhz = 0, caps = 0;
        const bool ifAvailable = idle && getActiveFobosIfPlanSafely(&mode, &state, &mhz, &caps) == FOBOS_ERR_OK;
        corrected->setEnabled(ifAvailable && state == 0);
        plan->setEnabled(ifAvailable && state == 0);
        if (auto *model = qobject_cast<QStandardItemModel *>(plan->model())) model->item(3)->setEnabled(ifAvailable && (caps & 2u));
        // The editor reports the precise protocol mismatch after activation;
        // keeping the button available avoids an unexplained disabled action.
        ifRules->setEnabled(ifAvailable);
        ifRules->setToolTip(configAvailable && (config[2] < 7 || !(config[3] & 8u))
            ? (uk ? QStringLiteral("Цей образ підтримує вибір ПЧ, але не редаговану таблицю правил. Потрібен протокол r7 з підтримкою таблиці.")
                  : QStringLiteral("This image supports IF selection but not an editable rules table. Protocol r7 with table support is required."))
            : QString());
        ifStatus->setText(ifAvailable
            ? (uk ? QStringLiteral("Поточна ПЧ: %1 МГц; можливості: 0x%2%3.")
                    .arg(mhz).arg(caps, 0, 16)
                    .arg(configAvailable && (config[2] < 7 || !(config[3] & 8u))
                         ? QStringLiteral("; редаговані правила недоступні для цього протоколу") : QString())
                  : QStringLiteral("Active IF: %1 MHz; capabilities: 0x%2%3.")
                    .arg(mhz).arg(caps, 0, 16)
                    .arg(configAvailable && (config[2] < 7 || !(config[3] & 8u))
                         ? QStringLiteral("; editable rules are unavailable for this protocol") : QString()))
            : (uk ? QStringLiteral("Зупиніть приймання для читання та зміни ПЧ.")
                  : QStringLiteral("Stop reception to read or change IF settings.")));
        const QSignalBlocker correctedBlocker(corrected);
        corrected->setChecked(useCorrectedFobosCommands);
        const bool dual = info.present && !info.serial.isEmpty() && isObriiDualFirmware(info.hardwareRevision, info.firmwareVersion);
        libraryBox->setVisible(dual || dualFobosSwitchPending);
        library->setEnabled(dual && idle && !dualFobosSwitchPending);
        if (dual && !dualFobosSwitchPending) {
            const int apiIndex = info.apiKind == FobosApiKind::Standard ? 2 : 0;
            const QSignalBlocker blocker(library);
            library->setCurrentIndex(apiIndex + (dualFobosStockLibrary.value(info.serial) ? 1 : 0));
            if (auto *model = qobject_cast<QStandardItemModel *>(library->model())) {
                model->item(1)->setEnabled(fobosStockLibraryAvailable(FobosApiKind::Agile));
                model->item(3)->setEnabled(fobosStockLibraryAvailable(FobosApiKind::Standard));
            }
        }
        libraryStatus->setText(dualFobosSwitchStatus.isEmpty()
            ? (uk ? QStringLiteral("Зупиніть приймання перед зміною бібліотеки.") : QStringLiteral("Stop reception before changing the library."))
            : dualFobosSwitchStatus);
    };
    connect(corrected, &QCheckBox::toggled, dialog, [this, corrected](bool checked) {
        const bool previous = useCorrectedFobosCommands;
        if (checked == previous || !isIdle() || !hasActiveFobosDevice()) return;
        useCorrectedFobosCommands = checked;
        if (!prepareFobosSessionFromSettings(QStringLiteral("IF mode change"))) {
            useCorrectedFobosCommands = previous;
            const QSignalBlocker blocker(corrected); corrected->setChecked(previous);
        }
        savePersistentSettings();
    });
    connect(plan, QOverload<int>::of(&QComboBox::activated), dialog, [this, plan](int index) {
        const int previous = fobosIfTestMode;
        const int requested = plan->itemData(index).toInt();
        if (requested == previous || !isIdle() || !hasActiveFobosDevice()) return;
        fobosIfTestMode = requested;
        if (!prepareFobosSessionFromSettings(QStringLiteral("global IF selection"))) {
            fobosIfTestMode = previous;
            const QSignalBlocker blocker(plan); plan->setCurrentIndex((std::max)(0, plan->findData(previous)));
        }
        savePersistentSettings();
    });
    connect(library, QOverload<int>::of(&QComboBox::activated), dialog, [this](int index) {
        selectDualFobosLibrary(index >= 2 ? FobosApiKind::Standard : FobosApiKind::Agile, (index % 2) != 0);
    });
    auto *timer = new QTimer(dialog);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, dialog, refresh);
    refresh(); timer->start(); dialog->show();
}

bool YourClassName::applyFobosRuntimeSettings() {
    unsigned int cfg[16]={};
    const int probe=getActiveFobosReceiverConfigSafely(cfg,sizeof(cfg));
    fobosTuneRequiresRestart = probe == FOBOS_ERR_OK && (cfg[3] & FobosRuntimeCaps::RestartTune);
    fobosRawLiveTune = fobosTuneRequiresRestart && (cfg[3] & FobosRuntimeCaps::RawLiveTune);
    if(probe==FOBOS_ERR_UNSUPPORTED){fobosActiveMixerBiasCode=0;return true;}
    if(probe!=FOBOS_ERR_OK)return false;
    QSettings settings(persistentSettingsFilePath(),QSettings::IniFormat);
    // Receiver-side metadata stamping is diagnostic opt-in: it can delay live
    // RF controls even when the host decoder itself is inexpensive.
    const bool metadata=settings.value("receiver/streamMetadata",false).toBool();
    const unsigned int settle=(std::clamp)(settings.value("receiver/settleBlocks",16).toUInt(),0u,256u);
    const unsigned int bias=(std::clamp)(settings.value("receiver/mixerBiasCode",1).toUInt(),1u,7u);
    const unsigned int count=settings.value("receiver/deviceDmaCount",4).toUInt()==2?2u:4u;
    if((cfg[3]&16u) && setActiveFobosStreamMetadataSafely(metadata?1u:0u,settle)!=FOBOS_ERR_OK)return false;
    if((cfg[3]&128u) && setActiveFobosDmaCountSafely(count)!=FOBOS_ERR_OK)return false;
    if((cfg[3]&64u) && pendingSettings.inputMode==INPUT_RF && cfg[10]!=bias) {
        if(setActiveFobosMixerBiasSafely(bias)!=FOBOS_ERR_OK)return false;
        double actual=pendingSettings.centerFrequency;
        if(setCalibratedActiveFrequencySafely(pendingSettings.centerFrequency,&actual)!=FOBOS_ERR_OK)return false;
        pendingSettings.actualFrequency=actual;
    }
    unsigned int confirmed[16]={};
    if(getActiveFobosReceiverConfigSafely(confirmed,sizeof(confirmed))!=FOBOS_ERR_OK)return false;
    fobosActiveMixerBiasCode=confirmed[10];
    qInfo() << "[Fobos runtime] configured" << "metadataRequested" << metadata
            << "metadataConfirmed" << confirmed[8] << "settleBlocks" << confirmed[9];
    return true;
}

QWidget *YourClassName::createFobosRuntimeControls(QWidget *parent) {
    const bool uk=uiLanguage==QStringLiteral("uk");
    auto *box=new QGroupBox(uk?QStringLiteral("Метрики й експериментальні режими Фобоса")
        :QStringLiteral("Fobos telemetry and experimental modes"),parent);
    auto *layout=new QGridLayout(box);
    layout->setColumnStretch(1, 1);
    layout->setColumnStretch(3, 1);
    auto *note=new QLabel(uk
        ?QStringLiteral("Мітки нумерують IQ-блоки після захоплення FX3 і позначають перестроювання. Перехідні й несумісні блоки пропускаються. У Sync діє штатний потік. Доступні семплрейти вибираються в розділі приймача.")
        :QStringLiteral("Metadata numbers IQ blocks after FX3 capture and marks tuning transitions. Transitional and inconsistent blocks are omitted. Sync uses the native stream. Select available sample rates in the receiver section."),box);
    note->setWordWrap(true); note->setVisible(false);
    QSettings settings(persistentSettingsFilePath(),QSettings::IniFormat);
    auto *metadata=new QCheckBox(uk?QStringLiteral("Розширені мітки та відкидання перехідних IQ-блоків")
        :QStringLiteral("Extended metadata and transitional IQ rejection"),box);
    metadata->setObjectName(QStringLiteral("fobosStreamMetadata"));
    metadata->setToolTip(uk
        ?QStringLiteral("Вимкнено за замовчуванням: розширені мітки можуть суттєво затримувати перестроювання. Галочка вмикає і формування міток у приймачі, і перевірку перехідних IQ-блоків у програмі. Оцінка насичення АЦП та автозахист працюють окремо.")
        :QStringLiteral("Off by default: extended metadata can substantially delay retuning. Enables both receiver metadata generation and transitional IQ validation in the application. ADC saturation monitoring and overload protection operate separately."));
    metadata->setChecked(settings.value("receiver/streamMetadata",false).toBool());
    layout->addWidget(metadata, 0, 0, 1, 4);
    auto *settle=new QSpinBox(box);settle->setObjectName(QStringLiteral("fobosSettleBlocks"));
    settle->setRange(0,256);settle->setValue(settings.value("receiver/settleBlocks",16).toInt());
    settle->setToolTip(uk?QStringLiteral("Кількість блоків по4096 логічних IQ-пар після перестроювання. 16 блоків ≈1,31 мс при50 MS/s. Сканування додатково використовує штатну фазу усталення.")
        :QStringLiteral("Blocks of4096 produced logical IQ pairs after tuning.16 blocks ≈1.31 ms at50 MS/s. Scanning additionally uses its native stable-phase indication."));
    layout->addWidget(new QLabel(uk?QStringLiteral("Блоків усталення"):QStringLiteral("Settling blocks"), box), 1, 0);
    layout->addWidget(settle, 1, 1);
    auto *bias=new QComboBox(box);bias->setObjectName(QStringLiteral("fobosMixerBiasCode"));
    for(int i=1;i<=7;++i)bias->addItem(i==1?(uk?QStringLiteral("1 — штатна Agile"):QStringLiteral("1 — native Agile")):
        i==7?(uk?QStringLiteral("7 — як у стандартній libfobos"):QStringLiteral("7 — standard libfobos")):QString::number(i),i);
    bias->setCurrentIndex((std::clamp)(settings.value("receiver/mixerBiasCode",1).toInt(),1,7)-1);
    bias->setToolTip(uk?QStringLiteral("Код струму змішувачів, не дБ і не мА. Вплив на шум, інтермодуляцію та нагрів потребує порівняння. Початкове значення1 збережено.")
        :QStringLiteral("Mixer bias code, neither dB nor mA. Noise, intermodulation and heating require A/B qualification. Default1 is retained."));
    layout->addWidget(new QLabel(uk?QStringLiteral("Код змішувача"):QStringLiteral("Mixer bias"), box), 1, 2);
    layout->addWidget(bias, 1, 3);
    auto *dma=new QComboBox(box);dma->setObjectName(QStringLiteral("fobosDeviceDmaCount"));
    dma->addItem(uk?QStringLiteral("4 — штатне"):QStringLiteral("4 — native"),4);
    dma->addItem(uk?QStringLiteral("2 — експериментальне"):QStringLiteral("2 — experimental"),2);
    dma->setCurrentIndex(settings.value("receiver/deviceDmaCount",4).toInt()==2?1:0);
    dma->setToolTip(uk?QStringLiteral("Залиште4 для штатного потоку. У живому тесті2 буфери зменшували швидкість; це окремий експеримент."):
        QStringLiteral("Keep4 for the native stream. Live testing found lower throughput with2 buffers; this is a separate experiment."));
    layout->addWidget(new QLabel(uk?QStringLiteral("DMA-буферів"):QStringLiteral("DMA buffers"), box), 2, 0);
    layout->addWidget(dma, 2, 1);
    auto *status=new QLabel(box);status->setWordWrap(true);layout->addWidget(status, 2, 2, 1, 2);
    auto *apply=new QPushButton(uk?QStringLiteral("Застосувати після «Стоп»"):QStringLiteral("Apply while stopped"),box);
    auto *read=new QPushButton(uk?QStringLiteral("Зчитати апаратні регістри RFFC…"):QStringLiteral("Read RFFC hardware registers…"),box);
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->addWidget(apply);
    actions->addWidget(read);
    layout->addLayout(actions, 3, 0, 1, 4);
    unsigned int cfg[16]={};const bool available=isIdle() && !(processor && processor->isRunning()) &&
        hasActiveFobosDevice() && !isNetworkClientMode() && getActiveFobosReceiverConfigSafely(cfg,sizeof(cfg))==FOBOS_ERR_OK;
    const unsigned int caps=available?cfg[3]:0;
    status->setProperty("runtimeCaps",caps);
    const auto refresh=[this,metadata,settle,bias,dma,apply,read,status,uk] {
        const bool idle=isIdle() && !(processor && processor->isRunning()) && hasActiveFobosDevice() && !isNetworkClientMode();
        // The settings window stays open while the main UI can switch devices.
        // Re-probe only while stopped, once per second; never poll streaming USB.
        const int ticks=status->property("runtimeProbeTicks").toInt()+1;
        status->setProperty("runtimeProbeTicks",ticks%4);
        if(idle && ticks>=4) {
            unsigned int current[16]={};
            const unsigned int supported=getActiveFobosReceiverConfigSafely(current,sizeof(current))==FOBOS_ERR_OK?current[3]:0;
            if(supported!=status->property("runtimeCaps").toUInt())status->setProperty("hasResult",false);
            status->setProperty("runtimeCaps",supported);
        }
        const unsigned int caps=status->property("runtimeCaps").toUInt();
        metadata->setEnabled(idle && (caps&16u));settle->setEnabled(idle && (caps&16u));
        bias->setEnabled(idle && (caps&64u) && pendingSettings.inputMode==INPUT_RF);
        dma->setEnabled(idle && (caps&128u));read->setEnabled(idle && (caps&64u) && pendingSettings.inputMode==INPUT_RF);
        apply->setEnabled(idle && (caps&(16u|64u|128u)));
        if(!status->property("hasResult").toBool())status->setText((caps&(16u|64u|128u))?(idle?(uk?QStringLiteral("Приймач готовий до зміни параметрів."):QStringLiteral("Receiver ready for configuration.")):
            (uk?QStringLiteral("Перед зміною параметрів зупиніть приймання та сканування."):QStringLiteral("Stop reception/scanning before changing configuration."))):
            (uk?QStringLiteral("Ці можливості недоступні для поточного приймача/прошивки; штатне керування збережено."):QStringLiteral("Unavailable for this receiver/firmware; native controls retained.")));
    };
    connect(apply,&QPushButton::clicked,box,[this,metadata,settle,bias,dma,status,uk] {
        if(!isIdle() || !hasActiveFobosDevice())return;
        status->setProperty("hasResult",true);
        unsigned int current[16]={};
        if(getActiveFobosReceiverConfigSafely(current,sizeof(current))!=FOBOS_ERR_OK || !(current[3]&(16u|64u|128u))) {
            status->setText(uk?QStringLiteral("Ці можливості недоступні для поточного приймача/прошивки."):
                QStringLiteral("These features are unavailable for the current receiver/firmware."));
            return;
        }
        QSettings settings(persistentSettingsFilePath(),QSettings::IniFormat);
        const QStringList keys={"receiver/streamMetadata","receiver/settleBlocks","receiver/mixerBiasCode","receiver/deviceDmaCount"};
        QVariantList old;QVector<bool> existed;for(const auto &key:keys){old<<settings.value(key);existed<<settings.contains(key);}
        settings.setValue(keys[0],metadata->isChecked());settings.setValue(keys[1],settle->value());
        settings.setValue(keys[2],bias->currentData());settings.setValue(keys[3],dma->currentData());
        settings.sync();
        if(settings.status()!=QSettings::NoError || !prepareFobosSessionFromSettings(QStringLiteral("receiver runtime configuration"))) {
            for(int i=0;i<keys.size();++i) {if(existed[i])settings.setValue(keys[i],old[i]);else settings.remove(keys[i]);}settings.sync();
            const bool restored=prepareFobosSessionFromSettings(QStringLiteral("receiver runtime rollback"));
            status->setText(uk?QStringLiteral("Не вдалося застосувати. Відновлення попередніх параметрів: %1.").arg(restored?QStringLiteral("підтверджено"):QStringLiteral("не підтверджено")):
                QStringLiteral("Apply failed. Previous configuration restored: %1.").arg(restored));
        } else {
            populateSampleRates();
            status->setText(uk?QStringLiteral("Застосовано й підтверджено приймачем."):QStringLiteral("Applied and confirmed by receiver."));
        }
    });
    connect(read,&QPushButton::clicked,box,[this,box,uk] {
        if(!isIdle())return;unsigned short regs[32]={};
        const int result=getActiveFobosRffcRegistersSafely(regs,sizeof(regs));
        auto *dialog=new QDialog(box);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->resize(460,500);
        dialog->setWindowTitle(uk?QStringLiteral("Апаратне читання RFFC"):QStringLiteral("RFFC hardware readback"));
        auto *layout=new QVBoxLayout(dialog);auto *text=new QPlainTextEdit(dialog);text->setReadOnly(true);
        QString dump=result==FOBOS_ERR_OK?(uk?QStringLiteral("Апаратні регістри. PLL LOCK ще не інтерпретується.\n"):QStringLiteral("Hardware registers. PLL LOCK is not interpreted.\n")):
            QStringLiteral("Read failed: %1\n").arg(result);
        if(result==FOBOS_ERR_OK)for(int i=0;i<32;++i)dump+=QStringLiteral("%1: %2\n").arg(i,2,16,QChar('0')).arg(regs[i],4,16,QChar('0'));
        text->setPlainText(dump);layout->addWidget(text);auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close,dialog);
        connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::close);layout->addWidget(buttons);dialog->show();
    });
    auto *timer=new QTimer(box);timer->setInterval(250);connect(timer,&QTimer::timeout,box,refresh);refresh();timer->start();return box;
}
