#include "fpgalogiceditor.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <functional>

namespace {
constexpr qreal kWidth = 154;
constexpr qreal kHeight = 94;

class LogicItem final : public QGraphicsRectItem {
public:
    std::function<void()> moved;
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override {
        const QVariant result = QGraphicsRectItem::itemChange(change, value);
        if (change == ItemPositionHasChanged && moved) moved();
        return result;
    }
};

QString sourceLabel(int id, const FpgaLogicBlock &block) {
    if (id == 0) return QStringLiteral("IQ вхід I");
    if (id == 1) return QStringLiteral("IQ вхід Q");
    for (const auto &node : block.nodes)
        if (node.id == id) return QStringLiteral("%1 · %2").arg(id).arg(FpgaLogicBlock::opLabel(node.op));
    return QStringLiteral("%1 · ?").arg(id);
}
}

FpgaLogicEditor::FpgaLogicEditor(const QString &projectRoot, QWidget *parent)
    : QDialog(parent), projectRoot_(projectRoot),
      filePath_(QDir(projectRoot).filePath(QStringLiteral(".obrii-logic.json"))) {
    setWindowTitle(QStringLiteral("Власний FPGA-блок · Obrii FPGA Lab"));
    setMinimumSize(850, 530);
    resize(1120, 660);
    setStyleSheet(QStringLiteral(
        "QDialog,QWidget{background:#0d1114;color:#cbd6d6;}"
        "QGraphicsView{background:#10181b;border:1px solid #2b4042;}"
        "QPushButton,QComboBox,QSpinBox,QLineEdit{background:#172024;border:1px solid #365153;padding:4px;color:#d9e2e1;}"
        "QPushButton:hover{background:#244244;}"
        "QPushButton:disabled{color:#697677;border-color:#263637;}"));

    auto *root = new QVBoxLayout(this);
    auto *toolbar = new QHBoxLayout;
    primitive_ = new QComboBox(this);
    primitive_->setObjectName(QStringLiteral("logicPrimitive"));
    for (const QString &op : {QStringLiteral("const16"), QStringLiteral("add16"),
                              QStringLiteral("sub16"), QStringLiteral("and16"),
                              QStringLiteral("or16"), QStringLiteral("xor16"),
                              QStringLiteral("not16"), QStringLiteral("mul_q15"),
                              QStringLiteral("reg16"), QStringLiteral("lut4")})
        primitive_->addItem(FpgaLogicBlock::opLabel(op), op);
    auto *addButton = new QPushButton(QStringLiteral("Додати"), this);
    addButton->setObjectName(QStringLiteral("logicAddNode"));
    auto *removeButton = new QPushButton(QStringLiteral("Видалити"), this);
    auto *openButton = new QPushButton(QStringLiteral("Відкрити"), this);
    auto *saveButton = new QPushButton(QStringLiteral("Зберегти"), this);
    saveButton->setObjectName(QStringLiteral("logicSave"));
    auto *saveAsButton = new QPushButton(QStringLiteral("Зберегти як"), this);
    auto *exportButton = new QPushButton(QStringLiteral("Експорт RTL"), this);
    auto *checkButton = new QPushButton(QStringLiteral("Перевірити Yosys"), this);
    auto *closeButton = new QPushButton(QStringLiteral("Закрити"), this);
    toolbar->addWidget(primitive_);
    toolbar->addWidget(addButton);
    toolbar->addWidget(removeButton);
    toolbar->addStretch();
    toolbar->addWidget(openButton);
    toolbar->addWidget(saveButton);
    toolbar->addWidget(saveAsButton);
    toolbar->addWidget(exportButton);
    toolbar->addWidget(checkButton);
    toolbar->addWidget(closeButton);
    root->addLayout(toolbar);

    auto *body = new QHBoxLayout;
    scene_ = new QGraphicsScene(this);
    view_ = new QGraphicsView(scene_, this);
    view_->setObjectName(QStringLiteral("logicCanvas"));
    view_->setRenderHint(QPainter::Antialiasing);
    view_->setDragMode(QGraphicsView::ScrollHandDrag);
    body->addWidget(view_, 1);
    auto *inspector = new QWidget(this);
    inspector->setFixedWidth(242);
    auto *form = new QFormLayout(inspector);
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setMaxLength(64);
    form->addRow(QStringLiteral("Назва"), nameEdit_);
    for (int index = 0; index < 4; ++index) {
        inputChoices_[index] = new QComboBox(this);
        form->addRow(QStringLiteral("Вхід %1").arg(index + 1), inputChoices_[index]);
    }
    constantEdit_ = new QSpinBox(this);
    constantEdit_->setRange(-32768, 32767);
    form->addRow(QStringLiteral("Константа"), constantEdit_);
    truthEdit_ = new QLineEdit(this);
    truthEdit_->setMaxLength(4);
    truthEdit_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9a-fA-F]{0,4}")), truthEdit_));
    truthEdit_->setToolTip(QStringLiteral("16 біт HEX; індекс біта LUT = D:C:B:A"));
    form->addRow(QStringLiteral("LUT4 HEX"), truthEdit_);
    outputIChoice_ = new QComboBox(this);
    outputQChoice_ = new QComboBox(this);
    form->addRow(QStringLiteral("IQ вихід I"), outputIChoice_);
    form->addRow(QStringLiteral("IQ вихід Q"), outputQChoice_);
    body->addWidget(inspector);
    root->addLayout(body, 1);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    root->addWidget(status_);

    connect(addButton, &QPushButton::clicked, this, [this]() {
        if (block_.nodes.size() >= 64) {
            QMessageBox::information(this, QStringLiteral("Вузли"),
                                     QStringLiteral("Ліміт блока: 64 вузли."));
            return;
        }
        capturePositions();
        FpgaLogicNode node;
        node.id = nextId();
        if (node.id < 0) return;
        node.op = primitive_->currentData().toString();
        const int count = FpgaLogicBlock::inputCount(node.op);
        for (int index = 0; index < count; ++index) node.inputs << (index % 2);
        node.position = QPointF(245 + (block_.nodes.size() % 3) * 185,
                                350 + (block_.nodes.size() / 3) * 135);
        block_.nodes << node;
        selectedId_ = node.id;
        markDirty();
        draw();
    });
    connect(removeButton, &QPushButton::clicked, this, [this]() {
        if (selectedId_ < 2) return;
        for (const auto &node : block_.nodes)
            if (node.inputs.contains(selectedId_)) {
                QMessageBox::information(this, QStringLiteral("Вузол"),
                    QStringLiteral("Спочатку перепідключіть вузол %1.").arg(node.id));
                return;
            }
        if (block_.outputI == selectedId_ || block_.outputQ == selectedId_) {
            QMessageBox::information(this, QStringLiteral("Вузол"),
                                     QStringLiteral("Спочатку перепідключіть виходи I/Q."));
            return;
        }
        capturePositions();
        for (int index = 0; index < block_.nodes.size(); ++index)
            if (block_.nodes[index].id == selectedId_) { block_.nodes.removeAt(index); break; }
        selectedId_ = -1;
        markDirty();
        draw();
    });
    connect(openButton, &QPushButton::clicked, this, [this]() {
        if (!confirmClose()) return;
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Відкрити блок"),
                            filePath_, QStringLiteral("FPGA logic (*.json)"));
        if (!path.isEmpty()) load(path);
    });
    connect(saveButton, &QPushButton::clicked, this, [this]() { save(); });
    connect(saveAsButton, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Зберегти власний блок"),
            filePath_, QStringLiteral("FPGA logic (*.json)"));
        if (path.isEmpty()) return;
        const QString previous = filePath_;
        filePath_ = path;
        if (!save()) filePath_ = previous;
    });
    connect(exportButton, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Експорт RTL"),
            QDir(projectRoot_).filePath(QStringLiteral("obrii_user_logic.v")), QStringLiteral("Verilog (*.v)"));
        if (!path.isEmpty()) exportRtl(path);
    });
    connect(checkButton, &QPushButton::clicked, this, [this]() {
        if (!save()) return;
        const QString path = QDir(projectRoot_).filePath(QStringLiteral(".obrii-openxc7/obrii_user_logic.v"));
        if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !exportRtl(path)) return;
        requestedSynthesisRtl_ = path;
        accept();
    });
    connect(closeButton, &QPushButton::clicked, this, [this]() { reject(); });
    connect(nameEdit_, &QLineEdit::textEdited, this, [this](const QString &value) {
        block_.name = value;
        markDirty();
    });
    connect(scene_, &QGraphicsScene::selectionChanged, this, [this]() {
        if (drawing_) return;
        selectedId_ = -1;
        for (auto *item : scene_->selectedItems()) {
            const int id = item->data(0).toInt();
            if (id >= 2) { selectedId_ = id; break; }
        }
        updateInspector();
    });
    for (int index = 0; index < 4; ++index)
        connect(inputChoices_[index], QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, index](int) {
            auto *node = selectedNode();
            if (!node || index >= node->inputs.size()) return;
            node->inputs[index] = inputChoices_[index]->currentData().toInt();
            markDirty();
            updateConnections();
        });
    connect(outputIChoice_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        block_.outputI = outputIChoice_->currentData().toInt(); markDirty(); updateConnections();
    });
    connect(outputQChoice_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        block_.outputQ = outputQChoice_->currentData().toInt(); markDirty(); updateConnections();
    });
    connect(constantEdit_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        auto *node = selectedNode();
        if (!node) return;
        node->constant = value; markDirty();
    });
    connect(truthEdit_, &QLineEdit::textEdited, this, [this](const QString &value) {
        auto *node = selectedNode();
        if (!node || value.isEmpty()) return;
        bool okay = false;
        const uint parsed = value.toUInt(&okay, 16);
        if (okay) { node->truthTable = parsed; markDirty(); }
    });
    if (QFileInfo::exists(filePath_)) load(filePath_);
    else draw();
}

