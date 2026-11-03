#include "meshpacketdialog.h"
#include "meshproject.h"
#include "meshiqbearer.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace Mesh {
namespace {
QString u(const char *s){return QString::fromUtf8(s);}
QString pathText(const QJsonArray &a){QStringList p;for(const auto &v:a)p<<v.toString();return p.join(" -> ");}
QTableWidgetItem *cell(const QString &text){auto item=new QTableWidgetItem(text);item->setFlags(item->flags()&~Qt::ItemIsEditable);return item;}
QString statusText(const QString &code){
    static const QMap<QString,QString> names{{"delivered",u("Доставлено")},{"no_route",u("Немає маршруту")},{"ttl_expired",u("Вичерпано TTL")},
        {"duplicate",u("Дублікат")},{"queue_full",u("Черга заповнена")},{"link_down",u("Розрив зв'язку")},{"node_down",u("Вузол вимкнений")},{"time_limit",u("Межа часу")},
        {"modem_timeout",u("Модем: немає декодування")},{"modem_tx_rejected",u("Модем: TX відхилено")},{"modem_stopped",u("Модем зупинено")}};
    return names.value(code,code);
}
}
PacketDialog::PacketDialog(const Project &project,QWidget *parent):QDialog(parent) {
    setAttribute(Qt::WA_DontShowOnScreen,parent&&parent->testAttribute(Qt::WA_DontShowOnScreen));
    setObjectName("packetLab");setWindowTitle(u("Пакетна мережа"));resize(1000,680);
    setAttribute(Qt::WA_DeleteOnClose);
    auto layout=new QVBoxLayout(this);auto commands=new QHBoxLayout;
    mode=new QComboBox;mode->setObjectName("packetBearer");mode->addItems({u("Пакетна модель"),"IQ · AX.25 / AFSK1200 / FM"});commands->addWidget(mode);
    mode->setToolTip(u("Зміна режиму скидає експеримент. IQ: до 8 локальних вузлів, один пакет до 256 байтів, без фізичного RF."));
    auto button=[&](QStyle::StandardPixmap icon,const QString &tip,const QString &name){
        auto b=new QPushButton(style()->standardIcon(icon),name,this);b->setToolTip(tip);commands->addWidget(b);return b;
    };
    run=button(QStyle::SP_MediaPause,u("Пауза / автовиконання"),u("Автовиконання"));run->setCheckable(true);run->setChecked(true);run->setMinimumWidth(160);run->setObjectName("packetRun");
    step=button(QStyle::SP_MediaSkipForward,u("Одна подія"),{});step->setObjectName("packetStep");
    auto reset=button(QStyle::SP_BrowserReload,u("Скинути експеримент"),{});reset->setObjectName("packetReset");
    commands->addStretch();
    auto load=button(QStyle::SP_DialogOpenButton,u("Відкрити пакетний сценарій"),{});
    auto scenario=button(QStyle::SP_DialogSaveButton,u("Зберегти відтворюваний сценарій"),u("Сценарій"));
    loadButton=load;scenarioButton=scenario;
    auto result=button(QStyle::SP_DialogSaveButton,u("Зберегти результати та трасу переходів"),u("Результати"));
    layout->addLayout(commands);
    auto form=new QGridLayout;source=new QComboBox;destination=new QComboBox;ttl=new QSpinBox;ttl->setRange(0,64);ttl->setValue(16);
    source->setObjectName("packetSource");destination->setObjectName("packetDestination");ttl->setObjectName("packetTtl");
    for(auto combo:{source,destination}){combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);combo->setMinimumContentsLength(10);}
    form->addWidget(new QLabel(u("Від")),0,0);form->addWidget(source,0,1);form->addWidget(new QLabel(u("До")),0,2);form->addWidget(destination,0,3);form->addWidget(new QLabel("TTL"),0,4);form->addWidget(ttl,0,5);
    text=new QLineEdit;text->setMaxLength(1024);text->setObjectName("packetText");text->setText("TEST");
    sendButton=new QPushButton(style()->standardIcon(QStyle::SP_ArrowRight),u("Надіслати"));sendButton->setObjectName("packetSend");
    form->addWidget(new QLabel("UTF-8"),1,0);form->addWidget(text,1,1,1,4);form->addWidget(sendButton,1,5);form->setColumnStretch(1,1);form->setColumnStretch(3,1);layout->addLayout(form);
    routeLabel=new QLabel;routeLabel->setWordWrap(true);routeLabel->setObjectName("packetRoute");layout->addWidget(routeLabel);
    metrics=new QLabel;metrics->setWordWrap(true);metrics->setObjectName("packetMetrics");layout->addWidget(metrics);
    errorLabel=new QLabel;errorLabel->setWordWrap(true);layout->addWidget(errorLabel);
    auto tabs=new QTabWidget;layout->addWidget(tabs,1);
    auto packetPage=new QWidget;auto packetLayout=new QVBoxLayout(packetPage);
    packets=new QTableWidget(0,7);packets->setObjectName("packetResults");packets->setHorizontalHeaderLabels({"ID",u("Від"),u("До"),u("Стан"),u("Затримка, ms"),u("Переходи"),u("Фактичний шлях")});
    packets->setSelectionBehavior(QAbstractItemView::SelectRows);packets->setSelectionMode(QAbstractItemView::SingleSelection);
    packets->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);packets->horizontalHeader()->setStretchLastSection(true);packetLayout->addWidget(packets,1);
    encoding=new QComboBox;encoding->addItems({"UTF-8","HEX"});packetLayout->addWidget(encoding);
    payload=new QPlainTextEdit;payload->setReadOnly(true);payload->setMaximumHeight(90);packetLayout->addWidget(payload);tabs->addTab(packetPage,u("Пакети"));
    nodes=new QTableWidget(0,8);nodes->setObjectName("packetNodes");nodes->setHorizontalHeaderLabels({"ID",u("Назва"),u("Доступний"),u("Черга"),u("Вхід"),u("Переслано"),u("Доставлено"),u("Втрати")});
    nodes->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);nodes->horizontalHeader()->setStretchLastSection(true);tabs->addTab(nodes,u("Вузли"));
    links=new QTableWidget(0,4);links->setObjectName("packetLinks");links->setHorizontalHeaderLabels({"TX","RX",u("Увімкнений"),u("Затримка, ms")});
    links->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);links->horizontalHeader()->setStretchLastSection(true);tabs->addTab(links,u("Зв'язки"));
    for(auto table:{packets,nodes,links})table->setStyleSheet(
        "QTableWidget{background:#101923;color:#e2eaf4;gridline-color:#405369;selection-background-color:#36586f;selection-color:white;}"
        "QHeaderView::section{background-color:#293e52;color:#e2eaf4;padding:5px;border:1px solid #405369;}"
        "QTableCornerButton::section{background-color:#293e52;border:1px solid #405369;}");
    connect(sendButton,&QPushButton::clicked,this,&PacketDialog::send);
    connect(text,&QLineEdit::returnPressed,this,&PacketDialog::send);
    connect(run,&QPushButton::toggled,this,[this](bool on){run->setIcon(style()->standardIcon(on?QStyle::SP_MediaPause:QStyle::SP_MediaPlay));run->setText(on?u("Автовиконання"):u("Пауза"));refresh();});
    connect(step,&QPushButton::clicked,this,[this]{engine.step();refresh();});
    connect(reset,&QPushButton::clicked,this,&PacketDialog::restartMode);
    connect(mode,QOverload<int>::of(&QComboBox::currentIndexChanged),this,&PacketDialog::restartMode);
    connect(load,&QPushButton::clicked,this,&PacketDialog::open);connect(scenario,&QPushButton::clicked,this,[this]{save(false);});connect(result,&QPushButton::clicked,this,[this]{save(true);});
    connect(source,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{if(!busy)refresh();});
    connect(destination,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{if(!busy)refresh();});
    connect(packets,&QTableWidget::itemSelectionChanged,this,&PacketDialog::displayPayload);
    connect(encoding,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{displayPayload();});
    timer.setInterval(20);connect(&timer,&QTimer::timeout,this,[this]{engine.advanceTo(qMin(PacketEngine::MaximumTimeMs,bearer?iqClock.elapsed():engine.now()+20),512);refresh();});
    for(const auto &l:project.links)gains.insert(l.tx+"/"+l.rx,l.gainDb);
    QString error;if(!engine.configure(project,&error))errorLabel->setText(error);
    populate();refresh();
}
PacketDialog::~PacketDialog(){timer.stop();bearer.reset();}
QJsonObject PacketDialog::iqTopology() const {
    auto topology=engine.topology();auto ls=topology.value("links").toArray();
    for(int i=0;i<ls.size();++i){auto l=ls[i].toObject();l["gainDb"]=gains.value(l.value("tx").toString()+"/"+l.value("rx").toString(),-6);ls[i]=l;}
    topology["links"]=ls;return topology;
}
void PacketDialog::restartMode() {
    timer.stop();bearer.reset();const auto initial=engine.initialTopology();QString error;
    if(!engine.configure(initial,&error)){errorLabel->setText(error);return;}
    errorLabel->clear();
    if(mode->currentIndex()==1){
        bearer=std::make_unique<IqBearer>();iqClock.start();run->setChecked(true);
        connect(bearer.get(),&IqBearer::readyChanged,this,[this]{refresh();});
        connect(bearer.get(),&IqBearer::failed,this,[this](const QString &s){engine.abortIq();errorLabel->setText(s);refresh();});
        connect(bearer.get(),&IqBearer::decoded,this,[this](const QString &at,const QByteArray &wire){
            IqEnvelope e;QString problem;
            engine.advanceTo(qMin(PacketEngine::MaximumTimeMs,iqClock.elapsed()),512);
            if(IqModem::decode(wire,&e,&problem)&&e.next==at)engine.receiveIq(e.flight,e.transmitter,at,e.packet,&problem);
            if(!problem.isEmpty())errorLabel->setText(problem);refresh();
        });
        if(!bearer->start(iqTopology(),&error)||!engine.attachIqBearer([this](quint64 id,const QString &from,const QString &to,const PacketEngine::Datagram &p,QString *e){return bearer->transmit(id,from,to,p,e);},&error)){
            bearer.reset();errorLabel->setText(error);
        }
    }
    populate();refresh();
}
void PacketDialog::populate() {
    busy=true;const auto from=source->currentData().toString(),to=destination->currentData().toString();
    source->clear();destination->clear();nodes->setRowCount(0);links->setRowCount(0);packets->setRowCount(0);shown=0;payload->clear();
    const auto topology=engine.topology();
    for(const auto &v:topology.value("nodes").toArray()){
        const auto n=v.toObject();const auto id=n.value("id").toString(),name=n.value("name").toString();
        source->addItem(name+" ["+id+"]",id);destination->addItem(name+" ["+id+"]",id);
        const int row=nodes->rowCount();nodes->insertRow(row);nodes->setItem(row,0,cell(id));nodes->setItem(row,1,cell(name));
        auto box=new QCheckBox;box->setChecked(n.value("enabled").toBool());nodes->setCellWidget(row,2,box);
        for(int col=3;col<8;++col)nodes->setItem(row,col,cell("0"));
        connect(box,&QCheckBox::toggled,this,[this,id,box](bool on){if(busy)return;QString error;if(!engine.setNodeEnabled(id,on,&error)){QSignalBlocker block(box);box->setChecked(!on);errorLabel->setText(error);}else {if(bearer)bearer->updateNode(id,on);errorLabel->clear();}refresh();});
    }
    source->setCurrentIndex(qMax(0,source->findData(from)));destination->setCurrentIndex(to.isEmpty()?qMin(2,destination->count()-1):qMax(0,destination->findData(to)));
    for(const auto &v:topology.value("links").toArray()){
        const auto l=v.toObject();const auto tx=l.value("tx").toString(),rx=l.value("rx").toString();
        const int row=links->rowCount();links->insertRow(row);links->setItem(row,0,cell(tx));links->setItem(row,1,cell(rx));
        auto box=new QCheckBox;box->setChecked(l.value("enabled").toBool());links->setCellWidget(row,2,box);
        auto delay=new QSpinBox;delay->setRange(0,60000);delay->setValue(l.value("delayMs").toInt());if(bearer)delay->setMaximum(500);delay->setKeyboardTracking(false);links->setCellWidget(row,3,delay);
        connect(box,&QCheckBox::toggled,this,[this,tx,rx,box,delay]{updateLink(tx,rx,box,delay);});
        connect(delay,QOverload<int>::of(&QSpinBox::valueChanged),this,[this,tx,rx,box,delay]{updateLink(tx,rx,box,delay);});
    }
    busy=false;
}
void PacketDialog::updateLink(const QString &tx,const QString &rx,QCheckBox *box,QSpinBox *delay) {
    if(busy)return;const auto previous=engine.topology().value("links").toArray();QString error;
    if(!engine.setLink(tx,rx,box->isChecked(),delay->value(),&error)){
        for(const auto &v:previous){const auto l=v.toObject();if(l.value("tx")==tx&&l.value("rx")==rx){QSignalBlocker b(box),d(delay);box->setChecked(l.value("enabled").toBool());delay->setValue(l.value("delayMs").toInt());}}
        errorLabel->setText(error);
    } else {if(bearer)bearer->updateLink(tx,rx,box->isChecked(),delay->value(),gains.value(tx+"/"+rx,-6));errorLabel->clear();}refresh();
}
void PacketDialog::send() {
    if(mode->currentIndex()==1&&(!bearer||!bearer->ready()||engine.pending()))return;
    if(bearer)engine.advanceTo(qMin(PacketEngine::MaximumTimeMs,iqClock.elapsed()),512);
    QString error;const auto id=engine.send(source->currentData().toString(),destination->currentData().toString(),text->text().toUtf8(),ttl->value(),&error);
    errorLabel->setText(id.isEmpty()?error:QString());refresh();
}
void PacketDialog::refresh() {
    const auto state=engine.snapshot(false);
    metrics->setText(QString::fromUtf8("%1 ms · Черга %2 · Створено %3 · Доставлено %4 · Переслано %5 · Втрачено %6 · Середня затримка %7 ms")
        .arg(engine.now()).arg(engine.pending()).arg(state.value("submitted").toString()).arg(state.value("delivered").toString())
        .arg(state.value("forwarded").toString()).arg(state.value("dropped").toString()).arg(state.value("meanDeliveryMs").toDouble(),0,'f',1));
    const auto route=engine.route(source->currentData().toString(),destination->currentData().toString());
    routeLabel->setText(u("Маршрут min-hop: ")+(route.isEmpty()?u("немає"):route.mid(0,8).join(" -> ")+(route.size()>8?" ...":"")));
    routeLabel->setToolTip(route.join(" -> "));
    if(mode->currentIndex()==1){
        const auto stats=bearer?bearer->statistics():QJsonObject();
        routeLabel->setText(routeLabel->text()+QString::fromUtf8(" · IQ 48 kS/s · %1 · TX %2 / RX %3 · ≤256 B").arg(bearer&&bearer->ready()?u("готовий"):u("не готовий")).arg(stats.value("txBursts").toString("0"),stats.value("decoded").toString("0")));
    }
    for(const auto &v:engine.results(shown)){
        const auto r=v.toObject();const int row=packets->rowCount();packets->insertRow(row);
        const QStringList values{r.value("id").toString(),r.value("source").toString(),r.value("destination").toString(),statusText(r.value("status").toString()),
            QString::number(r.value("latencyMs").toDouble(),'f',0),QString::number(r.value("hops").toInt()),pathText(r.value("path").toArray())};
        for(int col=0;col<values.size();++col)packets->setItem(row,col,cell(values[col]));
        packets->item(row,0)->setData(Qt::UserRole,r);shown=r.value("serial").toString().toULongLong();
        if(packets->rowCount()>256)packets->removeRow(0);
    }
    const auto stats=state.value("nodes").toArray();
    for(int row=0;row<nodes->rowCount()&&row<stats.size();++row){const auto n=stats[row].toObject();if(n.value("id")==nodes->item(row,0)->text()){
        const QStringList fields{QString::number(n.value("queued").toInt()),n.value("received").toString(),n.value("forwarded").toString(),n.value("delivered").toString(),n.value("dropped").toString()};
        for(int i=0;i<fields.size();++i)nodes->item(row,i+3)->setText(fields[i]);
    }}
    const bool iq=mode->currentIndex()==1;
    mode->setEnabled(!engine.pending());run->setEnabled(!iq);step->setEnabled(!iq&&!run->isChecked()&&engine.pending()>0);
    sendButton->setEnabled(!iq||(bearer&&bearer->ready()&&!engine.pending()));
    nodes->setEnabled(!iq||!engine.pending());links->setEnabled(!iq||!engine.pending());
    loadButton->setEnabled(!iq);scenarioButton->setEnabled(!iq);
    if(run->isChecked()&&engine.pending()>0){if(!timer.isActive())timer.start();}else timer.stop();
}
void PacketDialog::displayPayload() {
    const int row=packets->currentRow();if(row<0||!packets->item(row,0)){payload->clear();return;}
    const auto r=packets->item(row,0)->data(Qt::UserRole).toJsonObject();
    const auto bytes=QByteArray::fromBase64(r.value("payloadBase64").toString().toLatin1());
    payload->setPlainText(encoding->currentIndex()==0?QString::fromUtf8(bytes):QString::fromLatin1(bytes.toHex(' ')));
}
void PacketDialog::save(bool results) {
    if(bearer&&!results)return;
    if(!bearer)run->setChecked(false);const auto scenario=results?QJsonObject():engine.scenario();refresh();
    const auto path=QFileDialog::getSaveFileName(this,results?u("Результати пакетної симуляції"):u("Пакетний сценарій"),results?"packet-results.json":"packet-scenario.json","JSON (*.json)");
    if(path.isEmpty())return;QString error;
    auto output=results?engine.snapshot():scenario;
    if(bearer){output["iqBearer"]=bearer->statistics();output["iqTopology"]=iqTopology();}
    if(!saveJson(path,output,&error))errorLabel->setText(error);
}
void PacketDialog::open() {
    run->setChecked(false);const auto path=QFileDialog::getOpenFileName(this,u("Пакетний сценарій"),{},"JSON (*.json)");if(path.isEmpty())return;
    QJsonObject object;QString error;PacketEngine restored;
    if(!loadJson(path,&object,&error)||!PacketEngine::replay(object,&restored,&error)){errorLabel->setText(error);return;}
    engine=std::move(restored);gains.clear();errorLabel->clear();populate();refresh();
}
}
