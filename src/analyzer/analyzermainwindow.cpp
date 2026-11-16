#include "analyzermainwindow.h"
#include "analyzerinstrumentdialog.h"
#include "multisdrdialog.h"

#include "analyzerspectrumwidget.h"
#include "spectrummarkerpairsdialog.h"
#include "apphelp.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMessageBox>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString frequencyText(double hz) {
    if (std::abs(hz) >= 1.0e9) return QStringLiteral("%1 GHz").arg(hz / 1.0e9, 0, 'f', 6);
    if (std::abs(hz) >= 1.0e6) return QStringLiteral("%1 MHz").arg(hz / 1.0e6, 0, 'f', 6);
    if (std::abs(hz) >= 1.0e3) return QStringLiteral("%1 kHz").arg(hz / 1.0e3, 0, 'f', 3);
    return QStringLiteral("%1 Hz").arg(hz, 0, 'f', 2);
}

std::vector<float> uniformFrequencies(double minimumHz, double maximumHz, std::size_t count) {
    std::vector<float> result(count);
    if (count == 0) return result;
    const double denominator = static_cast<double>(std::max<std::size_t>(1, count - 1));
    for (std::size_t i = 0; i < count; ++i)
        result[i] = static_cast<float>(minimumHz + (maximumHz - minimumHz) * i / denominator);
    return result;
}
}