void FpgaLogicEditor::closeEvent(QCloseEvent *event) {
    if (confirmClose()) event->accept();
    else event->ignore();
}

void FpgaLogicEditor::reject() {
    if (confirmClose()) QDialog::reject();
}

bool FpgaLogicEditor::confirmClose() {
    if (!dirty_) return true;
    const auto choice = QMessageBox::question(this, QStringLiteral("Власний блок"),
        QStringLiteral("Зберегти зміни блока?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    return choice == QMessageBox::Discard || (choice == QMessageBox::Save && save());
}

bool FpgaLogicEditor::load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
        QMessageBox::warning(this, QStringLiteral("Відкрити блок"), QStringLiteral("Не вдалося прочитати JSON блока."));
        return false;
    }
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse);
    QString error;
    FpgaLogicBlock candidate;
    if (parse.error != QJsonParseError::NoError || !document.isObject() ||
        !FpgaLogicBlock::fromJson(document.object(), &candidate, &error)) {
        QMessageBox::warning(this, QStringLiteral("Відкрити блок"),
                             error.isEmpty() ? parse.errorString() : error);
        return false;
    }
    block_ = candidate;
    filePath_ = path;
    dirty_ = false;
    selectedId_ = -1;
    draw();
    return true;
}

bool FpgaLogicEditor::save() {
    capturePositions();
    const QStringList errors = block_.validate();
    if (!errors.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Зберегти блок"), errors.join(QLatin1Char('\n')));
        return false;
    }
    QSaveFile file(filePath_);
    const QByteArray content = QJsonDocument(block_.toJson()).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(content) != content.size() ||
        !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("Зберегти блок"), file.errorString());
        return false;
    }
    dirty_ = false;
    updateStatus();
    return true;
}

