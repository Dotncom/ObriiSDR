#include "propagationlabdialog.h"
#include "qthmapwidget.h"
#include "solarconditionswidget.h"
#include "appsettingsutils.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QTabWidget>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

class PropagationProfileWidget : public QWidget {
public:
    propagation::PathResult path;
    bool uk;
    explicit PropagationProfileWidget(bool language, QWidget *parent) : QWidget(parent), uk(language) {
        setMinimumSize(360, 240);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.fillRect(rect(), QColor(20,23,25));
        const QRectF plot = QRectF(rect()).adjusted(64, 38, -18, -46);
        double low = 1e100, high = -1e100;
        for (const auto &v : path.points) {
            if (std::isfinite(v.terrainM)) {low = (std::min)(low,v.terrainM);high=(std::max)(high,v.terrainM);}
            if (std::isfinite(v.rayM)) {low=(std::min)(low,v.rayM-v.fresnelM);high=(std::max)(high,v.rayM+v.fresnelM);}
            if (std::isfinite(v.adjustedTerrainM)) high=(std::max)(high,v.adjustedTerrainM);
        }
        if (high < low || path.distanceM <= 0) {
            p.setPen(Qt::white);p.drawText(plot,Qt::AlignCenter,uk?QString::fromUtf8(u8"Немає профілю висот"):"No elevation profile");return;
        }
        const double padding=(std::max)(10.0,(high-low)*0.1);low-=padding;high+=padding;
        auto xy=[&](double d,double h){return QPointF(plot.left()+d/path.distanceM*plot.width(),
            plot.bottom()-(h-low)/(high-low)*plot.height());};
        p.setPen(QColor(59,66,72));
        for(int i=0;i<=5;++i) {
            const double x=plot.left()+plot.width()*i/5, y=plot.bottom()-plot.height()*i/5;
            p.drawLine(QPointF(x,plot.top()),QPointF(x,plot.bottom()));
            p.drawLine(QPointF(plot.left(),y),QPointF(plot.right(),y));
            p.setPen(QColor(207,214,217));
            p.drawText(QRectF(x-35,plot.bottom()+6,70,20),Qt::AlignCenter,QString::number(path.distanceM/1000*i/5,'f',2));
            p.drawText(QRectF(0,y-10,58,20),Qt::AlignRight|Qt::AlignVCenter,QString::number(low+(high-low)*i/5,'f',0));
            p.setPen(QColor(59,66,72));
        }
        p.setPen(Qt::white);p.drawText(QRectF(64,5,plot.width(),22),Qt::AlignLeft,
            uk?QString::fromUtf8(u8"Висота, м MSL + кривизна Землі"):"Elevation, m MSL + Earth curvature");
        p.drawText(QRectF(64,height()-24,plot.width(),20),Qt::AlignCenter,uk?QString::fromUtf8(u8"Відстань, км"):"Distance, km");
        p.save();p.setClipRect(plot);
        // Fill each contiguous terrain run once, avoiding seams between sample polygons.
        QPainterPath terrainFill;
        bool run=false;double lastDistance=0;
        for(const auto &v:path.points) {
            const double h=std::isfinite(v.adjustedTerrainM)?v.adjustedTerrainM:v.terrainM;
            if(!std::isfinite(h)) {
                if(run){terrainFill.lineTo(xy(lastDistance,low));terrainFill.closeSubpath();run=false;}
                continue;
            }
            if(!run){terrainFill.moveTo(xy(v.distanceM,low));run=true;}
            terrainFill.lineTo(xy(v.distanceM,h));lastDistance=v.distanceM;
        }
        if(run){terrainFill.lineTo(xy(lastDistance,low));terrainFill.closeSubpath();}
        p.fillPath(terrainFill,QColor(75,109,79));
        for(int i=1;i<path.points.size();++i) {
            const auto &a=path.points[i-1], &b=path.points[i];
            const double ha=std::isfinite(a.adjustedTerrainM)?a.adjustedTerrainM:a.terrainM;
            const double hb=std::isfinite(b.adjustedTerrainM)?b.adjustedTerrainM:b.terrainM;
            if(std::isfinite(ha)&&std::isfinite(hb)) {
                p.setPen(QPen(QColor(126,179,129),1.5));p.drawLine(xy(a.distanceM,ha),xy(b.distanceM,hb));
            }
            if(!std::isfinite(a.rayM)||!std::isfinite(b.rayM)) continue;
            p.setPen(QPen(QColor(246,223,117),1.5,Qt::DashLine));p.drawLine(xy(a.distanceM,a.rayM),xy(b.distanceM,b.rayM));
            for(double fraction:{-1.0,-0.6,0.6,1.0}) {
                p.setPen(QPen(std::abs(fraction)<1?QColor(110,217,227):QColor(95,126,164),1,Qt::DotLine));
                p.drawLine(xy(a.distanceM,a.rayM+fraction*a.fresnelM),xy(b.distanceM,b.rayM+fraction*b.fresnelM));
            }
        }
        if(!path.points.isEmpty()) for(const auto &v:{path.points.front(),path.points.back()}) {
            if(std::isfinite(v.rayM)&&std::isfinite(v.terrainM)) {p.setPen(QPen(Qt::white,3));p.drawLine(xy(v.distanceM,v.terrainM),xy(v.distanceM,v.rayM));}
        }
        p.restore();
    }
};

