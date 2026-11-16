#include "fpgalabwindow.h"
#include "b210labpanel.h"
#include "fpgalogicblock.h"
#include "fpgalogiceditor.h"
#include "apphelp.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFontDatabase>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QStyle>

#include <algorithm>
#include <cmath>

namespace {
const QStringList kSourceSuffixes = {
    QStringLiteral("v"), QStringLiteral("sv"), QStringLiteral("vh"),
    QStringLiteral("svh"), QStringLiteral("vhd"), QStringLiteral("vhdl"),
    QStringLiteral("xdc"), QStringLiteral("sdc"), QStringLiteral("tcl"), QStringLiteral("txt"),
    QStringLiteral("py"), QStringLiteral("patch"), QStringLiteral("sh")};
const QStringList kSkipDirectories = {
    QStringLiteral(".git"), QStringLiteral(".Xil"), QStringLiteral(".cache"),
    QStringLiteral(".runs"), QStringLiteral(".sim"), QStringLiteral("ip_user_files"),
    QStringLiteral("build"), QStringLiteral(".obrii-openxc7")};

bool withinRoot(const QString &root, const QString &path) {
    const QString base = QDir::fromNativeSeparators(QDir(root).canonicalPath());
    const QString file = QDir::fromNativeSeparators(QFileInfo(path).canonicalFilePath());
    return !base.isEmpty() && !file.isEmpty() &&
        (file == base || file.startsWith(base + QLatin1Char('/'),
#ifdef Q_OS_WIN
                                         Qt::CaseInsensitive
#else
                                         Qt::CaseSensitive
#endif
                                         ));
}

QString stageLabel(const QString &type) {
    if (type == QStringLiteral("passthrough")) return QStringLiteral("Наскрізний IQ");
    if (type == QStringLiteral("ddc")) return QStringLiteral("DDC / зсув частоти");
    if (type == QStringLiteral("fir")) return QStringLiteral("FIR / фільтр");
    if (type == QStringLiteral("decimator")) return QStringLiteral("Децимація");
    if (type == QStringLiteral("trigger")) return QStringLiteral("Тригер рівня");
    if (type == QStringLiteral("custom")) return QStringLiteral("Власний IQ-блок");
    return type;
}

bool readLogicBlock(const QString &path, QJsonObject *snapshot, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
        if (error) *error = QStringLiteral("Не вдалося прочитати JSON власного блока (ліміт 1 МБ).");
        return false;
    }
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse);
    FpgaLogicBlock block;
    if (parse.error != QJsonParseError::NoError || !document.isObject() ||
        !FpgaLogicBlock::fromJson(document.object(), &block, error)) {
        if (error && error->isEmpty()) *error = parse.errorString();
        return false;
    }
    *snapshot = block.toJson();
    return true;
}

QString streamRate(double rate) {
    if (rate >= 1.0e6) return QStringLiteral("%1 MS/s").arg(rate / 1.0e6, 0, 'g', 4);
    if (rate >= 1.0e3) return QStringLiteral("%1 kS/s").arg(rate / 1.0e3, 0, 'g', 4);
    return QStringLiteral("%1 S/s").arg(rate, 0, 'g', 4);
}

QString clockRateText(double rate) {
    if (rate >= 1.0e6) return QStringLiteral("%1 MHz").arg(rate / 1.0e6, 0, 'g', 4);
    if (rate >= 1.0e3) return QStringLiteral("%1 kHz").arg(rate / 1.0e3, 0, 'g', 4);
    return QStringLiteral("%1 Hz").arg(rate, 0, 'g', 4);
}
}

