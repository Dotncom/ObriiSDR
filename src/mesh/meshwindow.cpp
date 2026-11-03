#include "meshwindow.h"
#include "meshspectrum.h"
#include "meshpacketdialog.h"
#include "meshremotedialog.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDateTime>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QRegularExpressionValidator>
#include <QStandardItemModel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <functional>
#include <algorithm>

namespace Mesh {
namespace {
QString tr(const char *s){return QString::fromUtf8(s);}
void showError(QWidget *w,const QString &e){QMessageBox::warning(w,tr("Obrii · конструктор"),e);}
class Scene : public QGraphicsScene {
public:using QGraphicsScene::QGraphicsScene;
    void drawBackground(QPainter *p,const QRectF &r) override {
        p->fillRect(r,QColor("#101720"));p->setPen(QColor("#202c39"));
        for(qreal x=std::floor(r.left()/32)*32;x<r.right();x+=32)p->drawLine(QPointF(x,r.top()),QPointF(x,r.bottom()));
        for(qreal y=std::floor(r.top()/32)*32;y<r.bottom();y+=32)p->drawLine(QPointF(r.left(),y),QPointF(r.right(),y));
    }
};
class NodeItem : public QGraphicsRectItem {
public:
    std::function<void(QPointF)> moved;
    explicit NodeItem(const QString &id):QGraphicsRectItem(0,0,180,90) {
        setData(0,"node");setData(1,id);setFlags(ItemIsSelectable|ItemIsMovable|ItemSendsGeometryChanges);setZValue(1);
    }
    QVariant itemChange(GraphicsItemChange change,const QVariant &v) override {
        if(change==ItemPositionHasChanged&&moved)moved(v.toPointF());return QGraphicsRectItem::itemChange(change,v);
    }
    void paint(QPainter *p,const QStyleOptionGraphicsItem *,QWidget *) override {
        p->setRenderHint(QPainter::Antialiasing);p->setBrush(QColor("#1b2938"));p->setPen(QPen(isSelected()?QColor("#58c9ef"):QColor("#52687e"),isSelected()?2:1));p->drawRoundedRect(rect(),10,10);
        p->setPen(QColor("#ecf1f7"));QFont f("Segoe UI",10);f.setBold(true);p->setFont(f);p->drawText(QRectF(12,8,156,24),data(3).toString());
        f.setBold(false);p->setFont(f);p->setPen(QColor("#a5b7ca"));p->drawText(QRectF(12,31,156,20),data(1).toString()+" · "+data(4).toString());
        const auto phase=data(2).toString();p->setPen(phase=="Ready"?QColor("#69d59b"):QColor("#c4ced8"));p->drawText(QRectF(12,56,156,21),phase.isEmpty()?tr("Не запущено"):phase);
    }
};
class View : public QGraphicsView {
public:using QGraphicsView::QGraphicsView;
protected:
    void wheelEvent(QWheelEvent *event) override {
        if(event->modifiers()&Qt::ControlModifier){const auto current=transform().m11();const auto next=std::clamp(current*(event->angleDelta().y()>0?1.15:1.0/1.15),0.005,8.0);scale(next/current,next/current);event->accept();}
        else QGraphicsView::wheelEvent(event);
    }
};
QDoubleSpinBox *spin(double min,double max,int decimals,QWidget *parent) {
    auto s=new QDoubleSpinBox(parent);s->setRange(min,max);s->setDecimals(decimals);s->setKeyboardTracking(false);return s;
}
QString nodeLabel(const Project &p,const QString &id){for(const auto &n:p.nodes)if(n.id==id)return n.name;return id;}
bool linkMatches(const Link &link,const QJsonObject &applied) {
    return !applied.isEmpty()&&applied.value("enabled").toBool()==link.enabled&&applied.value("delayMs").toInt()==link.delayMs&&
        std::abs(applied.value("gainDb").toDouble()-link.gainDb)<0.0001;
}
void unavailable(QComboBox *combo,int index,const QString &reason){
    if(auto model=qobject_cast<QStandardItemModel *>(combo->model())){model->item(index)->setEnabled(false);model->item(index)->setToolTip(reason);}
}
QJsonObject directionalPort(const Node &node,const QString &direction){
    const auto ports=node.ports.isEmpty()?defaultNodePorts():node.ports;
    for(const auto &value:ports)if(value.toObject().value("direction")==direction)return value.toObject();
    return {{"id","radio"},{"direction",direction},{"backend","network"},{"enabled",false}};
}
bool remoteOptions(const Project &project){
    for(const auto &node:project.nodes)if(!node.ports.isEmpty()||node.protocol.value("type").toString("lab")!="lab")return true;
    for(const auto &link:project.links)if(link.bearer!="inherit"||link.txPort!="radio"||link.rxPort!="radio"||!link.modem.isEmpty())return true;
    return false;
}
bool privateOptions(const QJsonValue &value,int depth=0){
    if(depth>16)return true;
    if(value.isArray())for(const auto &item:value.toArray())if(privateOptions(item,depth+1))return true;
    if(value.isObject()){const auto object=value.toObject();for(auto it=object.begin();it!=object.end();++it){
        const auto key=it.key().toLower();if(key.contains("key")||key.contains("secret")||key.contains("password")||key.contains("token")||key.contains("psk")||QStringList{"python","pythonpath","moduledirectory","statedirectory"}.contains(key)||privateOptions(it.value(),depth+1))return true;
    }}return false;
}
}
Window::Window(const QString &libraryFile,QWidget *parent):QMainWindow(parent),libraryPath(libraryFile),runtime(this) {
    setFont(QFont("Segoe UI",9));
    setStyleSheet("QMainWindow,QDialog{background:#17212c;color:#e0e8f0;} QWidget{font-size:12px;} QDockWidget{color:#d4e2ef;} QLabel,QCheckBox{color:#d4e2ef;} QLineEdit,QComboBox,QSpinBox,QDoubleSpinBox,QPlainTextEdit,QTableWidget{background:#101923;color:#e2eaf4;border:1px solid #405369;padding:4px;} QPushButton,QToolButton{background:#293e52;color:#e1edf7;border:1px solid #48617a;padding:6px;} QPushButton:disabled,QToolButton:disabled{color:#66798b;} QToolBar,QStatusBar{background:#1c2a38;color:#b8cddd;} QHeaderView::section{background:#293e52;color:white;padding:5px;}");
    setObjectName("meshDesigner");resize(1280,820);QString error;
    if(QFileInfo::exists(libraryPath)) {
        if(!loadLibrary(libraryPath,&presets,&error)) {
            presets.push_back({"local","Local PC","local",{}, {},QCoreApplication::applicationDirPath(),22});
            QTimer::singleShot(0,this,[this,error]{showError(this,error);});
        }
    } else {
        presets.push_back({"local","Local PC","local",{}, {},QCoreApplication::applicationDirPath(),22});
        QDir().mkpath(QFileInfo(libraryPath).absolutePath());saveLibrary(libraryPath,presets,&error);
    }
    if(presets.isEmpty())presets.push_back({"local","Local PC","local",{}, {},QCoreApplication::applicationDirPath(),22});
    project=example(presets.first().id);
    auto toolbar=addToolBar(tr("Сценарій"));toolbar->setMovable(false);
    auto add=[toolbar,this](const QString &name,std::function<void()> f){auto a=toolbar->addAction(name);QObject::connect(a,&QAction::triggered,toolbar,f);return a;};
    add(tr("Новий"),[this]{if(runtime.state()!=Runtime::Idle||!maybeSave())return;project=example(presets.isEmpty()?"local":presets.first().id);projectPath.clear();modified=false;rebuild();});
    add(tr("Відкрити"),[this]{if(runtime.state()!=Runtime::Idle||!maybeSave())return;auto path=QFileDialog::getOpenFileName(this,tr("Сценарій"),{},"JSON (*.json)");if(path.isEmpty())return;QString e;if(!loadProjectFile(path,&e))showError(this,e);});
    add(tr("Зберегти"),[this]{QString path=projectPath;if(path.isEmpty())path=QFileDialog::getSaveFileName(this,tr("Зберегти сценарій"),"network.json","JSON (*.json)");if(path.isEmpty())return;QString e;if(!saveProjectFile(path,&e))showError(this,e);});
    toolbar->addSeparator();addAction=add(tr("+ Вузол"),[this]{addNode();});connectAction=add(tr("+ Зв'язок"),[this]{addLink();});removeAction=add(tr("Видалити"),[this]{removeSelected();});
    add(tr("Умістити"),[this]{view->fitInView(scene->itemsBoundingRect().adjusted(-35,-35,35,35),Qt::KeepAspectRatio);});
    libraryAction=add(tr("Бібліотека пристроїв"),[this]{editLibrary();});mediumAction=add(tr("Середовище"),[this]{editMedium();});
    packetsAction=add(tr("Пакетна мережа"),[this]{showPacketLab();});packetsAction->setObjectName("packetLabAction");
    packetsAction->setToolTip(tr("Окрема пакетна симуляція зі знімка графа, без IQ та RF"));
    auto remoteAction=add(tr("Реальні екземпляри"),[this]{showRemoteMesh();});remoteAction->setObjectName("remoteMeshAction");
    remoteAction->setToolTip(tr("Приєднати вже запущені служби mesh-вузлів; без запуску процесів чи фізичного RF"));
    toolbar->addSeparator();startAction=add(tr("▶ Запустити"),[this]{QString e;if(!runtime.start(project,presets,&e))showError(this,e);else topologyPending=false;refreshRuntime();});
    stopAction=add(tr("■ Зупинити"),[this]{runtime.stop();});
    scene=new Scene(this);view=new View(scene,this);view->setToolTip(tr("Ctrl + колесо — масштаб; перетягування вузлів — позиція на полотні."));view->setObjectName("topologyCanvas");view->setRenderHint(QPainter::Antialiasing);view->setDragMode(QGraphicsView::RubberBandDrag);setCentralWidget(view);
    auto dock=new QDockWidget(tr("Параметри"),this);dock->setMinimumWidth(300);dock->setMaximumWidth(410);properties=new QStackedWidget(dock);
    properties->setStyleSheet("QStackedWidget{background:#17212c;}");
    auto propertyScroll=new QScrollArea(dock);propertyScroll->setWidgetResizable(true);propertyScroll->setFrameShape(QFrame::NoFrame);
    propertyScroll->setStyleSheet("QScrollArea{background:#17212c;}");
    propertyScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);propertyScroll->setWidget(properties);
    dock->setWidget(propertyScroll);addDockWidget(Qt::RightDockWidgetArea,dock);
    auto hint=new QLabel(tr("Вибери вузол або зв'язок.\n\nВузол прив'язаний до профілю пристрою.\nАдреси й шляхи редагуються в бібліотеці."));hint->setWordWrap(true);hint->setMargin(16);properties->addWidget(hint);
    auto nodePage=new QWidget;auto form=new QFormLayout(nodePage);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    nodeName=new QLineEdit;nodeName->setObjectName("nodeName");device=new QComboBox;mode=new QComboBox;mode->addItems({"am","nfm","wfm","usb","lsb","cw"});
    frequency=spin(1,1e12,0,this);frequency->setObjectName("listeningHz");frequency->setSuffix(" Hz");bandwidth=spin(100,2e6,0,this);bandwidth->setSuffix(" Hz");level=spin(0,0.95,2,this);level->setSingleStep(0.05);
    form->addRow(tr("Назва"),nodeName);form->addRow(tr("Пристрій / Obrii"),device);form->addRow(tr("Частота прийому"),frequency);form->addRow(tr("Смуга"),bandwidth);form->addRow(tr("Демодуляція"),mode);form->addRow(tr("Рівень тестового TX"),level);
    applyButton=new QPushButton(tr("Застосувати до RX"));applyButton->setObjectName("applyRadio");form->addRow(applyButton);
    connect(applyButton,&QPushButton::clicked,this,[this]{for(const auto &n:project.nodes)if(n.id==selectedNode()){QString e;if(!runtime.apply(n,&e))showError(this,e);}});
    testButton=new QPushButton(tr("Тестовий CW → середовище"));testButton->setObjectName("testCw");form->addRow(testButton);
    connect(testButton,&QPushButton::clicked,this,[this]{QString e;if(!runtime.testSignal(selectedNode(),&e))showError(this,e);});
    auto rxButtons=new QWidget;auto rxLayout=new QHBoxLayout(rxButtons);rxLayout->setContentsMargins(0,0,0,0);
    startRxButton=new QPushButton(tr("Запустити RX"));startRxButton->setObjectName("startNodeRx");
    stopRxButton=new QPushButton(tr("Зупинити RX"));stopRxButton->setObjectName("stopNodeRx");
    rxLayout->addWidget(startRxButton);rxLayout->addWidget(stopRxButton);form->addRow(rxButtons);
    connect(startRxButton,&QPushButton::clicked,this,[this]{QString e;if(!runtime.setNodeRunning(selectedNode(),true,&e))showError(this,e);});
    connect(stopRxButton,&QPushButton::clicked,this,[this]{QString e;if(!runtime.setNodeRunning(selectedNode(),false,&e))showError(this,e);});
    stopTxButton=new QPushButton(tr("Зупинити тестовий TX"));stopTxButton->setObjectName("stopNodeTx");form->addRow(stopTxButton);
    connect(stopTxButton,&QPushButton::clicked,this,[this]{QString e;if(!runtime.stopSignal(selectedNode(),&e))showError(this,e);});
    actual=new QLabel;actual->setWordWrap(true);actual->setTextInteractionFlags(Qt::TextSelectableByMouse);form->addRow(tr("Підтверджено RX"),actual);properties->addWidget(nodePage);
    auto makePort=[&](const QString &direction,QComboBox **backend,QLineEdit **id,QLineEdit **binding,QCheckBox **active){
        *backend=new QComboBox;(*backend)->addItem(tr("Мережевий"),"network");(*backend)->addItem(direction=="rx"?tr("Фізичний RX · локальний дозвіл"):tr("Фізичний TX · локальне озброєння"),"radio");
        if(direction=="tx")(*backend)->setToolTip(tr("TX потребує radioTxAllowed і локально озброєного backend. Віддалений конструктор не озброює радіо."));
        else (*backend)->setToolTip(tr("RX потребує radioRxAllowed і локально дозволеної пари receiverId/channelId; конструктор не запускає приймач."));
        (*backend)->setObjectName(direction=="rx"?"nodeRxBackend":"nodeTxBackend");
        *id=new QLineEdit("radio");(*id)->setMaxLength(64);(*id)->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z][A-Za-z0-9_.-]{0,63}"),*id));
        (*id)->setObjectName(direction=="rx"?"nodeRxPortId":"nodeTxPortId");
        *binding=new QLineEdit(direction=="rx"?"main":"rx0");(*binding)->setMaxLength(64);(*binding)->setObjectName(direction=="rx"?"nodeReceiverId":"nodeChannelId");
        *active=new QCheckBox(tr("Увімкнено"));(*active)->setChecked(true);(*active)->setObjectName(direction=="rx"?"nodeRxPortEnabled":"nodeTxPortEnabled");
        const auto label=direction=="rx"?QString("RX"):QString("TX");form->addRow(label+tr(" · backend"),*backend);form->addRow(label+tr(" · ID порту"),*id);
        form->addRow(direction=="rx"?tr("RX · приймач"):tr("TX · канал"),*binding);form->addRow(label,*active);
        connect(*backend,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{editNodeOptions();});
        connect(*id,&QLineEdit::textEdited,this,[this]{editNodeOptions();});connect(*binding,&QLineEdit::textEdited,this,[this]{editNodeOptions();});
        connect(*active,&QCheckBox::toggled,this,[this]{editNodeOptions();});
    };
    makePort("rx",&rxBackend,&rxPortId,&receiverId,&rxPortEnabled);makePort("tx",&txBackend,&txPortId,&channelId,&txPortEnabled);
    rxChannelId=new QLineEdit("rx0");rxChannelId->setMaxLength(64);rxChannelId->setObjectName("nodeRxChannelId");rxCenter=spin(1,1e12,0,this);rxCenter->setObjectName("nodeRxCenterHz");rxCenter->setSuffix(" Hz");
    rxModemCopy=new QPushButton(style()->standardIcon(QStyle::SP_BrowserReload),tr("Модем вхідного зв'язку"));rxModemCopy->setObjectName("nodeCopyRxModem");rxModemCopy->setToolTip(tr("Копіює канонічний модем першого вхідного IQ-зв'язку. Для RF LoRa потрібен OS ≥ 2; повні параметри є в JSON портів."));
    form->addRow(tr("RX · канал"),rxChannelId);form->addRow(tr("RX · центр"),rxCenter);form->addRow(rxModemCopy);
    connect(rxChannelId,&QLineEdit::textEdited,this,[this]{editNodeOptions();});connect(rxCenter,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this]{editNodeOptions();});connect(rxModemCopy,&QPushButton::clicked,this,&Window::copyRxModem);
    portsJson=new QPlainTextEdit;portsJson->setObjectName("nodePortsJson");portsJson->setMaximumHeight(130);portsJson->setMaximumBlockCount(256);
    portsJsonApply=new QPushButton(style()->standardIcon(QStyle::SP_DialogApplyButton),tr("Застосувати JSON портів"));portsJsonApply->setObjectName("nodePortsJsonApply");form->addRow(portsJson);form->addRow(portsJsonApply);connect(portsJsonApply,&QPushButton::clicked,this,[this]{applyNodeJson(true);});
    nodeProtocol=new QComboBox;nodeProtocol->setObjectName("nodeProtocol");nodeProtocol->addItem(tr("Лабораторний"),"lab");nodeProtocol->addItem("Meshtastic","meshtastic");
    nodeProtocol->addItem("Reticulum","reticulum");nodeProtocol->addItem(tr("LoRaWAN · потрібен backend"),"lorawan");nodeProtocol->setItemData(3,tr("Потрібна capability nativeProtocols: lorawan і локально встановлений профіль; наявність підтверджує служба."),Qt::ToolTipRole);
    nodeNumber=spin(1,4294967295.0,0,this);nodeNumber->setObjectName("nodeNumber");nodeNumber->setSingleStep(1);nodeNumber->setToolTip(tr("32-бітний native-номер; зарезервовані номери перевіряє протокольний backend."));
    nodeRole=new QComboBox;nodeRole->setObjectName("nodeRole");for(const auto &role:{"router","client","off"})nodeRole->addItem(role,role);
    form->addRow(tr("Протокол"),nodeProtocol);form->addRow(tr("Номер Meshtastic"),nodeNumber);form->addRow(tr("Роль Meshtastic"),nodeRole);
    protocolProfile=new QLineEdit;protocolProfile->setObjectName("nodeProtocolProfileId");protocolProfile->setMaxLength(64);protocolProfile->setToolTip(tr("ID локального профілю host-служби. Ключі й шляхи до backend не зберігаються у сценарії."));form->addRow(tr("Локальний профіль"),protocolProfile);
    protocolJson=new QPlainTextEdit;protocolJson->setObjectName("nodeProtocolJson");protocolJson->setMaximumHeight(110);protocolJson->setMaximumBlockCount(128);protocolJsonApply=new QPushButton(style()->standardIcon(QStyle::SP_DialogApplyButton),tr("Застосувати JSON протоколу"));protocolJsonApply->setObjectName("nodeProtocolJsonApply");form->addRow(protocolJson);form->addRow(protocolJsonApply);
    connect(protocolProfile,&QLineEdit::textEdited,this,[this]{editNodeOptions();});connect(protocolJsonApply,&QPushButton::clicked,this,[this]{applyNodeJson(false);});
    protocolHint=new QLabel;protocolHint->setWordWrap(true);form->addRow(protocolHint);
    connect(nodeProtocol,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{editNodeOptions();});connect(nodeRole,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{editNodeOptions();});
    connect(nodeNumber,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this]{editNodeOptions();});
    connect(nodeName,&QLineEdit::textEdited,this,[this]{editDesired();});
    for(auto s:{frequency,bandwidth,level})connect(s,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this]{editDesired();});
    for(auto c:{device,mode})connect(c,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{editDesired();});
    auto linkPage=new QWidget;auto lf=new QFormLayout(linkPage);linkTitle=new QLabel;gain=spin(-120,60,1,this);gain->setSuffix(" dB");delay=new QSpinBox;delay->setRange(0,500);delay->setSuffix(" ms");enabled=new QCheckBox(tr("Увімкнений"));
    lf->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);lf->setRowWrapPolicy(QFormLayout::WrapLongRows);
    gain->setObjectName("linkGainDb");delay->setObjectName("linkDelayMs");enabled->setObjectName("linkEnabled");
    lf->addRow(linkTitle);lf->addRow(tr("Підсилення / ослаблення"),gain);lf->addRow(tr("Затримка"),delay);lf->addRow(enabled);
    linkBearer=new QComboBox;linkBearer->setObjectName("linkBearer");linkBearer->addItem(tr("Успадкувати режим сеансу"),"inherit");linkBearer->addItem(tr("Дані · TCP"),"data");linkBearer->addItem(tr("IQ · між вузлами"),"iq");
    linkTxPort=new QLineEdit("radio");linkRxPort=new QLineEdit("radio");linkTxPort->setObjectName("linkTxPort");linkRxPort->setObjectName("linkRxPort");
    for(auto field:{linkTxPort,linkRxPort}){field->setMaxLength(64);field->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z][A-Za-z0-9_.-]{0,63}"),field));}
    linkModem=new QComboBox;linkModem->setObjectName("linkModem");linkModem->addItem("AFSK1200 / FM","afsk1200");linkModem->addItem("LoRa / CSS","lora");
    lf->addRow(tr("Носій зв'язку"),linkBearer);lf->addRow(tr("TX · ID порту"),linkTxPort);lf->addRow(tr("RX · ID порту"),linkRxPort);lf->addRow(tr("Модем IQ"),linkModem);
    loraSf=new QSpinBox;loraSf->setRange(7,12);loraSf->setValue(7);loraSf->setObjectName("linkLoraSf");loraSf->setToolTip(tr("Spreading Factor 7–12. Більший SF збільшує тривалість символу."));
    loraBandwidth=new QComboBox;loraBandwidth->setObjectName("linkLoraBandwidth");for(int value:{31250,62500,125000,250000,500000})loraBandwidth->addItem(QString::number(value)+" Hz",value);loraBandwidth->setCurrentIndex(loraBandwidth->findData(125000));
    loraBandwidth->setToolTip(tr("Смуга LoRa. Частота IQ дорівнює BW × OS; її підтримку перевіряє вузол."));
    loraCodingRate=new QComboBox;loraCodingRate->setObjectName("linkLoraCodingRate");for(int value=1;value<=4;++value)loraCodingRate->addItem("4/"+QString::number(4+value),value);
    loraCodingRate->setToolTip(tr("CR 1–4 означає кодування 4/5–4/8."));loraOversampling=new QComboBox;loraOversampling->setObjectName("linkLoraOversampling");for(int value:{1,2,4,8})loraOversampling->addItem(QString::number(value),value);
    loraOversampling->setToolTip(tr("Кількість IQ-відліків на чип: 1, 2, 4 або 8."));
    loraPreamble=new QSpinBox;loraPreamble->setRange(6,1024);loraPreamble->setValue(16);loraPreamble->setObjectName("linkLoraPreamble");
    loraSyncWord=new QSpinBox;loraSyncWord->setRange(0,255);loraSyncWord->setValue(43);loraSyncWord->setDisplayIntegerBase(16);loraSyncWord->setPrefix("0x");loraSyncWord->setObjectName("linkLoraSyncWord");
    lf->addRow("LoRa · SF",loraSf);lf->addRow("LoRa · BW",loraBandwidth);lf->addRow("LoRa · CR",loraCodingRate);lf->addRow("LoRa · OS",loraOversampling);lf->addRow(tr("LoRa · преамбула"),loraPreamble);lf->addRow("LoRa · sync",loraSyncWord);
    for(auto combo:{linkBearer,linkModem,loraBandwidth,loraCodingRate,loraOversampling})connect(combo,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{editLinkOptions();});
    for(auto field:{linkTxPort,linkRxPort})connect(field,&QLineEdit::textEdited,this,[this]{editLinkOptions();});
    for(auto field:{loraSf,loraPreamble,loraSyncWord})connect(field,QOverload<int>::of(&QSpinBox::valueChanged),this,[this]{editLinkOptions();});
    applyLinkButton=new QPushButton(tr("Застосувати зв'язок"));applyLinkButton->setObjectName("applyLink");lf->addRow(applyLinkButton);
    linkActual=new QLabel;linkActual->setObjectName("appliedLinkState");linkActual->setWordWrap(true);linkActual->setTextInteractionFlags(Qt::TextSelectableByMouse);lf->addRow(tr("Застосовано"),linkActual);
    connect(applyLinkButton,&QPushButton::clicked,this,[this]{for(const auto &l:project.links)if(l.tx+"/"+l.rx==selectedLink){QString e;if(!runtime.applyLink(l,&e))showError(this,e);break;}});
    applyLinkButton->setToolTip(tr("Нові умови діють на наступні IQ-кадри. Буферизовані дані дограються за попередніми умовами."));properties->addWidget(linkPage);
    auto editLink=[this]{if(formBusy||selectedLink.isEmpty())return;for(auto &l:project.links)if(l.tx+"/"+l.rx==selectedLink){l.gainDb=gain->value();l.delayMs=delay->value();l.enabled=enabled->isChecked();}dirty();refreshTopology();};
    connect(gain,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[editLink]{editLink();});connect(delay,QOverload<int>::of(&QSpinBox::valueChanged),this,[editLink]{editLink();});connect(enabled,&QCheckBox::toggled,this,[editLink]{editLink();});
    auto logDock=new QDockWidget(tr("Події сеансу"),this);logs=new QPlainTextEdit;logs->setReadOnly(true);logs->setMaximumBlockCount(500);logDock->setWidget(logs);addDockWidget(Qt::BottomDockWidgetArea,logDock);logDock->setMaximumHeight(190);
    spectrumDock=new QDockWidget(tr("Спектр вузла"),this);spectrumDock->setObjectName("nodeSpectrumDock");
    auto preview=new QWidget;auto previewLayout=new QVBoxLayout(preview);auto previewControls=new QHBoxLayout;
    previewEnabled=new QCheckBox(tr("Живий перегляд"));previewEnabled->setObjectName("nodePreviewEnabled");previewEnabled->setChecked(true);
    auto low=spin(-240,119,0,preview),high=spin(-239,120,0,preview);low->setValue(-140);high->setValue(0);
    low->setObjectName("previewMinimumDbfs");high->setObjectName("previewMaximumDbfs");
    low->setSuffix(" dBFS");high->setSuffix(" dBFS");
    previewControls->addWidget(previewEnabled);previewControls->addStretch();previewControls->addWidget(new QLabel(tr("Мін.")));
    previewControls->addWidget(low);previewControls->addWidget(new QLabel(tr("Макс.")));previewControls->addWidget(high);
    previewLayout->addLayout(previewControls);spectrumTitle=new QLabel;previewLayout->addWidget(spectrumTitle);
    spectrumView=new SpectrumView;previewLayout->addWidget(spectrumView);spectrumDock->setWidget(preview);
    addDockWidget(Qt::BottomDockWidgetArea,spectrumDock);tabifyDockWidget(logDock,spectrumDock);spectrumDock->raise();
    connect(previewEnabled,&QCheckBox::toggled,this,[this]{refreshSpectrum();});
    connect(spectrumDock,&QDockWidget::visibilityChanged,this,[this]{refreshSpectrum();});
    connect(low,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this,low,high]{if(low->value()>=high->value())high->setValue(low->value()+1);spectrumView->setRange(low->value(),high->value());});
    connect(high,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this,low,high]{if(high->value()<=low->value())low->setValue(high->value()-1);spectrumView->setRange(low->value(),high->value());});
    status=new QLabel;statusBar()->addWidget(status,1);statusBar()->addPermanentWidget(new QLabel(tr("Етап 1 · віртуальне IQ-середовище")));
    connect(scene,&QGraphicsScene::selectionChanged,this,&Window::selection);
    connect(&runtime,&Runtime::changed,this,[this]{refreshTopology();if(closing&&runtime.state()==Runtime::Idle)QTimer::singleShot(0,this,&QWidget::close);});
    connect(&runtime,&Runtime::topologyChanged,this,&Window::refreshTopology);
    connect(&runtime,&Runtime::nodeChanged,this,[this](const QString &id){if(auto item=nodeItems.value(id)){item->setData(2,runtime.snapshot(id).value("phase").toString());item->update();}refreshRuntime();});
    connect(&runtime,&Runtime::log,logs,&QPlainTextEdit::appendPlainText);
    connect(&runtime,&Runtime::spectrumChanged,this,[this](const QString &id){if(id==selectedNode())refreshSpectrum();});
    rebuild();refreshRuntime();resizeDocks({dock},{340},Qt::Horizontal);
    QTimer::singleShot(0,this,[this]{view->fitInView(scene->itemsBoundingRect().adjusted(-35,-35,35,35),Qt::KeepAspectRatio);});
}
void Window::dirty(){modified=true;setWindowTitle(tr("Obrii Mesh Designer · ")+(projectPath.isEmpty()?tr("Новий сценарій"):QFileInfo(projectPath).fileName())+" *");}
QString Window::selectedNode() const {for(auto item:scene->selectedItems())if(item->data(0)=="node")return item->data(1).toString();return {};}
void Window::rebuild() {
    formBusy=true;nodeItems.clear();linkItems.clear();scene->clear();
    for(const auto &n:project.nodes){auto item=new NodeItem(n.id);item->setData(3,n.name);item->setData(4,n.presetId);item->setPos(n.position);scene->addItem(item);nodeItems[n.id]=item;
        item->moved=[this,id=n.id](QPointF p){for(auto &node:project.nodes)if(node.id==id)node.position=p;dirty();linksGeometry();};}
    for(const auto &l:project.links){auto item=new QGraphicsPathItem;item->setData(0,"link");item->setData(1,l.tx+"/"+l.rx);item->setFlag(QGraphicsItem::ItemIsSelectable);scene->addItem(item);linkItems[l.tx+"/"+l.rx]=item;}
    linksGeometry();scene->setSceneRect(scene->itemsBoundingRect().adjusted(-400,-300,600,450));formBusy=false;selection();
    if(!modified)setWindowTitle(tr("Obrii Mesh Designer · ")+(projectPath.isEmpty()?tr("Новий сценарій"):QFileInfo(projectPath).fileName()));
}
Window::~Window() {
    formBusy=true;
    runtime.disconnect(this);
    for(auto child:findChildren<QObject *>())child->disconnect(this);
    for(auto item:nodeItems)static_cast<NodeItem *>(item)->moved={};
}
void Window::linksGeometry() {
    for(const auto &l:project.links){auto item=static_cast<QGraphicsPathItem *>(linkItems.value(l.tx+"/"+l.rx));auto a=nodeItems.value(l.tx),b=nodeItems.value(l.rx);if(!item||!a||!b)continue;
        QPointF p1=a->pos()+QPointF(90,45),p2=b->pos()+QPointF(90,45);QLineF line(p1,p2);if(line.length()<1)continue;
        const QPointF unit=(p2-p1)/line.length();p1+=unit*95;p2-=unit*95;
        QPainterPath path(p1);path.lineTo(p2);const QPointF normal(-unit.y(),unit.x());path.moveTo(p2);path.lineTo(p2-unit*15+normal*6);path.lineTo(p2-unit*15-normal*6);path.closeSubpath();item->setPath(path);
        const auto confirmed=confirmedLinks.value(l.tx+"/"+l.rx);
        const bool live=runtime.state()==Runtime::Running,changed=live&&!linkMatches(l,confirmed);
        const bool on=live&&!confirmed.isEmpty()?confirmed.value("enabled").toBool():l.enabled;
        const QColor color=changed?QColor("#e8b45d"):on?QColor("#52a9c2"):QColor("#64707b");
        item->setPen(QPen(color,2,on?Qt::SolidLine:Qt::DashLine));item->setBrush(color);
        item->setToolTip(l.tx+":"+l.txPort+" → "+l.rx+":"+l.rxPort+QString(" · %1 · %2 · %3 dB · %4 ms").arg(l.bearer,l.modem.value("type").toString("afsk1200")).arg(l.gainDb).arg(l.delayMs)+(changed?tr(" · не застосовано"):""));}
}
void Window::selection() {
    if(formBusy)return;formBusy=true;selectedLink.clear();properties->setCurrentIndex(0);
    const auto id=selectedNode();
    if(!id.isEmpty())for(int index=0;index<project.nodes.size();++index){const auto &n=project.nodes[index];if(n.id!=id)continue;
        properties->setCurrentIndex(1);nodeName->setText(n.name);device->clear();for(const auto &p:presets)device->addItem(p.name+(p.kind=="local"?"":" · SSH"),p.id);
        device->setCurrentIndex(device->findData(n.presetId));frequency->setValue(n.listeningHz);bandwidth->setValue(n.bandwidthHz);level->setValue(n.txLevel);mode->setCurrentText(n.mode);
        const auto rx=directionalPort(n,"rx"),tx=directionalPort(n,"tx");rxBackend->setCurrentIndex(rxBackend->findData(rx.value("backend").toString("network")));txBackend->setCurrentIndex(txBackend->findData(tx.value("backend").toString("network")));
        rxPortId->setText(rx.value("id").toString("radio"));txPortId->setText(tx.value("id").toString("radio"));receiverId->setText(rx.value("receiverId").toString("main"));channelId->setText(tx.value("channelId").toString("rx0"));
        rxChannelId->setText(rx.value("channelId").toString("rx0"));rxCenter->setValue(rx.value("centerHz").toDouble(n.listeningHz));protocolProfile->setText(n.protocol.value("profileId").toString());
        portsJson->setPlainText(QString::fromUtf8(QJsonDocument(n.ports.isEmpty()?defaultNodePorts():n.json().value("ports").toArray()).toJson()));protocolJson->setPlainText(QString::fromUtf8(QJsonDocument(n.json().value("protocol").toObject()).toJson()));
        rxPortEnabled->setChecked(rx.value("enabled").toBool(true));txPortEnabled->setChecked(tx.value("enabled").toBool(true));nodeProtocol->setCurrentIndex(nodeProtocol->findData(n.protocol.value("type").toString("lab")));
        nodeNumber->setValue(n.protocol.value("nodeNumber").toDouble(0x100+index));const auto role=n.protocol.value("relayRole").toString(n.protocol.value("role").toString("router"));if(nodeRole->findData(role)<0)nodeRole->addItem(role,role);nodeRole->setCurrentIndex(nodeRole->findData(role));break;
    }
    if(id.isEmpty())for(auto item:scene->selectedItems())if(item->data(0)=="link"){
        selectedLink=item->data(1).toString();for(const auto &l:project.links)if(l.tx+"/"+l.rx==selectedLink){
            properties->setCurrentIndex(2);linkTitle->setText(nodeLabel(project,l.tx)+" → "+nodeLabel(project,l.rx));gain->setValue(l.gainDb);delay->setValue(l.delayMs);enabled->setChecked(l.enabled);
            linkBearer->setCurrentIndex(linkBearer->findData(l.bearer));linkTxPort->setText(l.txPort);linkRxPort->setText(l.rxPort);linkModem->setCurrentIndex(linkModem->findData(l.modem.value("type").toString("afsk1200")));
            loraSf->setValue(l.modem.value("sf").toInt(7));const int bw=l.modem.value("bw").toInt(125000);if(loraBandwidth->findData(bw)<0)loraBandwidth->addItem(QString::number(bw)+" Hz",bw);loraBandwidth->setCurrentIndex(loraBandwidth->findData(bw));
            loraCodingRate->setCurrentIndex(loraCodingRate->findData(l.modem.value("cr").toInt(1)));loraOversampling->setCurrentIndex(loraOversampling->findData(l.modem.value("os").toInt(1)));loraPreamble->setValue(l.modem.value("preamble").toInt(16));loraSyncWord->setValue(l.modem.value("syncWord").toInt(43));
        }break;
    }
    formBusy=false;refreshRuntime();
}
void Window::editDesired() {
    if(formBusy)return;const auto id=selectedNode();for(auto &n:project.nodes)if(n.id==id){n.name=nodeName->text();n.presetId=device->currentData().toString();n.listeningHz=frequency->value();n.bandwidthHz=bandwidth->value();n.txLevel=level->value();n.mode=mode->currentText();if(auto item=nodeItems.value(id)){item->setData(3,n.name);item->setData(4,n.presetId);item->update();}}dirty();
}
void Window::editNodeOptions(){
    if(formBusy||runtime.state()!=Runtime::Idle)return;
    for(auto &node:project.nodes)if(node.id==selectedNode()){
        auto ports=node.ports.isEmpty()?defaultNodePorts():node.ports;
        auto update=[&](const QString &direction,QComboBox *backend,QLineEdit *id,QLineEdit *binding,QCheckBox *enabled){
            int index=-1;for(int i=0;i<ports.size();++i)if(ports[i].toObject().value("direction")==direction){index=i;break;}
            auto port=index<0?QJsonObject():ports[index].toObject();port["id"]=id->text();port["direction"]=direction;port["backend"]=backend->currentData().toString();port["enabled"]=enabled->isChecked();
            port[direction=="rx"?"receiverId":"channelId"]=binding->text();if(index<0)ports.append(port);else ports[index]=port;
        };
        update("rx",rxBackend,rxPortId,receiverId,rxPortEnabled);update("tx",txBackend,txPortId,channelId,txPortEnabled);
        for(int index=0;index<ports.size();++index){auto port=ports[index].toObject();if(port.value("direction")!="rx")continue;
            port["channelId"]=rxChannelId->text();port["centerHz"]=rxCenter->value();
            if(port.value("backend")=="radio"&&!port.contains("modem")){QJsonObject modem{{"type","afsk1200"}};for(const auto &link:project.links)if(link.rx==node.id&&link.bearer=="iq"){modem=link.json().value("modem").toObject();if(modem.isEmpty())modem={{"type","afsk1200"}};break;}port["modem"]=modem;}
            ports[index]=port;break;
        }node.ports=ports;
        auto protocol=node.json().value("protocol").toObject();const auto type=nodeProtocol->currentData().toString();
        if(protocol.value("type").toString("lab")!=type)protocol={};protocol["type"]=type;node.protocol=protocol;
        if(!protocolProfile->text().trimmed().isEmpty())node.protocol["profileId"]=protocolProfile->text().trimmed();else node.protocol.remove("profileId");
        if(nodeProtocol->currentData()=="meshtastic"){
            node.protocol["nodeNumber"]=nodeNumber->value();
            node.protocol["relayRole"]=nodeRole->currentData().toString();
        }
        portsJson->setPlainText(QString::fromUtf8(QJsonDocument(node.json().value("ports").toArray()).toJson()));protocolJson->setPlainText(QString::fromUtf8(QJsonDocument(node.json().value("protocol").toObject()).toJson()));
        break;
    }
    dirty();refreshRuntime();
}
void Window::applyNodeJson(bool ports){
    if(formBusy||runtime.state()!=Runtime::Idle)return;const auto bytes=(ports?portsJson:protocolJson)->toPlainText().toUtf8();QJsonParseError parse;const auto document=QJsonDocument::fromJson(bytes,&parse);
    if(bytes.size()>16384||parse.error!=QJsonParseError::NoError||(ports?!document.isArray():!document.isObject())){showError(this,tr("Перевір JSON і тип значення; межа 16 КіБ."));return;}
    const QJsonValue value=ports?QJsonValue(document.array()):QJsonValue(document.object());if(privateOptions(value)){showError(this,tr("Ключі та шляхи backend налаштовуються локально на host-службі. Використайте profileId."));return;}
    for(auto &node:project.nodes)if(node.id==selectedNode()){auto candidate=node;if(ports)candidate.ports=document.array();else candidate.protocol=document.object();const auto error=validateNodeOptions(candidate);if(!error.isEmpty()){showError(this,error);return;}node=candidate;dirty();selection();return;}
}
void Window::copyRxModem(){
    if(formBusy||runtime.state()!=Runtime::Idle)return;
    for(auto &node:project.nodes)if(node.id==selectedNode())for(const auto &link:project.links)if(link.rx==node.id&&link.bearer=="iq"){
        auto ports=node.ports.isEmpty()?defaultNodePorts():node.ports;
        for(int index=0;index<ports.size();++index){auto port=ports[index].toObject();if(port.value("direction")!="rx")continue;port["modem"]=link.json().value("modem").toObject();ports[index]=port;node.ports=ports;dirty();selection();return;}
    }
}
void Window::editLinkOptions(){
    if(formBusy||runtime.state()!=Runtime::Idle||selectedLink.isEmpty())return;
    for(auto &link:project.links)if(link.tx+"/"+link.rx==selectedLink){
        link.bearer=linkBearer->currentData().toString();link.txPort=linkTxPort->text();link.rxPort=linkRxPort->text();auto modem=link.json().value("modem").toObject();modem["type"]=linkModem->currentData().toString();link.modem=modem;
        if(linkModem->currentData()=="lora"){
            link.modem["sf"]=loraSf->value();link.modem["bw"]=loraBandwidth->currentData().toInt();link.modem["cr"]=loraCodingRate->currentData().toInt();link.modem["os"]=loraOversampling->currentData().toInt();
            link.modem["preamble"]=loraPreamble->value();link.modem["syncWord"]=loraSyncWord->value();
        }else for(const auto key:{"sf","bw","cr","os","preamble","syncWord","invertIq","ldro","explicitHeader","payloadCrc"})link.modem.remove(key);
        break;
    }
    dirty();linksGeometry();refreshRuntime();
}
void Window::refreshRuntime() {
    const bool idle=runtime.state()==Runtime::Idle,live=runtime.state()==Runtime::Running;
    const bool extended=remoteOptions(project);startAction->setEnabled(idle&&!extended);stopAction->setEnabled(!idle&&runtime.state()!=Runtime::Stopping);
    startAction->setToolTip(extended?tr("Порти, протоколи й носії зв'язків запускаються через «Реальні екземпляри», не через локальний IQ-хаб."):QString());packetsAction->setEnabled(!extended);
    for(auto action:{addAction,connectAction,removeAction,libraryAction,mediumAction})action->setEnabled(idle);
    device->setEnabled(idle);
    for(auto combo:{rxBackend,txBackend,nodeProtocol,linkBearer,linkModem})combo->setEnabled(idle);
    for(auto field:{rxPortId,txPortId,receiverId,channelId,rxChannelId,protocolProfile,linkTxPort,linkRxPort})field->setEnabled(idle);
    rxCenter->setEnabled(idle);rxModemCopy->setEnabled(idle);portsJson->setEnabled(idle);portsJsonApply->setEnabled(idle);protocolJson->setEnabled(idle);protocolJsonApply->setEnabled(idle);
    rxPortEnabled->setEnabled(idle);txPortEnabled->setEnabled(idle);
    const bool meshtastic=nodeProtocol->currentData()=="meshtastic";nodeNumber->setEnabled(idle&&meshtastic);nodeRole->setEnabled(idle&&meshtastic);
    protocolHint->setText(nodeProtocol->currentData()=="reticulum"?tr("Reticulum: потрібен host-sidecar; доступність підтверджує служба."):nodeProtocol->currentData()=="lorawan"?tr("LoRaWAN: потрібні локальний профіль і підтримка служби."):QString());
    const bool lora=idle&&linkBearer->currentData()!="data"&&linkModem->currentData()=="lora";
    for(auto combo:{loraBandwidth,loraCodingRate,loraOversampling})combo->setEnabled(lora);for(auto field:{loraSf,loraPreamble,loraSyncWord})field->setEnabled(lora);
    linkModem->setEnabled(idle&&linkBearer->currentData()!="data");
    const auto selected=runtime.snapshot(selectedNode());
    const bool ready=live&&selected.value("phase").toString()!="Control unavailable"&&!selected.isEmpty();
    const bool running=selected.value("applied").toObject().value("running").toBool();
    const bool pending=selected.value("runningPending").toBool();
    applyButton->setEnabled(ready&&!pending);testButton->setEnabled(live&&!selectedNode().isEmpty()&&!selected.value("txRunning").toBool());
    startRxButton->setEnabled(ready&&!running&&!pending);stopRxButton->setEnabled(ready&&running&&!pending);
    stopTxButton->setEnabled(live&&selected.value("txRunning").toBool());
    const auto hub=runtime.hubSnapshot();
    applyLinkButton->setEnabled(false);
    linkActual->setText(idle?tr("Буде задано під час запуску"):tr("Очікування хаба"));
    for(const auto &l:project.links)if(l.tx+"/"+l.rx==selectedLink) {
        const auto applied=confirmedLinks.value(selectedLink);
        applyLinkButton->setEnabled(live&&!hub.contains("controlError")&&!hub.value("pending").toBool()&&!linkMatches(l,applied));
        if(live&&!applied.isEmpty())linkActual->setText(QString("%1 · %2 dB · %3 ms\n%4 %5%6")
            .arg(applied.value("enabled").toBool()?tr("Увімкнений"):tr("Вимкнений"))
            .arg(applied.value("gainDb").toDouble(),0,'f',1).arg(applied.value("delayMs").toInt())
            .arg(tr("Ревізія")).arg(hub.value("revision").toString())
            .arg(hub.value("pending").toBool()?tr(" · застосовується"):linkMatches(l,applied)?"":tr(" · є незастосовані зміни")));
    }
    const auto controlError=hub.value("controlError").toString(),lastError=hub.value("lastLinkError").toString();
    if(live&&(!controlError.isEmpty()||!lastError.isEmpty()))linkActual->setText(linkActual->text()+"\n"+
        (controlError.isEmpty()?tr("Остання помилка: ")+lastError:tr("Керування: ")+controlError));
    const QStringList states{tr("Зупинено"),tr("Запуск"),tr("Працює"),tr("Завершення")};status->setText(QString("%1 · %2 SDR · %3 · %4 Hz / %5 S/s%6").arg(states[int(runtime.state())]).arg(project.nodes.size()).arg(project.mixIq?tr("Сума IQ"):tr("Кадри IQ")).arg(project.centerHz,0,'f',0).arg(project.sampleRate).arg(topologyPending?tr(" · зв'язки мають незастосовані зміни"):""));
    const auto s=runtime.snapshot(selectedNode());const auto a=s.value("applied").toObject();
    actual->setText(s.isEmpty()?tr("Ще не запущено"):QString("%1 · PID %2\nCenter: %3 Hz\nListen: %4 Hz\nBW: %5 Hz · samples: %6\n%7")
        .arg(s.value("phase").toString()).arg(s.value("pid").toDouble(),0,'f',0).arg(a.value("centerFrequencyHz").toDouble(),0,'f',0)
        .arg(a.value("listeningFrequencyHz").toDouble(),0,'f',0).arg(a.value("audioBandwidthHz").toDouble(),0,'f',0).arg(a.value("samples").toDouble(),0,'f',0).arg(s.value("detail").toString()));
    if(live&&!a.isEmpty())for(const auto &n:project.nodes)if(n.id==selectedNode()) {
        const QMap<QString,int> codes{{"am",0},{"nfm",1},{"wfm",7},{"usb",3},{"lsb",4},{"cw",6}};
        if(std::abs(a.value("listeningFrequencyHz").toDouble()-n.listeningHz)>0.5||std::abs(a.value("audioBandwidthHz").toDouble()-n.bandwidthHz)>0.5||a.value("modulationType").toInt()!=codes.value(n.mode))
            actual->setText(actual->text()+tr("\nЄ незастосовані зміни RX"));
    }
    refreshSpectrum();
}
void Window::refreshTopology() {
    confirmedLinks.clear();topologyPending=false;
    if(runtime.state()!=Runtime::Idle)for(const auto &value:runtime.hubSnapshot().value("links").toArray()){
        const auto link=value.toObject();confirmedLinks.insert(link.value("tx").toString()+"/"+link.value("rx").toString(),link);
    }
    if(runtime.state()==Runtime::Running)for(const auto &link:project.links)
        topologyPending|=!linkMatches(link,confirmedLinks.value(link.tx+"/"+link.rx));
    linksGeometry();refreshRuntime();
}
void Window::refreshSpectrum() {
    if(!spectrumView)return;
    const QString id=selectedNode();
    const bool active=runtime.state()==Runtime::Running&&!id.isEmpty()&&previewEnabled->isChecked()&&
        spectrumDock->isVisible()&&!isMinimized();
    runtime.inspectNode(active?id:QString());
    spectrumTitle->setText(id.isEmpty()?tr("Вузол не вибрано"):nodeLabel(project,id)+" · "+id);
    const auto snapshot=runtime.snapshot(id),applied=snapshot.value("applied").toObject();
    if(!active){spectrumView->clear(tr("Перегляд вимкнено"));return;}
    if(!applied.value("running").toBool()||snapshot.value("runningPending").toBool()){
        spectrumView->clear(tr("RX зупинено або перемикається"));return;
    }
    const auto trace=snapshot.value("trace").toObject();
    const qint64 received=qint64(trace.value("receivedMs").toDouble());
    if(trace.isEmpty()||QDateTime::currentMSecsSinceEpoch()-received>2000){
        const auto error=snapshot.value("previewError").toString();
        spectrumView->clear(error.isEmpty()?tr("Очікування нових даних RX"):error.left(180));return;
    }
    spectrumView->setListener(applied.value("listeningFrequencyHz").toDouble(),applied.value("audioBandwidthHz").toDouble());
    if(spectrumView->setTrace(trace))spectrumTitle->setText(spectrumTitle->text()+" · "+QDateTime::fromMSecsSinceEpoch(received).toString("HH:mm:ss"));
}
void Window::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if(event->type()==QEvent::WindowStateChange)refreshSpectrum();
}
void Window::addNode(){Node n;int i=1;while(nodeItems.contains("node"+QString::number(i)))++i;n.id="node"+QString::number(i);n.name="SDR "+QString::number(i);n.presetId=presets.isEmpty()?"":presets.first().id;n.listeningHz=project.centerHz;n.position={double((project.nodes.size()%3)*250),double((project.nodes.size()/3)*150)};project.nodes<<n;dirty();rebuild();nodeItems[n.id]->setSelected(true);}
void Window::addLink(){if(project.nodes.size()<2)return;QDialog d(this);d.setWindowTitle(tr("Напрямлений радіозв'язок"));QFormLayout f(&d);QComboBox tx,rx;for(const auto &n:project.nodes){tx.addItem(n.name,n.id);rx.addItem(n.name,n.id);}rx.setCurrentIndex(1);f.addRow("TX",&tx);f.addRow("RX",&rx);QDialogButtonBox buttons(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);f.addRow(&buttons);connect(&buttons,&QDialogButtonBox::accepted,&d,&QDialog::accept);connect(&buttons,&QDialogButtonBox::rejected,&d,&QDialog::reject);if(d.exec()!=QDialog::Accepted)return;
    Link l;l.tx=tx.currentData().toString();l.rx=rx.currentData().toString();Project p=project;p.links<<l;const auto e=p.validate(presets);if(!e.isEmpty()){showError(this,e);return;}project=p;dirty();rebuild();}