QString PropagationLabDialog::text(const char *en,const char *ua) const {return QString::fromUtf8(uk?ua:en);}
PropagationLabDialog::PropagationLabDialog(bool ukrainian,QWidget *parent,const QString &settingsFile)
    : QDialog(parent),uk(ukrainian),
      settingsPath(settingsFile.isEmpty()?persistentSettingsFilePath():settingsFile) {
    setWindowTitle(text("Radio propagation lab",u8"Лабораторія радіопоширення"));
    setAttribute(Qt::WA_DeleteOnClose,false);resize(1100,740);setMinimumSize(740,520);
    parameters.rx={0.04,0.04};
    auto *root=new QVBoxLayout(this);
    auto *toolbar=new QHBoxLayout;
    picking=new QComboBox(this);picking->setObjectName("propagationMarkerClass");
    picking->addItems({text("Pan",u8"Рух мапи"),text("Transmitter (TX)",u8"Передавач (TX)"),text("Receiver (RX)",u8"Приймач (RX)")});
    picking->setToolTip(text("Ctrl+click: TX; Shift+click: RX in any mode. Left-drag an existing marker to move it. Drag empty space to pan in Pan mode. Right-click a marker to remove it.",u8"Ctrl+клік: TX; Shift+клік: RX у будь-якому режимі. Перетягніть маркер лівою кнопкою, щоб перемістити його. У режимі руху перетягування порожнього місця рухає мапу. Правий клік по маркеру видаляє його."));
    reference=new QComboBox(this);reference->setObjectName("propagationQthMarker");reference->setMinimumContentsLength(10);reference->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    reference->addItem(text("No QTH markers",u8"Немає QTH-маркерів"),-1);
    reference->setToolTip(text("Saved QTH marker to copy into a propagation endpoint.",u8"Збережений QTH-маркер для перенесення у точку розрахунку радіопоширення."));
    useTxButton=new QPushButton(text("QTH to TX",u8"QTH → TX"),this);useTxButton->setObjectName("useQthMarkerTx");
    useRxButton=new QPushButton(text("QTH to RX",u8"QTH → RX"),this);useRxButton->setObjectName("useQthMarkerRx");
    useTxButton->setToolTip(text("Copy the selected QTH marker coordinates to TX and center the map there. Requires a saved QTH marker.",u8"Скопіювати координати вибраного QTH-маркера до TX і показати точку на мапі. Потрібен збережений QTH-маркер."));
    useRxButton->setToolTip(text("Copy the selected QTH marker coordinates to RX and center the map there. Requires a saved QTH marker.",u8"Скопіювати координати вибраного QTH-маркера до RX і показати точку на мапі. Потрібен збережений QTH-маркер."));
    auto *center=new QPushButton(text("Center TX",u8"Центр TX"),this);
    swapButton=new QPushButton(QString::fromUtf8(u8"TX ↔ RX"),this);swapButton->setObjectName("swapPropagationEndpoints");
    swapButton->setToolTip(text("Swap points, antenna heights, gains and cable losses. TX power and RX sensitivity keep their roles.",u8"Обміняти точки, висоти антен, підсилення та втрати кабелів. Потужність TX і чутливість RX зберігають свої ролі."));
    toolbar->addWidget(picking);toolbar->addWidget(reference,1);toolbar->addWidget(useTxButton);toolbar->addWidget(useRxButton);toolbar->addWidget(swapButton);toolbar->addWidget(center);root->addLayout(toolbar);
    splitter=new QSplitter(Qt::Horizontal,this);root->addWidget(splitter,1);
    controls=new QTabWidget(splitter);controls->setMinimumWidth(270);controls->setMaximumWidth(370);
    auto makeForm=[&](const QString &title) {
        auto *scroll=new QScrollArea(controls);scroll->setWidgetResizable(true);
        auto *contents=new QWidget(scroll);auto *form=new QFormLayout(contents);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        scroll->setWidget(contents);controls->addTab(scroll,title);return form;
    };
    auto number=[&](QFormLayout *form,const QString &name,double min,double max,double value,int decimals,
                   const std::function<void(double)> &setter,const QString &unit=QString(),const char *key="") {
        auto *spin=new QDoubleSpinBox(this);spin->setRange(min,max);spin->setDecimals(decimals);spin->setValue(value);
        spin->setObjectName(QString::fromLatin1(key));settingsControls.append(spin);
        spin->setSuffix(unit);spin->setKeyboardTracking(false);form->addRow(name,spin);
        connect(spin,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this,setter](double v){setter(v);invalidate();});return spin;
    };
    auto bind=[&](const char *key,QFormLayout *form,const QString &name,double min,double max,double propagation::Parameters::*member,int decimals,const QString &unit=QString()) {
        auto *spin=number(form,name,min,max,parameters.*member,decimals,[this,member](double v){parameters.*member=v;},unit,key);
        parameterSpins.append(qMakePair(spin,member));return spin;
    };
    auto *link=makeForm(text("Link",u8"Лінія"));
    txLat=number(link,text("TX latitude",u8"TX широта"),-80,80,parameters.tx.latitude,6,[this](double v){parameters.tx.latitude=v;txPresent=true;},QString(),"txLatitude");
    txLon=number(link,text("TX longitude",u8"TX довгота"),-180,180,parameters.tx.longitude,6,[this](double v){parameters.tx.longitude=v;txPresent=true;},QString(),"txLongitude");
    rxLat=number(link,text("RX latitude",u8"RX широта"),-80,80,parameters.rx.latitude,6,[this](double v){parameters.rx.latitude=v;rxPresent=true;},QString(),"rxLatitude");
    rxLon=number(link,text("RX longitude",u8"RX довгота"),-180,180,parameters.rx.longitude,6,[this](double v){parameters.rx.longitude=v;rxPresent=true;},QString(),"rxLongitude");
    bind("frequencyMHz",link,text("Frequency",u8"Частота"),0.1,100000,&propagation::Parameters::frequencyMHz,4," MHz");
    bind("powerW",link,text("TX power",u8"Потужність TX"),0.000001,1000000,&propagation::Parameters::powerW,6," W");
    bind("txHeightM",link,text("TX height AGL",u8"Висота TX над землею"),0.5,3000,&propagation::Parameters::txHeightM,1," m");
    bind("rxHeightM",link,text("RX height AGL",u8"Висота RX над землею"),0.5,3000,&propagation::Parameters::rxHeightM,1," m");
    txElevationLabel=new QLabel(this);txElevationLabel->setObjectName("txElevation");
    rxElevationLabel=new QLabel(this);rxElevationLabel->setObjectName("rxElevation");
    for(auto *label:{txElevationLabel,rxElevationLabel}){
        label->setTextFormat(Qt::PlainText);label->setWordWrap(true);
        label->setToolTip(text("Terrain elevation / antenna top elevation above mean sea level, from local HGT data. Flat reference assumes 0 m terrain.",u8"Висота рельєфу / верху антени над рівнем моря, з локальних HGT. Рівна поверхня умовно має висоту 0 м."));
    }
    link->addRow(text("TX elevation MSL",u8"Висота TX MSL"),txElevationLabel);
    link->addRow(text("RX elevation MSL",u8"Висота RX MSL"),rxElevationLabel);
    bind("txGainDbi",link,text("TX antenna gain",u8"Підсилення антени TX"),-50,100,&propagation::Parameters::txGainDbi,1," dBi");
    bind("rxGainDbi",link,text("RX antenna gain",u8"Підсилення антени RX"),-50,100,&propagation::Parameters::rxGainDbi,1," dBi");
    bind("txLossDb",link,text("TX cable loss",u8"Втрати кабелю TX"),0,100,&propagation::Parameters::txLossDb,1," dB");
    bind("rxLossDb",link,text("RX cable loss",u8"Втрати кабелю RX"),0,100,&propagation::Parameters::rxLossDb,1," dB");
    bind("additionalLossDb",link,text("Additional loss",u8"Додаткові втрати"),0,200,&propagation::Parameters::additionalLossDb,1," dB");
    bind("sensitivityDbm",link,text("RX sensitivity",u8"Чутливість RX"),-200,0,&propagation::Parameters::sensitivityDbm,1," dBm");
    bind("fadeMarginDb",link,text("Fade reserve",u8"Запас на завмирання"),0,100,&propagation::Parameters::fadeMarginDb,1," dB");
    auto *environment=makeForm(text("Terrain / model",u8"Рельєф / модель"));
    auto *model=new QComboBox(this);model->addItems({text("Terrain + dominant knife edge",u8"Рельєф + головна перешкода"),"ITM / Longley-Rice",text("Free space (reference)",u8"Вільний простір (еталон)")});
    environment->addRow(text("Model",u8"Модель"),model);
    connect(model,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){parameters.model=propagation::Model(i);invalidate();});
    auto *terrain=new QComboBox(this);terrain->addItems({"SRTM HGT",text("Flat reference (0 m)",u8"Рівна поверхня (0 м)")});environment->addRow(text("Elevation",u8"Висоти"),terrain);
    directory=new QLineEdit(this);directory->setObjectName("terrainDirectory");directory->setPlaceholderText(text("Folder with *.hgt",u8"Папка з *.hgt"));
    auto *browse=new QPushButton("...",this);browse->setMaximumWidth(36);auto *folder=new QHBoxLayout;folder->addWidget(directory);folder->addWidget(browse);
    environment->addRow(text("HGT folder",u8"Папка HGT"),folder);
    connect(terrain,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){parameters.flatTerrain=i==1;invalidate();});
    connect(directory,&QLineEdit::textChanged,this,[this](const QString &s){parameters.terrainDirectory=s;invalidate();});
    connect(browse,&QPushButton::clicked,this,[this](){const auto folder=QFileDialog::getExistingDirectory(this,text("SRTM HGT folder",u8"Папка SRTM HGT"),directory->text());if(!folder.isEmpty())directory->setText(folder);});
    auto *guide=new QPushButton(text("SRTM data source",u8"Джерело висот SRTM"),this);environment->addRow(guide);
    connect(guide,&QPushButton::clicked,this,[](){QDesktopServices::openUrl(QUrl("https://www.earthdata.nasa.gov/data/catalog/lpcloud-srtmgl1-003"));});
    bind("radiusKm",environment,text("Coverage radius",u8"Радіус покриття"),0.1,200,&propagation::Parameters::radiusKm,1," km");
    auto *grid=new QComboBox(this);for(int n:{32,64,96,128,256,512})grid->addItem(QString::number(n)+" x "+QString::number(n),n);grid->setCurrentIndex(1);environment->addRow(text("Coverage grid",u8"Сітка покриття"),grid);
    grid->setToolTip(text("Doubling the grid side requires about four times as many path calculations. Calculation runs only by button, on a low-priority worker.",u8"Подвоєння сторони сітки потребує приблизно вчетверо більше розрахунків трас. Розрахунок запускається лише кнопкою в низькопріоритетному потоці."));
    connect(grid,qOverload<int>(&QComboBox::currentIndexChanged),this,[this,grid](int){parameters.gridSize=grid->currentData().toInt();invalidate();});
    bind("profileStepM",environment,text("Terrain sample step",u8"Крок профілю висот"),10,2000,&propagation::Parameters::profileStepM,0," m")->setToolTip(text("Path profile: up to 4096 samples. Coverage rays: up to 256 samples; actual spacing can be larger than this setting.",u8"Профіль: до 4096 точок. Промені покриття: до 256 точок; фактичний крок може бути більшим за вказаний."));
    bind("earthK",environment,text("Effective Earth k",u8"Ефективний радіус k"),0.5,3,&propagation::Parameters::earthK,3)->setToolTip(text("Geometry and knife-edge only; ITM uses its own refractivity model.",u8"Лише геометрія та knife-edge; ITM має власну модель рефракції."));
    auto *itmForm=makeForm("ITM");
    auto *climate=new QComboBox(this);climate->addItems({text("Equatorial",u8"Екваторіальний"),text("Continental subtropical",u8"Континентальний субтропічний"),text("Maritime subtropical",u8"Морський субтропічний"),text("Desert",u8"Пустельний"),text("Continental temperate",u8"Континентальний помірний"),text("Maritime temperate, land",u8"Морський помірний, суходіл"),text("Maritime temperate, sea",u8"Морський помірний, море")});climate->setCurrentIndex(4);itmForm->addRow(text("Climate",u8"Клімат"),climate);
    connect(climate,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){parameters.climate=i+1;invalidate();});
    auto *pol=new QComboBox(this);pol->addItems({text("Horizontal",u8"Горизонтальна"),text("Vertical",u8"Вертикальна")});pol->setCurrentIndex(1);itmForm->addRow(text("Polarization",u8"Поляризація"),pol);
    connect(pol,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){parameters.polarization=i;invalidate();});
    bind("refractivity",itmForm,"N0",250,400,&propagation::Parameters::refractivity,1);
    bind("permittivity",itmForm,text("Permittivity",u8"Діелектрична проникність"),1.01,100,&propagation::Parameters::permittivity,2);
    bind("conductivity",itmForm,text("Conductivity",u8"Провідність"),0.000001,10,&propagation::Parameters::conductivity,6," S/m");
    bind("confidencePercent",itmForm,text("Confidence",u8"Довірча ймовірність"),1,99,&propagation::Parameters::confidencePercent,0," %");
    bind("reliabilityPercent",itmForm,text("Reliability",u8"Надійність"),1,99,&propagation::Parameters::reliabilityPercent,0," %");
    auto *itmNote=new QLabel(text("ITM: 20 MHz-20 GHz. Below 1 km: explicit knife-edge fallback. Statistical estimate, not a measured coverage guarantee.",u8"ITM: 20 МГц–20 ГГц. До 1 км: наближена модель перешкоди. Статистична оцінка, не вимірювання чи гарантія зв'язку."),this);itmNote->setWordWrap(true);itmForm->addRow(itmNote);
    auto *colorForm=makeForm(text("Overlay",u8"Оверлей"));
    const auto colorRange=[this,colorForm](const char *key,const QString &label,double value,
                                         double minimum,double maximum,const char *unit){
        auto *spin=new QDoubleSpinBox(this);spin->setObjectName(QString::fromLatin1(key));
        spin->setRange(minimum,maximum);spin->setDecimals(1);spin->setValue(value);
        spin->setSuffix(QString::fromLatin1(unit));colorForm->addRow(label,spin);settingsControls.append(spin);
        return spin;
    };
    rxColorMinimum=colorRange("rxColorMinimum",text("RX color minimum",u8"Мін. кольору RX"),-150,-240,99," dBm");
    rxColorMaximum=colorRange("rxColorMaximum",text("RX color maximum",u8"Макс. кольору RX"),-20,-239,100," dBm");
    marginColorMinimum=colorRange("marginColorMinimum",text("Margin color minimum",u8"Мін. кольору запасу"),-40,-200,199," dB");
    marginColorMaximum=colorRange("marginColorMaximum",text("Margin color maximum",u8"Макс. кольору запасу"),40,-199,200," dB");
    const auto wireRange=[this](QDoubleSpinBox *low,QDoubleSpinBox *high){
        connect(low,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this,low,high](double value){
            if(!settingsReady)return;
            if(value>=high->value()){const QSignalBlocker block(high);high->setValue(value+1);}
            updateRaster();scheduleSave();
        });
        connect(high,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this,low,high](double value){
            if(!settingsReady)return;
            if(value<=low->value()){const QSignalBlocker block(low);low->setValue(value-1);}
            updateRaster();scheduleSave();
        });
    };
    wireRange(rxColorMinimum,rxColorMaximum);wireRange(marginColorMinimum,marginColorMaximum);
    controls->addTab(new SolarConditionsWidget(uk,controls),text("HF / Sun",u8"КХ / Сонце"));
    map=new QthMapWidget(splitter);map->setUiLanguage(uk?"uk":"en");map->setPositionVisible(false);map->setMarkerCreationEnabled(false);
    map->setPositionStatusVisible(false);
    map->setOnlineDiskCacheEnabled(false);map->setMapZoomRange(3,19);map->setMapZoom(11);map->centerOn(0,0);
    splitter->setSizes({300,780});
    auto *display=new QHBoxLayout;
    tiles=new QComboBox(this);tiles->setObjectName("propagationMapSource");
    tiles->addItems({text("QTH grid",u8"Сітка QTH"),text("Local XYZ map",u8"Локальна XYZ мапа"),"OpenStreetMap",text("QTH online provider",u8"Онлайн-провайдер QTH")});
    online=new QCheckBox(text("Allow online map",u8"Дозволити онлайн-мапу"),this);
    online->setToolTip(text("Online tiles disclose the displayed geographic area to the map provider. Coordinates are not included in NOAA requests.",u8"Онлайн-тайли розкривають провайдеру видиму область мапи. Запити NOAA координат не містять."));
    layer=new QComboBox(this);layer->addItems({text("RX level",u8"Рівень RX"),text("Link margin",u8"Запас зв'язку"),text("Elevation",u8"Висоти")});
    opacity=new QSlider(Qt::Horizontal,this);opacity->setRange(10,100);opacity->setValue(65);opacity->setMaximumWidth(120);
    distanceLabel=new QLabel(this);distanceLabel->setObjectName("propagationDistance");
    distanceLabel->setTextFormat(Qt::PlainText);distanceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    distanceLabel->setToolTip(text("Approximate great-circle distance between TX and RX on Earth's surface, without terrain or antenna heights. Updates without calculating coverage.",u8"Приблизна відстань між TX і RX по поверхні Землі, без урахування рельєфу та висот антен. Оновлюється без розрахунку покриття."));
    display->addWidget(tiles);display->addWidget(online);display->addWidget(layer);display->addWidget(opacity);display->addStretch();display->addWidget(distanceLabel);root->addLayout(display);
    legend=new QLabel(this);root->addWidget(legend);
    auto *actions=new QHBoxLayout;
    coverageButton=new QPushButton(text("Calculate coverage",u8"Розрахувати покриття"),this);coverageButton->setObjectName("calculateCoverage");
    pathButton=new QPushButton(text("TX-RX profile",u8"Профіль TX–RX"),this);pathButton->setObjectName("showPathProfile");
    pathButton->setToolTip(text("Calculate and open the terrain profile. Reopen the existing result without recalculating until parameters change.",u8"Розрахувати й відкрити профіль рельєфу. Якщо параметри не змінювались, повторно відкриває готовий результат без перерахунку."));
    auto *csv=new QPushButton(text("Profile CSV",u8"Профіль CSV"),this);
    cancelButton=new QPushButton(text("Cancel",u8"Скасувати"),this);cancelButton->setEnabled(false);
    progress=new QProgressBar(this);progress->setRange(0,100);progress->setValue(0);
    actions->addWidget(coverageButton);actions->addWidget(pathButton);actions->addWidget(csv);actions->addWidget(cancelButton);actions->addWidget(progress,1);root->addLayout(actions);
    status=new QLabel(this);status->setWordWrap(true);status->setMaximumHeight(90);status->setTextFormat(Qt::PlainText);root->addWidget(status);
    auto *warning=new QLabel(text("Approximate terrestrial coverage. No building/vegetation clutter, antenna pattern, or HF ionospheric/ground-wave prediction. Missing terrain is transparent. Settings and points are saved locally; online maps require permission each session.",u8"Наближене наземне покриття. Не враховує будівлі/рослинність, діаграму антени та КХ іоносферне/поверхневе поширення. Невідомі висоти прозорі. Налаштування й точки зберігаються локально; онлайн-мапа потребує дозволу в кожному сеансі."),this);warning->setWordWrap(true);root->addWidget(warning);
    connect(tiles,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){updateMapSource();});
    connect(online,&QCheckBox::toggled,this,[this](bool){updateMapSource();});
    connect(picking,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){map->setPointPickingEnabled(i!=0);map->setMarkerCreationEnabled(i!=0);});
    connect(map,&QthMapWidget::pointPicked,this,[this](double lat,double lon){setPoint(picking->currentIndex()==1,{lat,lon});});
    connect(map,&QthMapWidget::userMarkerMoved,this,[this](double lat,double lon,int number){
        if(number==1||number==2)setPoint(number==1,{lat,lon});
    });
    connect(map,&QthMapWidget::userMarkerAdded,this,[this](double lat,double lon,const QString &,int){
        if(!worker&&picking->currentIndex()!=0)setPoint(picking->currentIndex()==1,{lat,lon});
    });
    connect(map,&QthMapWidget::userMarkerRemoved,this,[this](double,double,const QString &,int number){
        if(worker)return;
        if(number==1)txPresent=false;else if(number==2)rxPresent=false;
        invalidate();
    });
    auto useMarker=[this](bool tx){
        const int i=reference->currentData().toInt();
        if(worker||i<0||i>=referenceMarkers.size())return;
        const auto &marker=referenceMarkers[i];
        if(!std::isfinite(marker.latitude)||!std::isfinite(marker.longitude))return;
        setPoint(tx,{marker.latitude,marker.longitude});
        const auto &point=tx?parameters.tx:parameters.rx;
        map->centerOn(point.latitude,point.longitude);
        status->setText(text("QTH marker copied to ",u8"QTH-маркер перенесено до ")+(tx?"TX: ":"RX: ")+marker.name);
        scheduleSave();
    };
    connect(useTxButton,&QPushButton::clicked,this,[useMarker](){useMarker(true);});connect(useRxButton,&QPushButton::clicked,this,[useMarker](){useMarker(false);});
    connect(reference,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){updateReferenceActions();});
    connect(center,&QPushButton::clicked,this,[this](){if(txPresent){map->centerOn(parameters.tx.latitude,parameters.tx.longitude);scheduleSave();}});
    connect(swapButton,&QPushButton::clicked,this,&PropagationLabDialog::swapEndpoints);
    connect(layer,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){updateRaster();});
    connect(opacity,&QSlider::valueChanged,this,[this](int){updateRaster();});
    connect(coverageButton,&QPushButton::clicked,this,[this](){calculate(true);});
    connect(pathButton,&QPushButton::clicked,this,[this](){
        if(result.path.points.isEmpty())calculate(false);else showProfile();
    });
    connect(csv,&QPushButton::clicked,this,&PropagationLabDialog::exportProfile);
    connect(cancelButton,&QPushButton::clicked,this,[this](){cancel.store(true);});
    const auto registerChoice=[this](QComboBox *combo,const char *key){
        combo->setObjectName(QString::fromLatin1(key));settingsControls.append(combo);
        connect(combo,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){scheduleSave();});
    };
    registerChoice(model,"model");registerChoice(terrain,"terrain");registerChoice(grid,"gridSize");
    registerChoice(climate,"climate");registerChoice(pol,"polarization");
    registerChoice(picking,"propagationMarkerClass");registerChoice(tiles,"propagationMapSource");registerChoice(layer,"layer");
    opacity->setObjectName("coverageOpacity");
    connect(opacity,&QSlider::valueChanged,this,[this](int){scheduleSave();});
    connect(controls,&QTabWidget::currentChanged,this,[this](int){scheduleSave();});
    connect(splitter,&QSplitter::splitterMoved,this,[this](int,int){scheduleSave();});
    connect(map,&QthMapWidget::mapZoomChanged,this,[this](int){scheduleSave();});
    saveTimer=new QTimer(this);saveTimer->setSingleShot(true);saveTimer->setInterval(750);
    connect(saveTimer,&QTimer::timeout,this,&PropagationLabDialog::saveSettings);
    elevationTimer=new QTimer(this);elevationTimer->setSingleShot(true);elevationTimer->setInterval(150);
    connect(elevationTimer,&QTimer::timeout,this,&PropagationLabDialog::updateEndpointElevations);
    map->setToolTip(picking->toolTip());map->installEventFilter(this);installEventFilter(this);
    loadSettings();settingsReady=true;
    updateReferenceActions();
    if(rxColorMaximum->value()<=rxColorMinimum->value())rxColorMaximum->setValue(rxColorMinimum->value()+1);
    if(marginColorMaximum->value()<=marginColorMinimum->value())marginColorMaximum->setValue(marginColorMinimum->value()+1);
    invalidate();
    updateEndpointElevations();
}
PropagationLabDialog::~PropagationLabDialog(){saveSettings();cancel.store(true);if(worker){worker->wait();delete worker;}}
void PropagationLabDialog::closeEvent(QCloseEvent *event){saveSettings();cancel.store(true);if(profileDialog)profileDialog->hide();QDialog::closeEvent(event);}