AnalyzerMainWindow::AnalyzerMainWindow(bool connectOnStart, QWidget *parent)
    : QMainWindow(parent) {
    const QString mainSettingsPath = QDir(
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
                                             .filePath(QStringLiteral("ObriiSDR/ObriiSDR.ini"));
    QSettings applicationSettings(mainSettingsPath, QSettings::IniFormat);
    ukrainianUi = applicationSettings.value(QStringLiteral("ui/language"),
                                             QStringLiteral("en"))
                       .toString().startsWith(QStringLiteral("uk"), Qt::CaseInsensitive);
    buildUi();
    connect(&liveClient, &AnalyzerFrameClient::connected, this, [this]() {
        standaloneReceiver.stop();
        connectButton->setText(uiText(QStringLiteral("Disconnect")));
        setStatus(uiText(QStringLiteral("LIVE: connected to Obrii SDR")), QColor(76, 220, 139));
        updateSourceControls();
    });
    connect(&liveClient, &AnalyzerFrameClient::disconnected, this, [this]() {
        connectButton->setText(uiText(QStringLiteral("Connect Obrii")));
        setStatus(uiText(QStringLiteral("Disconnected")), QColor(230, 180, 70));
        updateSourceControls();
    });
    connect(&liveClient, &AnalyzerFrameClient::connectionError, this, [this](const QString &message) {
        if (!standaloneReceiver.isRunning()) {
            if (automaticAttachCheck->isChecked())
                setStatus(uiText(QStringLiteral("Obrii SDR is not running; standalone receiver is available")),
                          QColor(110, 190, 230));
            else
                setStatus(uiText(QStringLiteral("Connection: %1")).arg(message), QColor(235, 104, 91));
        }
    });
    connect(&liveClient, &AnalyzerFrameClient::frameReady,
            this, &AnalyzerMainWindow::consumeFrame);
    connect(&standaloneReceiver, &AnalyzerReceiver::frameReady,
            this, &AnalyzerMainWindow::consumeFrame);
    connect(&standaloneReceiver, &AnalyzerReceiver::runningChanged, this, [this](bool running) {
        if (!running && sweepActive) {
            sweepActive = false;
            sweepUsesNativeAgileScan = false;
            sweepSegmentFrameCounts.clear();
            sweepButton->setText(uiText(QStringLiteral("Start wide sweep")));
            setCenterFrequencyHz(sweepOriginalCenterHz);
        }
        receiverStartButton->setText(running ? uiText(QStringLiteral("Stop receiver"))
                                             : uiText(QStringLiteral("Start receiver")));
        updateSourceControls();
    });
    connect(&standaloneReceiver, &AnalyzerReceiver::receiverError, this, [this](const QString &message) {
        setStatus(message, QColor(235, 104, 91));
    });
    attachRetryTimer = new QTimer(this);
    // ObriiSDR creates the local endpoint near the beginning of its startup.
    // A short poll releases an analyzer-owned USB device before the main UI
    // reaches receiver initialization, without doing work in the RX path.
    attachRetryTimer->setInterval(200);
    connect(attachRetryTimer, &QTimer::timeout, this, [this]() {
        attachRetryTimer->setInterval(standaloneReceiver.isRunning() ? 200 : 1000);
        if (!sweepActive && automaticAttachCheck->isChecked() && !liveClient.isConnected()) connectLive();
    });
    attachRetryTimer->start();
    automaticAttachCheck->setChecked(connectOnStart || automaticAttachCheck->isChecked());
    QTimer::singleShot(0, this, [this]() {
        refreshReceivers();
        if (automaticAttachCheck->isChecked()) connectLive();
    });
}

AnalyzerMainWindow::~AnalyzerMainWindow() {
    saveSettings();
    standaloneReceiver.stop();
}

QString AnalyzerMainWindow::uiText(const QString &english) const {
    if (!ukrainianUi) return english;
    static const QHash<QString, QString> ukrainian = {
        {QStringLiteral("Instrument"), QStringLiteral("Прилад")},
        {QStringLiteral("Open recording"), QStringLiteral("Відкрити запис")},
        {QStringLiteral("Play"), QStringLiteral("Відтворити")},
        {QStringLiteral("Pause"), QStringLiteral("Пауза")},
        {QStringLiteral("Connect Obrii"), QStringLiteral("Підключити Obrii")},
        {QStringLiteral("Disconnect"), QStringLiteral("Відключити")},
        {QStringLiteral("Export CSV"), QStringLiteral("Експорт CSV")},
        {QStringLiteral("Multi-SDR..."), QStringLiteral("Multi-SDR...")},
        {QStringLiteral("Instrument lab..."), QStringLiteral("Лабораторія приладів...")},
        {QStringLiteral("  No source"), QStringLiteral("  Немає джерела")},
        {QStringLiteral("Scale:"), QStringLiteral("Масштаб:")},
        {QStringLiteral("Extra zoom:"), QStringLiteral("Дод. масштаб:")},
        {QStringLiteral("%1% span"), QStringLiteral("%1% смуги")},
        {QStringLiteral("Max"), QStringLiteral("Макс.")},
        {QStringLiteral("Min"), QStringLiteral("Мін.")},
        {QStringLiteral("Analyzer controls"), QStringLiteral("Керування аналізатором")},
        {QStringLiteral("Receiver / source"), QStringLiteral("Приймач / джерело")},
        {QStringLiteral("Refresh"), QStringLiteral("Оновити")},
        {QStringLiteral("Start receiver"), QStringLiteral("Запустити приймач")},
        {QStringLiteral("Stop receiver"), QStringLiteral("Зупинити приймач")},
        {QStringLiteral("Prefer running Obrii SDR"), QStringLiteral("Надавати перевагу запущеному Obrii SDR")},
        {QStringLiteral("Center:"), QStringLiteral("Центр:")},
        {QStringLiteral("Sample rate:"), QStringLiteral("Частота дискретизації:")},
        {QStringLiteral("Output scale:"), QStringLiteral("Масштаб виходу:")},
        {QStringLiteral("Reduction:"), QStringLiteral("Редукція:")},
        {QStringLiteral("Input:"), QStringLiteral("Вхід:")},
        {QStringLiteral("Auto (max 65,536)"), QStringLiteral("Авто (макс. 65 536)")},
        {QStringLiteral("Peak (preserve narrow signals)"), QStringLiteral("Пік (зберігати вузькі сигнали)")},
        {QStringLiteral("Average in dB"), QStringLiteral("Середнє у дБ")},
        {QStringLiteral("Average linear power (RMS)"), QStringLiteral("Середня лінійна потужність (RMS)")},
        {QStringLiteral("Center sample"), QStringLiteral("Центральний відлік")},
        {QStringLiteral("Minimum"), QStringLiteral("Мінімум")},
        {QStringLiteral("RF / tuner"), QStringLiteral("RF / тюнер")},
        {QStringLiteral("RX channel %1"), QStringLiteral("Канал RX %1")},
        {QStringLiteral("%1 MS/s (source)"), QStringLiteral("%1 Мсемпл/с (джерело)")},
        {QStringLiteral("Amplitude"), QStringLiteral("Амплітуда")},
        {QStringLiteral("Scale top:"), QStringLiteral("Верх шкали:")},
        {QStringLiteral("Scale bottom:"), QStringLiteral("Низ шкали:")},
        {QStringLiteral("Visible frequency range"), QStringLiteral("Видимий діапазон частот")},
        {QStringLiteral("Use complete receiver span"), QStringLiteral("Використовувати повну смугу приймача")},
        {QStringLiteral("Start:"), QStringLiteral("Початок:")},
        {QStringLiteral("Stop:"), QStringLiteral("Кінець:")},
        {QStringLiteral("Bandwidth / sweep"), QStringLiteral("Смуга / сканування")},
        {QStringLiteral("Replay speed:"), QStringLiteral("Швидкість відтворення:")},
        {QStringLiteral("Sweep start:"), QStringLiteral("Початок сканування:")},
        {QStringLiteral("Sweep stop:"), QStringLiteral("Кінець сканування:")},
        {QStringLiteral("Overlap:"), QStringLiteral("Перекриття:")},
        {QStringLiteral("Discard edges:"), QStringLiteral("Відкинути краї:")},
        {QStringLiteral("Frames / segment:"), QStringLiteral("Кадрів / сегмент:")},
        {QStringLiteral("Fobos sweep mode:"), QStringLiteral("Режим сканування Fobos:")},
        {QStringLiteral("UHD gain:"), QStringLiteral("Підсилення UHD:")},
        {QStringLiteral("Antenna:"), QStringLiteral("Антена:")},
        {QStringLiteral("FPGA image:"), QStringLiteral("Образ FPGA:")},
        {QStringLiteral("Choose FPGA image"), QStringLiteral("Вибрати образ FPGA")},
        {QStringLiteral("Receiver rejected the next sweep frequency"), QStringLiteral("Приймач відхилив наступну частоту сканування")},
        {QStringLiteral("Auto"), QStringLiteral("Авто")},
        {QStringLiteral("Agile native scan"), QStringLiteral("Нативне сканування Agile")},
        {QStringLiteral("Normal retune scan"), QStringLiteral("Звичайне сканування переналаштуванням")},
        {QStringLiteral("Continuous / live sweep"), QStringLiteral("Безперервне сканування")},
        {QStringLiteral("Start wide sweep"), QStringLiteral("Почати широке сканування")},
        {QStringLiteral("Stop live sweep"), QStringLiteral("Зупинити сканування")},
        {QStringLiteral("Cancel sweep"), QStringLiteral("Скасувати сканування")},
        {QStringLiteral("Detector"), QStringLiteral("Детектор")},
        {QStringLiteral("Mode:"), QStringLiteral("Режим:")},
        {QStringLiteral("Frames:"), QStringLiteral("Кадри:")},
        {QStringLiteral("Sample"), QStringLiteral("Відлік")},
        {QStringLiteral("Positive peak"), QStringLiteral("Додатний пік")},
        {QStringLiteral("Negative peak"), QStringLiteral("Від'ємний пік")},
        {QStringLiteral("Average"), QStringLiteral("Середнє")},
        {QStringLiteral("Median"), QStringLiteral("Медіана")},
        {QStringLiteral("Quasi-peak"), QStringLiteral("Квазіпік")},
        {QStringLiteral("Traces"), QStringLiteral("Траси")},
        {QStringLiteral("Current"), QStringLiteral("Поточна")},
        {QStringLiteral("Clear traces"), QStringLiteral("Очистити траси")},
        {QStringLiteral("Markers"), QStringLiteral("Маркери")},
        {QStringLiteral("Pairs..."), QStringLiteral("Пари...")},
        {QStringLiteral("No active markers"), QStringLiteral("Немає активних маркерів")},
        {QStringLiteral("Peak"), QStringLiteral("Пік")},
        {QStringLiteral("Prev"), QStringLiteral("Попер.")},
        {QStringLiteral("Next"), QStringLiteral("Наст.")},
        {QStringLiteral("Clear"), QStringLiteral("Очистити")},
        {QStringLiteral("Waiting for spectrum data"), QStringLiteral("Очікування даних спектра")},
        {QStringLiteral("Ready"), QStringLiteral("Готово")},
        {QStringLiteral("No compatible receiver found"), QStringLiteral("Сумісний приймач не знайдено")},
        {QStringLiteral("Scanning receivers..."), QStringLiteral("Пошук приймачів...")},
        {QStringLiteral("Scanning..."), QStringLiteral("Пошук...")},
        {QStringLiteral("LIVE: connected to Obrii SDR"), QStringLiteral("НАЖИВО: підключено до Obrii SDR")},
        {QStringLiteral("Disconnected"), QStringLiteral("Відключено")},
        {QStringLiteral("Obrii SDR is not running; standalone receiver is available"), QStringLiteral("Obrii SDR не запущено; доступний автономний приймач")},
        {QStringLiteral("Connection: %1"), QStringLiteral("Підключення: %1")},
        {QStringLiteral("Receiver stopped"), QStringLiteral("Приймач зупинено")},
        {QStringLiteral("No compatible receiver selected"), QStringLiteral("Не вибрано сумісний приймач")},
        {QStringLiteral("Standalone receiver is starting"), QStringLiteral("Автономний приймач запускається")},
        {QStringLiteral("Wide sweep cancelled"), QStringLiteral("Широке сканування скасовано")},
        {QStringLiteral("Disconnect Obrii SDR before a wide sweep"), QStringLiteral("Відключіть Obrii SDR перед широким скануванням")},
        {QStringLiteral("Wide sweep requires the RF / tuner input"), QStringLiteral("Широке сканування потребує входу RF / тюнер")},
        {QStringLiteral("Restarting wide sweep with new receiver settings..."), QStringLiteral("Перезапуск широкого сканування з новими налаштуваннями приймача...")},
        {QStringLiteral("Wide sweep: %1"), QStringLiteral("Широке сканування: %1")},
        {QStringLiteral("Sweep stop frequency must be above start frequency"), QStringLiteral("Кінцева частота сканування має бути вищою за початкову")},
        {QStringLiteral("Sweep capture bandwidth must be positive"), QStringLiteral("Смуга захоплення сканування має бути додатною")},
        {QStringLiteral("Sweep edge discard must be between 0 and 45 percent"), QStringLiteral("Відкидання країв має бути від 0 до 45 відсотків")},
        {QStringLiteral("Sweep overlap must be between 0 and 95 percent"), QStringLiteral("Перекриття сканування має бути від 0 до 95 відсотків")},
        {QStringLiteral("Sweep settings leave no usable bandwidth"), QStringLiteral("Налаштування сканування не залишають корисної смуги")},
        {QStringLiteral("Wide sweep exceeds the receiver tuning range"), QStringLiteral("Широке сканування виходить за діапазон приймача")},
        {QStringLiteral("Could not initialize wide sweep storage"), QStringLiteral("Не вдалося підготувати буфер широкого сканування")},
        {QStringLiteral("Native Agile scan requires a Fobos Agile receiver"), QStringLiteral("Нативне сканування Agile потребує приймача Fobos Agile")},
        {QStringLiteral("Native Agile scan needs 2-256 segments and FFT no larger than 4,194,304"), QStringLiteral("Нативне сканування Agile потребує 2-256 сегментів і FFT не більше 4 194 304")},
        {QStringLiteral("Could not start native Agile sweep receiver"), QStringLiteral("Не вдалося запустити нативне сканування приймача Agile")},
        {QStringLiteral("Could not start the receiver for wide sweep"), QStringLiteral("Не вдалося запустити приймач для широкого сканування")},
        {QStringLiteral("Receiver rejected the first sweep frequency"), QStringLiteral("Приймач відхилив першу частоту сканування")},
        {QStringLiteral("Wide sweep rejected an incompatible Agile scan frame"), QStringLiteral("Широке сканування відхилило несумісний кадр Agile")},
        {QStringLiteral("Wide sweep rejected an incompatible receiver frame"), QStringLiteral("Широке сканування відхилило несумісний кадр приймача")},
        {QStringLiteral("Could not restart continuous sweep storage"), QStringLiteral("Не вдалося перезапустити буфер безперервного сканування")},
        {QStringLiteral("Receiver rejected the next live sweep pass"), QStringLiteral("Приймач відхилив наступний прохід безперервного сканування")},
        {QStringLiteral("%1: segment 1 / %2"), QStringLiteral("%1: сегмент 1 / %2")},
        {QStringLiteral("%1: segment %2 / %3"), QStringLiteral("%1: сегмент %2 / %3")},
        {QStringLiteral("%1 sweep: %2 / %3 segments"), QStringLiteral("Сканування %1: %2 / %3 сегментів")},
        {QStringLiteral("%1 pass %2 complete; restarting (%3 missing bins)"), QStringLiteral("%1: прохід %2 завершено; перезапуск (%3 пропущених бінів)")},
        {QStringLiteral("Wide sweep complete: %1 segments, %2 missing bins"), QStringLiteral("Широке сканування завершено: %1 сегментів, %2 пропущених бінів")},
        {QStringLiteral("Native Agile sweep"), QStringLiteral("Нативне сканування Agile")},
        {QStringLiteral("Wide sweep"), QStringLiteral("Широке сканування")},
        {QStringLiteral("Live sweep"), QStringLiteral("Безперервне сканування")},
        {QStringLiteral("Live Agile sweep"), QStringLiteral("Безперервне сканування Agile")},
        {QStringLiteral("Live Agile"), QStringLiteral("Agile наживо")},
        {QStringLiteral("Agile"), QStringLiteral("Agile")},
        {QStringLiteral("OBRII LIVE"), QStringLiteral("OBRII НАЖИВО")},
        {QStringLiteral("RECEIVER"), QStringLiteral("ПРИЙМАЧ")},
        {QStringLiteral("  LIVE SWEEP  pass %1  %2 .. %3"), QStringLiteral("  СКАНУВАННЯ НАЖИВО  прохід %1  %2 .. %3")},
        {QStringLiteral("  WIDE SWEEP  %1 .. %2"), QStringLiteral("  ШИРОКЕ СКАНУВАННЯ  %1 .. %2")},
        {QStringLiteral("  FILE  %1"), QStringLiteral("  ФАЙЛ  %1")},
        {QStringLiteral("No measurement"), QStringLiteral("Немає вимірювання")},
        {QStringLiteral("%1 .. %2 | Span %3 | Peak %4 @ %5 | SNR %6 dB | Power %7"),
         QStringLiteral("%1 .. %2 | Смуга %3 | Пік %4 @ %5 | SNR %6 дБ | Потужність %7")},
        {QStringLiteral("Replay complete"), QStringLiteral("Відтворення завершено")},
        {QStringLiteral("Connecting to Obrii SDR..."), QStringLiteral("Підключення до Obrii SDR...")},
        {QStringLiteral("Visible frequency range is outside the source span"), QStringLiteral("Видимий діапазон частот поза смугою джерела")},
        {QStringLiteral("Open Obrii spectrum recording"), QStringLiteral("Відкрити запис спектра Obrii")},
        {QStringLiteral("Obrii spectrum (*.fbspec *.spectrum);;All files (*)"), QStringLiteral("Спектр Obrii (*.fbspec *.spectrum);;Усі файли (*)")},
        {QStringLiteral("Export spectrum CSV"), QStringLiteral("Експорт спектра CSV")},
        {QStringLiteral("Exported %1"), QStringLiteral("Експортовано %1")},
        {QStringLiteral("Range: %1 .. %2\nSpan: %3; bins: %4\nPeak: %5 dBFS @ %6\nNoise: %7 dBFS; SNR: %8 dB\nChannel power: %9 dBFS\nWidth -3/-6/-20 dB: %10 / %11 / %12\nOBW 90/95/99%: %13 / %14 / %15\nACPR lower/upper: %16 / %17 dB"),
         QStringLiteral("Діапазон: %1 .. %2\nСмуга: %3; бінів: %4\nПік: %5 dBFS @ %6\nШум: %7 dBFS; SNR: %8 дБ\nПотужність каналу: %9 dBFS\nШирина -3/-6/-20 дБ: %10 / %11 / %12\nOBW 90/95/99%: %13 / %14 / %15\nACPR нижній/верхній: %16 / %17 дБ")},
        {QStringLiteral("Auto selects native indexed scanning for a compatible Fobos Agile plan. Normal retune uses the universal sequential tuning path."),
         QStringLiteral("Авто обирає нативне індексоване сканування для сумісного плану Fobos Agile. Звичайне переналаштування використовує універсальний послідовний шлях.")},
        {QStringLiteral("Actual: %1 Hz/bin, FFT %2"), QStringLiteral("Фактично: %1 Гц/бін, FFT %2")},
        {QStringLiteral("Output: about %1 points%2"), QStringLiteral("Вихід: близько %1 точок%2")},
        {QStringLiteral(" (high memory/UI load)"), QStringLiteral(" (високе навантаження пам'яті/інтерфейсу)")},
        {QStringLiteral("FFT is controlled by the connected Obrii SDR instance."), QStringLiteral("FFT керується підключеним екземпляром Obrii SDR.")},
        {QStringLiteral("Changing FFT automatically restarts the standalone receiver."), QStringLiteral("Зміна FFT автоматично перезапускає автономний приймач або сканування.")},
        {QStringLiteral("FFT is calculated from the active Hz/bin field."), QStringLiteral("FFT обчислюється з активного поля Гц/бін.")}
    };
    return ukrainian.value(english, english);
}

void AnalyzerMainWindow::translateStaticUi() {
    if (!ukrainianUi) return;
    setWindowTitle(uiText(windowTitle()));
    for (QAbstractButton *button : findChildren<QAbstractButton *>()) {
        button->setText(uiText(button->text()));
        button->setToolTip(uiText(button->toolTip()));
    }
    for (QLabel *label : findChildren<QLabel *>()) {
        label->setText(uiText(label->text()));
        label->setToolTip(uiText(label->toolTip()));
    }
    for (QGroupBox *group : findChildren<QGroupBox *>())
        group->setTitle(uiText(group->title()));
    for (QDockWidget *dock : findChildren<QDockWidget *>())
        dock->setWindowTitle(uiText(dock->windowTitle()));
    for (QToolBar *toolbar : findChildren<QToolBar *>())
        toolbar->setWindowTitle(uiText(toolbar->windowTitle()));
    for (QComboBox *combo : findChildren<QComboBox *>()) {
        for (int index = 0; index < combo->count(); ++index)
            combo->setItemText(index, uiText(combo->itemText(index)));
        combo->setToolTip(uiText(combo->toolTip()));
    }
}

void AnalyzerMainWindow::buildUi() {
    setWindowTitle(QStringLiteral("Obrii Analyzer"));
    auto *helpMenu = menuBar()->addMenu(ukrainianUi ? QStringLiteral("Довідка")
                                                   : QStringLiteral("Help"));
    auto *helpAction = helpMenu->addAction(ukrainianUi ? QStringLiteral("Довідка Analyzer")
                                                      : QStringLiteral("Analyzer help"));
    helpAction->setShortcut(QKeySequence::HelpContents);
    connect(helpAction, &QAction::triggered, this, [this]() {
        showApplicationHelpDialog(this, ukrainianUi ? QStringLiteral("uk") : QStringLiteral("en"),
                                  QStringLiteral("Obrii Analyzer"));
    });
    resize(1320, 780);
    setMinimumSize(900, 560);
    setStyleSheet(QStringLiteral(
        "QMainWindow,QWidget{background:#0d1114;color:#cbd6d6;}"
        "QGroupBox{border:1px solid #2a4142;margin-top:10px;padding-top:8px;font-weight:600;}"
        "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 4px;color:#8fe0c2;}"
        "QPushButton,QComboBox,QSpinBox,QDoubleSpinBox{background:#172024;border:1px solid #365153;padding:4px;}"
        "QPushButton:checked{background:#275746;border-color:#62c99f;}"
        "QToolBar{background:#11191c;border-bottom:1px solid #2a4142;spacing:6px;}"
        "QStatusBar{background:#0a0e10;border-top:1px solid #2a4142;}"));

    auto *toolbar = addToolBar(QStringLiteral("Instrument"));
    toolbar->setMovable(false);
    auto *openButton = new QPushButton(QStringLiteral("Open recording"), this);
    playButton = new QPushButton(QStringLiteral("Play"), this);
    connectButton = new QPushButton(QStringLiteral("Connect Obrii"), this);
    auto *exportButton = new QPushButton(QStringLiteral("Export CSV"), this);
    auto *multiSdrButton = new QPushButton(QStringLiteral("Multi-SDR..."), this);
    auto *instrumentLabButton = new QPushButton(QStringLiteral("Instrument lab..."), this);
    toolbar->addWidget(openButton);
    toolbar->addWidget(playButton);
    toolbar->addSeparator();
    toolbar->addWidget(connectButton);
    toolbar->addSeparator();
    toolbar->addWidget(exportButton);
    toolbar->addWidget(multiSdrButton);
    toolbar->addWidget(instrumentLabButton);
    sourceLabel = new QLabel(QStringLiteral("  No source"), this);
    sourceLabel->setMinimumWidth(300);
    toolbar->addWidget(sourceLabel);

    auto *displayPanel = new QWidget(this);
    auto *displayLayout = new QGridLayout(displayPanel);
    displayLayout->setContentsMargins(6, 6, 6, 4);
    displayLayout->setHorizontalSpacing(6);
    displayLayout->setVerticalSpacing(3);

    auto *levelSliders = new QWidget(displayPanel);
    auto *levelLayout = new QVBoxLayout(levelSliders);
    levelLayout->setContentsMargins(0, 0, 0, 0);
    levelLayout->setSpacing(4);
    const auto addLevelSlider = [levelSliders, levelLayout](const QString &caption,
                                                            QSlider **slider,
                                                            QLabel **valueLabel) {
        auto *column = new QVBoxLayout();
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(1);
        auto *title = new QLabel(caption, levelSliders);
        title->setAlignment(Qt::AlignHCenter);
        *valueLabel = new QLabel(QStringLiteral("--"), levelSliders);
        (*valueLabel)->setAlignment(Qt::AlignHCenter);
        (*valueLabel)->setMinimumWidth(42);
        *slider = new QSlider(Qt::Vertical, levelSliders);
        (*slider)->setRange(-2000, 1000);
        (*slider)->setTickPosition(QSlider::TicksBothSides);
        (*slider)->setTickInterval(200);
        column->addWidget(title);
        column->addWidget(*valueLabel);
        column->addWidget(*slider, 1, Qt::AlignHCenter);
        levelLayout->addLayout(column);
    };
    addLevelSlider(QStringLiteral("Max"), &scaleTopSlider, &scaleTopValueLabel);
    addLevelSlider(QStringLiteral("Min"), &scaleBottomSlider, &scaleBottomValueLabel);
    levelSliders->setMaximumWidth(54);

    spectrum = new AnalyzerSpectrumWidget(displayPanel);
    frequencyZoomSlider = new QSlider(Qt::Horizontal, displayPanel);
    frequencyZoomSlider->setRange(1, 1000);
    frequencyZoomSlider->setTickPosition(QSlider::TicksAbove);
    frequencyZoomSlider->setTickInterval(100);
    frequencyZoomValueLabel = new QLabel(displayPanel);
    frequencyZoomValueLabel->setMinimumWidth(96);
    frequencyZoomValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    frequencyZoomDivisorSlider = new QSlider(Qt::Horizontal, displayPanel);
    frequencyZoomDivisorSlider->setRange(1, 100);
    frequencyZoomDivisorSlider->setPageStep(5);
    frequencyZoomDivisorSlider->setInvertedAppearance(true);
    frequencyZoomDivisorValueLabel = new QLabel(displayPanel);
    frequencyZoomDivisorValueLabel->setMinimumWidth(96);
    frequencyZoomDivisorValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *zoomCaption = new QLabel(QStringLiteral("Scale:"), displayPanel);
    auto *zoomDivisorCaption = new QLabel(QStringLiteral("Extra zoom:"), displayPanel);
    displayLayout->addWidget(levelSliders, 0, 0);
    displayLayout->addWidget(spectrum, 0, 1, 1, 3);
    displayLayout->addWidget(zoomCaption, 1, 1);
    displayLayout->addWidget(frequencyZoomSlider, 1, 2);
    displayLayout->addWidget(frequencyZoomValueLabel, 1, 3);
    displayLayout->addWidget(zoomDivisorCaption, 2, 1);
    displayLayout->addWidget(frequencyZoomDivisorSlider, 2, 2);
    displayLayout->addWidget(frequencyZoomDivisorValueLabel, 2, 3);
    displayLayout->setColumnStretch(2, 1);
    displayLayout->setRowStretch(0, 1);
    setCentralWidget(displayPanel);

    auto *controlsDock = new QDockWidget(QStringLiteral("Analyzer controls"), this);
    controlsDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    controlsDock->setMinimumWidth(285);
    auto *controls = new QWidget(controlsDock);
    auto *controlsLayout = new QVBoxLayout(controls);
    controlsLayout->setContentsMargins(8, 8, 8, 8);

    QSettings saved;
    auto *receiverToggle = new QToolButton(controls);
    receiverToggle->setText(QStringLiteral("Receiver / source"));
    receiverToggle->setCheckable(true);
    receiverToggle->setChecked(!saved.value(QStringLiteral("receiver/sectionCollapsed"), false).toBool());
    receiverToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    receiverToggle->setArrowType(receiverToggle->isChecked() ? Qt::DownArrow : Qt::RightArrow);
    receiverToggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *receiverBox = new QGroupBox(controls);
    auto *receiverLayout = new QGridLayout(receiverBox);
    receiverCombo = new QComboBox(receiverBox);
    receiverCombo->setObjectName(QStringLiteral("analyzerReceiver"));
    receiverRefreshButton = new QPushButton(QStringLiteral("Refresh"), receiverBox);
    receiverStartButton = new QPushButton(QStringLiteral("Start receiver"), receiverBox);
    receiverStartButton->setObjectName(QStringLiteral("analyzerStartReceiver"));
    automaticAttachCheck = new QCheckBox(QStringLiteral("Prefer running Obrii SDR"), receiverBox);
    automaticAttachCheck->setChecked(saved.value(QStringLiteral("source/autoAttach"), true).toBool());
    centerUnitCombo = new QComboBox(receiverBox);
    centerUnitCombo->addItem(QStringLiteral("GHz"), 1.0e9);
    centerUnitCombo->addItem(QStringLiteral("MHz"), 1.0e6);
    centerUnitCombo->addItem(QStringLiteral("kHz"), 1.0e3);
    const double savedCenterUnit = saved.value(QStringLiteral("receiver/centerUnitFactor"), 1.0e6).toDouble();
    int savedCenterUnitIndex = centerUnitCombo->findData(savedCenterUnit);
    centerUnitCombo->setCurrentIndex(savedCenterUnitIndex >= 0 ? savedCenterUnitIndex : 1);
    centerUnitFactor = centerUnitCombo->currentData().toDouble();
    centerSpin = new QDoubleSpinBox(receiverBox);
    centerSpin->setObjectName(QStringLiteral("analyzerCenter"));
    const double savedCenterHz = saved.value(
        QStringLiteral("receiver/centerHz"),
        saved.value(QStringLiteral("receiver/centerMHz"), 100.0).toDouble() * 1.0e6).toDouble();
    updateCenterFrequencyUnit();
    setCenterFrequencyHz(savedCenterHz);
    sampleRateCombo = new QComboBox(receiverBox);
    sampleRateCombo->setObjectName(QStringLiteral("analyzerSampleRate"));
    sampleRateCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    receiverFftCombo = new QComboBox(receiverBox);
    for (int length : {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536,
                       131072, 262144, 524288, 1048576, 2097152, 4194304,
                       8388608, 16777216, 33554432, 67108864})
        receiverFftCombo->addItem(QString::number(length), length);
    const int savedFft = saved.value(QStringLiteral("receiver/fftLength"), 65536).toInt();
    receiverFftCombo->setCurrentIndex(std::max(0, receiverFftCombo->findData(savedFft)));
    hzPerBinCheck = new QCheckBox(QStringLiteral("Hz/bin"), receiverBox);
    hzPerBinCheck->setChecked(saved.value(QStringLiteral("receiver/useHzPerBin"), false).toBool());
    hzPerBinSpin = new QDoubleSpinBox(receiverBox);
    hzPerBinSpin->setDecimals(4);
    hzPerBinSpin->setRange(0.1, 1000000.0);
    hzPerBinSpin->setSuffix(QStringLiteral(" Hz"));
    hzPerBinSpin->setValue(saved.value(QStringLiteral("receiver/hzPerBin"), 1000.0).toDouble());
    resolutionActualLabel = new QLabel(receiverBox);
    resolutionActualLabel->setWordWrap(true);
    displayReductionRatioCombo = new QComboBox(receiverBox);
    displayReductionRatioCombo->addItem(QStringLiteral("Auto (max 65,536)"), 0);
    for (int ratio : {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096})
        displayReductionRatioCombo->addItem(QStringLiteral("1:%1").arg(ratio), ratio);
    displayReductionRatioCombo->setCurrentIndex(std::max(0, displayReductionRatioCombo->findData(
        saved.value(QStringLiteral("receiver/displayReductionRatio"), 0).toInt())));
    displayReductionMethodCombo = new QComboBox(receiverBox);
    displayReductionMethodCombo->addItem(QStringLiteral("Peak (preserve narrow signals)"), 0);
    displayReductionMethodCombo->addItem(QStringLiteral("Average in dB"), 1);
    displayReductionMethodCombo->addItem(QStringLiteral("Average linear power (RMS)"), 2);
    displayReductionMethodCombo->addItem(QStringLiteral("Center sample"), 3);
    displayReductionMethodCombo->addItem(QStringLiteral("Minimum"), 4);
    displayReductionMethodCombo->setCurrentIndex(std::max(0, displayReductionMethodCombo->findData(
        saved.value(QStringLiteral("receiver/displayReductionMethod"), 0).toInt())));
    displayReductionInfoLabel = new QLabel(receiverBox);
    displayReductionInfoLabel->setWordWrap(true);
    receiverInputCombo = new QComboBox(receiverBox);
    receiverInputCombo->addItem(QStringLiteral("RF / tuner"), 0);
    receiverInputCombo->addItem(QStringLiteral("HF1 direct"), 1);
    receiverInputCombo->addItem(QStringLiteral("HF2 direct"), 2);
    receiverInputCombo->setCurrentIndex(std::clamp(
        saved.value(QStringLiteral("receiver/directChannel"),
                    saved.value(QStringLiteral("receiver/direct"), false).toBool() ? 1 : 0).toInt(), 0, 2));
    receiverLnaSpin = new QSpinBox(receiverBox);
    receiverLnaSpin->setRange(0, 40);
    receiverLnaSpin->setSuffix(QStringLiteral(" dB"));
    receiverLnaSpin->setValue(saved.value(QStringLiteral("receiver/lnaDb"), 1).toInt());
    receiverVgaSpin = new QSpinBox(receiverBox);
    receiverVgaSpin->setRange(0, 62);
    receiverVgaSpin->setSuffix(QStringLiteral(" dB"));
    receiverVgaSpin->setValue(saved.value(QStringLiteral("receiver/vgaDb"), 0).toInt());
    receiverCapabilityLabel = new QLabel(receiverBox);
    receiverCapabilityLabel->setWordWrap(true);
    receiverCapabilityLabel->setStyleSheet(QStringLiteral("color:#91a3a5;"));
    receiverLayout->addWidget(receiverCombo, 0, 0, 1, 2);
    receiverLayout->addWidget(receiverRefreshButton, 0, 2);
    receiverLayout->addWidget(new QLabel(QStringLiteral("Center:"), receiverBox), 1, 0);
    receiverLayout->addWidget(centerSpin, 1, 1);
    receiverLayout->addWidget(centerUnitCombo, 1, 2);
    receiverLayout->addWidget(new QLabel(QStringLiteral("Sample rate:"), receiverBox), 2, 0);
    receiverLayout->addWidget(sampleRateCombo, 2, 1, 1, 2);
    receiverLayout->addWidget(new QLabel(QStringLiteral("FFT:"), receiverBox), 3, 0);
    receiverLayout->addWidget(receiverFftCombo, 3, 1, 1, 2);
    receiverLayout->addWidget(hzPerBinCheck, 4, 0);
    receiverLayout->addWidget(hzPerBinSpin, 4, 1, 1, 2);
    receiverLayout->addWidget(resolutionActualLabel, 5, 0, 1, 3);
    receiverLayout->addWidget(new QLabel(QStringLiteral("Output scale:"), receiverBox), 6, 0);
    receiverLayout->addWidget(displayReductionRatioCombo, 6, 1, 1, 2);
    receiverLayout->addWidget(new QLabel(QStringLiteral("Reduction:"), receiverBox), 7, 0);
    receiverLayout->addWidget(displayReductionMethodCombo, 7, 1, 1, 2);
    receiverLayout->addWidget(displayReductionInfoLabel, 8, 0, 1, 3);
    receiverLayout->addWidget(new QLabel(QStringLiteral("Input:"), receiverBox), 9, 0);
    receiverLayout->addWidget(receiverInputCombo, 9, 1, 1, 2);
    receiverLegacyGainLabel = new QLabel(QStringLiteral("LNA / VGA:"), receiverBox);
    receiverLayout->addWidget(receiverLegacyGainLabel, 10, 0);
    receiverLayout->addWidget(receiverLnaSpin, 10, 1);
    receiverLayout->addWidget(receiverVgaSpin, 10, 2);
    uhdSettingsPanel = new QWidget(receiverBox);
    auto *uhdForm = new QFormLayout(uhdSettingsPanel);
    uhdForm->setContentsMargins(0, 0, 0, 0);
    uhdGainSpin = new QDoubleSpinBox(uhdSettingsPanel);
    uhdGainSpin->setObjectName(QStringLiteral("analyzerUhdGain"));
    uhdGainSpin->setRange(0.0, 100.0);
    uhdGainSpin->setSuffix(QStringLiteral(" dB"));
    uhdGainSpin->setValue(saved.value(QStringLiteral("receiver/uhdGainDb"), 30.0).toDouble());
    uhdAntennaCombo = new QComboBox(uhdSettingsPanel);
    uhdAntennaCombo->setObjectName(QStringLiteral("analyzerUhdAntenna"));
    uhdAntennaCombo->setEditable(true);
    uhdAntennaCombo->addItems({QString(), QStringLiteral("RX2"), QStringLiteral("TX/RX")});
    uhdAntennaCombo->setCurrentText(saved.value(QStringLiteral("receiver/uhdAntenna")).toString());
    auto *imageRow = new QWidget(uhdSettingsPanel);
    auto *imageLayout = new QHBoxLayout(imageRow);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    QSettings mainSettings(QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
                               .filePath(QStringLiteral("ObriiSDR/ObriiSDR.ini")), QSettings::IniFormat);
    uhdImageEdit = new QLineEdit(saved.value(QStringLiteral("receiver/uhdFpgaImage"),
        mainSettings.value(QStringLiteral("receiver/uhdFpgaImagePath"))).toString(), imageRow);
    uhdImageEdit->setObjectName(QStringLiteral("analyzerUhdImage"));
    auto *browseImage = new QPushButton(QStringLiteral("…"), imageRow);
    browseImage->setToolTip(QStringLiteral("Choose FPGA image"));
    imageLayout->addWidget(uhdImageEdit, 1);
    imageLayout->addWidget(browseImage);
    connect(browseImage, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, uiText(QStringLiteral("Choose FPGA image")),
            uhdImageEdit->text(), QStringLiteral("FPGA (*.bin *.bit);;All files (*)"));
        if (!path.isEmpty()) uhdImageEdit->setText(path);
    });
    uhdForm->addRow(QStringLiteral("UHD gain:"), uhdGainSpin);
    uhdForm->addRow(QStringLiteral("Antenna:"), uhdAntennaCombo);
    uhdForm->addRow(QStringLiteral("FPGA image:"), imageRow);
    receiverLayout->addWidget(uhdSettingsPanel, 11, 0, 1, 3);
    receiverLayout->addWidget(receiverCapabilityLabel, 12, 0, 1, 3);
    receiverLayout->addWidget(receiverStartButton, 13, 0, 1, 3);
    receiverLayout->addWidget(automaticAttachCheck, 14, 0, 1, 3);
    controlsLayout->addWidget(receiverToggle);
    controlsLayout->addWidget(receiverBox);
    receiverBox->setVisible(receiverToggle->isChecked());
    connect(receiverToggle, &QToolButton::toggled, this, [receiverBox, receiverToggle](bool expanded) {
        receiverBox->setVisible(expanded);
        receiverToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        QSettings saved;
        saved.setValue(QStringLiteral("receiver/sectionCollapsed"), !expanded);
    });

    auto *amplitudeBox = new QGroupBox(QStringLiteral("Amplitude"), controls);
    auto *amplitudeForm = new QFormLayout(amplitudeBox);
    scaleTopSpin = new QDoubleSpinBox(amplitudeBox);
    scaleTopSpin->setRange(-199.0, 100.0);
    scaleTopSpin->setDecimals(1);
    scaleTopSpin->setSuffix(QStringLiteral(" dBFS"));
    scaleTopSpin->setValue(saved.value(QStringLiteral("display/topDb"), 10.0).toDouble());
    scaleBottomSpin = new QDoubleSpinBox(amplitudeBox);
    scaleBottomSpin->setRange(-200.0, 99.0);
    scaleBottomSpin->setDecimals(1);
    scaleBottomSpin->setSuffix(QStringLiteral(" dBFS"));
    scaleBottomSpin->setValue(saved.value(QStringLiteral("display/bottomDb"), -140.0).toDouble());
    scaleTopSlider->setValue(qRound(scaleTopSpin->value() * 10.0));
    scaleBottomSlider->setValue(qRound(scaleBottomSpin->value() * 10.0));
    const int legacyZoomPercent = std::clamp(
        saved.value(QStringLiteral("display/zoomPercent"), 100).toInt(), 1, 100);
    frequencyZoomSlider->setValue(std::clamp(
        saved.value(QStringLiteral("display/zoomTenthsPercent"), legacyZoomPercent * 10).toInt(),
        1, 1000));
    frequencyZoomDivisorSlider->setValue(std::clamp(
        saved.value(QStringLiteral("display/zoomDivisor"), 1).toInt(), 1, 100));
    scaleTopValueLabel->setText(QStringLiteral("%1 dB").arg(scaleTopSpin->value(), 0, 'f', 1));
    scaleBottomValueLabel->setText(QStringLiteral("%1 dB").arg(scaleBottomSpin->value(), 0, 'f', 1));
    frequencyZoomValueLabel->setText(uiText(QStringLiteral("%1% span"))
                                         .arg(frequencyZoomSlider->value() / 10.0, 0, 'f', 1));
    frequencyZoomDivisorValueLabel->setText(
        QStringLiteral("1:%1").arg(frequencyZoomDivisorSlider->value()));
    amplitudeForm->addRow(QStringLiteral("Scale top:"), scaleTopSpin);
    amplitudeForm->addRow(QStringLiteral("Scale bottom:"), scaleBottomSpin);
    controlsLayout->addWidget(amplitudeBox);

    auto *frequencyBox = new QGroupBox(QStringLiteral("Visible frequency range"), controls);
    auto *frequencyForm = new QFormLayout(frequencyBox);
    automaticViewCheck = new QCheckBox(QStringLiteral("Use complete receiver span"), frequencyBox);
    automaticViewCheck->setChecked(saved.value(QStringLiteral("display/automaticFrequency"), true).toBool());
    viewMinimumSpin = new QDoubleSpinBox(frequencyBox);
    viewMaximumSpin = new QDoubleSpinBox(frequencyBox);
    for (QDoubleSpinBox *spin : {viewMinimumSpin, viewMaximumSpin}) {
        spin->setDecimals(6);
        spin->setRange(0.0, 8000.0);
        spin->setSuffix(QStringLiteral(" MHz"));
    }
    viewMinimumSpin->setValue(saved.value(QStringLiteral("display/minimumMHz"), 95.0).toDouble());
    viewMaximumSpin->setValue(saved.value(QStringLiteral("display/maximumMHz"), 105.0).toDouble());
    frequencyForm->addRow(automaticViewCheck);
    frequencyForm->addRow(QStringLiteral("Start:"), viewMinimumSpin);
    frequencyForm->addRow(QStringLiteral("Stop:"), viewMaximumSpin);
    controlsLayout->addWidget(frequencyBox);

    auto *bandwidthBox = new QGroupBox(QStringLiteral("Bandwidth / sweep"), controls);
    auto *bandwidthForm = new QFormLayout(bandwidthBox);
    rbwLabel = new QLabel(QStringLiteral("--"), bandwidthBox);
    vbwSpin = new QDoubleSpinBox(bandwidthBox);
    vbwSpin->setRange(0.0, 10000.0);
    vbwSpin->setDecimals(1);
    vbwSpin->setSuffix(QStringLiteral(" Hz"));
    cycleSpin = new QDoubleSpinBox(bandwidthBox);
    cycleSpin->setRange(0.01, 10.0);
    cycleSpin->setDecimals(2);
    cycleSpin->setValue(1.0);
    cycleSpin->setSuffix(QStringLiteral(" x"));
    sweepStartSpin = new QDoubleSpinBox(bandwidthBox);
    sweepStopSpin = new QDoubleSpinBox(bandwidthBox);
    sweepStartSpin->setObjectName(QStringLiteral("analyzerSweepStart"));
    sweepStopSpin->setObjectName(QStringLiteral("analyzerSweepStop"));
    for (QDoubleSpinBox *spin : {sweepStartSpin, sweepStopSpin}) {
        spin->setDecimals(6);
        spin->setRange(0.0, 100000.0);
        spin->setSuffix(QStringLiteral(" MHz"));
    }
    sweepStartSpin->setValue(saved.value(QStringLiteral("sweep/startMHz"), 88.0).toDouble());
    sweepStopSpin->setValue(saved.value(QStringLiteral("sweep/stopMHz"), 108.0).toDouble());
    sweepOverlapSpin = new QDoubleSpinBox(bandwidthBox);
    sweepOverlapSpin->setRange(0.0, 90.0);
    sweepOverlapSpin->setDecimals(1);
    sweepOverlapSpin->setSuffix(QStringLiteral(" %"));
    sweepOverlapSpin->setValue(saved.value(QStringLiteral("sweep/overlapPercent"), 15.0).toDouble());
    sweepEdgeSpin = new QDoubleSpinBox(bandwidthBox);
    sweepEdgeSpin->setRange(0.0, 40.0);
    sweepEdgeSpin->setDecimals(1);
    sweepEdgeSpin->setSuffix(QStringLiteral(" % / edge"));
    sweepEdgeSpin->setValue(saved.value(QStringLiteral("sweep/edgePercent"), 8.0).toDouble());
    sweepAverageFramesSpin = new QSpinBox(bandwidthBox);
    sweepAverageFramesSpin->setRange(1, 64);
    sweepAverageFramesSpin->setValue(saved.value(QStringLiteral("sweep/averageFrames"), 1).toInt());
    sweepFobosModeCombo = new QComboBox(bandwidthBox);
    sweepFobosModeCombo->addItem(QStringLiteral("Auto"), 0);
    sweepFobosModeCombo->addItem(QStringLiteral("Agile native scan"), 1);
    sweepFobosModeCombo->addItem(QStringLiteral("Normal retune scan"), 2);
    {
        const int wantedMode = saved.value(QStringLiteral("sweep/fobosMode"), 0).toInt();
        const int wantedIndex = sweepFobosModeCombo->findData(wantedMode);
        sweepFobosModeCombo->setCurrentIndex(wantedIndex >= 0 ? wantedIndex : 0);
    }
    sweepFobosModeCombo->setToolTip(QStringLiteral(
        "Auto selects native indexed scanning for a compatible Fobos Agile plan. "
        "Normal retune uses the universal sequential tuning path."));
    sweepContinuousCheck = new QCheckBox(QStringLiteral("Continuous / live sweep"), bandwidthBox);
    sweepContinuousCheck->setChecked(saved.value(QStringLiteral("sweep/continuous"), false).toBool());
    sweepButton = new QPushButton(QStringLiteral("Start wide sweep"), bandwidthBox);
    sweepButton->setObjectName(QStringLiteral("analyzerWideSweep"));
    bandwidthForm->addRow(QStringLiteral("RBW/bin:"), rbwLabel);
    bandwidthForm->addRow(QStringLiteral("VBW:"), vbwSpin);
    bandwidthForm->addRow(QStringLiteral("Replay speed:"), cycleSpin);
    bandwidthForm->addRow(QStringLiteral("Sweep start:"), sweepStartSpin);
    bandwidthForm->addRow(QStringLiteral("Sweep stop:"), sweepStopSpin);
    bandwidthForm->addRow(QStringLiteral("Overlap:"), sweepOverlapSpin);
    bandwidthForm->addRow(QStringLiteral("Discard edges:"), sweepEdgeSpin);
    bandwidthForm->addRow(QStringLiteral("Frames / segment:"), sweepAverageFramesSpin);
    bandwidthForm->addRow(QStringLiteral("Fobos sweep mode:"), sweepFobosModeCombo);
    bandwidthForm->addRow(sweepContinuousCheck);
    bandwidthForm->addRow(sweepButton);
    controlsLayout->addWidget(bandwidthBox);

    auto *detectorBox = new QGroupBox(QStringLiteral("Detector"), controls);
    auto *detectorForm = new QFormLayout(detectorBox);
    detectorCombo = new QComboBox(detectorBox);
    detectorCombo->addItems({QStringLiteral("Sample"), QStringLiteral("Positive peak"),
                             QStringLiteral("Negative peak"), QStringLiteral("RMS"),
                             QStringLiteral("Average"), QStringLiteral("Median"),
                             QStringLiteral("Quasi-peak")});
    detectorFrames = new QSpinBox(detectorBox);
    detectorFrames->setRange(1, 256);
    detectorFrames->setValue(8);
    detectorForm->addRow(QStringLiteral("Mode:"), detectorCombo);
    detectorForm->addRow(QStringLiteral("Frames:"), detectorFrames);
    controlsLayout->addWidget(detectorBox);

    auto *traceBox = new QGroupBox(QStringLiteral("Traces"), controls);
    auto *traceLayout = new QVBoxLayout(traceBox);
    auto *traceRow = new QHBoxLayout();
    currentTraceCheck = new QCheckBox(QStringLiteral("Current"), traceBox);
    maxHoldCheck = new QCheckBox(QStringLiteral("Max"), traceBox);
    minHoldCheck = new QCheckBox(QStringLiteral("Min"), traceBox);
    averageCheck = new QCheckBox(QStringLiteral("Average"), traceBox);
    currentTraceCheck->setChecked(saved.value(QStringLiteral("traces/current"), true).toBool());
    maxHoldCheck->setChecked(saved.value(QStringLiteral("traces/max"), false).toBool());
    minHoldCheck->setChecked(saved.value(QStringLiteral("traces/min"), false).toBool());
    averageCheck->setChecked(saved.value(QStringLiteral("traces/average"), false).toBool());
    traceRow->addWidget(currentTraceCheck);
    traceRow->addWidget(maxHoldCheck);
    traceRow->addWidget(minHoldCheck);
    traceRow->addWidget(averageCheck);
    auto *percentileRow = new QHBoxLayout();
    p50Check = new QCheckBox(QStringLiteral("P50"), traceBox);
    p90Check = new QCheckBox(QStringLiteral("P90"), traceBox);
    p99Check = new QCheckBox(QStringLiteral("P99"), traceBox);
    p50Check->setChecked(saved.value(QStringLiteral("traces/p50"), false).toBool());
    p90Check->setChecked(saved.value(QStringLiteral("traces/p90"), false).toBool());
    p99Check->setChecked(saved.value(QStringLiteral("traces/p99"), false).toBool());
    percentileRow->addWidget(p50Check);
    percentileRow->addWidget(p90Check);
    percentileRow->addWidget(p99Check);
    auto *resetTrace = new QPushButton(QStringLiteral("Clear traces"), traceBox);
    traceLayout->addLayout(traceRow);
    traceLayout->addLayout(percentileRow);
    traceLayout->addWidget(resetTrace);
    controlsLayout->addWidget(traceBox);

    auto *markerBox = new QGroupBox(QStringLiteral("Markers"), controls);
    auto *markerLayout = new QVBoxLayout(markerBox);
    auto *markerRow = new QHBoxLayout();
    markerCombo = new QComboBox(markerBox);
    while (analyzer.markerCount() < SpectrumScienceAnalyzer::InteractiveMarkerCount)
        analyzer.addMarker();
    const QJsonArray savedMarkerPairs = QJsonDocument::fromJson(
        saved.value(QStringLiteral("markers/pairs")).toByteArray()).array();
    if (!savedMarkerPairs.isEmpty()) {
        const QJsonObject primaryPair = savedMarkerPairs.first().toObject();
        analyzer.setMeasurementMarkerPair(primaryPair.value(QStringLiteral("first")).toInt(0),
                                          primaryPair.value(QStringLiteral("second")).toInt(1));
    }
    for (int index = 0; index < analyzer.markerCount(); ++index)
        markerCombo->addItem(analyzer.marker(index).label, index);
    auto *peakButton = new QPushButton(QStringLiteral("Peak"), markerBox);
    auto *previousButton = new QPushButton(QStringLiteral("Prev"), markerBox);
    auto *nextButton = new QPushButton(QStringLiteral("Next"), markerBox);
    auto *clearButton = new QPushButton(QStringLiteral("Clear"), markerBox);
    auto *pairsButton = new QPushButton(QStringLiteral("Pairs..."), markerBox);
    markerRow->addWidget(markerCombo);
    markerRow->addWidget(peakButton);
    markerRow->addWidget(previousButton);
    markerRow->addWidget(nextButton);
    markerRow->addWidget(clearButton);
    markerRow->addWidget(pairsButton);
    markerLabel = new QLabel(QStringLiteral("No active markers"), markerBox);
    markerLabel->setWordWrap(true);
    markerLayout->addLayout(markerRow);
    markerLayout->addWidget(markerLabel);
    controlsLayout->addWidget(markerBox);
    controlsLayout->addStretch(1);
    auto *controlsScroll = new QScrollArea(controlsDock);
    controlsScroll->setWidgetResizable(true);
    controlsScroll->setFrameShape(QFrame::NoFrame);
    controlsScroll->setWidget(controls);
    controlsDock->setWidget(controlsScroll);
    addDockWidget(Qt::RightDockWidgetArea, controlsDock);

    metricsLabel = new QLabel(QStringLiteral("Waiting for spectrum data"), this);
    metricsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusLabel = new QLabel(QStringLiteral("Ready"), this);
    statusBar()->addWidget(statusLabel, 1);
    statusBar()->addPermanentWidget(metricsLabel, 3);

    playbackTimer = new QTimer(this);
    playbackTimer->setSingleShot(true);
    receiverRestartTimer = new QTimer(this);
    receiverRestartTimer->setSingleShot(true);
    receiverRestartTimer->setInterval(250);
    connect(receiverRestartTimer, &QTimer::timeout, this, [this]() {
        if (liveClient.isConnected()) {
            sweepRestartPending = false;
            return;
        }
        if (sweepRestartPending) {
            const double originalCenterHz = sweepOriginalCenterHz;
            sweepRestartPending = false;
            sweepActive = false;
            sweepUsesNativeAgileScan = false;
            sweepSegmentFrameCounts.clear();
            sweepButton->setText(uiText(QStringLiteral("Start wide sweep")));
            standaloneReceiver.stop();
            setCenterFrequencyHz(originalCenterHz);
            setStatus(uiText(QStringLiteral("Restarting wide sweep with new receiver settings...")),
                      QColor(230, 180, 70));
            toggleWideSweep();
            return;
        }
        if (!standaloneReceiver.isRunning()) return;
        standaloneReceiver.stop();
        toggleStandaloneReceiver();
    });
    connect(playbackTimer, &QTimer::timeout, this, &AnalyzerMainWindow::readNextRecordedFrame);
    connect(openButton, &QPushButton::clicked, this, &AnalyzerMainWindow::openRecording);
    connect(playButton, &QPushButton::clicked, this, &AnalyzerMainWindow::togglePlayback);
    connect(connectButton, &QPushButton::clicked, this, [this]() {
        if (liveClient.isConnected()) {
            automaticAttachCheck->setChecked(false);
            liveClient.disconnectFromPublisher();
        } else {
            automaticAttachCheck->setChecked(true);
            connectLive();
        }
    });
    connect(exportButton, &QPushButton::clicked, this, &AnalyzerMainWindow::exportCurrentCsv);
    connect(instrumentLabButton, &QPushButton::clicked, this, [this]() {
        if (!instrumentDialog) {
            instrumentDialog = new AnalyzerInstrumentDialog(ukrainianUi, this);
            connect(instrumentDialog, &QObject::destroyed, this, [this]() {
                instrumentDialog = nullptr;
            });
        }
        instrumentDialog->show();
        instrumentDialog->raise();
        instrumentDialog->activateWindow();
    });
    connect(multiSdrButton, &QPushButton::clicked, this, [this]() {
        if (sweepActive) toggleWideSweep();
        standaloneReceiver.stop();
        automaticAttachCheck->setChecked(false);
        liveClient.disconnectFromPublisher();
        MultiSdrDialog dialog(ukrainianUi, this);
        dialog.exec();
    });
    connect(sweepButton, &QPushButton::clicked, this, &AnalyzerMainWindow::toggleWideSweep);
    connect(scaleTopSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (value <= scaleBottomSpin->value()) scaleBottomSpin->setValue(value - 1.0);
        const QSignalBlocker blocker(scaleTopSlider);
        scaleTopSlider->setValue(qRound(value * 10.0));
        scaleTopValueLabel->setText(QStringLiteral("%1 dB").arg(value, 0, 'f', 1));
        applyAnalyzerSettings();
    });
    connect(scaleBottomSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (value >= scaleTopSpin->value()) scaleTopSpin->setValue(value + 1.0);
        const QSignalBlocker blocker(scaleBottomSlider);
        scaleBottomSlider->setValue(qRound(value * 10.0));
        scaleBottomValueLabel->setText(QStringLiteral("%1 dB").arg(value, 0, 'f', 1));
        applyAnalyzerSettings();
    });
    connect(scaleTopSlider, &QSlider::valueChanged, this, [this](int value) {
        scaleTopSpin->setValue(value / 10.0);
    });
    connect(scaleBottomSlider, &QSlider::valueChanged, this, [this](int value) {
        scaleBottomSpin->setValue(value / 10.0);
    });
    connect(frequencyZoomSlider, &QSlider::valueChanged, this, [this](int value) {
        frequencyZoomValueLabel->setText(
            uiText(QStringLiteral("%1% span")).arg(value / 10.0, 0, 'f', 1));
        applyFrequencyView();
    });
    connect(frequencyZoomDivisorSlider, &QSlider::valueChanged, this, [this](int value) {
        frequencyZoomDivisorValueLabel->setText(QStringLiteral("1:%1").arg(value));
        applyFrequencyView();
    });
    connect(receiverRefreshButton, &QPushButton::clicked, this, &AnalyzerMainWindow::refreshReceivers);
    connect(receiverStartButton, &QPushButton::clicked, this, &AnalyzerMainWindow::toggleStandaloneReceiver);
    connect(receiverCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        populateReceiverInputs();
        populateSampleRates();
        updateSourceControls();
    });
    connect(sampleRateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateResolutionControls(); scheduleStandaloneReceiverRestart(); });
    connect(receiverFftCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        updateResolutionControls();
        scheduleStandaloneReceiverRestart();
    });
    connect(hzPerBinCheck, &QCheckBox::toggled, this, [this](bool) {
        updateResolutionControls();
        saveSettings();
        scheduleStandaloneReceiverRestart();
    });
    connect(hzPerBinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { updateResolutionControls(); });
    connect(hzPerBinSpin, &QDoubleSpinBox::editingFinished,
            this, &AnalyzerMainWindow::scheduleStandaloneReceiverRestart);
    connect(displayReductionRatioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        updateResolutionControls(); saveSettings(); scheduleStandaloneReceiverRestart();
    });
    connect(displayReductionMethodCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { saveSettings(); scheduleStandaloneReceiverRestart(); });
    connect(receiverInputCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateResolutionControls(); scheduleStandaloneReceiverRestart(); });
    connect(centerUnitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        const double frequencyHz = centerSpin->value() * centerUnitFactor;
        centerUnitFactor = centerUnitCombo->currentData().toDouble();
        updateCenterFrequencyUnit();
        setCenterFrequencyHz(frequencyHz);
        saveSettings();
    });
    connect(centerSpin, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (standaloneReceiver.isRunning())
            standaloneReceiver.retune(selectedCenterFrequencyHz());
        saveSettings();
    });
    const auto applyLiveGains = [this](int) {
        if (standaloneReceiver.isRunning()) {
            standaloneReceiver.setGains(receiverLnaSpin->value(), receiverVgaSpin->value());
            saveSettings();
        }
    };
    connect(receiverLnaSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, applyLiveGains);
    connect(receiverVgaSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, applyLiveGains);
    connect(automaticAttachCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled && !liveClient.isConnected()) connectLive();
        saveSettings();
    });
    connect(automaticViewCheck, &QCheckBox::toggled, this, [this](bool) { applyFrequencyView(); });
    connect(viewMinimumSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { applyFrequencyView(); });
    connect(viewMaximumSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { applyFrequencyView(); });
    connect(vbwSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { applyAnalyzerSettings(); });
    connect(detectorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { applyAnalyzerSettings(); });
    connect(detectorFrames, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { applyAnalyzerSettings(); });
    for (QCheckBox *box : {currentTraceCheck, maxHoldCheck, minHoldCheck, averageCheck,
                           p50Check, p90Check, p99Check})
        connect(box, &QCheckBox::toggled, this, [this](bool) { applyAnalyzerSettings(); refreshDisplay(); });
    connect(resetTrace, &QPushButton::clicked, this, [this]() { analyzer.resetTraces(); refreshDisplay(); });
    connect(markerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) { activeMarker = std::max(0, index); });
    connect(spectrum, &AnalyzerSpectrumWidget::frequencyClicked, this, [this](double hz) {
        analyzer.setMarker(activeMarker, hz); refreshMarkers(); refreshDisplay();
    });
    connect(spectrum, &AnalyzerSpectrumWidget::frequencyPanRequested, this, [this](double deltaHz) {
        if (currentFrequencies.size() < 2 || sourceFrequencies.size() < 2) return;
        const double sourceMinimum = sourceFrequencies.front();
        const double sourceMaximum = sourceFrequencies.back();
        const double visibleSpan = currentFrequencies.back() - currentFrequencies.front();
        if (!(visibleSpan > 0.0) || visibleSpan >= sourceMaximum - sourceMinimum) return;
        double minimum = currentFrequencies.front() + deltaHz;
        minimum = std::clamp(minimum, sourceMinimum, sourceMaximum - visibleSpan);
        const double maximum = minimum + visibleSpan;
        const QSignalBlocker blockZoom(frequencyZoomSlider);
        const QSignalBlocker blockZoomDivisor(frequencyZoomDivisorSlider);
        const QSignalBlocker blockAutomatic(automaticViewCheck);
        const QSignalBlocker blockMinimum(viewMinimumSpin);
        const QSignalBlocker blockMaximum(viewMaximumSpin);
        frequencyZoomSlider->setValue(1000);
        frequencyZoomDivisorSlider->setValue(1);
        frequencyZoomValueLabel->setText(uiText(QStringLiteral("%1% span")).arg(QStringLiteral("100.0")));
        frequencyZoomDivisorValueLabel->setText(QStringLiteral("1:1"));
        automaticViewCheck->setChecked(false);
        viewMinimumSpin->setEnabled(true);
        viewMaximumSpin->setEnabled(true);
        viewMinimumSpin->setValue(minimum / 1.0e6);
        viewMaximumSpin->setValue(maximum / 1.0e6);
        applyFrequencyView();
    });
    connect(spectrum, &AnalyzerSpectrumWidget::frequencyRangeSelected,
            this, [this](double startHz, double endHz) {
        analyzer.setMarker(analyzer.measurementMarkerFirst(), startHz);
        analyzer.setMarker(analyzer.measurementMarkerSecond(), endHz);
        refreshMarkers();
        refreshDisplay();
    });
    connect(peakButton, &QPushButton::clicked, this, [this]() {
        analyzer.setMarker(activeMarker, analyzer.strongestPeakFrequency()); refreshMarkers(); refreshDisplay();
    });
    connect(previousButton, &QPushButton::clicked, this, [this]() {
        analyzer.setMarker(activeMarker, analyzer.adjacentPeakFrequency(activeMarker, -1)); refreshMarkers(); refreshDisplay();
    });
    connect(nextButton, &QPushButton::clicked, this, [this]() {
        analyzer.setMarker(activeMarker, analyzer.adjacentPeakFrequency(activeMarker, 1)); refreshMarkers(); refreshDisplay();
    });
    connect(clearButton, &QPushButton::clicked, this, [this]() {
        analyzer.clearMarker(activeMarker); refreshMarkers(); refreshDisplay();
    });
    connect(pairsButton, &QPushButton::clicked, this, [this]() {
        if (!markerPairsDialog) {
            QVector<SpectrumScienceMarker> markers;
            markers.reserve(analyzer.markerCount());
            for (int index = 0; index < analyzer.markerCount(); ++index)
                markers.append(analyzer.marker(index));
            markerPairsDialog = new SpectrumMarkerPairsDialog(ukrainianUi, this);
            markerPairsDialog->setMarkers(markers);
            QSettings settings;
            const QJsonArray savedPairs = QJsonDocument::fromJson(
                settings.value(QStringLiteral("markers/pairs")).toByteArray()).array();
            markerPairsDialog->setPairConfiguration(savedPairs);
            connect(markerPairsDialog, &SpectrumMarkerPairsDialog::primaryPairChanged,
                    this, [this](int first, int second) {
                analyzer.setMeasurementMarkerPair(first, second);
                refreshMetrics();
                refreshDisplay();
            });
            connect(markerPairsDialog, &SpectrumMarkerPairsDialog::configurationChanged,
                    this, [this]() {
                QSettings settings;
                settings.setValue(QStringLiteral("markers/pairs"),
                                  QJsonDocument(markerPairsDialog->pairConfiguration())
                                      .toJson(QJsonDocument::Compact));
            });
        }
        markerPairsDialog->show();
        markerPairsDialog->raise();
        markerPairsDialog->activateWindow();
    });
    applyAnalyzerSettings();
    analyzer.setMetricsUpdateIntervalMs(100);
    populateReceiverInputs();
    populateSampleRates();
    updateResolutionControls();
    viewMinimumSpin->setEnabled(!automaticViewCheck->isChecked());
    viewMaximumSpin->setEnabled(!automaticViewCheck->isChecked());
    connect(automaticViewCheck, &QCheckBox::toggled, this, [this](bool automatic) {
        viewMinimumSpin->setEnabled(!automatic);
        viewMaximumSpin->setEnabled(!automatic);
    });
    updateSourceControls();

    receiverScanWatcher = new QFutureWatcher<QVector<AnalyzerReceiverDevice>>(this);
    connect(receiverScanWatcher, &QFutureWatcher<QVector<AnalyzerReceiverDevice>>::finished,
            this, [this]() {
        const QString wantedBackend = receiverCombo->currentData(Qt::UserRole).toString();
        const int wantedIndex = receiverCombo->currentData(Qt::UserRole + 1).toInt();
        const QString wantedAddress = receiverCombo->currentData(Qt::UserRole + 2).toString();
        receiverDevices = receiverScanWatcher->result();
        const QSignalBlocker blocker(receiverCombo);
        receiverCombo->clear();
        int selected = -1;
        for (const AnalyzerReceiverDevice &item : receiverDevices) {
            receiverCombo->addItem(item.label, item.backendId);
            receiverCombo->setItemData(receiverCombo->count() - 1, item.nativeIndex, Qt::UserRole + 1);
            receiverCombo->setItemData(receiverCombo->count() - 1, item.uhdDeviceAddress, Qt::UserRole + 2);
            if (item.backendId == wantedBackend &&
                (wantedAddress.isEmpty() ? item.nativeIndex == wantedIndex : item.uhdDeviceAddress == wantedAddress))
                selected = receiverCombo->count() - 1;
        }
        if (receiverCombo->count() == 0)
            receiverCombo->addItem(uiText(QStringLiteral("No compatible receiver found")));
        else
            receiverCombo->setCurrentIndex(selected >= 0 ? selected : 0);
        populateReceiverInputs();
        populateSampleRates();
        updateReceiverControlRanges();
        receiverRefreshButton->setText(uiText(QStringLiteral("Refresh")));
        updateSourceControls();
    });
    translateStaticUi();
}