FpgaLabWindow::FpgaLabWindow(QWidget *parent) : QMainWindow(parent) {
    resize(1180, 760);
    setMinimumSize(750, 480);
    setWindowTitle(QStringLiteral("Obrii FPGA Lab"));
    setStyleSheet(QStringLiteral(
        "QMainWindow,QWidget{background:#0d1114;color:#cbd6d6;}"
        "QToolBar{background:#11191c;border-bottom:1px solid #2a4142;spacing:5px;}"
        "QToolButton{padding:5px;color:#cbd6d6;}"
        "QToolButton:hover,QPushButton:hover{background:#244244;}"
        "QPushButton,QComboBox,QSpinBox,QDoubleSpinBox,QLineEdit{background:#172024;border:1px solid #365153;padding:4px;color:#d9e2e1;}"
        "QPushButton:disabled{color:#697677;border-color:#263637;}"
        "QTreeWidget,QPlainTextEdit,QGraphicsView{background:#10181b;color:#d9e2e1;border:1px solid #2b4042;selection-background-color:#2c665e;}"
        "QHeaderView::section{background:#192326;color:#b8d8ce;border:0;padding:4px;}"
        "QTabWidget::pane{border:1px solid #2b4042;}"
        "QTabBar::tab{background:#162024;color:#acb8b8;padding:6px 12px;}"
        "QTabBar::tab:selected{background:#274545;color:#f3f7f6;}"
        "QScrollBar{background:#152023;}"
        "QScrollBar::handle{background:#365153;min-width:18px;min-height:18px;}"
        "QStatusBar{background:#0a0e10;border-top:1px solid #2a4142;}"));

    auto *toolbar = addToolBar(QStringLiteral("Проєкт"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto *openFolderAction = toolbar->addAction(QStringLiteral("Відкрити теку"));
    auto *openFileAction = toolbar->addAction(QStringLiteral("Відкрити файл"));
    toolbar->addSeparator();
    auto *saveSourceAction = toolbar->addAction(QStringLiteral("Зберегти код"));
    auto *savePipelineAction = toolbar->addAction(QStringLiteral("Зберегти схему"));
    savePipelineAction_ = savePipelineAction;
    newB210SourceAction_ = toolbar->addAction(QStringLiteral("Новий HDL-файл…"));
    newB210SourceAction_->setEnabled(false);
    toolbar->addSeparator();
    auto *vivadoMenu = menuBar()->addMenu(QStringLiteral("Vivado (необов'язково)"));
    auto *helpMenu = menuBar()->addMenu(QStringLiteral("Довідка"));
    auto *helpAction = helpMenu->addAction(QStringLiteral("Довідка FPGA Lab"));
    helpAction->setShortcut(QKeySequence::HelpContents);
    connect(helpAction, &QAction::triggered, this, [this]() {
        showApplicationHelpDialog(this, QStringLiteral("uk"), QStringLiteral("Obrii FPGA Lab"));
    });
    auto *buildAction = vivadoMenu->addAction(QStringLiteral("Зібрати у Vivado"));
    vivadoBuildAction_ = buildAction;
    auto *stopAction = toolbar->addAction(QStringLiteral("Зупинити збірку"));
    stopBuildAction_ = stopAction;
    auto *vivadoAction = vivadoMenu->addAction(QStringLiteral("Шлях до Vivado"));
    openFolderAction->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    openFileAction->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    saveSourceAction->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    savePipelineAction->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    stopAction->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    stopAction->setEnabled(false);

    auto *splitter = new QSplitter(this);
    fileTree_ = new QTreeWidget(splitter);
    fileTree_->setObjectName(QStringLiteral("fpgaFileTree"));
    fileTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    fileTree_->setHeaderLabel(QStringLiteral("Файли проєкту"));
    fileTree_->setMinimumWidth(170);
    fileTree_->header()->setStretchLastSection(true);
    auto *right = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(right);
    projectSummary_ = new QLabel(QStringLiteral("Відкрийте теку з RTL-проєктом або готовий образ .bin."), right);
    projectSummary_->setWordWrap(true);
    rightLayout->addWidget(projectSummary_);

    auto *tabs = new QTabWidget(right);
    tabs_ = tabs;
    sourceEditor_ = new QPlainTextEdit(tabs);
    sourceEditor_->setObjectName(QStringLiteral("fpgaSourceEditor"));
    sourceEditor_->setReadOnly(true);
    sourceEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    sourceEditor_->setPlaceholderText(QStringLiteral("Виберіть Verilog, VHDL, XDC або Tcl у дереві."));
    QFont editorFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    sourceEditor_->setFont(editorFont);
    tabs->addTab(sourceEditor_, QStringLiteral("Вихідний код"));

    auto *pipelineTab = new QWidget(tabs);
    auto *pipelineLayout = new QVBoxLayout(pipelineTab);
    pipelineSummary_ = new QLabel(QStringLiteral("Ескіз окремий від .xpr; синтез доступний для підтриманих блоків."), pipelineTab);
    pipelineSummary_->setObjectName(QStringLiteral("fpgaPipelineSummary"));
    pipelineSummary_->setWordWrap(true);
    pipelineLayout->addWidget(pipelineSummary_);
    auto *presetRow = new QHBoxLayout;
    pipelinePreset_ = new QComboBox(pipelineTab);
    pipelinePreset_->setObjectName(QStringLiteral("fpgaPipelinePreset"));
    pipelinePreset_->setEnabled(false);
    auto *applyPreset = new QPushButton(QStringLiteral("Застосувати"), pipelineTab);
    auto *savePreset = new QPushButton(QStringLiteral("Зберегти як"), pipelineTab);
    auto *deletePreset = new QPushButton(QStringLiteral("Видалити"), pipelineTab);
    presetRow->addWidget(new QLabel(QStringLiteral("Пресет:"), pipelineTab));
    presetRow->addWidget(pipelinePreset_, 1);
    presetRow->addWidget(applyPreset);
    presetRow->addWidget(savePreset);
    presetRow->addWidget(deletePreset);
    pipelineLayout->addLayout(presetRow);
    auto *rateControls = new QHBoxLayout;
    auto *pipelineControls = new QHBoxLayout;
    inputRate_ = new QDoubleSpinBox(pipelineTab);
    inputRate_->setRange(1.0, 1000000000.0);
    inputRate_->setDecimals(0);
    inputRate_->setValue(10000000.0);
    inputRate_->setSuffix(QStringLiteral(" S/s"));
    inputRate_->setToolTip(QStringLiteral("Очікувана частота дискретизації на вході FPGA"));
    clockRate_ = new QDoubleSpinBox(pipelineTab);
    clockRate_->setRange(1.0, 1000000000.0);
    clockRate_->setDecimals(0);
    clockRate_->setValue(100000000.0);
    clockRate_->setSuffix(QStringLiteral(" Hz"));
    clockRate_->setToolTip(QStringLiteral("Частота такту обробки IQ; один відлік на такт максимум"));
    synthPipelineButton_ = new QPushButton(QStringLiteral("Синтезувати RTL"), pipelineTab);
    synthPipelineButton_->setToolTip(QStringLiteral("Згенерувати окремий Verilog-модуль і перевірити його Yosys"));
    synthPipelineButton_->setEnabled(false);
    stageType_ = new QComboBox(pipelineTab);
    stageType_->setObjectName(QStringLiteral("fpgaPipelineStageType"));
    for (const QString &type : {QStringLiteral("ddc"), QStringLiteral("fir"),
                                QStringLiteral("decimator"), QStringLiteral("trigger"),
                                QStringLiteral("custom"), QStringLiteral("passthrough")})
        stageType_->addItem(stageLabel(type), type);
    auto *addStage = new QPushButton(QStringLiteral("Додати"), pipelineTab);
    addStage->setObjectName(QStringLiteral("fpgaAddStage"));
    auto *removeStage = new QPushButton(QStringLiteral("Видалити"), pipelineTab);
    auto *moveLeft = new QPushButton(QStringLiteral("Вліво"), pipelineTab);
    auto *moveRight = new QPushButton(QStringLiteral("Вправо"), pipelineTab);
    logicEditorButton_ = new QPushButton(QStringLiteral("Власний блок"), pipelineTab);
    logicEditorButton_->setObjectName(QStringLiteral("fpgaLogicEditor"));
    logicEditorButton_->setToolTip(QStringLiteral("Відкрити редактор окремого IQ16 FPGA-блока"));
    logicEditorButton_->setEnabled(false);
    pipelineEditButtons_ = {addStage, removeStage, moveLeft, moveRight,
                            applyPreset, savePreset, deletePreset};
    for (auto *button : pipelineEditButtons_) button->setEnabled(false);
    stageType_->setEnabled(false);
    inputRate_->setEnabled(false);
    clockRate_->setEnabled(false);
    savePipelineAction_->setEnabled(false);
    rateControls->addWidget(new QLabel(QStringLiteral("IQ вхід:"), pipelineTab));
    rateControls->addWidget(inputRate_);
    rateControls->addWidget(new QLabel(QStringLiteral("Такт:"), pipelineTab));
    rateControls->addWidget(clockRate_);
    rateControls->addStretch();
    rateControls->addWidget(synthPipelineButton_);
    pipelineLayout->addLayout(rateControls);
    pipelineControls->addWidget(stageType_);
    pipelineControls->addWidget(addStage);
    pipelineControls->addWidget(removeStage);
    pipelineControls->addWidget(moveLeft);
    pipelineControls->addWidget(moveRight);
    pipelineControls->addWidget(logicEditorButton_);
    pipelineControls->addStretch();
    pipelineLayout->addLayout(pipelineControls);
    connect(logicEditorButton_, &QPushButton::clicked, this, [this]() {
        if (projectRoot_.isEmpty() || !binaryFile_.isEmpty() ||
            buildProcess_->state() != QProcess::NotRunning) return;
        FpgaLogicEditor editor(projectRoot_, this);
        if (editor.exec() != QDialog::Accepted || editor.requestedSynthesisRtl().isEmpty()) return;
        QString script;
        QString error;
        if (!OpenXc7Project::writeModuleCheck(editor.requestedSynthesisRtl(),
                QStringLiteral("obrii_user_logic"), openXc7OutputDir(), &script, &error)) {
            QMessageBox::warning(this, QStringLiteral("Власний блок"), error);
            return;
        }
        tabs_->setCurrentWidget(buildLog_);
        launchOpenXc7Script(script, QStringLiteral("user_logic"));
    });

    auto *pipelineSplit = new QSplitter(pipelineTab);
    pipelineScene_ = new QGraphicsScene(pipelineSplit);
    pipelineView_ = new QGraphicsView(pipelineScene_, pipelineSplit);
    pipelineView_->setRenderHint(QPainter::Antialiasing);
    pipelineView_->setDragMode(QGraphicsView::ScrollHandDrag);
    pipelineView_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    pipelineView_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *inspector = new QWidget(pipelineSplit);
    auto *form = new QFormLayout(inspector);
    stageFrequency_ = new QDoubleSpinBox(inspector);
    stageFrequency_->setRange(-1000000000.0, 1000000000.0);
    stageFrequency_->setDecimals(0);
    stageFrequency_->setSuffix(QStringLiteral(" Hz"));
    stageBandwidth_ = new QDoubleSpinBox(inspector);
    stageBandwidth_->setRange(1.0, 1000000000.0);
    stageBandwidth_->setDecimals(0);
    stageBandwidth_->setSuffix(QStringLiteral(" Hz"));
    stageFactor_ = new QSpinBox(inspector);
    stageFactor_->setRange(2, 1024);
    form->addRow(QStringLiteral("Зсув / поріг"), stageFrequency_);
    form->addRow(QStringLiteral("Зріз FIR"), stageBandwidth_);
    form->addRow(QStringLiteral("Коеф. децимації"), stageFactor_);
    stageLogicButton_ = new QPushButton(QStringLiteral("Завантажити блок..."), inspector);
    stageLogicButton_->setObjectName(QStringLiteral("fpgaStageLogic"));
    stageLogicButton_->setToolTip(QStringLiteral("Замінити знімок власного блока у вибраному етапі"));
    stageLogicName_ = new QLabel(inspector);
    stageLogicName_->setWordWrap(true);
    form->addRow(QStringLiteral("Схема"), stageLogicName_);
    form->addRow(QStringLiteral("Власний блок"), stageLogicButton_);
    form->addItem(new QSpacerItem(1, 1, QSizePolicy::Minimum, QSizePolicy::Expanding));
    pipelineSplit->addWidget(pipelineView_);
    pipelineSplit->addWidget(inspector);
    pipelineSplit->setStretchFactor(0, 1);
    inspector->setMinimumWidth(180);
    inspector->setMaximumWidth(270);
    pipelineLayout->addWidget(pipelineSplit, 1);
    const int pipelineTabIndex = tabs->addTab(pipelineTab, QStringLiteral("Ескіз IQ-тракту"));
    tabs->setTabToolTip(pipelineTabIndex, QStringLiteral(
        "Окремий експериментальний план. Це не структура відкритого .xpr; "
        "RTL експортується лише для підтриманих блоків."));

    auto *openXc7Tab = new QWidget(tabs);
    auto *openXc7Layout = new QVBoxLayout(openXc7Tab);
    auto *openXc7Controls = new QGridLayout;
    auto *openXc7ToolRow = new QHBoxLayout;
    auto *refreshButton = new QPushButton(QStringLiteral("Перевірити проєкт"), openXc7Tab);
    auto *preflightButton = new QPushButton(QStringLiteral("Перевірити тулчейн"), openXc7Tab);
    auto *moduleButton = new QPushButton(QStringLiteral("Синтезувати вибраний файл"), openXc7Tab);
    bitstreamButton_ = new QPushButton(QStringLiteral("Зібрати .bit"), openXc7Tab);
    auto *openOutputButton = new QPushButton(QStringLiteral("Результати"), openXc7Tab);
    moduleTopEdit_ = new QLineEdit(openXc7Tab);
    moduleTopEdit_->setPlaceholderText(QStringLiteral("top-модуль"));
    moduleTopEdit_->setToolTip(QStringLiteral("Назва модуля у вибраному Verilog-файлі"));
    nixCheck_ = new QCheckBox(QStringLiteral("Nix (~10 ГБ при першому запуску)"), openXc7Tab);
    nixCheck_->setToolTip(QStringLiteral("Перше використання Nix завантажить відкритий тулчейн (близько 10 ГБ)."));
    nixCheck_->setChecked(QSettings().value(QStringLiteral("fpga_lab/openxc7_nix"), false).toBool());
    openXc7ToolRow->addWidget(refreshButton);
    openXc7ToolRow->addWidget(preflightButton);
    openXc7ToolRow->addWidget(nixCheck_);
    openXc7ToolRow->addStretch();
    openXc7Layout->addLayout(openXc7ToolRow);
    openXc7Controls->addWidget(new QLabel(QStringLiteral("Top:"), openXc7Tab), 0, 0);
    openXc7Controls->addWidget(moduleTopEdit_, 0, 1);
    openXc7Controls->addWidget(moduleButton, 0, 2);
    openXc7Controls->setColumnStretch(1, 1);
    openXc7Layout->addLayout(openXc7Controls);
    auto *buildRow = new QHBoxLayout;
    buildRow->addWidget(bitstreamButton_);
    buildRow->addWidget(openOutputButton);
    buildRow->addStretch();
    openXc7Layout->addLayout(buildRow);
    openXc7ReportView_ = new QPlainTextEdit(openXc7Tab);
    openXc7ReportView_->setObjectName(QStringLiteral("openXc7Report"));
    openXc7ReportView_->setReadOnly(true);
    openXc7ReportView_->setFont(editorFont);
    openXc7ReportView_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    openXc7ReportView_->setPlainText(QStringLiteral("Відкрийте .xpr для аналізу сумісності з openXC7."));
    openXc7Layout->addWidget(openXc7ReportView_, 1);
    openXc7Buttons_ = {refreshButton, preflightButton, moduleButton, openOutputButton,
                       synthPipelineButton_};
    bitstreamButton_->setEnabled(false);
    tabs->addTab(openXc7Tab, QStringLiteral("openXC7"));

    buildLog_ = new QPlainTextEdit(tabs);
    buildLog_->setReadOnly(true);
    buildLog_->setFont(editorFont);
    buildLog_->setPlaceholderText(QStringLiteral("Журнал перевірки та збірки FPGA."));
    tabs->addTab(buildLog_, QStringLiteral("Збірка"));
    b210Panel_ = new B210LabPanel([this](QString *error) {
        if (stages_.isEmpty() || std::any_of(stages_.cbegin(), stages_.cend(), [](const auto &stage) {
                return stage.type != QStringLiteral("passthrough");
            })) {
            *error = QStringLiteral("Додайте принаймні один наскрізний IQ-блок. Для цього профілю потрібен тракт без затримки та зміни семплрейту.");
            return QByteArray{};
        }
        return FpgaPipelineCompiler::generateVerilog(stages_, inputRate_->value(), clockRate_->value(), error);
    }, [this](const QString &path) { openPath(path); }, [this] {
        return buildProcess_->state() == QProcess::NotRunning &&
               (!sourceEditor_->document()->isModified() || saveSource());
    }, tabs);
    tabs->addTab(b210Panel_, QStringLiteral("B210 Kintex-7"));
    connect(newB210SourceAction_, &QAction::triggered, this, [this]() {
        if (!b210SourceProject_ || b210Panel_->busy() || !confirmDiscard()) return;
        bool accepted = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Новий HDL-файл"),
            QStringLiteral("Назва модуля"), QLineEdit::Normal, {}, &accepted).trimmed();
        if (!accepted) return;
        const QRegularExpression identifier(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
        const auto folder = QDir(projectRoot_).filePath(QStringLiteral("src/user"));
        if (!identifier.match(name).hasMatch() || !withinRoot(projectRoot_, folder) ||
            QFileInfo(folder).isSymLink()) {
            statusBar()->showMessage(QStringLiteral("Вкажіть допустиму назву Verilog і перевірте теку src/user."), 5000);
            return;
        }
        QFile file(QDir(folder).filePath(name + QStringLiteral(".v")));
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            statusBar()->showMessage(file.errorString(), 5000);
            return;
        }
        const QByteArray stub = QStringLiteral("module %1 ();\nendmodule\n").arg(name).toUtf8();
        if (file.write(stub) != stub.size() || !file.flush()) {
            statusBar()->showMessage(file.errorString(), 5000);
            return;
        }
        file.close();
        scanProject();
        loadSource(file.fileName());
        tabs_->setCurrentWidget(sourceEditor_);
    });
    rightLayout->addWidget(tabs, 1);
    splitter->addWidget(fileTree_);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);
    statusBar()->showMessage(QStringLiteral("Готово"));

    connect(openFolderAction, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("Відкрити FPGA-проєкт"));
        if (!path.isEmpty()) openPath(path);
    });
    connect(openFileAction, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Відкрити FPGA-проєкт або образ"),
            {}, QStringLiteral("FPGA (*.xpr *.bin .obrii-b210.json);;Усі файли (*)"));
        if (!path.isEmpty()) openPath(path);
    });
    connect(saveSourceAction, &QAction::triggered, this, [this]() { saveSource(); });
    connect(savePipelineAction, &QAction::triggered, this, [this]() { savePipeline(); });
    connect(buildAction, &QAction::triggered, this, [this]() { startBuild(); });
    connect(stopAction, &QAction::triggered, this, [this]() { stopBuild(); });
    connect(refreshButton, &QPushButton::clicked, this, [this]() { refreshOpenXc7Report(); });
    connect(preflightButton, &QPushButton::clicked, this, [this]() { startOpenXc7Preflight(); });
    connect(moduleButton, &QPushButton::clicked, this, [this]() { startOpenXc7ModuleCheck(); });
    connect(bitstreamButton_, &QPushButton::clicked, this, [this]() { startOpenXc7BitstreamBuild(); });
    connect(synthPipelineButton_, &QPushButton::clicked, this, [this]() { synthesizePipelineRtl(); });
    const auto refreshPresets = [this]() {
        const QJsonObject saved = QJsonDocument::fromJson(
            QSettings().value(QStringLiteral("fpga_lab/pipeline_presets")).toByteArray()).object();
        pipelinePreset_->clear();
        pipelinePreset_->addItem(QStringLiteral("Оберіть пресет"), QString());
        for (const QString &name : saved.keys()) pipelinePreset_->addItem(name, name);
    };
    refreshPresets();
    connect(savePreset, &QPushButton::clicked, this, [this, refreshPresets]() {
        if (projectRoot_.isEmpty() || !binaryFile_.isEmpty()) return;
        bool accepted = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Пресет IQ-тракту"),
            QStringLiteral("Назва:"), QLineEdit::Normal, {}, &accepted).trimmed();
        if (!accepted) return;
        if (name.isEmpty() || name.size() > 64 || name.contains(QLatin1Char('\n')) ||
            name.contains(QLatin1Char('\r'))) {
            QMessageBox::information(this, QStringLiteral("Пресет"),
                QStringLiteral("Вкажіть назву до 64 символів без перенесення рядка."));
            return;
        }
        QSettings settings;
        QJsonObject saved = QJsonDocument::fromJson(
            settings.value(QStringLiteral("fpga_lab/pipeline_presets")).toByteArray()).object();
        if (saved.contains(name) && QMessageBox::question(this, QStringLiteral("Пресет"),
            QStringLiteral("Замінити пресет «%1»?").arg(name)) != QMessageBox::Yes) return;
        saved.insert(name, pipelineJson());
        settings.setValue(QStringLiteral("fpga_lab/pipeline_presets"),
                          QJsonDocument(saved).toJson(QJsonDocument::Compact));
        refreshPresets();
        pipelinePreset_->setCurrentIndex(pipelinePreset_->findData(name));
    });
    connect(applyPreset, &QPushButton::clicked, this, [this]() {
        if (projectRoot_.isEmpty() || !binaryFile_.isEmpty()) return;
        const QString name = pipelinePreset_->currentData().toString();
        if (name.isEmpty()) return;
        if (pipelineDirty_ && QMessageBox::question(this, QStringLiteral("Пресет"),
            QStringLiteral("Замінити поточний незбережений ескіз?")) != QMessageBox::Yes) return;
        const QJsonObject saved = QJsonDocument::fromJson(
            QSettings().value(QStringLiteral("fpga_lab/pipeline_presets")).toByteArray()).object();
        if (!applyPipelineJson(saved.value(name).toObject())) {
            QMessageBox::warning(this, QStringLiteral("Пресет"), QStringLiteral("Пресет пошкоджено."));
            return;
        }
        pipelineDirty_ = true;
        drawPipeline();
    });
    connect(deletePreset, &QPushButton::clicked, this, [this, refreshPresets]() {
        const QString name = pipelinePreset_->currentData().toString();
        if (name.isEmpty() || QMessageBox::question(this, QStringLiteral("Пресет"),
            QStringLiteral("Видалити пресет «%1»?").arg(name)) != QMessageBox::Yes) return;
        QSettings settings;
        QJsonObject saved = QJsonDocument::fromJson(
            settings.value(QStringLiteral("fpga_lab/pipeline_presets")).toByteArray()).object();
        saved.remove(name);
        settings.setValue(QStringLiteral("fpga_lab/pipeline_presets"),
                          QJsonDocument(saved).toJson(QJsonDocument::Compact));
        refreshPresets();
    });
    connect(openOutputButton, &QPushButton::clicked, this, [this]() {
        const QString path = openXc7OutputDir();
        if (QDir(path).exists()) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    connect(nixCheck_, &QCheckBox::toggled, this, [](bool enabled) {
        QSettings().setValue(QStringLiteral("fpga_lab/openxc7_nix"), enabled);
    });
    connect(vivadoAction, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Вкажіть виконуваний Vivado"));
        if (!path.isEmpty()) vivadoPath_ = path;
    });
    connect(fileTree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (item && !item->data(0, Qt::UserRole).toString().isEmpty())
            loadSource(item->data(0, Qt::UserRole).toString());
    });
    connect(fileTree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *item, QTreeWidgetItem *previous) {
        if (item && !item->data(0, Qt::UserRole).toString().isEmpty())
            if (!loadSource(item->data(0, Qt::UserRole).toString()) && previous) {
                const QSignalBlocker blocker(fileTree_);
                fileTree_->setCurrentItem(previous);
            }
        if (item && item->data(0, Qt::UserRole).toString() == sourceFile_ &&
            QStringList{QStringLiteral("v"), QStringLiteral("sv")}.contains(QFileInfo(sourceFile_).suffix().toLower()))
            moduleTopEdit_->setText(QFileInfo(sourceFile_).completeBaseName());
    });
    connect(fileTree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        auto *item = fileTree_->itemAt(pos);
        if (!item) return;
        const QString path = item->data(0, Qt::UserRole).toString();
        if (QFileInfo(path).suffix().compare(QStringLiteral("tcl"), Qt::CaseInsensitive) != 0 ||
            !withinRoot(projectRoot_, path)) return;
        QMenu menu(this);
        auto *useScript = menu.addAction(QStringLiteral("Використати як сценарій збірки Vivado"));
        if (menu.exec(fileTree_->viewport()->mapToGlobal(pos)) == useScript) {
            buildTclFile_ = path;
            updateProjectSummary();
        }
    });
    connect(addStage, &QPushButton::clicked, this, [this]() {
        if (projectRoot_.isEmpty() || !binaryFile_.isEmpty()) return;
        if (stages_.size() >= 64) {
            QMessageBox::information(this, QStringLiteral("Ескіз IQ-тракту"),
                                     QStringLiteral("Ліміт ескізу: 64 блоки."));
            return;
        }
        if (stageType_->currentData().toString() == QStringLiteral("custom")) {
            const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Власний IQ-блок"),
                QDir(projectRoot_).filePath(QStringLiteral(".obrii-logic.json")),
                QStringLiteral("FPGA logic (*.json)"));
            if (path.isEmpty()) return;
            QString error;
            if (!addCustomStageFromFile(path, &error))
                QMessageBox::warning(this, QStringLiteral("Власний блок"), error);
            return;
        }
        Stage stage;
        stage.type = stageType_->currentData().toString();
        stages_.append(stage);
        selectedStage_ = stages_.size() - 1;
        pipelineDirty_ = true;
        drawPipeline();
    });
    connect(stageLogicButton_, &QPushButton::clicked, this, [this]() {
        if (selectedStage_ < 0 || selectedStage_ >= stages_.size() ||
            stages_[selectedStage_].type != QStringLiteral("custom")) return;
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Замінити власний блок"),
            QDir(projectRoot_).filePath(QStringLiteral(".obrii-logic.json")),
            QStringLiteral("FPGA logic (*.json)"));
        if (path.isEmpty()) return;
        QJsonObject snapshot;
        QString error;
        if (!readLogicBlock(path, &snapshot, &error)) {
            QMessageBox::warning(this, QStringLiteral("Власний блок"), error);
            return;
        }
        stages_[selectedStage_].customLogic = snapshot;
        pipelineDirty_ = true;
        drawPipeline();
    });
    connect(removeStage, &QPushButton::clicked, this, [this]() {
        if (selectedStage_ < 0 || selectedStage_ >= stages_.size()) return;
        stages_.removeAt(selectedStage_);
        selectedStage_ = -1;
        pipelineDirty_ = true;
        drawPipeline();
    });
    connect(moveLeft, &QPushButton::clicked, this, [this]() {
        if (selectedStage_ <= 0 || selectedStage_ >= stages_.size()) return;
        stages_.swapItemsAt(selectedStage_, selectedStage_ - 1);
        --selectedStage_;
        pipelineDirty_ = true;
        drawPipeline();
    });
    connect(moveRight, &QPushButton::clicked, this, [this]() {
        if (selectedStage_ < 0 || selectedStage_ + 1 >= stages_.size()) return;
        stages_.swapItemsAt(selectedStage_, selectedStage_ + 1);
        ++selectedStage_;
        pipelineDirty_ = true;
        drawPipeline();
    });
    connect(pipelineScene_, &QGraphicsScene::selectionChanged, this, [this]() {
        const auto selected = pipelineScene_->selectedItems();
        if (!selected.isEmpty()) selectedStage_ = selected.first()->data(0).toInt();
        updateInspector();
    });
    connect(stageFrequency_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) {
        if (selectedStage_ < 0 || selectedStage_ >= stages_.size()) return;
        if (stages_[selectedStage_].type == QStringLiteral("trigger"))
            stages_[selectedStage_].thresholdDbfs = value;
        else stages_[selectedStage_].frequencyHz = value;
        pipelineDirty_ = true;
        drawPipeline();
    });
    connect(stageBandwidth_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) {
        if (selectedStage_ < 0 || selectedStage_ >= stages_.size()) return;
        stages_[selectedStage_].bandwidthHz = value;
        pipelineDirty_ = true;
        drawPipeline();
    });
    connect(stageFactor_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int value) {
        if (selectedStage_ < 0 || selectedStage_ >= stages_.size()) return;
        stages_[selectedStage_].factor = value;
        pipelineDirty_ = true;
        drawPipeline();
    });
    connect(inputRate_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double) { pipelineDirty_ = true; drawPipeline(); });
    connect(clockRate_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double) { pipelineDirty_ = true; drawPipeline(); });
    buildProcess_ = new QProcess(this);
    connect(buildProcess_, &QProcess::readyReadStandardOutput, this, [this]() {
        buildLog_->appendPlainText(QString::fromLocal8Bit(buildProcess_->readAllStandardOutput()));
    });
    connect(buildProcess_, &QProcess::readyReadStandardError, this, [this]() {
        buildLog_->appendPlainText(QString::fromLocal8Bit(buildProcess_->readAllStandardError()));
    });
    connect(buildProcess_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && code == 0) {
            if (activeBuildKind_ == QStringLiteral("preflight"))
                buildLog_->appendPlainText(QStringLiteral("openXC7 доступний у вибраному середовищі."));
            else if (activeBuildKind_ == QStringLiteral("module"))
                buildLog_->appendPlainText(QStringLiteral("Синтез модуля завершено. Це ще не образ FPGA."));
            else if (activeBuildKind_ == QStringLiteral("openxc7"))
                buildLog_->appendPlainText(QStringLiteral("Збірка .bit завершена. Образ не перевірено на приймачі й не завантажено в нього."));
            else
                buildLog_->appendPlainText(QStringLiteral("Vivado завершив збірку. Перевірте звіт та образ перед використанням."));
        } else {
            buildLog_->appendPlainText(QStringLiteral("Процес завершився з помилкою (код %1). Образ не завантажено.").arg(code));
        }
        setBuildBusy(false);
        if (!buildScriptPath_.isEmpty()) QFile::remove(buildScriptPath_);
        buildScriptPath_.clear();
        activeBuildKind_.clear();
    });
    updateInspector();
    drawPipeline();
}

