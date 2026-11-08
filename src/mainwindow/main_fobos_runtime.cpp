#include "main.h"
#include "appsettingsutils.h"
#include "../core/fobosruntimecaps.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

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
    auto *layout=new QFormLayout(box);layout->setRowWrapPolicy(QFormLayout::WrapAllRows);
    auto *note=new QLabel(uk
        ?QStringLiteral("Мітки нумерують IQ-блоки після захоплення FX3 і позначають перестроювання. Перехідні й несумісні блоки пропускаються. У Sync діє штатний потік. Доступні семплрейти вибираються в розділі приймача.")
        :QStringLiteral("Metadata numbers IQ blocks after FX3 capture and marks tuning transitions. Transitional and inconsistent blocks are omitted. Sync uses the native stream. Select available sample rates in the receiver section."),box);
    note->setWordWrap(true);layout->addRow(note);
    QSettings settings(persistentSettingsFilePath(),QSettings::IniFormat);
    auto *metadata=new QCheckBox(uk?QStringLiteral("Розширені мітки та відкидання перехідних IQ-блоків")
        :QStringLiteral("Extended metadata and transitional IQ rejection"),box);
    metadata->setObjectName(QStringLiteral("fobosStreamMetadata"));
    metadata->setToolTip(uk
        ?QStringLiteral("Вимкнено за замовчуванням: розширені мітки можуть суттєво затримувати перестроювання. Галочка вмикає і формування міток у приймачі, і перевірку перехідних IQ-блоків у програмі. Оцінка насичення АЦП та автозахист працюють окремо.")
        :QStringLiteral("Off by default: extended metadata can substantially delay retuning. Enables both receiver metadata generation and transitional IQ validation in the application. ADC saturation monitoring and overload protection operate separately."));
    metadata->setChecked(settings.value("receiver/streamMetadata",false).toBool());layout->addRow(metadata);
    auto *settle=new QSpinBox(box);settle->setObjectName(QStringLiteral("fobosSettleBlocks"));
    settle->setRange(0,256);settle->setValue(settings.value("receiver/settleBlocks",16).toInt());
    settle->setToolTip(uk?QStringLiteral("Кількість блоків по4096 логічних IQ-пар після перестроювання. 16 блоків ≈1,31 мс при50 MS/s. Сканування додатково використовує штатну фазу усталення.")
        :QStringLiteral("Blocks of4096 produced logical IQ pairs after tuning.16 blocks ≈1.31 ms at50 MS/s. Scanning additionally uses its native stable-phase indication."));
    layout->addRow(uk?QStringLiteral("Блоків усталення"):QStringLiteral("Settling blocks"),settle);
    auto *bias=new QComboBox(box);bias->setObjectName(QStringLiteral("fobosMixerBiasCode"));
    for(int i=1;i<=7;++i)bias->addItem(i==1?(uk?QStringLiteral("1 — штатна Agile"):QStringLiteral("1 — native Agile")):
        i==7?(uk?QStringLiteral("7 — як у стандартній libfobos"):QStringLiteral("7 — standard libfobos")):QString::number(i),i);
    bias->setCurrentIndex((std::clamp)(settings.value("receiver/mixerBiasCode",1).toInt(),1,7)-1);
    bias->setToolTip(uk?QStringLiteral("Код струму змішувачів, не дБ і не мА. Вплив на шум, інтермодуляцію та нагрів потребує порівняння. Початкове значення1 збережено.")
        :QStringLiteral("Mixer bias code, neither dB nor mA. Noise, intermodulation and heating require A/B qualification. Default1 is retained."));
    layout->addRow(uk?QStringLiteral("Код струму змішувача"):QStringLiteral("Mixer bias code"),bias);
    auto *dma=new QComboBox(box);dma->setObjectName(QStringLiteral("fobosDeviceDmaCount"));
    dma->addItem(uk?QStringLiteral("4 — штатне"):QStringLiteral("4 — native"),4);
    dma->addItem(uk?QStringLiteral("2 — експериментальне"):QStringLiteral("2 — experimental"),2);
    dma->setCurrentIndex(settings.value("receiver/deviceDmaCount",4).toInt()==2?1:0);
    dma->setToolTip(uk?QStringLiteral("Залиште4 для штатного потоку. У живому тесті2 буфери зменшували швидкість; це окремий експеримент."):
        QStringLiteral("Keep4 for the native stream. Live testing found lower throughput with2 buffers; this is a separate experiment."));
    layout->addRow(uk?QStringLiteral("DMA-буферів на джерело"):QStringLiteral("DMA buffers per producer"),dma);
    auto *status=new QLabel(box);status->setWordWrap(true);layout->addRow(status);
    auto *apply=new QPushButton(uk?QStringLiteral("Застосувати після «Стоп»"):QStringLiteral("Apply while stopped"),box);
    auto *read=new QPushButton(uk?QStringLiteral("Зчитати апаратні регістри RFFC…"):QStringLiteral("Read RFFC hardware registers…"),box);
    layout->addRow(apply);layout->addRow(read);
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