void AnalyzerMainWindow::applyAnalyzerSettings() {
    analyzer.setTraceEnabled(maxHoldCheck->isChecked(), minHoldCheck->isChecked(), averageCheck->isChecked());
    analyzer.setDetector(detectorCombo->currentIndex(), detectorFrames->value());
    analyzer.setVbwHz(vbwSpin->value());
    analyzer.setPercentileTraces(p50Check->isChecked(), p90Check->isChecked(), p99Check->isChecked());
    spectrum->setAmplitudeBounds(scaleTopSpin->value(), scaleBottomSpin->value());
}

void AnalyzerMainWindow::consumeFrame(const AnalyzerSpectrumFrame &frame) {
    playbackRunning = false;
    playbackTimer->stop();
    playButton->setText(uiText(QStringLiteral("Play")));
    if (consumeWideSweepFrame(frame)) return;
    sourceFrequencies = uniformFrequencies(frame.minimumHz, frame.maximumHz, frame.levelsDb.size());
    sourceLevels = frame.levelsDb;
    currentFftLength = frame.sourceFftLength;
    sourceBinWidthHz = currentFftLength > 0
                           ? std::abs(frame.maximumHz - frame.minimumHz) / currentFftLength
                           : 0.0;
    const QString receiverBackend = receiverCombo->currentData(Qt::UserRole).toString();
    if (standaloneReceiver.isRunning() &&
        receiverBackend.startsWith(QStringLiteral("fobos-")) &&
        receiverInputCombo->currentData().toInt() != 0)
        sourceBinWidthHz *= 2.0;
    if (liveClient.isConnected()) {
        const QSignalBlocker blockCenter(centerSpin);
        setCenterFrequencyHz(frame.centerHz);
        setSelectedSampleRateHz(std::abs(frame.maximumHz - frame.minimumHz));
        updateResolutionControls();
    }
    sourceLabel->setText(QStringLiteral("  %1  %2 .. %3")
                         .arg(liveClient.isConnected() ? uiText(QStringLiteral("OBRII LIVE"))
                                                       : uiText(QStringLiteral("RECEIVER")))
                         .arg(frequencyText(frame.minimumHz), frequencyText(frame.maximumHz)));
    if (automaticViewCheck->isChecked()) {
        const QSignalBlocker blockMinimum(viewMinimumSpin);
        const QSignalBlocker blockMaximum(viewMaximumSpin);
        viewMinimumSpin->setValue(frame.minimumHz / 1.0e6);
        viewMaximumSpin->setValue(frame.maximumHz / 1.0e6);
    }
    applyFrequencyView();
}