bool FpgaLabWindow::addCustomStageFromFile(const QString &path, QString *error) {
    if (projectRoot_.isEmpty() || !binaryFile_.isEmpty()) {
        if (error) *error = QStringLiteral("Відкрийте проєкт із джерелами FPGA.");
        return false;
    }
    if (stages_.size() >= 64) {
        if (error) *error = QStringLiteral("Ліміт ескізу: 64 блоки.");
        return false;
    }
    Stage stage;
    stage.type = QStringLiteral("custom");
    if (!readLogicBlock(path, &stage.customLogic, error)) return false;
    stages_.append(stage);
    selectedStage_ = stages_.size() - 1;
    pipelineDirty_ = true;
    drawPipeline();
    return true;
}

bool FpgaLabWindow::confirmDiscard() {
    if (!sourceEditor_->document()->isModified() && !pipelineDirty_) return true;
    const auto result = QMessageBox::question(this, QStringLiteral("Незбережені зміни"),
        QStringLiteral("Зберегти зміни перед продовженням?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (result == QMessageBox::Cancel) return false;
    if (result == QMessageBox::Discard) return true;
    if (sourceEditor_->document()->isModified() && !saveSource()) return false;
    if (pipelineDirty_ && !savePipeline()) return false;
    return true;
}

void FpgaLabWindow::closeEvent(QCloseEvent *event) {
    if (b210Panel_->busy()) {
        statusBar()->showMessage(QStringLiteral("Дочекайтеся завершення завдання B210."), 5000);
        event->ignore();
        return;
    }
    if (buildProcess_->state() != QProcess::NotRunning) {
        QMessageBox::warning(this, QStringLiteral("Збірка триває"),
                             QStringLiteral("Зупиніть збірку перед закриттям програми."));
        event->ignore();
        return;
    }
    if (confirmDiscard()) event->accept();
    else event->ignore();
}

bool FpgaLabWindow::openPath(const QString &path) {
    if (buildProcess_->state() != QProcess::NotRunning || b210Panel_->busy()) {
        QMessageBox::warning(this, QStringLiteral("Збірка триває"),
                             QStringLiteral("Зупиніть поточний процес перед відкриттям іншого проєкту."));
        return false;
    }
    const QFileInfo info(path);
    if (!info.exists() || info.isSymLink()) return false;
    if (!confirmDiscard()) return false;
    const QString suffix = info.suffix().toLower();
    if (info.isFile() && suffix != QStringLiteral("xpr") && suffix != QStringLiteral("bin") &&
        info.fileName() != QStringLiteral(".obrii-b210.json")) {
        QMessageBox::warning(this, QStringLiteral("Формат"), QStringLiteral("Відкрийте теку, .xpr, .obrii-b210.json або .bin."));
        return false;
    }
    projectRoot_ = info.isDir() ? info.canonicalFilePath() : info.canonicalPath();
    projectFile_ = suffix == QStringLiteral("xpr") ? info.canonicalFilePath() : QString();
    buildTclFile_.clear();
    binaryFile_ = suffix == QStringLiteral("bin") ? info.canonicalFilePath() : QString();
    b210SourceProject_ = binaryFile_.isEmpty() &&
        QFileInfo::exists(QDir(projectRoot_).filePath(QStringLiteral(".obrii-b210.json")));
    newB210SourceAction_->setEnabled(b210SourceProject_);
    b210Panel_->setOpenPath(projectRoot_, binaryFile_);
    sourceFile_.clear();
    moduleTopEdit_->clear();
    sourceHash_.clear();
    sourceBom_ = false;
    sourceCrLf_ = false;
    sourceEditor_->clear();
    sourceEditor_->setReadOnly(true);
    sourceEditor_->document()->setModified(false);
    stages_.clear();
    selectedStage_ = -1;
    pipelineDirty_ = false;
    {
        const QSignalBlocker blocker(inputRate_);
        inputRate_->setValue(10000000.0);
    }
    {
        const QSignalBlocker blocker(clockRate_);
        clockRate_->setValue(100000000.0);
    }
    const bool editableProject = binaryFile_.isEmpty();
    inputRate_->setEnabled(editableProject);
    clockRate_->setEnabled(editableProject);
    pipelinePreset_->setEnabled(editableProject);
    synthPipelineButton_->setEnabled(editableProject);
    logicEditorButton_->setEnabled(editableProject);
    stageType_->setEnabled(editableProject);
    savePipelineAction_->setEnabled(editableProject);
    for (auto *button : pipelineEditButtons_) button->setEnabled(editableProject);
    scanProject();
    if (editableProject) loadPipeline();
    drawPipeline();
    updateProjectSummary();
    refreshOpenXc7Report();
    setWindowTitle(QStringLiteral("Obrii FPGA Lab - %1").arg(QFileInfo(projectRoot_).fileName()));
    return true;
}

void FpgaLabWindow::scanProject() {
    fileTree_->clear();
    if (!binaryFile_.isEmpty()) {
        auto *item = new QTreeWidgetItem(fileTree_, {QFileInfo(binaryFile_).fileName()});
        item->setToolTip(0, QStringLiteral("Готовий образ: перегляд без редагування"));
        return;
    }
    struct Pending { QString path; QTreeWidgetItem *parent; int depth; };
    QVector<Pending> pending = {{projectRoot_, nullptr, 0}};
    int files = 0;
    while (!pending.isEmpty() && files < 3000) {
        const Pending current = pending.takeLast();
        if (current.depth > 12) continue;
        QDir dir(current.path);
        const auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                                QDir::Name | QDir::DirsFirst);
        for (const QFileInfo &entry : entries) {
            if (entry.isSymLink()) continue;
            if (entry.isDir()) {
                const QString name = entry.fileName();
                if (kSkipDirectories.contains(name, Qt::CaseInsensitive) ||
                    (!b210SourceProject_ && name.endsWith(QStringLiteral(".srcs"), Qt::CaseInsensitive)) ||
                    name.endsWith(QStringLiteral(".runs"), Qt::CaseInsensitive) ||
                    name.endsWith(QStringLiteral(".cache"), Qt::CaseInsensitive) ||
                    name.endsWith(QStringLiteral(".gen"), Qt::CaseInsensitive) ||
                    name.endsWith(QStringLiteral(".hw"), Qt::CaseInsensitive) ||
                    name.endsWith(QStringLiteral(".ip_user_files"), Qt::CaseInsensitive)) continue;
                auto *item = current.parent
                    ? new QTreeWidgetItem(current.parent, {entry.fileName()})
                    : new QTreeWidgetItem(fileTree_, {entry.fileName()});
                pending.append({entry.filePath(), item, current.depth + 1});
            } else if (kSourceSuffixes.contains(entry.suffix().toLower()) ||
                       entry.suffix().compare(QStringLiteral("xpr"), Qt::CaseInsensitive) == 0) {
                auto *item = current.parent
                    ? new QTreeWidgetItem(current.parent, {entry.fileName()})
                    : new QTreeWidgetItem(fileTree_, {entry.fileName()});
                item->setData(0, Qt::UserRole, entry.absoluteFilePath());
                if (entry.suffix().compare(QStringLiteral("xpr"), Qt::CaseInsensitive) == 0 && projectFile_.isEmpty())
                    projectFile_ = entry.absoluteFilePath();
                ++files;
            }
        }
    }
    if (files >= 3000) statusBar()->showMessage(QStringLiteral("Показано перші 3000 файлів; згенеровані теки пропущені."));
}

bool FpgaLabWindow::loadSource(const QString &path) {
    if (!withinRoot(projectRoot_, path) || QFileInfo(path).isSymLink()) return false;
    if (path == sourceFile_) return true;
    if (!confirmDiscard()) return false;
    QFile file(path);
    if (file.size() > 4 * 1024 * 1024 || !file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Файл"), QStringLiteral("Файл завеликий або недоступний."));
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.contains('\0')) {
        QMessageBox::warning(this, QStringLiteral("Файл"), QStringLiteral("Бінарний файл не відкривається у редакторі коду."));
        return false;
    }
    const bool hasBom = bytes.startsWith("\xEF\xBB\xBF");
    const QByteArray content = hasBom ? bytes.mid(3) : bytes;
    const QString decoded = QString::fromUtf8(content);
    if (decoded.toUtf8() != content) {
        QMessageBox::warning(this, QStringLiteral("Кодування"),
            QStringLiteral("Файл не є UTF-8; редагування вимкнено, щоб не пошкодити його."));
        return false;
    }
    sourceFile_ = path;
    sourceHash_ = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    sourceBom_ = hasBom;
    sourceCrLf_ = bytes.contains("\r\n");
    sourceEditor_->setPlainText(decoded);
    sourceEditor_->setReadOnly(QFileInfo(path).suffix().compare(QStringLiteral("xpr"), Qt::CaseInsensitive) == 0);
    sourceEditor_->document()->setModified(false);
    statusBar()->showMessage(QDir(projectRoot_).relativeFilePath(path));
    return true;
}

bool FpgaLabWindow::saveSource() {
    if (sourceFile_.isEmpty() || sourceEditor_->isReadOnly()) return !sourceEditor_->document()->isModified();
    if (!withinRoot(projectRoot_, sourceFile_)) return false;
    QFile current(sourceFile_);
    if (!current.open(QIODevice::ReadOnly) ||
        QCryptographicHash::hash(current.readAll(), QCryptographicHash::Sha256) != sourceHash_) {
        QMessageBox::warning(this, QStringLiteral("Зовнішня зміна"),
            QStringLiteral("Файл змінився на диску. Збереження скасовано, щоб не затерти нову версію."));
        return false;
    }
    current.close();
    QByteArray bytes = sourceEditor_->toPlainText().toUtf8();
    if (sourceCrLf_) bytes.replace("\n", "\r\n");
    if (sourceBom_) bytes.prepend("\xEF\xBB\xBF");
    QSaveFile file(sourceFile_);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("Збереження"), QStringLiteral("Не вдалося атомарно зберегти файл."));
        return false;
    }
    sourceEditor_->document()->setModified(false);
    sourceHash_ = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    return true;
}