void Window::removeSelected(){const QString id=selectedNode();if(!id.isEmpty()){for(int i=project.nodes.size()-1;i>=0;--i)if(project.nodes[i].id==id)project.nodes.removeAt(i);for(int i=project.links.size()-1;i>=0;--i)if(project.links[i].tx==id||project.links[i].rx==id)project.links.removeAt(i);}else{for(int i=project.links.size()-1;i>=0;--i)if(project.links[i].tx+"/"+project.links[i].rx==selectedLink)project.links.removeAt(i);}dirty();rebuild();}
void Window::editMedium(){QDialog d(this);d.setWindowTitle(tr("Віртуальне середовище"));QFormLayout f(&d);auto center=spin(1,1e12,0,&d);center->setValue(project.centerHz);QComboBox rate;for(int n:{48000,96000,192000,384000})rate.addItem(QString::number(n),n);rate.setCurrentIndex(rate.findData(project.sampleRate));QCheckBox mixing(tr("Складати одночасні IQ-потоки"));mixing.setChecked(project.mixIq);f.addRow(tr("Центр, Hz"),center);f.addRow(tr("Частота дискретизації"),&rate);f.addRow(&mixing);QLabel note(tr("Чинний хаб: до 8 RX для суми IQ або до 32 RX для передачі кадрів. Частота й sample rate спільні для сеансу."));note.setWordWrap(true);f.addRow(&note);QDialogButtonBox b(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);f.addRow(&b);connect(&b,&QDialogButtonBox::accepted,&d,&QDialog::accept);connect(&b,&QDialogButtonBox::rejected,&d,&QDialog::reject);if(d.exec()!=QDialog::Accepted)return;Project p=project;for(auto &n:p.nodes)n.listeningHz+=center->value()-p.centerHz;p.centerHz=center->value();p.sampleRate=rate.currentData().toInt();p.mixIq=mixing.isChecked();const auto e=p.validate(presets);if(!e.isEmpty()){showError(this,e);return;}project=p;dirty();selection();refreshRuntime();}
void Window::editLibrary() {
    QDialog d(this);d.setAttribute(Qt::WA_DontShowOnScreen,testAttribute(Qt::WA_DontShowOnScreen));d.setWindowTitle(tr("Бібліотека пристроїв та інсталяцій Obrii"));d.resize(1100,430);QVBoxLayout layout(&d);
    QLabel note(tr("Профіль зберігає адресу та каталог Obrii; вузли посилаються на його ID. SSH-профілі готуються для наступного етапу віддаленого запуску. Паролі не зберігаються."));note.setWordWrap(true);layout.addWidget(&note);
    QTableWidget table(0,7);table.setObjectName("deviceLibrary");table.setHorizontalHeaderLabels({"ID",tr("Назва"),tr("Тип"),tr("Адреса"),"SSH user",tr("Порт"),tr("Каталог Obrii")});table.horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);table.horizontalHeader()->setStretchLastSection(true);layout.addWidget(&table);
    auto append=[&](const HostPreset &p){int row=table.rowCount();table.insertRow(row);const QStringList fields{p.id,p.name,p.kind,p.host,p.user,QString::number(p.sshPort),p.directory};for(int c=0;c<7;++c){auto item=new QTableWidgetItem(fields[c]);if(c==0)item->setFlags(item->flags()&~Qt::ItemIsEditable);table.setItem(row,c,item);}auto kind=new QComboBox;kind->addItem(tr("Цей ПК"),"local");kind->addItem(tr("SSH · Linux / Raspberry"),"ssh-posix");kind->setCurrentIndex(kind->findData(p.kind));table.setCellWidget(row,2,kind);};
    for(const auto &p:presets)append(p);
    auto collect=[&](){QVector<HostPreset> values;for(int r=0;r<table.rowCount();++r){auto text=[&](int c){return table.item(r,c)?table.item(r,c)->text().trimmed():QString();};values.push_back({text(0),text(1),static_cast<QComboBox *>(table.cellWidget(r,2))->currentData().toString(),text(3),text(4),text(6),text(5).toInt()});}return values;};
    QHBoxLayout tools;QPushButton add(tr("+ Пристрій")),remove(tr("Видалити")),browse(tr("Каталог…")),import(tr("Імпорт…")),exportButton(tr("Експорт…"));for(auto button:{&add,&remove,&browse,&import,&exportButton})tools.addWidget(button);layout.addLayout(&tools);
    connect(&add,&QPushButton::clicked,&d,[&]{int id=1;auto values=collect();while(std::any_of(values.begin(),values.end(),[&](const HostPreset &p){return p.id=="host"+QString::number(id);}))++id;HostPreset p;p.id="host"+QString::number(id);p.name=tr("Новий пристрій");p.directory=QCoreApplication::applicationDirPath();append(p);});
    connect(&remove,&QPushButton::clicked,&d,[&]{if(table.currentRow()>=0)table.removeRow(table.currentRow());});
    connect(&browse,&QPushButton::clicked,&d,[&]{if(table.currentRow()<0)return;auto dir=QFileDialog::getExistingDirectory(&d,tr("Каталог програм Obrii"));if(!dir.isEmpty())table.item(table.currentRow(),6)->setText(dir);});
    connect(&import,&QPushButton::clicked,&d,[&]{auto path=QFileDialog::getOpenFileName(&d,tr("Бібліотека"),{},"JSON (*.json)");if(path.isEmpty())return;QVector<HostPreset> values;QString e;if(!loadLibrary(path,&values,&e)){showError(&d,e);return;}table.setRowCount(0);for(const auto &p:values)append(p);});
    connect(&exportButton,&QPushButton::clicked,&d,[&]{auto path=QFileDialog::getSaveFileName(&d,tr("Експорт бібліотеки"),"devices.json","JSON (*.json)");if(path.isEmpty())return;QString e;if(!saveLibrary(path,collect(),&e))showError(&d,e);});
    QDialogButtonBox buttons(QDialogButtonBox::Save|QDialogButtonBox::Cancel);buttons.button(QDialogButtonBox::Save)->setText(tr("Зберегти"));buttons.button(QDialogButtonBox::Cancel)->setText(tr("Скасувати"));layout.addWidget(&buttons);connect(&buttons,&QDialogButtonBox::rejected,&d,&QDialog::reject);
    connect(&buttons,&QDialogButtonBox::accepted,&d,[&]{const auto values=collect();const auto e=project.validate(values);if(!e.isEmpty()){showError(&d,e);return;}QString error;if(!saveLibrary(libraryPath,values,&error)){showError(&d,error);return;}presets=values;d.accept();});d.exec();selection();
}
bool Window::saveProjectFile(const QString &path,QString *error){const auto e=project.validate(presets);if(!e.isEmpty()){if(error)*error=e;return false;}if(!saveJson(path,project.json(),error))return false;projectPath=path;modified=false;setWindowTitle(tr("Obrii Mesh Designer · ")+QFileInfo(path).fileName());return true;}
bool Window::loadProjectFile(const QString &path,QString *error){if(runtime.state()!=Runtime::Idle){if(error)*error="Stop session before loading";return false;}QJsonObject json;Project p;if(!loadJson(path,&json,error)||!Project::parse(json,&p,error))return false;const auto e=p.validate(presets);if(!e.isEmpty()){if(error)*error=e;return false;}project=p;projectPath=path;modified=false;rebuild();refreshRuntime();return true;}
bool Window::maybeSave(){if(!modified)return true;const auto answer=QMessageBox::question(this,tr("Сценарій змінено"),tr("Зберегти зміни?"),QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel);if(answer==QMessageBox::Cancel)return false;if(answer==QMessageBox::Discard)return true;QString path=projectPath;if(path.isEmpty())path=QFileDialog::getSaveFileName(this,tr("Сценарій"),"network.json","JSON (*.json)");if(path.isEmpty())return false;QString e;if(!saveProjectFile(path,&e)){showError(this,e);return false;}return true;}
void Window::showPacketLab() {
    if(packetDialog){packetDialog->show();packetDialog->raise();packetDialog->activateWindow();return;}
    auto snapshot=project;
    if(runtime.state()==Runtime::Running)for(auto &l:snapshot.links){
        const auto applied=confirmedLinks.value(l.tx+"/"+l.rx);
        l.enabled=!applied.isEmpty()&&applied.value("enabled").toBool();l.delayMs=applied.value("delayMs").toInt();
        if(!applied.isEmpty())l.gainDb=applied.value("gainDb").toDouble();
    }
    packetDialog=new PacketDialog(snapshot,this);packetDialog->show();
}
void Window::showRemoteMesh() {
    if(remoteDialog){remoteDialog->show();remoteDialog->raise();remoteDialog->activateWindow();return;}
    remoteDialog=new RemoteDialog(project,this);remoteDialog->show();
}
void Window::closeEvent(QCloseEvent *event){if(closing&&runtime.state()==Runtime::Idle){if(packetDialog)packetDialog->close();if(remoteDialog)remoteDialog->close();event->accept();return;}if(!maybeSave()){event->ignore();return;}if(packetDialog)packetDialog->close();if(remoteDialog)remoteDialog->close();if(runtime.state()!=Runtime::Idle){closing=true;runtime.stop();event->ignore();return;}event->accept();}
}