bool AnalyzerMainWindow::consumeWideSweepFrame(const AnalyzerSpectrumFrame &frame) {
    if (!sweepActive || sweepSegmentIndex < 0 ||
        sweepSegmentIndex >= sweepEngine.plan().segments.size())
        return false;
    const InstrumentSweepPlan &plan = sweepEngine.plan();
    const double tolerance = std::max(1000.0,
        plan.settings.captureBandwidthHz * 0.05);
    const int framesPerSegment = sweepAverageFramesSpin->value();
    int completedSegment = -1;

    if (sweepUsesNativeAgileScan) {
        int frameSegment = -1;
        double nearestDistance = std::numeric_limits<double>::max();
        for (int index = 0; index < plan.segments.size(); ++index) {
            const double distance = std::abs(frame.centerHz - plan.segments[index].centerHz);
            if (distance < nearestDistance) {
                nearestDistance = distance;
                frameSegment = index;
            }
        }
        if (frameSegment < 0 || nearestDistance > tolerance) return true;
        if (sweepSegmentFrameCounts.size() != plan.segments.size())
            sweepSegmentFrameCounts.fill(0, plan.segments.size());
        if (sweepSegmentFrameCounts[frameSegment] >= framesPerSegment) return true;
        if (!sweepEngine.addFrame(frameSegment, frame.minimumHz,
                                  frame.maximumHz, frame.levelsDb)) {
            sweepActive = false;
            standaloneReceiver.stopNativeAgileScan();
            sweepUsesNativeAgileScan = false;
            sweepButton->setText(uiText(QStringLiteral("Start wide sweep")));
            setStatus(uiText(QStringLiteral("Wide sweep rejected an incompatible Agile scan frame")),
                      QColor(235, 104, 91));
            updateSourceControls();
            return true;
        }
        ++sweepSegmentFrameCounts[frameSegment];
        if (sweepSegmentFrameCounts[frameSegment] < framesPerSegment) return true;
        completedSegment = frameSegment;
    } else {
        const InstrumentSweepSegment &segment = plan.segments[sweepSegmentIndex];
        if (std::abs(frame.centerHz - segment.centerHz) > tolerance) return true;
        if (sweepSettleFrames > 0) {
            --sweepSettleFrames;
            return true;
        }
        if (!sweepEngine.addFrame(sweepSegmentIndex, frame.minimumHz,
                              frame.maximumHz, frame.levelsDb)) {
            sweepActive = false;
            sweepButton->setText(uiText(QStringLiteral("Start wide sweep")));
            setStatus(uiText(QStringLiteral("Wide sweep rejected an incompatible receiver frame")),
                      QColor(235, 104, 91));
            updateSourceControls();
            return true;
        }
        ++sweepFramesAccumulated;
        if (sweepFramesAccumulated < framesPerSegment) return true;
        sweepFramesAccumulated = 0;
        completedSegment = sweepSegmentIndex;
    }

    if (sweepContinuousCheck->isChecked()) {
        sweepEngine.updateResultForSegment(completedSegment, sourceFrequencies, sourceLevels,
                                           static_cast<float>(scaleBottomSpin->value()));
        currentFftLength = static_cast<int>(sourceLevels.size());
        sourceBinWidthHz = sourceFrequencies.size() > 1
                               ? std::abs(sourceFrequencies.back() - sourceFrequencies.front()) /
                                     static_cast<double>(sourceFrequencies.size() - 1)
                               : 0.0;
        {
            const QSignalBlocker blockAutomatic(automaticViewCheck);
            automaticViewCheck->setChecked(true);
        }
        sourceLabel->setText(uiText(QStringLiteral("  LIVE SWEEP  pass %1  %2 .. %3"))
                                 .arg(sweepPassCount + 1)
                                 .arg(frequencyText(plan.settings.startHz),
                                      frequencyText(plan.settings.stopHz)));
        if (!sweepDisplayTimer.isValid() || sweepDisplayTimer.elapsed() >= 100) {
            applyFrequencyView();
            sweepDisplayTimer.restart();
        }
    }

    bool passComplete = false;
    int completedSegments = 0;
    if (sweepUsesNativeAgileScan) {
        completedSegments = static_cast<int>(std::count_if(
            sweepSegmentFrameCounts.cbegin(), sweepSegmentFrameCounts.cend(),
            [framesPerSegment](int count) { return count >= framesPerSegment; }));
        passComplete = completedSegments == plan.segments.size();
        if (!passComplete) {
            setStatus(uiText(QStringLiteral("%1 sweep: %2 / %3 segments"))
                          .arg(sweepContinuousCheck->isChecked() ? uiText(QStringLiteral("Live Agile"))
                                                                  : uiText(QStringLiteral("Agile")))
                          .arg(completedSegments)
                          .arg(plan.segments.size()),
                      QColor(110, 190, 230));
            return true;
        }
    } else {
        ++sweepSegmentIndex;
        passComplete = sweepSegmentIndex >= plan.segments.size();
        if (!passComplete) {
            sweepSettleFrames = 2;
            const double nextCenter = plan.segments[sweepSegmentIndex].centerHz;
            if (!standaloneReceiver.retune(nextCenter)) {
                sweepActive = false;
                standaloneReceiver.stop();
                setCenterFrequencyHz(sweepOriginalCenterHz);
                sweepButton->setText(uiText(QStringLiteral("Start wide sweep")));
                setStatus(uiText(QStringLiteral("Receiver rejected the next sweep frequency")), QColor(235, 104, 91));
                updateSourceControls();
                return true;
            }
            setCenterFrequencyHz(nextCenter);
            setStatus(uiText(QStringLiteral("%1: segment %2 / %3"))
                          .arg(sweepContinuousCheck->isChecked() ? uiText(QStringLiteral("Live sweep"))
                                                                  : uiText(QStringLiteral("Wide sweep")))
                          .arg(sweepSegmentIndex + 1)
                          .arg(plan.segments.size()),
                      QColor(110, 190, 230));
            return true;
        }
    }

    const InstrumentSweepResult result = sweepEngine.finish();
    if (sweepContinuousCheck->isChecked()) {
        if (sourceFrequencies.size() != result.frequenciesHz.size()) {
            sourceFrequencies = result.frequenciesHz;
            sourceLevels.assign(result.levelsDb.size(), static_cast<float>(scaleBottomSpin->value()));
        }
        for (std::size_t index = 0; index < result.levelsDb.size(); ++index) {
            if (std::isfinite(result.levelsDb[index])) sourceLevels[index] = result.levelsDb[index];
        }
        currentFftLength = static_cast<int>(sourceLevels.size());
        sourceBinWidthHz = sourceFrequencies.size() > 1
                               ? std::abs(sourceFrequencies.back() - sourceFrequencies.front()) /
                                     static_cast<double>(sourceFrequencies.size() - 1)
                               : 0.0;
        ++sweepPassCount;
        const InstrumentSweepPlan nextPlan = plan;
        if (!sweepEngine.begin(nextPlan)) {
            sweepActive = false;
            if (sweepUsesNativeAgileScan) standaloneReceiver.stopNativeAgileScan();
            sweepUsesNativeAgileScan = false;
            sweepButton->setText(uiText(QStringLiteral("Start wide sweep")));
            setStatus(uiText(QStringLiteral("Could not restart continuous sweep storage")),
                      QColor(235, 104, 91));
            updateSourceControls();
            return true;
        }
        sweepSegmentIndex = 0;
        sweepSettleFrames = sweepUsesNativeAgileScan ? 0 : 2;
        sweepFramesAccumulated = 0;
        sweepSegmentFrameCounts.fill(0, nextPlan.segments.size());
        if (!sweepUsesNativeAgileScan) {
            const double firstCenter = nextPlan.segments.front().centerHz;
            if (!standaloneReceiver.retune(firstCenter)) {
                sweepActive = false;
                sweepButton->setText(uiText(QStringLiteral("Start wide sweep")));
                setStatus(uiText(QStringLiteral("Receiver rejected the next live sweep pass")),
                          QColor(235, 104, 91));
                updateSourceControls();
                return true;
            }
            setCenterFrequencyHz(firstCenter);
        }
        applyFrequencyView();
        sweepDisplayTimer.restart();
        setStatus(uiText(QStringLiteral("%1 pass %2 complete; restarting (%3 missing bins)"))
                      .arg(sweepUsesNativeAgileScan ? uiText(QStringLiteral("Live Agile sweep"))
                                                   : uiText(QStringLiteral("Live sweep")))
                      .arg(sweepPassCount)
                      .arg(result.missingBins),
                  result.missingBins == 0 ? QColor(76, 220, 139) : QColor(230, 180, 70));
        return true;
    }

    sweepActive = false;
    sweepUsesNativeAgileScan = false;
    sweepButton->setText(uiText(QStringLiteral("Start wide sweep")));
    standaloneReceiver.stop();
    setCenterFrequencyHz(sweepOriginalCenterHz);
    sourceFrequencies = result.frequenciesHz;
    sourceLevels = result.levelsDb;
    currentFftLength = static_cast<int>(result.levelsDb.size());
    sourceBinWidthHz = result.frequenciesHz.size() > 1
                           ? std::abs(result.frequenciesHz.back() - result.frequenciesHz.front()) /
                                 static_cast<double>(result.frequenciesHz.size() - 1)
                           : 0.0;
    {
        const QSignalBlocker blockAutomatic(automaticViewCheck);
        automaticViewCheck->setChecked(true);
    }
    viewMinimumSpin->setEnabled(false);
    viewMaximumSpin->setEnabled(false);
    sourceLabel->setText(uiText(QStringLiteral("  WIDE SWEEP  %1 .. %2"))
                             .arg(frequencyText(sweepEngine.plan().settings.startHz),
                                  frequencyText(sweepEngine.plan().settings.stopHz)));
    analyzer.clear();
    applyFrequencyView();
    setStatus(uiText(QStringLiteral("Wide sweep complete: %1 segments, %2 missing bins"))
                  .arg(sweepEngine.plan().segments.size())
                  .arg(result.missingBins),
              result.missingBins == 0 ? QColor(76, 220, 139) : QColor(230, 180, 70));
    return true;
}