bool FpgaLogicEditor::exportRtl(const QString &path) {
    capturePositions();
    QString error;
    const QByteArray verilog = block_.generateVerilog(&error);
    if (verilog.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Експорт RTL"), error);
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(verilog) != verilog.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("Експорт RTL"), file.errorString());
        return false;
    }
    return true;
}

void FpgaLogicEditor::draw() {
    drawing_ = true;
    connections_.clear();
    nodeItems_.clear();
    scene_->clear();
    const auto addItem = [this](int id, const QString &title, QPointF position, bool movable) {
        auto *item = new LogicItem;
        item->setRect(0, 0, kWidth, kHeight);
        item->setPen(QPen(QColor(movable ? "#4c7775" : "#53727a"), 1.5));
        item->setBrush(QColor(movable ? "#183236" : "#253238"));
        item->setData(0, id);
        if (movable) item->setFlags(QGraphicsItem::ItemIsMovable |
                                   QGraphicsItem::ItemIsSelectable |
                                   QGraphicsItem::ItemSendsGeometryChanges);
        scene_->addItem(item);
        item->setPos(position);
        auto *label = new QGraphicsSimpleTextItem(title, item);
        label->setBrush(QColor("#e1eeee"));
        label->setPos(10, 10);
        label->setAcceptedMouseButtons(Qt::NoButton);
        if (movable) {
            auto *idLabel = new QGraphicsSimpleTextItem(QStringLiteral("#%1 · IQ16").arg(id), item);
            idLabel->setBrush(QColor("#a1b9b8"));
            idLabel->setPos(10, 43);
            idLabel->setAcceptedMouseButtons(Qt::NoButton);
            item->moved = [this]() { if (!drawing_) { capturePositions(); markDirty(); updateConnections(); } };
        }
        nodeItems_.insert(id, item);
        return item;
    };
    addItem(0, QStringLiteral("IQ вхід I"), QPointF(25, 70), false);
    addItem(1, QStringLiteral("IQ вхід Q"), QPointF(25, 210), false);
    qreal farEdge = 220;
    for (const auto &node : block_.nodes) {
        addItem(node.id, FpgaLogicBlock::opLabel(node.op), node.position, true);
        farEdge = qMax(farEdge, node.position.x() + kWidth);
    }
    const qreal outX = farEdge + 200;
    addItem(-1, QStringLiteral("IQ вихід I"), QPointF(outX, 70), false);
    addItem(-2, QStringLiteral("IQ вихід Q"), QPointF(outX, 210), false);
    if (auto *item = nodeItems_.value(selectedId_)) item->setSelected(true);
    scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-25, -25, 80, 80));
    drawing_ = false;
    updateConnections();
    updateInspector();
    updateStatus();
}