void PropagationLabDialog::setPoint(bool tx,const qth::GeoPosition &point){
    if(worker||!std::isfinite(point.latitude)||!std::isfinite(point.longitude))return;
    auto *lat=tx?txLat:rxLat;auto *lon=tx?txLon:rxLon;
    const QSignalBlocker latBlock(lat),lonBlock(lon);
    lat->setValue(point.latitude);lon->setValue(point.longitude);
    (tx?parameters.tx:parameters.rx)={lat->value(),lon->value()};
    (tx?txPresent:rxPresent)=true;invalidate();
}

void PropagationLabDialog::swapEndpoints(){
    if(worker)return;
    std::swap(parameters.tx,parameters.rx);std::swap(txPresent,rxPresent);
    std::swap(parameters.txHeightM,parameters.rxHeightM);
    std::swap(parameters.txGainDbi,parameters.rxGainDbi);
    std::swap(parameters.txLossDb,parameters.rxLossDb);
    for(const auto &binding:parameterSpins){
        const QSignalBlocker block(binding.first);binding.first->setValue(parameters.*binding.second);
    }
    const QSignalBlocker a(txLat),b(txLon),c(rxLat),d(rxLon);
    txLat->setValue(parameters.tx.latitude);txLon->setValue(parameters.tx.longitude);
    rxLat->setValue(parameters.rx.latitude);rxLon->setValue(parameters.rx.longitude);
    invalidate();
}