void AnalyzerMainWindow::consumeRecordedFrame(const SpectrumFrameRecord &frame) {
    sourceFrequencies = uniformFrequencies(frame.minFrequency, frame.maxFrequency, frame.magnitudes.size());
    sourceLevels = frame.magnitudes;
    currentFftLength = frame.fftLength;
    sourceBinWidthHz = currentFftLength > 0
                           ? std::abs(frame.maxFrequency - frame.minFrequency) / currentFftLength
                           : 0.0;
    applyFrequencyView();
}

void AnalyzerMainWindow::applyFrequencyView() {
    if (sourceFrequencies.size() != sourceLevels.size() || sourceFrequencies.empty()) return;
    double baseMinimum = sourceFrequencies.front();
    double baseMaximum = sourceFrequencies.back();
    if (!automaticViewCheck->isChecked()) {
        baseMinimum = std::max<double>(baseMinimum,
            std::min(viewMinimumSpin->value(), viewMaximumSpin->value()) * 1.0e6);
        baseMaximum = std::min<double>(baseMaximum,
            std::max(viewMinimumSpin->value(), viewMaximumSpin->value()) * 1.0e6);
    }
    if (!(baseMaximum > baseMinimum)) {
        setStatus(uiText(QStringLiteral("Visible frequency range is outside the source span")), QColor(235, 104, 91));
        return;
    }
    const double zoomFraction = (frequencyZoomSlider->value() / 10.0) / 100.0 /
                                std::max(1, frequencyZoomDivisorSlider->value());
    const double center = (baseMinimum + baseMaximum) * 0.5;
    const double visibleSpan = (baseMaximum - baseMinimum) * zoomFraction;
    const double minimum = center - visibleSpan * 0.5;
    const double maximum = center + visibleSpan * 0.5;
    const std::size_t first = static_cast<std::size_t>(
        std::lower_bound(sourceFrequencies.begin(), sourceFrequencies.end(), minimum) - sourceFrequencies.begin());
    const std::size_t last = static_cast<std::size_t>(
        std::upper_bound(sourceFrequencies.begin(), sourceFrequencies.end(), maximum) - sourceFrequencies.begin());
    if (last <= first + 1) return;
    currentFrequencies.assign(sourceFrequencies.begin() + static_cast<std::ptrdiff_t>(first),
                              sourceFrequencies.begin() + static_cast<std::ptrdiff_t>(last));
    currentLevels.assign(sourceLevels.begin() + static_cast<std::ptrdiff_t>(first),
                         sourceLevels.begin() + static_cast<std::ptrdiff_t>(last));
    analyzer.update(currentFrequencies, currentLevels);
    refreshDisplay();
}