QString FpgaLabWindow::pipelinePath() const {
    return QDir(projectRoot_).filePath(QStringLiteral(".obrii-fpga.json"));
}

bool FpgaLabWindow::savePipeline() {
    if (projectRoot_.isEmpty() || !binaryFile_.isEmpty()) return false;
    QSaveFile file(pipelinePath());
    const QByteArray content = QJsonDocument(pipelineJson()).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(content) != content.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("Збереження"), QStringLiteral("Не вдалося зберегти схему."));
        return false;
    }
    pipelineDirty_ = false;
    return true;
}

QJsonObject FpgaLabWindow::pipelineJson() const {
    QJsonArray array;
    for (const Stage &stage : stages_) {
        QJsonObject object;
        object.insert(QStringLiteral("type"), stage.type);
        object.insert(QStringLiteral("frequencyHz"), stage.frequencyHz);
        object.insert(QStringLiteral("bandwidthHz"), stage.bandwidthHz);
        object.insert(QStringLiteral("thresholdDbfs"), stage.thresholdDbfs);
        object.insert(QStringLiteral("factor"), stage.factor);
        if (stage.type == QStringLiteral("custom"))
            object.insert(QStringLiteral("customLogic"), stage.customLogic);
        array.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("schema"), 1);
    root.insert(QStringLiteral("inputSampleRate"), inputRate_->value());
    root.insert(QStringLiteral("clockHz"), clockRate_->value());
    root.insert(QStringLiteral("note"), QStringLiteral("Standalone RTL for supported stages only"));
    root.insert(QStringLiteral("stages"), array);
    return root;
}

