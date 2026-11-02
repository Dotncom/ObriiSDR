#include "meshwindow.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
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
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
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
    toolbar->addSeparator();startAction=add(tr("▶ Запустити"),[this]{QString e;if(!runtime.start(project,presets,&e))showError(this,e);else topologyPending=false;refreshRuntime();});
    stopAction=add(tr("■ Зупинити"),[this]{runtime.stop();});
    scene=new Scene(this);view=new View(scene,this);view->setToolTip(tr("Ctrl + колесо — масштаб; перетягування вузлів — позиція на полотні."));view->setObjectName("topologyCanvas");view->setRenderHint(QPainter::Antialiasing);view->setDragMode(QGraphicsView::RubberBandDrag);setCentralWidget(view);
    auto dock=new QDockWidget(tr("Параметри"),this);dock->setMinimumWidth(300);dock->setMaximumWidth(410);properties=new QStackedWidget(dock);dock->setWidget(properties);addDockWidget(Qt::RightDockWidgetArea,dock);
    auto hint=new QLabel(tr("Вибери вузол або зв'язок.\n\nВузол прив'язаний до профілю пристрою.\nАдреси й шляхи редагуються в бібліотеці."));hint->setWordWrap(true);hint->setMargin(16);properties->addWidget(hint);
    auto nodePage=new QWidget;auto form=new QFormLayout(nodePage);
    nodeName=new QLineEdit;nodeName->setObjectName("nodeName");device=new QComboBox;mode=new QComboBox;mode->addItems({"am","nfm","wfm","usb","lsb","cw"});
    frequency=spin(1,1e12,0,this);frequency->setObjectName("listeningHz");frequency->setSuffix(" Hz");bandwidth=spin(100,2e6,0,this);bandwidth->setSuffix(" Hz");level=spin(0,0.95,2,this);level->setSingleStep(0.05);
    form->addRow(tr("Назва"),nodeName);form->addRow(tr("Пристрій / Obrii"),device);form->addRow(tr("Частота прийому"),frequency);form->addRow(tr("Смуга"),bandwidth);form->addRow(tr("Демодуляція"),mode);form->addRow(tr("Рівень тестового TX"),level);
    applyButton=new QPushButton(tr("Застосувати до RX"));applyButton->setObjectName("applyRadio");form->addRow(applyButton);
    connect(applyButton,&QPushButton::clicked,this,[this]{for(const auto &n:project.nodes)if(n.id==selectedNode()){QString e;if(!runtime.apply(n,&e))showError(this,e);}});
    testButton=new QPushButton(tr("Тестовий CW → середовище"));testButton->setObjectName("testCw");form->addRow(testButton);
    connect(testButton,&QPushButton::clicked,this,[this]{QString e;if(!runtime.testSignal(selectedNode(),&e))showError(this,e);});
    actual=new QLabel;actual->setWordWrap(true);actual->setTextInteractionFlags(Qt::TextSelectableByMouse);form->addRow(tr("Підтверджено RX"),actual);properties->addWidget(nodePage);
    connect(nodeName,&QLineEdit::textEdited,this,[this]{editDesired();});
    for(auto s:{frequency,bandwidth,level})connect(s,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this]{editDesired();});
    for(auto c:{device,mode})connect(c,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{editDesired();});
    auto linkPage=new QWidget;auto lf=new QFormLayout(linkPage);linkTitle=new QLabel;gain=spin(-120,60,1,this);gain->setSuffix(" dB");delay=new QSpinBox;delay->setRange(0,500);delay->setSuffix(" ms");enabled=new QCheckBox(tr("Увімкнений"));
    lf->addRow(linkTitle);lf->addRow(tr("Підсилення / ослаблення"),gain);lf->addRow(tr("Затримка"),delay);lf->addRow(enabled);
    auto staged=new QLabel(tr("Зміни зв'язку набувають чинності після перезапуску сеансу. Автоматичної ретрансляції пакетів на цьому етапі немає."));staged->setWordWrap(true);lf->addRow(staged);properties->addWidget(linkPage);
    auto editLink=[this]{if(formBusy||selectedLink.isEmpty())return;for(auto &l:project.links)if(l.tx+"/"+l.rx==selectedLink){l.gainDb=gain->value();l.delayMs=delay->value();l.enabled=enabled->isChecked();}topologyPending=runtime.state()!=Runtime::Idle;dirty();linksGeometry();refreshRuntime();};
    connect(gain,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[editLink]{editLink();});connect(delay,QOverload<int>::of(&QSpinBox::valueChanged),this,[editLink]{editLink();});connect(enabled,&QCheckBox::toggled,this,[editLink]{editLink();});
    auto logDock=new QDockWidget(tr("Події сеансу"),this);logs=new QPlainTextEdit;logs->setReadOnly(true);logs->setMaximumBlockCount(500);logDock->setWidget(logs);addDockWidget(Qt::BottomDockWidgetArea,logDock);logDock->setMaximumHeight(190);
    status=new QLabel;statusBar()->addWidget(status,1);statusBar()->addPermanentWidget(new QLabel(tr("Етап 1 · віртуальне IQ-середовище")));
    connect(scene,&QGraphicsScene::selectionChanged,this,&Window::selection);
    connect(&runtime,&Runtime::changed,this,[this]{refreshRuntime();if(closing&&runtime.state()==Runtime::Idle)QTimer::singleShot(0,this,&QWidget::close);});
    connect(&runtime,&Runtime::nodeChanged,this,[this](const QString &id){if(auto item=nodeItems.value(id)){item->setData(2,runtime.snapshot(id).value("phase").toString());item->update();}refreshRuntime();});
    connect(&runtime,&Runtime::log,logs,&QPlainTextEdit::appendPlainText);
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
        const QColor color=l.enabled?QColor("#52a9c2"):QColor("#64707b");item->setPen(QPen(color,2,l.enabled?Qt::SolidLine:Qt::DashLine));item->setBrush(color);item->setToolTip(l.tx+" → "+l.rx+QString(" · %1 dB · %2 ms").arg(l.gainDb).arg(l.delayMs));}
}
void Window::selection() {
    if(formBusy)return;formBusy=true;selectedLink.clear();properties->setCurrentIndex(0);
    const auto id=selectedNode();
    if(!id.isEmpty())for(const auto &n:project.nodes)if(n.id==id){properties->setCurrentIndex(1);nodeName->setText(n.name);device->clear();for(const auto &p:presets)device->addItem(p.name+(p.kind=="local"?"":" · SSH"),p.id);device->setCurrentIndex(device->findData(n.presetId));frequency->setValue(n.listeningHz);bandwidth->setValue(n.bandwidthHz);level->setValue(n.txLevel);mode->setCurrentText(n.mode);break;}
    if(id.isEmpty())for(auto item:scene->selectedItems())if(item->data(0)=="link"){selectedLink=item->data(1).toString();for(const auto &l:project.links)if(l.tx+"/"+l.rx==selectedLink){properties->setCurrentIndex(2);linkTitle->setText(nodeLabel(project,l.tx)+" → "+nodeLabel(project,l.rx));gain->setValue(l.gainDb);delay->setValue(l.delayMs);enabled->setChecked(l.enabled);}break;}
    formBusy=false;refreshRuntime();
}
void Window::editDesired() {
    if(formBusy)return;const auto id=selectedNode();for(auto &n:project.nodes)if(n.id==id){n.name=nodeName->text();n.presetId=device->currentData().toString();n.listeningHz=frequency->value();n.bandwidthHz=bandwidth->value();n.txLevel=level->value();n.mode=mode->currentText();if(auto item=nodeItems.value(id)){item->setData(3,n.name);item->setData(4,n.presetId);item->update();}}dirty();
}
void Window::refreshRuntime() {
    const bool idle=runtime.state()==Runtime::Idle,live=runtime.state()==Runtime::Running;
    startAction->setEnabled(idle);stopAction->setEnabled(!idle&&runtime.state()!=Runtime::Stopping);
    for(auto action:{addAction,connectAction,removeAction,libraryAction,mediumAction})action->setEnabled(idle);
    device->setEnabled(idle);applyButton->setEnabled(live&&!selectedNode().isEmpty());testButton->setEnabled(live&&!selectedNode().isEmpty());
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
void Window::closeEvent(QCloseEvent *event){if(closing&&runtime.state()==Runtime::Idle){event->accept();return;}if(!maybeSave()){event->ignore();return;}if(runtime.state()!=Runtime::Idle){closing=true;runtime.stop();event->ignore();return;}event->accept();}
}