void AnalyzerMainWindow::refreshDisplay() {
    spectrum->setFrame(currentFrequencies, analyzer.levels());
    spectrum->setTraces(analyzer.maxHoldTrace(), analyzer.minHoldTrace(), analyzer.averageTrace(),
                        analyzer.percentile50Trace(), analyzer.percentile90Trace(), analyzer.percentile99Trace(),
                        maxHoldCheck->isChecked(), minHoldCheck->isChecked(), averageCheck->isChecked(),
                        p50Check->isChecked(), p90Check->isChecked(), p99Check->isChecked(),
                        currentTraceCheck->isChecked());
    refreshMarkers();
    refreshMetrics();
    if (currentFrequencies.size() > 1) {
        const double binWidth = sourceBinWidthHz > 0.0
                                    ? sourceBinWidthHz
                                    : std::abs(currentFrequencies.back() - currentFrequencies.front()) /
                                          static_cast<double>(currentFrequencies.size() - 1);
        rbwLabel->setText(QStringLiteral("%1 / %2")
                          .arg(frequencyText(binWidth), QString::number(currentFftLength)));
    }
}

void AnalyzerMainWindow::refreshMetrics() {
    const auto &m = analyzer.metrics();
    if (!m.valid) {
        metricsLabel->setText(uiText(QStringLiteral("No measurement")));
        metricsLabel->setToolTip(QString());
        spectrum->setMeasurementDetails(QString());
        return;
    }
    metricsLabel->setText(uiText(QStringLiteral("%1 .. %2 | Span %3 | Peak %4 @ %5 | SNR %6 dB | Power %7"))
        .arg(frequencyText(m.rangeStartHz), frequencyText(m.rangeEndHz),
             frequencyText(std::abs(m.rangeEndHz - m.rangeStartHz)))
        .arg(m.peakDb, 0, 'f', 2).arg(frequencyText(m.peakFrequencyHz))
        .arg(m.snrDb, 0, 'f', 2).arg(m.channelPowerDb, 0, 'f', 2));
    const QString details = uiText(QStringLiteral(
        "Range: %1 .. %2\nSpan: %3; bins: %4\nPeak: %5 dBFS @ %6\n"
        "Noise: %7 dBFS; SNR: %8 dB\nChannel power: %9 dBFS\n"
        "Width -3/-6/-20 dB: %10 / %11 / %12\n"
        "OBW 90/95/99%: %13 / %14 / %15\nACPR lower/upper: %16 / %17 dB"))
        .arg(frequencyText(m.rangeStartHz), frequencyText(m.rangeEndHz),
             frequencyText(std::abs(m.rangeEndHz - m.rangeStartHz)))
        .arg(m.binCount)
        .arg(m.peakDb, 0, 'f', 2).arg(frequencyText(m.peakFrequencyHz))
        .arg(m.noiseFloorDb, 0, 'f', 2).arg(m.snrDb, 0, 'f', 2)
        .arg(m.channelPowerDb, 0, 'f', 2)
        .arg(frequencyText(m.width3DbHz), frequencyText(m.width6DbHz), frequencyText(m.width20DbHz))
        .arg(frequencyText(m.occupiedBandwidth90Hz), frequencyText(m.occupiedBandwidth95Hz),
             frequencyText(m.occupiedBandwidthHz))
        .arg(m.acprLowerDb, 0, 'f', 2).arg(m.acprUpperDb, 0, 'f', 2);
    metricsLabel->setToolTip(details);
    const int first = analyzer.measurementMarkerFirst();
    const int second = analyzer.measurementMarkerSecond();
    const bool rangeSelected = analyzer.marker(first).enabled && analyzer.marker(second).enabled;
    spectrum->setMeasurementDetails(rangeSelected ? details : QString());
}

void AnalyzerMainWindow::refreshMarkers() {
    QVector<SpectrumScienceMarker> list;
    QStringList labels;
    for (int i = 0; i < analyzer.markerCount(); ++i) {
        const auto marker = analyzer.marker(i);
        list.append(marker);
        if (marker.enabled)
            labels.append(QStringLiteral("%1 %2 %3 dB").arg(marker.label, frequencyText(marker.frequencyHz)).arg(marker.levelDb, 0, 'f', 2));
    }
    spectrum->setMarkers(list);
    if (markerPairsDialog) markerPairsDialog->setMarkers(list);
    markerLabel->setText(labels.isEmpty() ? uiText(QStringLiteral("No active markers"))
                                          : labels.join(QStringLiteral(" | ")));
}

void AnalyzerMainWindow::openRecording() {
    const QString path = QFileDialog::getOpenFileName(this, uiText(QStringLiteral("Open Obrii spectrum recording")),
                                                      QString(), uiText(QStringLiteral("Obrii spectrum (*.fbspec *.spectrum);;All files (*)")));
    if (path.isEmpty()) return;
    auto nextReader = std::make_unique<SpectrumFrameReader>();
    QString error;
    if (!nextReader->open(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("Obrii Analyzer"), error);
        return;
    }
    automaticAttachCheck->setChecked(false);
    liveClient.disconnectFromPublisher();
    standaloneReceiver.stop();
    reader = std::move(nextReader);
    recordingPath = path;
    previousFrameElapsedMs = -1;
    playbackRunning = true;
    playButton->setText(uiText(QStringLiteral("Pause")));
    sourceLabel->setText(uiText(QStringLiteral("  FILE  %1")).arg(QFileInfo(path).fileName()));
    analyzer.clear();
    readNextRecordedFrame();
}

void AnalyzerMainWindow::togglePlayback() {
    if (!reader) { openRecording(); return; }
    playbackRunning = !playbackRunning;
    playButton->setText(playbackRunning ? uiText(QStringLiteral("Pause")) : uiText(QStringLiteral("Play")));
    if (playbackRunning) readNextRecordedFrame(); else playbackTimer->stop();
}

void AnalyzerMainWindow::readNextRecordedFrame() {
    if (!reader || !playbackRunning) return;
    SpectrumFrameRecord frame;
    QString error;
    if (!reader->readNext(frame, &error)) {
        if (!error.isEmpty()) setStatus(error, QColor(235, 104, 91));
        else setStatus(uiText(QStringLiteral("Replay complete")), QColor(110, 190, 230));
        playbackRunning = false;
        playButton->setText(uiText(QStringLiteral("Play")));
        return;
    }
    const qint64 previous = previousFrameElapsedMs;
    previousFrameElapsedMs = frame.elapsedMs;
    consumeRecordedFrame(frame);
    const double speed = std::max(0.01, cycleSpin->value());
    const int delay = previous < 0 ? 1 : std::clamp(qRound((frame.elapsedMs - previous) / speed), 1, 1000);
    playbackTimer->start(delay);
}

void AnalyzerMainWindow::exportCurrentCsv() {
    if (currentFrequencies.empty() || analyzer.levels().size() != currentFrequencies.size()) return;
    const QString path = QFileDialog::getSaveFileName(this, uiText(QStringLiteral("Export spectrum CSV")),
                                                      QStringLiteral("obrii_analyzer.csv"), QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Obrii Analyzer"), file.errorString());
        return;
    }
    QTextStream out(&file);
    out.setCodec("UTF-8");
    out << "frequency_hz,level_dbfs\n";
    for (std::size_t i = 0; i < currentFrequencies.size(); ++i)
        out << QString::number(currentFrequencies[i], 'f', 3) << ',' << QString::number(analyzer.levels()[i], 'f', 6) << '\n';
    setStatus(uiText(QStringLiteral("Exported %1")).arg(QFileInfo(path).fileName()), QColor(76, 220, 139));
}

void AnalyzerMainWindow::connectLive() {
    reader.reset();
    playbackRunning = false;
    playbackTimer->stop();
    playButton->setText(uiText(QStringLiteral("Play")));
    if (!standaloneReceiver.isRunning())
        setStatus(uiText(QStringLiteral("Connecting to Obrii SDR...")), QColor(230, 180, 70));
    liveClient.connectToPublisher();
}

void AnalyzerMainWindow::refreshReceivers() {
    if (standaloneReceiver.isRunning() ||
        (receiverScanWatcher && receiverScanWatcher->isRunning())) return;
    QSettings saved;
    const QString wantedBackend = receiverCombo->currentData(Qt::UserRole).toString().isEmpty()
                                      ? saved.value(QStringLiteral("receiver/backendId")).toString()
                                      : receiverCombo->currentData(Qt::UserRole).toString();
    const int wantedIndex = receiverCombo->currentData(Qt::UserRole + 1).isValid()
                                ? receiverCombo->currentData(Qt::UserRole + 1).toInt()
                                : saved.value(QStringLiteral("receiver/nativeIndex"), 0).toInt();
    const QString wantedAddress = receiverCombo->currentIndex() >= 0
        ? receiverCombo->currentData(Qt::UserRole + 2).toString()
        : saved.value(QStringLiteral("receiver/uhdAddress")).toString();
    const QSignalBlocker blocker(receiverCombo);
    receiverCombo->clear();
    receiverCombo->addItem(uiText(QStringLiteral("Scanning receivers...")), wantedBackend);
    receiverCombo->setItemData(0, wantedIndex, Qt::UserRole + 1);
    receiverCombo->setItemData(0, wantedAddress, Qt::UserRole + 2);
    receiverStartButton->setEnabled(false);
    sweepButton->setEnabled(false);
    receiverRefreshButton->setText(uiText(QStringLiteral("Scanning...")));
    receiverRefreshButton->setEnabled(false);
    receiverScanWatcher->setFuture(QtConcurrent::run([] {
        return AnalyzerReceiver::enumerateDevices(true);
    }));
}

void AnalyzerMainWindow::toggleStandaloneReceiver() {
    if (sweepActive) toggleWideSweep();
    if (standaloneReceiver.isRunning()) {
        standaloneReceiver.stop();
        receiverStartButton->setText(uiText(QStringLiteral("Start receiver")));
        updateSourceControls();
        setStatus(uiText(QStringLiteral("Receiver stopped")), QColor(230, 180, 70));
        return;
    }
    if (liveClient.isConnected()) {
        automaticAttachCheck->setChecked(false);
        liveClient.disconnectFromPublisher();
    }
    if (receiverCombo->currentIndex() < 0 ||
        receiverCombo->currentData(Qt::UserRole).toString().isEmpty()) {
        setStatus(uiText(QStringLiteral("No compatible receiver selected")), QColor(235, 104, 91));
        return;
    }
    const AnalyzerReceiverSettings settings = currentReceiverSettings(selectedCenterFrequencyHz());
    analyzer.clear();
    saveSettings();
    if (standaloneReceiver.start(settings)) {
        receiverStartButton->setText(uiText(QStringLiteral("Stop receiver")));
        updateSourceControls();
        setStatus(uiText(QStringLiteral("Standalone receiver is starting")), QColor(76, 220, 139));
    } else {
        receiverStartButton->setText(uiText(QStringLiteral("Start receiver")));
        updateSourceControls();
    }
}

AnalyzerReceiverSettings AnalyzerMainWindow::currentReceiverSettings(double centerHz) const {
    AnalyzerReceiverSettings settings;
    settings.backendId = receiverCombo->currentData(Qt::UserRole).toString();
    settings.nativeIndex = receiverCombo->currentData(Qt::UserRole + 1).toInt();
    settings.centerHz = centerHz;
    settings.sampleRateHz = selectedSampleRateHz();
    settings.fftLength = selectedFftLength();
    settings.displayReductionRatio = selectedDisplayReductionRatio();
    settings.displayReductionMode = displayReductionMethodCombo->currentData().toInt();
    if (settings.backendId == QStringLiteral("uhd-native")) {
        settings.rxChannel = receiverInputCombo->currentData().toInt();
        settings.uhdDeviceAddress = receiverCombo->currentData(Qt::UserRole + 2).toString();
        settings.uhdRxGainDb = uhdGainSpin->value();
        settings.uhdRxAntenna = uhdAntennaCombo->currentText().trimmed();
        settings.uhdFpgaImagePath = uhdImageEdit->text().trimmed();
    } else if (settings.backendId == QStringLiteral("soapy-sdr")) {
        settings.rxChannel = std::max(0, receiverInputCombo->currentData().toInt());
        settings.directChannel = 0;
        settings.directSampling = false;
    } else {
        settings.directChannel = receiverInputCombo->currentData().toInt();
        settings.directSampling = settings.backendId.startsWith(QStringLiteral("fobos-")) &&
                                  settings.directChannel != 0;
    }
    settings.lnaGainDb = receiverLnaSpin->value();
    settings.vgaGainDb = receiverVgaSpin->value();
    return settings;
}