bool FpgaLabWindow::applyPipelineJson(const QJsonObject &root) {
    if (root.value(QStringLiteral("schema")).toInt() != 1 ||
        !root.value(QStringLiteral("stages")).isArray()) return false;
    const QJsonArray array = root.value(QStringLiteral("stages")).toArray();
    if (array.size() > 64) return false;
    QVector<Stage> parsed;
    for (const QJsonValue &value : array) {
        if (!value.isObject()) return false;
        const QJsonObject object = value.toObject();
        const QString type = object.value(QStringLiteral("type")).toString();
        if (!QStringList{QStringLiteral("ddc"), QStringLiteral("fir"), QStringLiteral("decimator"),
                         QStringLiteral("trigger"), QStringLiteral("custom"),
                         QStringLiteral("passthrough")}.contains(type)) return false;
        Stage stage;
        stage.type = type;
        stage.frequencyHz = object.value(QStringLiteral("frequencyHz")).toDouble();
        stage.bandwidthHz = object.value(QStringLiteral("bandwidthHz")).toDouble(1000000.0);
        stage.thresholdDbfs = object.value(QStringLiteral("thresholdDbfs")).toDouble(-60.0);
        stage.factor = std::clamp(object.value(QStringLiteral("factor")).toInt(2), 2, 1024);
        if (type == QStringLiteral("custom")) {
            FpgaLogicBlock block;
            QString error;
            if (!FpgaLogicBlock::fromJson(object.value(QStringLiteral("customLogic")).toObject(),
                                          &block, &error)) return false;
            stage.customLogic = block.toJson();
        }
        parsed.append(stage);
    }
    stages_ = parsed;
    selectedStage_ = -1;
    {
        const QSignalBlocker blocker(inputRate_);
        inputRate_->setValue(root.value(QStringLiteral("inputSampleRate")).toDouble(10000000.0));
    }
    {
        const QSignalBlocker blocker(clockRate_);
        clockRate_->setValue(root.value(QStringLiteral("clockHz")).toDouble(100000000.0));
    }
    return true;
}