bool PropagationLabDialog::eventFilter(QObject *watched,QEvent *event){
    if(watched==map){
        if(event->type()==QEvent::MouseButtonPress){
            auto *mouse=static_cast<QMouseEvent *>(event);
            if(mouse->button()==Qt::LeftButton&&(mouse->modifiers()&(Qt::ControlModifier|Qt::ShiftModifier))){
                qth::GeoPosition point;
                if(map->positionAt(mouse->localPos(),&point)){
                    setPoint(mouse->modifiers().testFlag(Qt::ControlModifier),point);
                    mouse->accept();return true;
                }
            }
        }
        if(event->type()==QEvent::MouseButtonRelease||event->type()==QEvent::Wheel)scheduleSave();
    }else if(watched==this&&(event->type()==QEvent::Resize||event->type()==QEvent::Move))scheduleSave();
    return QDialog::eventFilter(watched,event);
}

void PropagationLabDialog::scheduleSave(){if(settingsReady&&saveTimer)saveTimer->start();}

void PropagationLabDialog::loadSettings(){
    QSettings settings(settingsPath,QSettings::IniFormat);settings.beginGroup("propagationLab");
    for(auto *control:settingsControls){
        if(!settings.contains(control->objectName()))continue;
        const auto value=settings.value(control->objectName());
        if(auto *spin=qobject_cast<QDoubleSpinBox *>(control)){
            bool ok=false;const double number=value.toDouble(&ok);
            if(ok&&std::isfinite(number))spin->setValue(number);
        }else if(auto *combo=qobject_cast<QComboBox *>(control)){
            bool ok=false;const int index=value.toInt(&ok);
            if(ok&&index>=0&&index<combo->count())combo->setCurrentIndex(index);
        }
    }
    txPresent=settings.value("txPresent",true).toBool();rxPresent=settings.value("rxPresent",true).toBool();
    directory->setText(settings.value("terrainDirectory").toString());
    opacity->setValue(settings.value("opacity",65).toInt());
    controls->setCurrentIndex((std::clamp)(settings.value("tab",0).toInt(),0,controls->count()-1));
    map->setMapZoom(settings.value("mapZoom",11).toInt());
    bool latOk=false,lonOk=false;
    const double lat=settings.value("mapLatitude",0.0).toDouble(&latOk);
    const double lon=settings.value("mapLongitude",0.0).toDouble(&lonOk);
    if(latOk&&lonOk&&std::isfinite(lat)&&std::isfinite(lon))map->centerOn(lat,lon);
    const auto geometry=settings.value("geometry").toByteArray();if(!geometry.isEmpty())restoreGeometry(geometry);
    const auto layout=settings.value("splitter").toByteArray();if(!layout.isEmpty())splitter->restoreState(layout);
    mapSourceRestored=settings.contains("propagationMapSource");
}