void AnalyzerMainWindow::toggleWideSweep() {
    if (sweepActive) {
        if (receiverRestartTimer) receiverRestartTimer->stop();
        sweepRestartPending = false;
        sweepActive = false;
        sweepButton->setText(uiText(QStringLiteral("Start wide sweep")));
        if (standaloneReceiver.isRunning()) standaloneReceiver.stop();
        setCenterFrequencyHz(sweepOriginalCenterHz);
        sweepUsesNativeAgileScan = false;
        sweepSegmentFrameCounts.clear();
        setStatus(uiText(QStringLiteral("Wide sweep cancelled")), QColor(230, 180, 70));
        updateSourceControls();
        return;
    }
    const QString backend = receiverCombo->currentData(Qt::UserRole).toString();
    const bool directFobos = backend.startsWith(QStringLiteral("fobos-")) &&
                             receiverInputCombo->currentData().toInt() != 0;
    if (backend.isEmpty() || receiverCombo->currentIndex() < 0) {
        setStatus(uiText(QStringLiteral("No compatible receiver selected")), QColor(235, 104, 91));
        return;
    }
    if (liveClient.isConnected()) {
        setStatus(uiText(QStringLiteral("Disconnect Obrii SDR before a wide sweep")),
                  QColor(235, 104, 91));
        return;
    }
    if (directFobos) {
        setStatus(uiText(QStringLiteral("Wide sweep requires the RF / tuner input")),
                  QColor(235, 104, 91));
        return;
    }
    const bool receiverWasRunning = standaloneReceiver.isRunning();

    InstrumentSweepSettings settings;
    settings.startHz = std::min(sweepStartSpin->value(), sweepStopSpin->value()) * 1.0e6;
    settings.stopHz = std::max(sweepStartSpin->value(), sweepStopSpin->value()) * 1.0e6;
    settings.captureBandwidthHz = selectedSampleRateHz();
    settings.overlapFraction = sweepOverlapSpin->value() / 100.0;
    settings.edgeDiscardFraction = sweepEdgeSpin->value() / 100.0;
    const int fftLength = std::max(2, selectedFftLength());
    const int reductionRatio = selectedDisplayReductionRatio();
    const int outputPoints = reductionRatio > 0
                                 ? std::max(2, (fftLength + reductionRatio - 1) / reductionRatio)
                                 : std::min(fftLength, 65536);
    settings.outputBinWidthHz = selectedSampleRateHz() / outputPoints;
    const InstrumentSweepPlan plan = InstrumentSweepEngine::createPlan(settings);
    if (!plan.isValid()) {
        setStatus(uiText(QStringLiteral("Wide sweep: %1")).arg(uiText(plan.error)), QColor(235, 104, 91));
        return;
    }

    for (const AnalyzerReceiverDevice &device : receiverDevices) {
        if (device.backendId != backend ||
            device.nativeIndex != receiverCombo->currentData(Qt::UserRole + 1).toInt())
            continue;
        if (device.maximumFrequencyHz > device.minimumFrequencyHz &&
            (plan.segments.front().centerHz < device.minimumFrequencyHz ||
             plan.segments.back().centerHz > device.maximumFrequencyHz)) {
            setStatus(uiText(QStringLiteral("Wide sweep exceeds the receiver tuning range")),
                      QColor(235, 104, 91));
            return;
        }
        break;
    }
    if (!sweepEngine.begin(plan)) {
        setStatus(uiText(QStringLiteral("Could not initialize wide sweep storage")), QColor(235, 104, 91));
        return;
    }

    sweepOriginalCenterHz = selectedCenterFrequencyHz();
    sweepSegmentIndex = 0;
    const int fobosSweepMode = sweepFobosModeCombo->currentData().toInt();
    const bool fobosBackend = backend.startsWith(QStringLiteral("fobos-"));
    const bool agileBackend = backend == QStringLiteral("fobos-agile");
    const bool nativeAgilePlanSupported = agileBackend &&
                                          plan.segments.size() >= 2 &&
                                          plan.segments.size() <= 256 &&
                                          selectedFftLength() <= 4194304;
    if (fobosBackend && fobosSweepMode == 1 && !agileBackend) {
        setStatus(uiText(QStringLiteral("Native Agile scan requires a Fobos Agile receiver")),
                  QColor(235, 104, 91));
        return;
    }
    if (fobosBackend && fobosSweepMode == 1 && !nativeAgilePlanSupported) {
        setStatus(uiText(QStringLiteral(
                      "Native Agile scan needs 2-256 segments and FFT no larger than 4,194,304")),
                  QColor(235, 104, 91));
        return;
    }
    sweepUsesNativeAgileScan = fobosBackend &&
                               (fobosSweepMode == 1 ||
                                (fobosSweepMode == 0 && nativeAgilePlanSupported));
    if (sweepUsesNativeAgileScan) {
        AnalyzerReceiverSettings receiverSettings = currentReceiverSettings(plan.segments.front().centerHz);
        receiverSettings.directSampling = false;
        receiverSettings.directChannel = 0;
        receiverSettings.agileScanFrequenciesHz.reserve(plan.segments.size());
        for (const InstrumentSweepSegment &segment : plan.segments)
            receiverSettings.agileScanFrequenciesHz.append(segment.centerHz);
        if (!standaloneReceiver.start(receiverSettings)) {
            sweepUsesNativeAgileScan = false;
            setStatus(uiText(QStringLiteral("Could not start native Agile sweep receiver")),
                      QColor(235, 104, 91));
            return;
        }
    } else if (!receiverWasRunning) {
        if (!standaloneReceiver.start(currentReceiverSettings(plan.segments.front().centerHz))) {
            setStatus(uiText(QStringLiteral("Could not start the receiver for wide sweep")),
                      QColor(235, 104, 91));
            return;
        }
    }
    sweepSettleFrames = sweepUsesNativeAgileScan ? 0 : 2;
    sweepFramesAccumulated = 0;
    sweepPassCount = 0;
    sweepSegmentFrameCounts.fill(0, plan.segments.size());
    sweepDisplayTimer.restart();
    sweepActive = true;
    sweepButton->setText(sweepContinuousCheck->isChecked()
                             ? uiText(QStringLiteral("Stop live sweep"))
                             : uiText(QStringLiteral("Cancel sweep")));
    analyzer.clear();
    sourceFrequencies.clear();
    sourceLevels.clear();
    const double firstCenter = plan.segments.front().centerHz;
    if (!sweepUsesNativeAgileScan && receiverWasRunning && !standaloneReceiver.retune(firstCenter)) {
        sweepActive = false;
        standaloneReceiver.stop();
        sweepButton->setText(uiText(QStringLiteral("Start wide sweep")));
        setStatus(uiText(QStringLiteral("Receiver rejected the first sweep frequency")), QColor(235, 104, 91));
        return;
    }
    setCenterFrequencyHz(firstCenter);
    saveSettings();
    setStatus(uiText(QStringLiteral("%1: segment 1 / %2"))
                  .arg(sweepUsesNativeAgileScan ? uiText(QStringLiteral("Native Agile sweep"))
                                               : uiText(QStringLiteral("Wide sweep")))
                  .arg(plan.segments.size()),
              QColor(110, 190, 230));
    updateSourceControls();
}

void AnalyzerMainWindow::updateSourceControls() {
    const bool receiverRunning = standaloneReceiver.isRunning();
    const bool externalLive = liveClient.isConnected();
    const QString backend = receiverCombo->currentData(Qt::UserRole).toString();
    const bool directFobos = backend.startsWith(QStringLiteral("fobos-")) &&
                             receiverInputCombo->currentData().toInt() != 0;
    receiverStartButton->setEnabled(!sweepActive && !externalLive &&
                                    (!receiverDevices.isEmpty() || receiverRunning));
    receiverRefreshButton->setEnabled(!sweepActive && !receiverRunning && !externalLive);
    receiverCombo->setEnabled(!sweepActive && !receiverRunning && !externalLive);
    centerSpin->setEnabled(!sweepActive && !externalLive && (!receiverRunning || !directFobos));
    centerUnitCombo->setEnabled(centerSpin->isEnabled());
    for (QWidget *widget : QVector<QWidget *>{sampleRateCombo, receiverFftCombo, hzPerBinCheck,
                                              hzPerBinSpin, displayReductionRatioCombo,
                                              displayReductionMethodCombo})
        widget->setEnabled(!externalLive);
    receiverInputCombo->setEnabled(!sweepActive && !externalLive);
    if (sweepButton)
        sweepButton->setEnabled(sweepActive ||
                                (!externalLive && !directFobos &&
                                 receiverCombo->currentIndex() >= 0 && !backend.isEmpty()));
    for (QWidget *widget : QVector<QWidget *>{sweepStartSpin, sweepStopSpin, sweepOverlapSpin,
                                              sweepEdgeSpin, sweepAverageFramesSpin,
                                              sweepContinuousCheck, sweepFobosModeCombo})
        widget->setEnabled(!sweepActive && !externalLive);
    if (sweepFobosModeCombo)
        sweepFobosModeCombo->setEnabled(!sweepActive && !externalLive &&
                                        backend.startsWith(QStringLiteral("fobos-")));
    updateResolutionControls();
    updateReceiverControlRanges();
}

void AnalyzerMainWindow::updateReceiverControlRanges() {
    const QString backend = receiverCombo->currentData(Qt::UserRole).toString();
    const bool fobos = backend.startsWith(QStringLiteral("fobos-"));
    const bool hackRf = backend == QStringLiteral("hackrf");
    const bool soapy = backend == QStringLiteral("soapy-sdr");
    const bool uhd = backend == QStringLiteral("uhd-native");
    const bool gainsAvailable = fobos || hackRf || soapy;
    receiverInputCombo->setEnabled((fobos || soapy || uhd) && !sweepActive && !liveClient.isConnected() &&
                                  (!uhd || !standaloneReceiver.isRunning()));
    uhdSettingsPanel->setVisible(uhd);
    uhdSettingsPanel->setEnabled(!sweepActive && !standaloneReceiver.isRunning() && !liveClient.isConnected());
    receiverLegacyGainLabel->setVisible(!uhd);
    receiverLnaSpin->setVisible(!uhd);
    receiverVgaSpin->setVisible(!uhd);
    receiverLnaSpin->setEnabled(gainsAvailable && !liveClient.isConnected());
    receiverVgaSpin->setEnabled((fobos || hackRf) && !liveClient.isConnected());
    if (fobos) {
        receiverLnaSpin->setRange(1, 3);
        receiverLnaSpin->setSuffix(QStringLiteral(" step"));
        receiverVgaSpin->setRange(0, 31);
        receiverVgaSpin->setSuffix(QStringLiteral(" step"));
    } else if (soapy) {
        receiverLnaSpin->setRange(-20, 100);
        receiverLnaSpin->setSuffix(QStringLiteral(" dB gain"));
        receiverVgaSpin->setRange(0, 0);
        receiverVgaSpin->setSuffix(QStringLiteral(""));
    } else {
        receiverLnaSpin->setRange(0, 40);
        receiverLnaSpin->setSuffix(QStringLiteral(" dB"));
        receiverVgaSpin->setRange(0, 62);
        receiverVgaSpin->setSuffix(QStringLiteral(" dB"));
    }

    QString capabilityText;
    for (const AnalyzerReceiverDevice &device : receiverDevices) {
        if (device.backendId != backend ||
            device.nativeIndex != receiverCombo->currentData(Qt::UserRole + 1).toInt())
            continue;
        capabilityText = device.capabilitySummary;
        if (device.minimumFrequencyHz > 0.0 &&
            device.maximumFrequencyHz > device.minimumFrequencyHz) {
            if (!capabilityText.isEmpty()) capabilityText += QStringLiteral(" | ");
            capabilityText += QStringLiteral("%1 .. %2")
                                  .arg(frequencyText(device.minimumFrequencyHz),
                                       frequencyText(device.maximumFrequencyHz));
        }
        if (!device.sampleRatesHz.isEmpty()) {
            if (!capabilityText.isEmpty()) capabilityText += QStringLiteral(" | ");
            capabilityText += QStringLiteral("RX rates: %1, max %2 MS/s")
                                  .arg(device.sampleRatesHz.size())
                                  .arg(device.sampleRatesHz.constLast() / 1.0e6, 0, 'f', 3);
        }
        break;
    }
    receiverCapabilityLabel->setText(capabilityText);
    receiverCapabilityLabel->setVisible(!capabilityText.isEmpty());
    updateCenterFrequencyUnit();
}

void AnalyzerMainWindow::populateReceiverInputs() {
    if (!receiverInputCombo || !receiverCombo) return;
    QSettings saved;
    const QString backend = receiverCombo->currentData(Qt::UserRole).toString();
    const QString previousBackend = receiverInputCombo->property("receiverBackend").toString();
    const int currentValue = receiverInputCombo->currentData().isValid()
                                 ? receiverInputCombo->currentData().toInt()
                                 : -1;
    int wanted = previousBackend == backend ? currentValue : -1;
    const QSignalBlocker blocker(receiverInputCombo);
    receiverInputCombo->clear();

    if (backend.startsWith(QStringLiteral("fobos-"))) {
        if (wanted < 0 || wanted > 2) {
            wanted = saved.value(QStringLiteral("receiver/directChannel"),
                                 saved.value(QStringLiteral("receiver/direct"), false).toBool() ? 1 : 0).toInt();
        }
        receiverInputCombo->addItem(uiText(QStringLiteral("RF / tuner")), 0);
        receiverInputCombo->addItem(QStringLiteral("HF1 direct"), 1);
        receiverInputCombo->addItem(QStringLiteral("HF2 direct"), 2);
    } else if (backend == QStringLiteral("uhd-native")) {
        if (wanted < 0) wanted = saved.value(QStringLiteral("receiver/uhdRxChannel"), 0).toInt();
        receiverInputCombo->addItem(uiText(QStringLiteral("RX channel %1")).arg(0), 0);
        receiverInputCombo->addItem(uiText(QStringLiteral("RX channel %1")).arg(1), 1);
    } else if (backend == QStringLiteral("soapy-sdr")) {
        if (wanted < 0)
            wanted = saved.value(QStringLiteral("receiver/soapyRxChannel"), 0).toInt();
        int channelCount = 1;
        for (const AnalyzerReceiverDevice &device : receiverDevices) {
            if (device.backendId == backend &&
                device.nativeIndex == receiverCombo->currentData(Qt::UserRole + 1).toInt()) {
                channelCount = std::max(1, device.rxChannels);
                break;
            }
        }
        for (int channel = 0; channel < channelCount; ++channel)
            receiverInputCombo->addItem(uiText(QStringLiteral("RX channel %1")).arg(channel), channel);
    } else {
        wanted = 0;
        receiverInputCombo->addItem(uiText(QStringLiteral("RF / tuner")), 0);
    }
    receiverInputCombo->setCurrentIndex(std::max(0, receiverInputCombo->findData(wanted)));
    receiverInputCombo->setProperty("receiverBackend", backend);
}