void FpgaLabWindow::loadPipeline() {
    QFile file(pipelinePath());
    if (!file.open(QIODevice::ReadOnly)) return;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.read(1024 * 1024), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject() ||
        !applyPipelineJson(doc.object())) {
        statusBar()->showMessage(QStringLiteral("Файл схеми має невідомий формат."));
    }
}

void FpgaLabWindow::drawPipeline() {
    pipelineScene_->clear();
    if (!binaryFile_.isEmpty()) {
        pipelineSummary_->setText(QStringLiteral("Готовий .bin: для редагування схеми відкрийте проєкт із джерелами."));
        updateInspector();
        return;
    }
    const auto check = FpgaPipelineCompiler::analyze(stages_, inputRate_->value(), clockRate_->value());
    const int count = stages_.size() + 2;
    for (int index = 0; index < count; ++index) {
        const qreal x = 24 + index * 168;
        if (index > 0) {
            auto *line = pipelineScene_->addLine(x - 43, 92, x - 1, 92, QPen(QColor(90, 150, 160), 2));
            line->setZValue(-1);
        }
        const bool endpoint = index == 0 || index == count - 1;
        const int stageIndex = index - 1;
        auto *rect = pipelineScene_->addRect(x, 54, 140, 76,
            QPen(endpoint ? QColor(150, 150, 150) : QColor(75, 170, 160), 2),
            QBrush(endpoint ? QColor(45, 55, 58) : QColor(34, 70, 69)));
        rect->setFlag(QGraphicsItem::ItemIsSelectable, !endpoint);
        rect->setData(0, stageIndex);
        const QString label = index == 0 ? QStringLiteral("IQ вхід")
            : index == count - 1 ? QStringLiteral("IQ вихід")
            : stages_[stageIndex].type == QStringLiteral("custom")
                ? stages_[stageIndex].customLogic.value(QStringLiteral("name")).toString().left(18)
                : stageLabel(stages_[stageIndex].type);
        if (!endpoint && stages_[stageIndex].type == QStringLiteral("custom"))
            rect->setToolTip(stages_[stageIndex].customLogic.value(QStringLiteral("name")).toString());
        auto *text = pipelineScene_->addText(label);
        text->setDefaultTextColor(Qt::white);
        text->setTextWidth(132);
        text->setPos(x + 5, 66);
        text->setAcceptedMouseButtons(Qt::NoButton);
        const double rate = index == 0 ? inputRate_->value()
            : index == count - 1 ? check.outputRateHz : check.contracts[stageIndex].outputRateHz;
        const QString detail = QStringLiteral("IQ16  %1%2").arg(streamRate(rate),
            index == count - 1 && !stages_.isEmpty() && stages_.last().type == QStringLiteral("trigger")
                ? QStringLiteral("  +подія") : QString());
        auto *detailText = pipelineScene_->addText(detail);
        QFont detailFont = detailText->font();
        detailFont.setPointSize(8);
        detailText->setFont(detailFont);
        detailText->setDefaultTextColor(QColor(170, 205, 200));
        detailText->setTextWidth(132);
        detailText->setPos(x + 5, 101);
        detailText->setAcceptedMouseButtons(Qt::NoButton);
        if (stageIndex == selectedStage_) rect->setSelected(true);
    }
    pipelineScene_->setSceneRect(0, 0, std::max(480, count * 168 + 20), 190);
    QString status = check.canGenerateRtl() ? QStringLiteral("RTL доступний")
        : (check.errors + check.rtlBlockers).join(QStringLiteral(" "));
    if (!check.warnings.isEmpty()) status += QStringLiteral("  Увага: ") + check.warnings.join(QStringLiteral(" "));
    pipelineSummary_->setText(QStringLiteral("IQ16, такт %1, вихід %2; блоків %3. %4. Окремо від B210 .xpr.")
                              .arg(clockRateText(clockRate_->value()), streamRate(check.outputRateHz))
                              .arg(stages_.size()).arg(status));
    updateInspector();
}