void FpgaLogicEditor::updateConnections() {
    for (const auto &connection : connections_) {
        scene_->removeItem(connection.path);
        delete connection.path;
    }
    connections_.clear();
    const auto connectEdge = [this](int source, int target, int port) {
        auto *from = nodeItems_.value(source, nullptr);
        auto *to = nodeItems_.value(target, nullptr);
        if (!from || !to) return;
        const QPointF a = from->pos() + QPointF(kWidth, kHeight / 2);
        const QPointF b = to->pos() + QPointF(0, target < 0 ? kHeight / 2 : 27 + port * 16);
        const qreal bend = qMax(35.0, qAbs(b.x() - a.x()) * 0.42);
        QPainterPath curve(a);
        curve.cubicTo(a + QPointF(bend, 0), b - QPointF(bend, 0), b);
        curve.moveTo(b + QPointF(-9, -5));
        curve.lineTo(b);
        curve.lineTo(b + QPointF(-9, 5));
        auto *path = scene_->addPath(curve, QPen(QColor("#5fc4aa"), 2));
        path->setZValue(-1);
        connections_ << Connection{path, source, target, port};
    };
    for (const auto &node : block_.nodes)
        for (int index = 0; index < node.inputs.size(); ++index)
            connectEdge(node.inputs[index], node.id, index);
    connectEdge(block_.outputI, -1, 0);
    connectEdge(block_.outputQ, -2, 0);
    scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-25, -25, 80, 80));
}

void FpgaLogicEditor::updateInspector() {
    QSignalBlocker nameBlock(nameEdit_);
    nameEdit_->setText(block_.name);
    auto *node = selectedNode();
    const auto fill = [this](QComboBox *box, int selected) {
        const QSignalBlocker guard(box);
        box->clear();
        box->addItem(sourceLabel(0, block_), 0);
        box->addItem(sourceLabel(1, block_), 1);
        for (const auto &item : block_.nodes) box->addItem(sourceLabel(item.id, block_), item.id);
        box->setCurrentIndex(box->findData(selected));
    };
    for (int index = 0; index < 4; ++index) {
        fill(inputChoices_[index], node && index < node->inputs.size() ? node->inputs[index] : 0);
        inputChoices_[index]->setEnabled(node && index < node->inputs.size());
    }
    fill(outputIChoice_, block_.outputI);
    fill(outputQChoice_, block_.outputQ);
    {
        const QSignalBlocker guard(constantEdit_);
        constantEdit_->setValue(node ? node->constant : 0);
        constantEdit_->setEnabled(node && node->op == QStringLiteral("const16"));
    }
    {
        const QSignalBlocker guard(truthEdit_);
        truthEdit_->setText(QStringLiteral("%1").arg(node ? node->truthTable : 0, 4, 16, QLatin1Char('0')).toUpper());
        truthEdit_->setEnabled(node && node->op == QStringLiteral("lut4"));
    }
}

void FpgaLogicEditor::updateStatus() {
    const QStringList issues = block_.validate();
    status_->setStyleSheet(issues.isEmpty() ? QStringLiteral("color:#a7d6be") : QStringLiteral("color:#edaf9d"));
    status_->setText(issues.isEmpty()
        ? QStringLiteral("IQ16 · clk · %1 вузлів · затримка %2 тактів%3")
              .arg(block_.nodes.size()).arg(block_.outputLatencyClocks())
              .arg(dirty_ ? QStringLiteral(" · змінено") : QString())
        : issues.first() + (issues.size() > 1 ? QStringLiteral(" (+%1)").arg(issues.size() - 1) : QString()));
}

void FpgaLogicEditor::capturePositions() {
    for (auto &node : block_.nodes)
        if (auto *item = nodeItems_.value(node.id)) node.position = item->pos();
}

void FpgaLogicEditor::markDirty() {
    dirty_ = true;
    updateStatus();
}

int FpgaLogicEditor::nextId() const {
    for (int id = 2; id <= 255; ++id) {
        bool used = false;
        for (const auto &node : block_.nodes) if (node.id == id) { used = true; break; }
        if (!used) return id;
    }
    return -1;
}

FpgaLogicNode *FpgaLogicEditor::selectedNode() {
    for (auto &node : block_.nodes) if (node.id == selectedId_) return &node;
    return nullptr;
}