void AnalyzerMainWindow::populateSampleRates() {
    if (!sampleRateCombo) return;
    QSettings saved;
    const double wanted = sampleRateCombo->currentData().isValid()
                              ? sampleRateCombo->currentData().toDouble()
                              : saved.value(QStringLiteral("receiver/sampleRateMsps"), 10.0).toDouble() * 1.0e6;
    const QString backend = receiverCombo ? receiverCombo->currentData(Qt::UserRole).toString() : QString();
    QVector<double> rates;
    if (backend == QStringLiteral("soapy-sdr")) {
        for (const AnalyzerReceiverDevice &device : receiverDevices) {
            if (device.backendId == backend &&
                device.nativeIndex == receiverCombo->currentData(Qt::UserRole + 1).toInt()) {
                rates = device.sampleRatesHz;
                break;
            }
        }
        if (rates.isEmpty()) rates = {1.0e6, 2.0e6, 5.0e6, 10.0e6};
    } else if (backend == QStringLiteral("hackrf")) {
        rates = {2.0e6, 4.0e6, 8.0e6, 10.0e6, 12.5e6, 16.0e6, 20.0e6};
    } else if (backend == QStringLiteral("rtl-sdr")) {
        rates = {1.024e6, 1.536e6, 2.048e6, 2.4e6, 2.56e6, 3.2e6};
    } else if (backend == QStringLiteral("uhd-native")) {
        rates = {1.0e6, 2.0e6, 4.0e6, 8.0e6, 10.0e6, 12.5e6, 15.36e6, 16.0e6,
                 20.0e6, 25.0e6, 30.72e6, 32.0e6, 40.0e6, 50.0e6, 56.0e6, 61.44e6};
    } else {
        rates = {8.0e6, 10.0e6, 12.5e6, 16.0e6, 20.0e6,
                 25.0e6, 32.0e6, 40.0e6, 50.0e6, 80.0e6};
    }
    const QSignalBlocker blocker(sampleRateCombo);
    sampleRateCombo->clear();
    for (double rate : rates)
        sampleRateCombo->addItem(QStringLiteral("%1 MS/s").arg(rate / 1.0e6, 0, 'f',
                                                                std::fmod(rate / 1.0e6, 1.0) == 0.0 ? 0 : 3),
                                 rate);
    int selected = -1;
    for (int index = 0; index < sampleRateCombo->count(); ++index) {
        if (std::abs(sampleRateCombo->itemData(index).toDouble() - wanted) < 0.5) {
            selected = index;
            break;
        }
    }
    if (selected < 0) {
        const double fallback = backend == QStringLiteral("rtl-sdr") ? 2.048e6 : 10.0e6;
        selected = sampleRateCombo->findData(fallback);
        if (selected < 0 && sampleRateCombo->count() > 0) {
            double nearestDistance = std::numeric_limits<double>::max();
            for (int index = 0; index < sampleRateCombo->count(); ++index) {
                const double distance = std::abs(sampleRateCombo->itemData(index).toDouble() - wanted);
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    selected = index;
                }
            }
        }
    }
    sampleRateCombo->setCurrentIndex(std::max(0, selected));
}

double AnalyzerMainWindow::selectedSampleRateHz() const {
    return sampleRateCombo ? sampleRateCombo->currentData().toDouble() : 0.0;
}

void AnalyzerMainWindow::setSelectedSampleRateHz(double sampleRateHz) {
    if (!sampleRateCombo || !std::isfinite(sampleRateHz) || sampleRateHz <= 0.0) return;
    int selected = -1;
    for (int index = 0; index < sampleRateCombo->count(); ++index) {
        if (std::abs(sampleRateCombo->itemData(index).toDouble() - sampleRateHz) < 0.5) {
            selected = index;
            break;
        }
    }
    const QSignalBlocker blocker(sampleRateCombo);
    if (selected < 0) {
        sampleRateCombo->addItem(uiText(QStringLiteral("%1 MS/s (source)")).arg(sampleRateHz / 1.0e6, 0, 'f', 6),
                                 sampleRateHz);
        selected = sampleRateCombo->count() - 1;
    }
    sampleRateCombo->setCurrentIndex(selected);
}

void AnalyzerMainWindow::scheduleStandaloneReceiverRestart() {
    if (receiverRestartTimer && standaloneReceiver.isRunning() && !liveClient.isConnected()) {
        sweepRestartPending = sweepActive;
        receiverRestartTimer->start();
    }
}

double AnalyzerMainWindow::selectedCenterFrequencyHz() const {
    return centerSpin ? centerSpin->value() * centerUnitFactor : 0.0;
}

void AnalyzerMainWindow::setCenterFrequencyHz(double frequencyHz) {
    if (!centerSpin || !std::isfinite(frequencyHz)) return;
    const QSignalBlocker blocker(centerSpin);
    centerSpin->setValue(frequencyHz / centerUnitFactor);
}

void AnalyzerMainWindow::updateCenterFrequencyUnit() {
    if (!centerSpin || !centerUnitCombo) return;
    const double factor = centerUnitCombo->currentData().toDouble();
    if (!(factor > 0.0)) return;
    double minimumHz = 0.0;
    double maximumHz = 8.0e9;
    if (receiverCombo) {
        const QString backend = receiverCombo->currentData(Qt::UserRole).toString();
        const int nativeIndex = receiverCombo->currentData(Qt::UserRole + 1).toInt();
        for (const AnalyzerReceiverDevice &device : receiverDevices) {
            if (device.backendId == backend && device.nativeIndex == nativeIndex &&
                device.maximumFrequencyHz > device.minimumFrequencyHz) {
                minimumHz = std::max(0.0, device.minimumFrequencyHz);
                maximumHz = device.maximumFrequencyHz;
                break;
            }
        }
    }
    centerSpin->setRange(minimumHz / factor, maximumHz / factor);
    if (factor >= 1.0e9) {
        centerSpin->setDecimals(9);
        centerSpin->setSingleStep(0.000001);
    } else if (factor >= 1.0e6) {
        centerSpin->setDecimals(6);
        centerSpin->setSingleStep(0.001);
    } else {
        centerSpin->setDecimals(3);
        centerSpin->setSingleStep(1.0);
    }
}

int AnalyzerMainWindow::selectedFftLength() const {
    if (!hzPerBinCheck || !hzPerBinCheck->isChecked())
        return receiverFftCombo ? receiverFftCombo->currentData().toInt() : 65536;
    const double requestedHz = std::max(0.1, hzPerBinSpin->value());
    qint64 length = qRound64(selectedSampleRateHz() / requestedHz);
    length = std::clamp<qint64>(length, 512, 67108864);
    if ((length & 1) != 0) ++length;
    return static_cast<int>(std::min<qint64>(length, 67108864));
}

int AnalyzerMainWindow::selectedDisplayReductionRatio() const {
    return displayReductionRatioCombo ? displayReductionRatioCombo->currentData().toInt() : 0;
}

void AnalyzerMainWindow::synchronizeResolutionFields() {
    if (synchronizingResolutionFields || !receiverFftCombo || !hzPerBinCheck ||
        !hzPerBinSpin || !sampleRateCombo) return;
    synchronizingResolutionFields = true;
    if (hzPerBinCheck->isChecked()) {
        const int calculatedLength = selectedFftLength();
        const QSignalBlocker blocker(receiverFftCombo);
        for (int index = receiverFftCombo->count() - 1; index >= 0; --index) {
            if (receiverFftCombo->itemData(index, Qt::UserRole + 1).toBool())
                receiverFftCombo->removeItem(index);
        }
        int index = receiverFftCombo->findData(calculatedLength);
        if (index < 0) {
            receiverFftCombo->addItem(QStringLiteral("%1 (Hz/bin)").arg(calculatedLength),
                                      calculatedLength);
            index = receiverFftCombo->count() - 1;
            receiverFftCombo->setItemData(index, true, Qt::UserRole + 1);
        }
        receiverFftCombo->setCurrentIndex(index);
    } else {
        int fftLength = std::max(1, receiverFftCombo->currentData().toInt());
        if (receiverFftCombo->currentData(Qt::UserRole + 1).toBool()) {
            const QSignalBlocker comboBlocker(receiverFftCombo);
            for (int index = receiverFftCombo->count() - 1; index >= 0; --index) {
                if (receiverFftCombo->itemData(index, Qt::UserRole + 1).toBool())
                    receiverFftCombo->removeItem(index);
            }
            int nearestIndex = -1;
            qint64 nearestDistance = std::numeric_limits<qint64>::max();
            for (int index = 0; index < receiverFftCombo->count(); ++index) {
                const qint64 distance = std::abs(
                    static_cast<qint64>(receiverFftCombo->itemData(index).toInt()) - fftLength);
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    nearestIndex = index;
                }
            }
            receiverFftCombo->setCurrentIndex(std::max(0, nearestIndex));
            fftLength = std::max(1, receiverFftCombo->currentData().toInt());
        }
        const double hzPerBin = selectedSampleRateHz() / fftLength;
        const QSignalBlocker blocker(hzPerBinSpin);
        hzPerBinSpin->setValue(std::clamp(hzPerBin, hzPerBinSpin->minimum(), hzPerBinSpin->maximum()));
    }
    synchronizingResolutionFields = false;
}

void AnalyzerMainWindow::updateResolutionControls() {
    if (!receiverFftCombo || !hzPerBinCheck || !hzPerBinSpin || !resolutionActualLabel ||
        !displayReductionRatioCombo || !displayReductionMethodCombo || !displayReductionInfoLabel) return;
    synchronizeResolutionFields();
    const bool editable = !liveClient.isConnected();
    const bool hzMode = hzPerBinCheck->isChecked();
    receiverFftCombo->setEnabled(editable && !hzMode);
    hzPerBinSpin->setEnabled(editable && hzMode);
    hzPerBinCheck->setStyleSheet(hzMode
        ? QStringLiteral("color:#70dca8;font-weight:600;")
        : QStringLiteral("color:#91a3a5;"));
    receiverFftCombo->setStyleSheet(!hzMode && editable
        ? QStringLiteral("QComboBox{color:#70dca8;font-weight:600;}QComboBox:disabled{color:#718083;}")
        : QStringLiteral("QComboBox{color:#718083;}QComboBox:disabled{color:#718083;}"));
    hzPerBinSpin->setStyleSheet(hzMode && editable
        ? QStringLiteral("QDoubleSpinBox{color:#70dca8;font-weight:600;}QDoubleSpinBox:disabled{color:#718083;}")
        : QStringLiteral("QDoubleSpinBox{color:#718083;}QDoubleSpinBox:disabled{color:#718083;}"));
    if (liveClient.isConnected())
        receiverFftCombo->setToolTip(uiText(QStringLiteral("FFT is controlled by the connected Obrii SDR instance.")));
    else if (standaloneReceiver.isRunning())
        receiverFftCombo->setToolTip(uiText(QStringLiteral("Changing FFT automatically restarts the standalone receiver.")));
    else if (hzMode)
        receiverFftCombo->setToolTip(uiText(QStringLiteral("FFT is calculated from the active Hz/bin field.")));
    else
        receiverFftCombo->setToolTip(QString());
    const int fftLength = std::max(1, selectedFftLength());
    const double actual = selectedSampleRateHz() / fftLength;
    resolutionActualLabel->setText(uiText(QStringLiteral("Actual: %1 Hz/bin, FFT %2"))
        .arg(actual, 0, 'f', actual < 10.0 ? 4 : actual < 100.0 ? 3 : 2)
        .arg(fftLength));
    const int ratio = selectedDisplayReductionRatio();
    const qint64 availablePoints = receiverInputCombo && receiverInputCombo->currentData().toInt() != 0
                                       ? static_cast<qint64>(fftLength) / 2 + 1
                                       : fftLength;
    const qint64 outputPoints = ratio > 0
                                    ? std::max<qint64>(2, (availablePoints + ratio - 1) / ratio)
                                    : std::min<qint64>(availablePoints, 65536);
    displayReductionInfoLabel->setText(uiText(QStringLiteral("Output: about %1 points%2"))
        .arg(QLocale().toString(outputPoints))
        .arg(outputPoints > 1048576 ? uiText(QStringLiteral(" (high memory/UI load)")) : QString()));
    displayReductionInfoLabel->setStyleSheet(outputPoints > 1048576
        ? QStringLiteral("color:#f0a060;") : QString());
}

void AnalyzerMainWindow::saveSettings() {
    if (!scaleTopSpin) return;
    QSettings saved;
    saved.setValue(QStringLiteral("source/autoAttach"), automaticAttachCheck->isChecked());
    saved.setValue(QStringLiteral("display/topDb"), scaleTopSpin->value());
    saved.setValue(QStringLiteral("display/bottomDb"), scaleBottomSpin->value());
    saved.setValue(QStringLiteral("display/automaticFrequency"), automaticViewCheck->isChecked());
    saved.setValue(QStringLiteral("display/minimumMHz"), viewMinimumSpin->value());
    saved.setValue(QStringLiteral("display/maximumMHz"), viewMaximumSpin->value());
    saved.setValue(QStringLiteral("display/zoomPercent"),
                   qRound(frequencyZoomSlider->value() / 10.0));
    saved.setValue(QStringLiteral("display/zoomTenthsPercent"), frequencyZoomSlider->value());
    saved.setValue(QStringLiteral("display/zoomDivisor"), frequencyZoomDivisorSlider->value());
    saved.setValue(QStringLiteral("traces/current"), currentTraceCheck->isChecked());
    saved.setValue(QStringLiteral("traces/max"), maxHoldCheck->isChecked());
    saved.setValue(QStringLiteral("traces/min"), minHoldCheck->isChecked());
    saved.setValue(QStringLiteral("traces/average"), averageCheck->isChecked());
    saved.setValue(QStringLiteral("traces/p50"), p50Check->isChecked());
    saved.setValue(QStringLiteral("traces/p90"), p90Check->isChecked());
    saved.setValue(QStringLiteral("traces/p99"), p99Check->isChecked());
    saved.setValue(QStringLiteral("receiver/centerHz"), selectedCenterFrequencyHz());
    saved.setValue(QStringLiteral("receiver/centerMHz"), selectedCenterFrequencyHz() / 1.0e6);
    saved.setValue(QStringLiteral("receiver/centerUnitFactor"), centerUnitFactor);
    saved.setValue(QStringLiteral("receiver/sampleRateMsps"), selectedSampleRateHz() / 1.0e6);
    saved.setValue(QStringLiteral("receiver/fftLength"), receiverFftCombo->currentData());
    saved.setValue(QStringLiteral("receiver/useHzPerBin"), hzPerBinCheck->isChecked());
    saved.setValue(QStringLiteral("receiver/hzPerBin"), hzPerBinSpin->value());
    saved.setValue(QStringLiteral("receiver/displayReductionRatio"), selectedDisplayReductionRatio());
    saved.setValue(QStringLiteral("receiver/displayReductionMethod"), displayReductionMethodCombo->currentData());
    const QString backend = receiverCombo->currentData(Qt::UserRole).toString();
    if (backend == QStringLiteral("uhd-native")) {
        saved.setValue(QStringLiteral("receiver/uhdAddress"), receiverCombo->currentData(Qt::UserRole + 2));
        saved.setValue(QStringLiteral("receiver/uhdRxChannel"), receiverInputCombo->currentData());
        saved.setValue(QStringLiteral("receiver/uhdGainDb"), uhdGainSpin->value());
        saved.setValue(QStringLiteral("receiver/uhdAntenna"), uhdAntennaCombo->currentText().trimmed());
        saved.setValue(QStringLiteral("receiver/uhdFpgaImage"), uhdImageEdit->text().trimmed());
    } else if (backend == QStringLiteral("soapy-sdr"))
        saved.setValue(QStringLiteral("receiver/soapyRxChannel"), receiverInputCombo->currentData());
    else
        saved.setValue(QStringLiteral("receiver/directChannel"), receiverInputCombo->currentData());
    saved.setValue(QStringLiteral("receiver/lnaDb"), receiverLnaSpin->value());
    saved.setValue(QStringLiteral("receiver/vgaDb"), receiverVgaSpin->value());
    saved.setValue(QStringLiteral("sweep/startMHz"), sweepStartSpin->value());
    saved.setValue(QStringLiteral("sweep/stopMHz"), sweepStopSpin->value());
    saved.setValue(QStringLiteral("sweep/overlapPercent"), sweepOverlapSpin->value());
    saved.setValue(QStringLiteral("sweep/edgePercent"), sweepEdgeSpin->value());
    saved.setValue(QStringLiteral("sweep/averageFrames"), sweepAverageFramesSpin->value());
    saved.setValue(QStringLiteral("sweep/fobosMode"), sweepFobosModeCombo->currentData());
    saved.setValue(QStringLiteral("sweep/continuous"), sweepContinuousCheck->isChecked());
    if (!receiverCombo->currentData(Qt::UserRole).toString().isEmpty()) {
        saved.setValue(QStringLiteral("receiver/backendId"), receiverCombo->currentData(Qt::UserRole));
        saved.setValue(QStringLiteral("receiver/nativeIndex"), receiverCombo->currentData(Qt::UserRole + 1));
    }
}

void AnalyzerMainWindow::setStatus(const QString &message, const QColor &color) {
    statusLabel->setText(message);
    statusLabel->setStyleSheet(QStringLiteral("color:%1;").arg(color.name()));
}