void FpgaLabWindow::updateInspector() {
    const bool valid = selectedStage_ >= 0 && selectedStage_ < stages_.size();
    stageFrequency_->setEnabled(valid);
    stageBandwidth_->setEnabled(valid);
    stageFactor_->setEnabled(valid);
    stageLogicButton_->setEnabled(valid && stages_[selectedStage_].type == QStringLiteral("custom"));
    stageLogicName_->setText(valid && stages_[selectedStage_].type == QStringLiteral("custom")
        ? stages_[selectedStage_].customLogic.value(QStringLiteral("name")).toString() : QString());
    if (!valid) return;
    const Stage &stage = stages_[selectedStage_];
    const QSignalBlocker frequencyBlocker(stageFrequency_);
    const QSignalBlocker bandwidthBlocker(stageBandwidth_);
    const QSignalBlocker factorBlocker(stageFactor_);
    stageFrequency_->setRange(stage.type == QStringLiteral("trigger") ? -160.0 : -1000000000.0,
                              stage.type == QStringLiteral("trigger") ? 0.0 : 1000000000.0);
    stageFrequency_->setSuffix(stage.type == QStringLiteral("trigger") ? QStringLiteral(" dBFS") : QStringLiteral(" Hz"));
    stageFrequency_->setValue(stage.type == QStringLiteral("trigger") ? stage.thresholdDbfs : stage.frequencyHz);
    stageBandwidth_->setValue(stage.bandwidthHz);
    stageFactor_->setValue(stage.factor);
    stageFrequency_->setEnabled(stage.type == QStringLiteral("ddc") || stage.type == QStringLiteral("trigger"));
    stageBandwidth_->setEnabled(stage.type == QStringLiteral("fir"));
    stageFactor_->setEnabled(stage.type == QStringLiteral("decimator"));
}

void FpgaLabWindow::updateProjectSummary() {
    if (!binaryFile_.isEmpty()) {
        QFile file(binaryFile_);
        QString hash;
        if (file.open(QIODevice::ReadOnly)) {
            QCryptographicHash digest(QCryptographicHash::Sha256);
            digest.addData(&file);
            hash = QString::fromLatin1(digest.result().toHex());
        }
        projectSummary_->setText(QStringLiteral("Готовий образ: %1 (%2 байт), SHA-256: %3. "
            "Для редагування потрібні HDL/XDC/Tcl джерела; .bin тут не змінюється.")
            .arg(QFileInfo(binaryFile_).fileName()).arg(QFileInfo(binaryFile_).size()).arg(hash));
    } else {
        const QString buildEntry = !buildTclFile_.isEmpty()
            ? QStringLiteral("Tcl збірки: %1").arg(QDir(projectRoot_).relativeFilePath(buildTclFile_))
            : projectFile_.isEmpty()
                ? QStringLiteral("Виберіть .xpr або Tcl-сценарій збірки.")
                : QStringLiteral("Vivado .xpr: %1").arg(QDir(projectRoot_).relativeFilePath(projectFile_));
        projectSummary_->setText(QStringLiteral("Проєкт: %1. %2").arg(projectRoot_, buildEntry));
    }
}

void FpgaLabWindow::startBuild() {
    if (b210Panel_->busy()) return;
    if ((projectFile_.isEmpty() && buildTclFile_.isEmpty()) || !binaryFile_.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Збірка"),
            QStringLiteral("Для збірки потрібен повний Vivado-проєкт .xpr або вибраний Tcl-сценарій."));
        return;
    }
    if (sourceEditor_->document()->isModified() && !saveSource()) return;
    if (pipelineDirty_ && !savePipeline()) return;
    QString vivado = vivadoPath_;
    if (vivado.isEmpty()) vivado = QStandardPaths::findExecutable(QStringLiteral("vivado"));
    if (vivado.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Vivado не знайдено"),
            QStringLiteral("Вкажіть шлях до Vivado. FPGA Lab сам не містить синтезатора FPGA."));
        return;
    }
    if (buildProcess_->state() != QProcess::NotRunning) return;
    // Tcl braces in a path would change the generated command; reject that rare case explicitly.
    if (buildTclFile_.isEmpty() && (projectFile_.contains(QLatin1Char('{')) || projectFile_.contains(QLatin1Char('}')))) {
        QMessageBox::warning(this, QStringLiteral("Шлях"), QStringLiteral("Шлях до .xpr містить фігурні дужки."));
        return;
    }
    QString scriptPath = buildTclFile_;
    if (scriptPath.isEmpty()) {
        scriptPath = QDir(QDir::tempPath()).filePath(
            QStringLiteral("obrii-fpga-%1.tcl").arg(QCoreApplication::applicationPid()));
        QSaveFile script(scriptPath);
        const QString commands = QStringLiteral(
            "open_project {%1}\n"
            "launch_runs impl_1 -to_step write_bitstream -jobs 4\n"
            "wait_on_run impl_1\n"
            "puts \"OBRII_IMPL_STATUS=[get_property STATUS [get_runs impl_1]]\"\n"
            "if {[get_property PROGRESS [get_runs impl_1]] ne \"100%\"} {error \"Implementation incomplete\"}\n"
            "close_project\n").arg(QDir::toNativeSeparators(projectFile_).replace(QLatin1Char('\\'), QLatin1Char('/')));
        if (!script.open(QIODevice::WriteOnly) || script.write(commands.toUtf8()) < 0 || !script.commit()) {
            QMessageBox::warning(this, QStringLiteral("Збірка"), QStringLiteral("Не вдалося створити тимчасовий Tcl-сценарій."));
            return;
        }
    }
    buildLog_->clear();
    buildLog_->appendPlainText(QStringLiteral("Vivado: %1\nСценарій: %2\nСхема Obrii є лише планом і не синтезується.\n")
        .arg(vivado, scriptPath));
    buildProcess_->setWorkingDirectory(projectRoot_);
    buildScriptPath_ = buildTclFile_.isEmpty() ? scriptPath : QString();
    buildProcess_->start(vivado, {QStringLiteral("-mode"), QStringLiteral("batch"),
                                 QStringLiteral("-source"), scriptPath,
                                 QStringLiteral("-nojournal"), QStringLiteral("-nolog")});
    if (!buildProcess_->waitForStarted(3000)) {
        if (!buildScriptPath_.isEmpty()) QFile::remove(buildScriptPath_);
        buildScriptPath_.clear();
        QMessageBox::warning(this, QStringLiteral("Vivado"), buildProcess_->errorString());
        return;
    }
    activeBuildKind_ = QStringLiteral("vivado");
    setBuildBusy(true);
}