void PropagationLabDialog::saveSettings(){
    if(!settingsReady)return;
    saveTimer->stop();
    QSettings settings(settingsPath,QSettings::IniFormat);settings.beginGroup("propagationLab");
    for(auto *control:settingsControls){
        if(auto *spin=qobject_cast<QDoubleSpinBox *>(control))settings.setValue(control->objectName(),spin->value());
        else if(auto *combo=qobject_cast<QComboBox *>(control))settings.setValue(control->objectName(),combo->currentIndex());
    }
    settings.setValue("txPresent",txPresent);settings.setValue("rxPresent",rxPresent);
    settings.setValue("terrainDirectory",directory->text());settings.setValue("opacity",opacity->value());
    settings.setValue("tab",controls->currentIndex());settings.setValue("geometry",saveGeometry());settings.setValue("splitter",splitter->saveState());
    const auto center=map->mapCenter();settings.setValue("mapLatitude",center.latitude);settings.setValue("mapLongitude",center.longitude);
    settings.setValue("mapZoom",map->mapZoomLevel());settings.endGroup();settings.sync();
    if(settings.status()!=QSettings::NoError)status->setText(text("Could not save propagation settings locally.",u8"Не вдалося локально зберегти налаштування радіопоширення."));
}
void PropagationLabDialog::setReferenceMarkers(const QVector<qth::UserMarker> &markers,const QString &tileDirectory) {
    if(worker) return;
    referenceMarkers=markers;reference->clear();
    for(int i=0;i<markers.size();++i)reference->addItem(markers[i].name,i);
    if(markers.isEmpty())reference->addItem(text("No QTH markers",u8"Немає QTH-маркерів"),-1);
    updateReferenceActions();
    map->setTileDirectory(tileDirectory);
}
void PropagationLabDialog::updateReferenceActions(){
    const int index=reference->currentData().toInt();
    const bool valid=reference->currentIndex()>=0&&reference->currentData().isValid()&&
        index>=0&&index<referenceMarkers.size()&&
        std::isfinite(referenceMarkers[index].latitude)&&std::isfinite(referenceMarkers[index].longitude);
    reference->setEnabled(!worker&&!referenceMarkers.isEmpty());
    useTxButton->setEnabled(!worker&&valid);useRxButton->setEnabled(!worker&&valid);
}
void PropagationLabDialog::updateMarkers(){
    if(!map) return;
    QVector<qth::UserMarker> markers;
    if(txPresent)markers.append({1,"TX",text("Transmitter",u8"Передавач"),parameters.tx.latitude,parameters.tx.longitude});
    if(rxPresent)markers.append({2,"RX",text("Receiver",u8"Приймач"),parameters.rx.latitude,parameters.rx.longitude});
    map->setUserMarkers(markers);
    if(txPresent&&rxPresent)map->setPathLine(parameters.tx,parameters.rx);else map->clearPathLine();
    if(distanceLabel){
        const double meters=txPresent&&rxPresent?propagation::distanceM(parameters.tx,parameters.rx):propagation::Unknown;
        const QString value=!std::isfinite(meters)?QStringLiteral("--"):
            meters>=1000.0?QString::number(meters/1000.0,'f',3)+text(" km",u8" км"):
                           QString::number(meters,'f',1)+text(" m",u8" м");
        distanceLabel->setText(text("Distance TX-RX: ",u8"Відстань TX–RX: ")+value);
    }
}
void PropagationLabDialog::updateEndpointElevations(){
    if(!map)return;
    propagation::SrtmTerrain terrain(parameters.terrainDirectory);
    QHash<int,QString> details;
    const auto update=[&](bool present,const qth::GeoPosition &point,double antennaHeight,QLabel *label,int id){
        const double ground=!present?propagation::Unknown:
            parameters.flatTerrain?0.0:terrain.elevation(point.latitude,point.longitude);
        const QString height=std::isfinite(ground)?
            QStringLiteral("%1 / %2 m MSL").arg(ground,0,'f',1).arg(ground+antennaHeight,0,'f',1):
            QStringLiteral("-- / -- m MSL");
        label->setText(height);
        if(present)details.insert(id,height+(parameters.flatTerrain?text(" (reference)",u8" (еталон)"):QString()));
    };
    update(txPresent,parameters.tx,parameters.txHeightM,txElevationLabel,1);
    update(rxPresent,parameters.rx,parameters.rxHeightM,rxElevationLabel,2);
    map->setUserMarkerDetailLabels(details);
}
void PropagationLabDialog::setOnlineMapSource(const QString &providerName,const QString &resolvedTemplate,
                                             const QString &attribution,int layerMode){
    savedTileTemplate=resolvedTemplate;savedAttribution=attribution;
    tiles->setItemText(3,text("QTH: ",u8"QTH: ")+providerName);
    if(!mapProviderInitialized&&!mapSourceRestored)tiles->setCurrentIndex(layerMode==2?3:layerMode==1?1:0);
    mapProviderInitialized=true;
    updateMapSource();
}
void PropagationLabDialog::updateMapSource(){
    const int mode=tiles->currentIndex();
    map->setOnlineTileTemplate(mode==3?savedTileTemplate:QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"));
    map->setOnlineAttribution(mode==3?savedAttribution:QString::fromUtf8(u8"© OpenStreetMap contributors"));
    map->setTileLayerMode(mode>=2&&online->isChecked()?QthMapWidget::TileLayerMode::OnlineXyz:
                         mode==1?QthMapWidget::TileLayerMode::LocalXyz:QthMapWidget::TileLayerMode::GridOnly);
}
void PropagationLabDialog::invalidate(){
    cancel.store(true);result={};updateMarkers();
    if(elevationTimer){
        map->setUserMarkerDetailLabels({});txElevationLabel->setText("-- / -- m MSL");rxElevationLabel->setText("-- / -- m MSL");
        elevationTimer->start();
    }
    if(map)map->setRasterOverlay({}, {}, 0);
    if(status)status->setText(text("Parameters changed; calculate to update the estimate.",u8"Параметри змінено; запустіть розрахунок для оновлення оцінки."));
    if(profile){profile->path={};profile->update();profileStatus->clear();}
    if(legend)legend->clear();
    scheduleSave();
}
void PropagationLabDialog::calculate(bool coverage){
    if(worker) return;
    if(!txPresent||(!coverage&&!rxPresent)){
        status->setText(text("Place TX for coverage, and both TX/RX for a path calculation.",u8"Для покриття встановіть TX; для розрахунку траси потрібні TX і RX."));return;
    }
    const QString error=propagation::validate(parameters);
    if(!error.isEmpty()){status->setText(error);return;}
    cancel.store(false);controls->setEnabled(false);picking->setEnabled(false);reference->setEnabled(false);swapButton->setEnabled(false);
    coverageButton->setEnabled(false);pathButton->setEnabled(false);cancelButton->setEnabled(true);map->setPointPickingEnabled(false);
    map->setMarkerEditingEnabled(false);
    status->setText(text("Calculating...",u8"Розрахунок..."));progress->setValue(0);
    const auto snapshot=parameters;
    worker=QThread::create([this,snapshot,coverage](){
        propagation::CoverageResult calculated;
        if(coverage)calculated=propagation::calculateCoverage(snapshot,&cancel,[this](int percent){QMetaObject::invokeMethod(progress,[this,percent](){progress->setValue(percent);},Qt::QueuedConnection);});
        else {
            propagation::SrtmTerrain dem(snapshot.terrainDirectory);calculated.parameters=snapshot;
            calculated.path=propagation::calculatePath(snapshot,[&](double lat,double lon){return dem.elevation(lat,lon);},&cancel);
            calculated.missingTiles=dem.unavailableTiles();calculated.cancelled=calculated.path.cancelled;
        }
        QMetaObject::invokeMethod(this,[this,calculated,coverage](){
            if(cancel.load()||calculated.cancelled){status->setText(text("Cancelled",u8"Скасовано"));return;}
            if(coverage)result=calculated;else result.path=calculated.path;
            QString info;
            if(coverage)info=text("Valid cells: ",u8"Валідних комірок: ")+QString::number(calculated.validCells)+
                text("; unknown: ",u8"; невідомих: ")+QString::number(calculated.unknownCells)+
                text("; ITM warnings: ",u8"; попереджень ITM: ")+QString::number(calculated.warningCells)+
                text("; <1 km fallback: ",u8"; наближення <1 км: ")+QString::number(calculated.shortPathCells)+". "+calculated.error;
            else info=calculated.path.valid?text("TX-RX path calculated",u8"Трасу TX–RX розраховано"):calculated.path.error;
            if(!calculated.missingTiles.isEmpty()) info+="\n"+text("Missing/void HGT: ",u8"Відсутні/невідомі HGT: ")+calculated.missingTiles.join(", ");
            status->setText(info);status->setToolTip(info);progress->setValue(100);updateRaster();
            if(!coverage|| (profileDialog&&profileDialog->isVisible()))showProfile();
        },Qt::QueuedConnection);
    });
    updateReferenceActions();
    connect(worker,&QThread::finished,this,[this](){auto *done=worker;worker=nullptr;done->deleteLater();controls->setEnabled(true);picking->setEnabled(true);updateReferenceActions();swapButton->setEnabled(true);coverageButton->setEnabled(true);pathButton->setEnabled(true);cancelButton->setEnabled(false);map->setPointPickingEnabled(picking->currentIndex()!=0);map->setMarkerEditingEnabled(true);});
    worker->start(QThread::LowPriority);
}
void PropagationLabDialog::updateRaster(){
    if(result.cells.isEmpty())return;
    const bool margin=layer->currentIndex()==1;
    const double low=(margin?marginColorMinimum:rxColorMinimum)->value();
    const double high=(margin?marginColorMaximum:rxColorMaximum)->value();
    map->setRasterOverlay(propagation::coverageImage(result,layer->currentIndex(),low,high),result.bounds,opacity->value()/100.0);
    QString scale=layer->currentIndex()==2?text("Terrain: auto-scaled MSL",u8"Висоти: автомасштаб MSL"):
        QString("%1 .. %2 %3").arg(low,0,'f',1).arg(high,0,'f',1).arg(margin?"dB":"dBm");
    legend->setText(text("Violet → blue → cyan → green → yellow → orange → red → light: ",u8"Фіолетовий → синій → блакитний → зелений → жовтий → помаранчевий → червоний → світлий: ")+scale);
    legend->setWordWrap(true);
}
void PropagationLabDialog::showProfile(){
    if(!profileDialog){profileDialog=new QDialog(this);profileDialog->setWindowTitle(text("TX-RX terrain profile",u8"Профіль рельєфу TX–RX"));profileDialog->resize(900,540);
        auto *layout=new QVBoxLayout(profileDialog);profile=new PropagationProfileWidget(uk,profileDialog);layout->addWidget(profile,1);
        auto *key=new QLabel(text("Green: terrain + curvature; yellow: line of sight; cyan: 60% first Fresnel zone; blue: full first Fresnel zone.",u8"Зелений: рельєф + кривизна; жовтий: пряма видимість; блакитний: 60% першої зони Френеля; синій: повна перша зона."),profileDialog);key->setWordWrap(true);layout->addWidget(key);
        profileStatus=new QLabel(profileDialog);profileStatus->setWordWrap(true);profileStatus->setTextFormat(Qt::PlainText);layout->addWidget(profileStatus);}
    profile->path=result.path;profile->update();const auto &r=result.path;
    auto n=[](double v){return std::isfinite(v)?QString::number(v,'f',1):QStringLiteral("--");};
    profileStatus->setText(QStringLiteral("%1 km | FSPL %2 dB | %3 %4 dB | RX %5 dBm | %6 %7 dB\n%8 %9 m | F1 %10%% | %11 %12 dB | ITM warnings 0x%13 %14\n%15")
        .arg(n(r.distanceM/1000),n(r.freeSpaceDb),text("Path loss",u8"Втрати траси"),n(r.pathLossDb),n(r.receivedDbm),text("Margin",u8"Запас"),n(r.marginDb))
        .arg(text("Min clearance",u8"Мін. просвіт"),n(r.minimumClearanceM),n(r.minimumFresnelRatio*100),text("Dominant edge estimate",u8"Оцінка головної перешкоди"),n(r.diffractionDb),QString::number(r.itmWarnings,16),r.shortPathFallback?"(<1 km)":"",r.error));
    if(isVisible()){profileDialog->show();profileDialog->raise();}
}
void PropagationLabDialog::exportProfile(){
    if(result.path.points.isEmpty()){status->setText(text("Calculate a path first",u8"Спочатку розрахуйте трасу"));return;}
    const auto file=QFileDialog::getSaveFileName(this,text("Export profile (includes coordinates)",u8"Експорт профілю (містить координати)"),"propagation-profile.csv","CSV (*.csv)");
    if(file.isEmpty())return;QSaveFile output(file);
    if(!output.open(QIODevice::WriteOnly|QIODevice::Text)){status->setText(output.errorString());return;}
    QTextStream stream(&output);stream.setCodec("UTF-8");stream.setRealNumberPrecision(12);
    stream<<"# TX="<<parameters.tx.latitude<<","<<parameters.tx.longitude<<"; RX="<<parameters.rx.latitude<<","<<parameters.rx.longitude<<"; MHz="<<parameters.frequencyMHz<<"\n";
    stream<<"distance_m,terrain_m_msl,terrain_curvature_m,los_m,fresnel1_radius_m\n";
    for(const auto &v:result.path.points)stream<<v.distanceM<<","<<v.terrainM<<","<<v.adjustedTerrainM<<","<<v.rayM<<","<<v.fresnelM<<"\n";
    stream.flush();status->setText(output.commit()?text("Profile exported",u8"Профіль експортовано"):output.errorString());
}