void FpgaLabWindow::stopBuild() {
    if (buildProcess_->state() == QProcess::NotRunning) return;
    buildProcess_->terminate();
    if (!buildProcess_->waitForFinished(3000)) buildProcess_->kill();
    buildLog_->appendPlainText(QStringLiteral("Процес зупинено. Перевірте, чи не залишилися дочірні процеси тулчейну."));
}

QString FpgaLabWindow::openXc7OutputDir() const {
    return projectRoot_.isEmpty() ? QString() : QDir(projectRoot_).filePath(QStringLiteral(".obrii-openxc7"));
}

void FpgaLabWindow::refreshOpenXc7Report() {
    openXc7Report_ = OpenXc7ProjectReport{};
    if (!projectFile_.isEmpty() && binaryFile_.isEmpty()) {
        openXc7Report_ = OpenXc7Project::analyze(projectFile_);
        openXc7ReportView_->setPlainText(openXc7Report_.summary());
        if (moduleTopEdit_->text().isEmpty()) moduleTopEdit_->setText(openXc7Report_.top);
    } else {
        openXc7ReportView_->setPlainText(binaryFile_.isEmpty()
            ? QStringLiteral("Оберіть .xpr для аналізу всього проєкту. Окремий Verilog-файл можна синтезувати й без .xpr.")
            : QStringLiteral("Готовий .bin не містить джерел для openXC7."));
    }
    bitstreamButton_->setEnabled(buildProcess_->state() == QProcess::NotRunning &&
                                 openXc7Report_.canBuildBitstream());
}

void FpgaLabWindow::setBuildBusy(bool busy) {
    vivadoBuildAction_->setEnabled(!busy);
    stopBuildAction_->setEnabled(busy);
    for (auto *button : openXc7Buttons_) button->setEnabled(!busy);
    bitstreamButton_->setEnabled(!busy && openXc7Report_.canBuildBitstream());
    synthPipelineButton_->setEnabled(!busy && !projectRoot_.isEmpty() && binaryFile_.isEmpty());
    logicEditorButton_->setEnabled(!busy && !projectRoot_.isEmpty() && binaryFile_.isEmpty());
}

void FpgaLabWindow::startOpenXc7Preflight() {
    if (projectRoot_.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("openXC7"), QStringLiteral("Спочатку відкрийте проєкт."));
        return;
    }
    QString script;
    QString error;
    if (!OpenXc7Project::writePreflight(openXc7OutputDir(), &script, &error)) {
        QMessageBox::warning(this, QStringLiteral("openXC7"), error);
        return;
    }
    launchOpenXc7Script(script, QStringLiteral("preflight"));
}

void FpgaLabWindow::startOpenXc7ModuleCheck() {
    if (sourceEditor_->document()->isModified() && !saveSource()) return;
    QString script;
    QString error;
    if (!OpenXc7Project::writeModuleCheck(sourceFile_, moduleTopEdit_->text().trimmed(),
                                          openXc7OutputDir(), &script, &error)) {
        QMessageBox::information(this, QStringLiteral("openXC7"), error);
        return;
    }
    launchOpenXc7Script(script, QStringLiteral("module"));
}

void FpgaLabWindow::startOpenXc7BitstreamBuild() {
    if (sourceEditor_->document()->isModified() && !saveSource()) return;
    if (pipelineDirty_ && !savePipeline()) return;
    refreshOpenXc7Report();
    QString script;
    QString error;
    if (!OpenXc7Project::writeBitstreamBuild(openXc7Report_, openXc7OutputDir(), &script, &error)) {
        QMessageBox::warning(this, QStringLiteral("openXC7"), error);
        return;
    }
    launchOpenXc7Script(script, QStringLiteral("openxc7"));
}

void FpgaLabWindow::synthesizePipelineRtl() {
    if (projectRoot_.isEmpty() || !binaryFile_.isEmpty()) return;
    if (pipelineDirty_ && !savePipeline()) return;
    const QString outputDir = openXc7OutputDir();
    const QString rtlPath = QDir(outputDir).filePath(QStringLiteral("obrii_iq_pipeline.v"));
    QString error;
    if (!FpgaPipelineCompiler::writeVerilog(stages_, inputRate_->value(),
                                             clockRate_->value(), rtlPath, &error)) {
        QMessageBox::information(this, QStringLiteral("Синтез RTL"), error);
        return;
    }
    QString script;
    if (!OpenXc7Project::writeModuleCheck(rtlPath, QStringLiteral("obrii_iq_pipeline"),
                                          outputDir, &script, &error)) {
        QMessageBox::warning(this, QStringLiteral("Синтез RTL"), error);
        return;
    }
    launchOpenXc7Script(script, QStringLiteral("pipeline"));
}

void FpgaLabWindow::launchOpenXc7Script(const QString &scriptPath, const QString &kind) {
    if (buildProcess_->state() != QProcess::NotRunning || b210Panel_->busy()) return;
    QString program;
    QStringList args;
#ifdef Q_OS_WIN
    program = QStandardPaths::findExecutable(QStringLiteral("wsl.exe"));
    if (program.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("openXC7"),
                             QStringLiteral("WSL не знайдено. Встановіть Linux/WSL з openXC7 або запускайте FPGA Lab на Linux."));
        return;
    }
    QProcess pathTool;
    pathTool.start(program, {QStringLiteral("--exec"), QStringLiteral("wslpath"),
                             QStringLiteral("-a"), QStringLiteral("-u"), QDir::toNativeSeparators(scriptPath)});
    if (!pathTool.waitForFinished(8000) || pathTool.exitStatus() != QProcess::NormalExit ||
        pathTool.exitCode() != 0) {
        if (pathTool.state() != QProcess::NotRunning) {
            pathTool.kill();
            pathTool.waitForFinished(1000);
        }
        QMessageBox::warning(this, QStringLiteral("openXC7"),
            QStringLiteral("WSL не зміг відкрити проєкт. Перевірте, що Linux-дистрибутив установлений і ця тека доступна у WSL."));
        return;
    }
    const QString linuxScript = QString::fromUtf8(pathTool.readAllStandardOutput()).trimmed();
    if (linuxScript.isEmpty()) return;
    args << QStringLiteral("--exec");
    if (nixCheck_->isChecked()) {
        args << QStringLiteral("bash") << QStringLiteral("-lc")
             << QStringLiteral("if [ -f \"$HOME/.nix-profile/etc/profile.d/nix.sh\" ]; then "
                               ". \"$HOME/.nix-profile/etc/profile.d/nix.sh\"; fi; "
                               "exec nix --extra-experimental-features 'nix-command flakes' "
                               "develop github:openXC7/toolchain-nix --command sh \"$1\"")
             << QStringLiteral("obrii-openxc7") << linuxScript;
    } else {
        args << QStringLiteral("sh") << linuxScript;
    }
#else
    if (nixCheck_->isChecked()) {
        program = QStandardPaths::findExecutable(QStringLiteral("nix"));
        if (program.isEmpty()) {
            const QString profileNix = QDir::homePath() + QStringLiteral("/.nix-profile/bin/nix");
            if (QFileInfo::exists(profileNix)) program = profileNix;
        }
        args << QStringLiteral("--extra-experimental-features") << QStringLiteral("nix-command flakes")
             << QStringLiteral("develop") << QStringLiteral("github:openXC7/toolchain-nix")
             << QStringLiteral("--command") << QStringLiteral("sh") << scriptPath;
    } else {
        program = QStandardPaths::findExecutable(QStringLiteral("sh"));
        args << scriptPath;
    }
    if (program.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("openXC7"), QStringLiteral("Не знайдено shell або Nix."));
        return;
    }
#endif
    buildLog_->clear();
    buildLog_->appendPlainText(QStringLiteral("openXC7: %1\nСценарій: %2\nПрошивання вимкнено.\n")
        .arg(kind, scriptPath));
    tabs_->setCurrentWidget(buildLog_);
    activeBuildKind_ = kind;
    buildProcess_->setWorkingDirectory(QFileInfo(scriptPath).absolutePath());
    setBuildBusy(true);
    buildProcess_->start(program, args);
    if (!buildProcess_->waitForStarted(3000)) {
        buildLog_->appendPlainText(buildProcess_->errorString());
        activeBuildKind_.clear();
        setBuildBusy(false);
    }
}
