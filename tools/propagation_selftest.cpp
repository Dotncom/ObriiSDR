#include "radiopropagation.h"
#include "propagationlabdialog.h"
#include "solarconditionswidget.h"
#include "ft8countrycatalog.h"
#include "qthmapwidget.h"
#include "../src/transmit/signalunits.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QThread>
#include <cmath>
#include <iostream>
#include "itm.h"

int main(int argc,char **argv){
    QApplication app(argc,argv);
    int failures=0,checks=0;
    auto check=[&](bool condition,const char *name){++checks;if(!condition){++failures;std::cerr<<"FAIL: "<<name<<"\n";}};
    const QString workspace=argc>1?QString::fromLocal8Bit(argv[1]):QDir::currentPath();
    check(signalunits::parse("48 ksps",true)==48000,"ksps input");
    check(signalunits::parse("2,4 Msps",true)==2400000,"Decimal comma Msps input");
    check(signalunits::parse("1 Gsps",true)==1e9,"Gsps input");
    check(signalunits::parse("48000",true)==48000,"Legacy bare sample rate");
    check(signalunits::parse("12.5 kHz",false)==12500&&signalunits::parse("0.8 GHz",false)==8e8,"Bandwidth engineering units");
    check(signalunits::parse(QString::fromUtf8(u8"20 МГц"),false)==2e7,"Ukrainian bandwidth units");
    check(!std::isfinite(signalunits::parse("10 Msps garbage",true))&&!std::isfinite(signalunits::parse("1 Gsps",false)),"Invalid/wrong units rejected");
    QComboBox rate;rate.setEditable(true);rate.setInsertPolicy(QComboBox::NoInsert);rate.addItem("48 ksps",48000);
    rate.setEditText("20 Msps");check(signalunits::sampleRateValue(&rate)==20000000,"Edited combo overrides stale preset data");
    rate.setEditText("3 Gsps");check(signalunits::sampleRateValue(&rate)==0,"Rate above safe integer limit rejected");
    check(signalunits::sampleRateText(1000000000)=="1 Gsps"&&signalunits::sampleRateText(2400000)=="2.4 Msps","Sample rate formatting");
    const QString output=QDir(workspace).filePath("build/propagation-tests");QDir().mkpath(output);
    QTemporaryDir terrain(output+"/dem-XXXXXX");
    QByteArray tile(1201*1201*2,0);
    auto set=[&](int row,int col,int value){const int i=(row*1201+col)*2;tile[i]=char((value>>8)&255);tile[i+1]=char(value&255);};
    for(int row=0;row<1201;++row)for(int col=0;col<1201;++col)set(row,col,row/12+col/12-50);
    auto write=[&](const QString &name){QFile f(terrain.path()+"/"+name);check(f.open(QIODevice::WriteOnly),"DEM fixture open");check(f.write(tile)==tile.size(),"DEM fixture write");};
    write("N00E000.hgt");write("S01W001.hgt");
    propagation::SrtmTerrain dem(terrain.path());
    check(std::abs(dem.elevation(0.5,0.5)-50)<1e-6,"SRTM big-endian signed orientation");
    check(std::abs(dem.elevation(0.99,0.0)+49)<1e-6,"SRTM negative height");
    check(std::abs(dem.elevation(-0.5,-0.5)-50)<1e-6,"SRTM negative tile floor");
    write("N02E000.SRTMGL1.hgt");write("N03E000.SRTMGL3.hgt");
    check(std::abs(dem.elevation(2.5,0.5)-50)<1e-6,"NASA SRTMGL1 filename");
    check(std::abs(dem.elevation(3.5,0.5)-50)<1e-6,"NASA SRTMGL3 filename");
    check(!std::isfinite(dem.elevation(3,3)),"Missing tile remains unknown");
    set(600,600,-32768);write("N01E000.hgt");
    check(!std::isfinite(dem.elevation(1.5,0.5)),"SRTM void remains unknown");
    check(std::abs(propagation::freeSpaceLossDb(1000,100)-72.447783)<0.001,"FSPL reference");
    check(std::abs(propagation::freeSpaceLossDb(2000,100)-propagation::freeSpaceLossDb(1000,100)-6.0206)<0.0001,"FSPL distance doubling");
    propagation::Parameters p;p.tx={0.5,0.5};p.rx=propagation::destination(p.tx,1.0,5000);p.powerW=1;
    p.txHeightM=30;p.rxHeightM=30;p.model=propagation::Model::FreeSpace;
    auto flat=[](double,double){return 0.0;};
    const auto path=propagation::calculatePath(p,flat);
    check(path.valid&&std::abs(path.distanceM-5000)<0.001,"Geodesic path distance");
    check(std::abs(path.receivedDbm-(30-path.pathLossDb))<1e-10,"One-watt link budget");
    auto stronger=p;stronger.powerW=4;stronger.txGainDbi=3;stronger.txLossDb=2;
    check(std::abs(propagation::calculatePath(stronger,flat).receivedDbm-path.receivedDbm-7.0206)<0.0001,"Power/gain/loss budget");
    p.model=propagation::Model::KnifeEdge;
    auto ridge=[&](double lat,double lon){const double d=propagation::distanceM(p.tx,{lat,lon});return 130*std::exp(-std::pow((d-2500)/400,2));};
    const auto obstruction=propagation::calculatePath(p,ridge);
    check(obstruction.valid&&obstruction.diffractionDb>15&&obstruction.minimumClearanceM<0,"Terrain diffraction");
    check(obstruction.pathLossDb>path.pathLossDb+15,"Obstructed loss greater than FSPL");
    const auto absent=propagation::calculatePath(p,[](double,double){return propagation::Unknown;});
    check(!absent.valid&&!absent.completeTerrain,"No implicit flat-terrain fallback");
    p.flatTerrain=true;p.gridSize=32;p.radiusKm=2;
    QElapsedTimer clock;clock.start();const auto coverage=propagation::calculateCoverage(p);
    check(coverage.validCells>400&&coverage.error.isEmpty(),"Flat-reference coverage");
    const auto image=propagation::coverageImage(coverage,1);
    check(image.size()==QSize(32,32)&&image.pixelColor(0,0).alpha()==0,"Outside-radius cells transparent");
    std::atomic_bool stop{true};check(propagation::calculateCoverage(p,&stop).cancelled,"Coverage cancellation");
    auto detailed=p;detailed.gridSize=256;detailed.radiusKm=0.1;detailed.profileStepM=2000;
    int updates=0,lastPercent=-1;bool monotonic=true;
    const auto dense=propagation::calculateCoverage(detailed,nullptr,[&](int percent){
        monotonic=monotonic&&percent>lastPercent;lastPercent=percent;++updates;
    });
    check(dense.cells.size()==256*256&&dense.validCells>40000&&dense.error.isEmpty(),"256 grid computes more independent coverage cells");
    check(monotonic&&lastPercent==100&&updates<=101,"Dense grids do not flood GUI with duplicate progress events");
    detailed.gridSize=512;
    check(propagation::validate(detailed).isEmpty()&&propagation::calculateCoverage(detailed,&stop).cancelled,"512 grid accepted and cancellable");
    detailed.gridSize=513;check(!propagation::validate(detailed).isEmpty(),"Grid allocation limit enforced");
    propagation::CoverageResult colors;colors.parameters.gridSize=16;colors.cells.resize(16*16);
    colors.cells[0].receivedDbm=-150;colors.cells[1].receivedDbm=-20;colors.cells[2].receivedDbm=-85;
    colors.cells[0].marginDb=-40;colors.cells[1].marginDb=40;
    const auto palette=propagation::coverageImage(colors,0,-150,-20);
    check(palette.pixelColor(0,0)==propagation::levelColor(0)&&palette.pixelColor(1,0)==propagation::levelColor(1),"Custom RX scale endpoints use full palette");
    check(palette.pixelColor(3,0).alpha()==0&&!std::isfinite(colors.cells[3].receivedDbm),"Unknown cells remain transparent and numerical data unchanged");
    const auto marginPalette=propagation::coverageImage(colors,1,-40,40);
    check(marginPalette.pixelColor(0,0)==palette.pixelColor(0,0)&&marginPalette.pixelColor(1,0)==palette.pixelColor(1,0),"Custom margin scale uses full palette");
    const auto narrow=propagation::coverageImage(colors,0,-100,-80);
    check(narrow.pixelColor(2,0)!=palette.pixelColor(2,0)&&colors.cells[2].receivedDbm==-85,"Recoloring changes only display, not calculated power");
    check(propagation::levelColor(-1)==propagation::levelColor(0)&&propagation::levelColor(2)==propagation::levelColor(1)&&
          propagation::levelColor(propagation::Unknown).alpha()==0,"Palette clamps finite levels and rejects unknown values");
    colors.cells[0].terrainM=42;colors.cells[1].terrainM=42;
    const auto elevation=propagation::coverageImage(colors,2,-150,-20);
    check(elevation.pixelColor(0,0).alpha()==255&&elevation.pixelColor(2,0).alpha()==0,"Constant terrain auto-scales without coloring unknown cells");
    colors.parameters.gridSize=0;check(propagation::coverageImage(colors,0).isNull(),"Malformed raster dimensions rejected");
    p.flatTerrain=false;p.terrainDirectory="missing-folder";
    check(propagation::calculateCoverage(p).validCells==0,"Missing DEM cannot report coverage");
    std::cout<<"Coverage 32x32: "<<clock.elapsed()<<" ms\n";
    // Official NTIA reference profiles and TLS parameters, without geographic fixtures.
    QFile csv(workspace+"/third_party/ntia_itm/p2p.csv"), profiles(workspace+"/third_party/ntia_itm/pfls.csv");
    check(csv.open(QIODevice::ReadOnly)&&profiles.open(QIODevice::ReadOnly),"NTIA reference inputs");csv.readLine();
    while(!csv.atEnd()){
        const auto fields=csv.readLine().trimmed().split(',');if(fields.size()!=13)continue;
        const auto raw=profiles.readLine().trimmed().split(',');std::vector<double> pfl;for(const auto &v:raw)pfl.push_back(v.toDouble());
        double loss=0;long warnings=0;
        const int code=ITM_P2P_TLS(fields[0].toDouble(),fields[1].toDouble(),pfl.data(),fields[7].toInt(),fields[4].toDouble(),
            fields[5].toDouble(),fields[6].toInt(),fields[2].toDouble(),fields[3].toDouble(),fields[11].toInt(),
            fields[8].toDouble(),fields[9].toDouble(),fields[10].toDouble(),&loss,&warnings);
        check(code<=1&&std::abs(loss-fields[12].toDouble())<0.02,"NTIA published TLS vector");
    }
    solar::Observation observation;
    check(solar::parseFlux(R"([{"time_tag":"2026-10-10T20:00:00","frequency":2800,"flux":126,"ninety_day_mean":115},{"time_tag":"2026-10-10T22:00:00","frequency":2800,"flux":124}])",&observation),"NOAA flux parse");
    check(observation.flux==124&&observation.flux90==115&&observation.fluxUtc.time().hour()==22&&observation.fluxUtc.timeSpec()==Qt::UTC,"NOAA latest observation UTC");
    check(solar::parseKp(R"([{"time_tag":"2026-10-11T09:00:00","Kp":3,"a_running":15},{"time_tag":"2026-10-11T06:00:00","Kp":1}])",&observation)&&observation.kp==3&&observation.ap==15,"NOAA Kp latest interval");
    check(solar::parseKp(R"([{"time_tag":"2026-10-11T09:00:00","Kp":3,"a_running":15}])",&observation),"NOAA repeated observation succeeds");
    check(!solar::parseFlux("{}",&observation)&&!solar::parseKp("[]",&observation),"NOAA malformed/empty input");
    auto countries=defaultFt8CountryCatalog();check(countries.size()==340,"All 340 DXCC entities embedded");
    check(evaluateFt8Callsign("UR5ABC",{},countries).country=="Ukraine","DXCC Ukraine");
    check(evaluateFt8Callsign("3D2C",{},countries).country=="Conway Reef","DXCC exact callsign override");
    check(evaluateFt8Callsign("EA8/K1ABC",{},countries).country=="Canary Islands","Portable location prefix");
    check(evaluateFt8Callsign("K1ABC/EA8",{},countries).country=="Canary Islands","Portable location suffix");
    check(evaluateFt8Callsign("R0ABC",{},countries).country=="Asiatic Russia","DXCC Russia division");
    for(auto &country:countries)if(country.country=="United States")country.blocked=true;
    check(!evaluateFt8Callsign("KH6ABC",{},countries).blocked,"Broad blocked prefix must not shadow Hawaii");
    Ft8CallsignRule legacy{"Russia",{"R","UA","UB","UC","UD","UE","UF","UG","UH","UI"},true};
    const auto migrated=mergeFt8CountryCatalog({legacy,{"Custom",{"ZZ9"},true}},countries);
    check(evaluateFt8Callsign("R0ABC",{},migrated).blocked&&evaluateFt8Callsign("UA3ABC",{},migrated).blocked,"Legacy Russia block migration");
    check(evaluateFt8Callsign("ZZ9ABC",{},migrated).blocked,"Custom rule migration");
    Ft8QsoRecord qso;qso.call="UR5ABC";qso.grid="KN34";Ft8QsoRecord newer=qso;newer.grid="KN35";
    Ft8QsoRecord invalid=qso;invalid.call="K1ABC";invalid.grid="BAD";
    const auto markers=ft8LogMapMarkers({qso,newer,invalid});
    const auto bounds=qth::maidenheadBounds("KN35");
    check(markers.size()==1&&markers[0].name=="UR5ABC"&&std::abs(markers[0].latitude-(bounds.minLatitude+bounds.maxLatitude)/2)<1e-9,"FT8 map duplicate/invalid locator handling");
    QTemporaryDir settingsDirectory;
    check(settingsDirectory.isValid(),"Isolated propagation settings directory");
    {
        PropagationLabDialog elevations(true,nullptr,settingsDirectory.filePath("heights.ini"));
        auto *txHeight=elevations.findChild<QLabel*>("txElevation");
        auto *rxHeight=elevations.findChild<QLabel*>("rxElevation");
        check(txHeight->text().contains("-- / --"),"Missing HGT elevation is not silently zero");
        elevations.findChild<QLineEdit*>("terrainDirectory")->setText(terrain.path());
        elevations.findChild<QDoubleSpinBox*>("txLatitude")->setValue(0.5);
        elevations.findChild<QDoubleSpinBox*>("txLongitude")->setValue(0.5);
        elevations.findChild<QDoubleSpinBox*>("rxLatitude")->setValue(0.5);
        elevations.findChild<QDoubleSpinBox*>("rxLongitude")->setValue(0.5);
        elevations.findChild<QDoubleSpinBox*>("txHeightM")->setValue(10);
        elevations.findChild<QDoubleSpinBox*>("rxHeightM")->setValue(3);
        const auto waitHeight=[&](){QElapsedTimer timer;timer.start();while(timer.elapsed()<250){app.processEvents();QThread::msleep(5);}};
        waitHeight();
        check(txHeight->text()=="50.0 / 60.0 m MSL"&&rxHeight->text()=="50.0 / 53.0 m MSL","HGT terrain plus independent antenna heights displayed");
        elevations.setAttribute(Qt::WA_DontShowOnScreen);elevations.resize(1100,760);elevations.show();app.processEvents();
        auto *heightMap=elevations.findChild<QthMapWidget*>();heightMap->centerOn(0.5,0.5);heightMap->setMapZoom(12);
        elevations.findChild<QDoubleSpinBox*>("txLongitude")->setValue(0.49);
        elevations.findChild<QDoubleSpinBox*>("rxLongitude")->setValue(0.51);waitHeight();
        QImage heightImage(elevations.size(),QImage::Format_ARGB32);elevations.render(&heightImage);
        check(heightImage.save(output+"/endpoint-heights-uk.png"),"Two-line height-label render");
        elevations.findChild<QDoubleSpinBox*>("rxLongitude")->setValue(0.5);
        elevations.findChild<QDoubleSpinBox*>("rxLatitude")->setValue(1.5);waitHeight();
        check(rxHeight->text().contains("-- / --"),"SRTM void cannot fabricate antenna MSL height");
        elevations.findChild<QComboBox*>("terrain")->setCurrentIndex(1);waitHeight();
        check(txHeight->text()=="0.0 / 10.0 m MSL"&&rxHeight->text()=="0.0 / 3.0 m MSL","Explicit flat reference displays synthetic ground and antenna heights");
        elevations.findChild<QDoubleSpinBox*>("txHeightM")->setValue(12);waitHeight();
        check(txHeight->text()=="0.0 / 12.0 m MSL","Antenna-height edit updates labels without coverage calculation");
    }
    for(bool uk:{false,true}){
        const QString settingsFile=settingsDirectory.filePath(uk?"uk.ini":"en.ini");
        PropagationLabDialog dialog(uk,nullptr,settingsFile);dialog.setAttribute(Qt::WA_DontShowOnScreen);dialog.show();app.processEvents();
        for(auto *c:dialog.findChildren<QCheckBox*>())check(!c->isChecked(),"Network disabled by default");
        dialog.setOnlineMapSource("MapTiler test","https://example.invalid/{z}/{x}/{y}.png?key=TEST_ONLY","Test attribution",2);
        auto *source=dialog.findChild<QComboBox*>("propagationMapSource");
        check(source&&source->currentIndex()==3&&source->currentText().contains("MapTiler"),"Saved QTH provider selected without enabling network");
        auto *map=dialog.findChild<QthMapWidget*>();
        auto *markerClass=dialog.findChild<QComboBox*>("propagationMarkerClass");
        auto *qthCombo=dialog.findChild<QComboBox*>("propagationQthMarker");
        auto *useTx=dialog.findChild<QPushButton*>("useQthMarkerTx");
        auto *useRx=dialog.findChild<QPushButton*>("useQthMarkerRx");
        check(qthCombo&&!qthCombo->isEnabled()&&!useTx->isEnabled()&&!useRx->isEnabled(),"No QTH markers disables import actions");
        const QVector<qth::UserMarker> references={{10,"Test A","",1.25,2.5},{11,"Test B","",3.5,4.25}};
        dialog.setReferenceMarkers(references,QString());qthCombo->setCurrentIndex(1);
        check(useTx->isEnabled()&&useRx->isEnabled(),"QTH selection enables import actions");
        useTx->click();
        check(dialog.findChild<QDoubleSpinBox*>("txLatitude")->value()==3.5&&
              dialog.findChild<QDoubleSpinBox*>("txLongitude")->value()==4.25&&
              map->mapCenter().latitude==3.5,"QTH to TX copies selected coordinates and centers map");
        qthCombo->setCurrentIndex(0);useRx->click();
        check(dialog.findChild<QDoubleSpinBox*>("rxLatitude")->value()==1.25&&
              dialog.findChild<QDoubleSpinBox*>("rxLongitude")->value()==2.5&&
              map->mapCenter().longitude==2.5,"QTH to RX copies a different selected marker");
        dialog.setReferenceMarkers({},QString());
        check(!useTx->isEnabled()&&!useRx->isEnabled(),"Clearing QTH list disables imports again");
        dialog.setReferenceMarkers(references,QString());
        dialog.findChild<QDoubleSpinBox*>("txLatitude")->setValue(0);
        dialog.findChild<QDoubleSpinBox*>("txLongitude")->setValue(0);
        dialog.findChild<QDoubleSpinBox*>("rxLatitude")->setValue(0.04);
        dialog.findChild<QDoubleSpinBox*>("rxLongitude")->setValue(0.04);map->centerOn(0,0);
        auto click=[&](Qt::MouseButton button,const QPointF &point,Qt::KeyboardModifiers modifiers=Qt::NoModifier){
            QMouseEvent event(QEvent::MouseButtonPress,point,button,button,modifiers);QApplication::sendEvent(map,&event);
            QMouseEvent release(QEvent::MouseButtonRelease,point,button,Qt::NoButton,modifiers);QApplication::sendEvent(map,&release);
        };
        const QPointF point=QRectF(map->rect().adjusted(18,18,-18,-34)).center();
        markerClass->setCurrentIndex(1);click(Qt::LeftButton,point);
        check(map->userMarkerList().size()==2&&std::abs(map->userMarkerList()[0].latitude)<0.001,"Mouse places TX by class");
        markerClass->setCurrentIndex(0);click(Qt::RightButton,point);
        const auto afterRemoval=map->userMarkerList();
        check(afterRemoval.size()==1&&afterRemoval.front().name=="RX","Mouse removes TX and does not recreate it");
        dialog.findChild<QPushButton*>("calculateCoverage")->click();
        check(dialog.findChild<QPushButton*>("calculateCoverage")->isEnabled(),"Missing TX cannot start calculation");
        markerClass->setCurrentIndex(1);click(Qt::RightButton,point);
        check(map->userMarkerList().size()==2,"Right-click empty position restores selected marker class");
        markerClass->setCurrentIndex(2);click(Qt::LeftButton,point+QPointF(60,0));
        check(map->userMarkerList().size()==2&&map->userMarkerList()[1].longitude>0,"Mouse places RX by class");
        auto *distance=dialog.findChild<QLabel*>("propagationDistance");
        check(distance&&!distance->text().contains("--"),"Distance displayed before calculating coverage");
        dialog.findChild<QDoubleSpinBox*>("rxLatitude")->setValue(dialog.findChild<QDoubleSpinBox*>("txLatitude")->value());
        dialog.findChild<QDoubleSpinBox*>("rxLongitude")->setValue(dialog.findChild<QDoubleSpinBox*>("txLongitude")->value());
        check(distance->text().contains("0.0"),"Coincident points display zero meters");
        dialog.findChild<QDoubleSpinBox*>("rxLongitude")->setValue(dialog.findChild<QDoubleSpinBox*>("txLongitude")->value()+0.001);
        check(distance->text().contains(QString::number(propagation::distanceM({0,0},{0,0.001}),'f',1)),"Manual coordinate edits update meter distance immediately");
        markerClass->setCurrentIndex(2);click(Qt::LeftButton,point-QPointF(70,0),Qt::ControlModifier);
        check(map->userMarkerList()[0].longitude<0&&markerClass->currentIndex()==2,"Ctrl click places TX without switching mode");
        markerClass->setCurrentIndex(1);click(Qt::LeftButton,point+QPointF(70,0),Qt::ShiftModifier);
        check(map->userMarkerList()[1].longitude>0&&markerClass->currentIndex()==1,"Shift click places RX without switching mode");
        const auto beforeSwap=map->userMarkerList();
        const QString expectedKm=QString::number(propagation::distanceM({beforeSwap[0].latitude,beforeSwap[0].longitude},
            {beforeSwap[1].latitude,beforeSwap[1].longitude})/1000.0,'f',3);
        check(distance->text().contains(expectedKm),"Mouse placement updates kilometer distance");
        const QString distanceBeforeSwap=distance->text();
        dialog.findChild<QDoubleSpinBox*>("txHeightM")->setValue(22);
        dialog.findChild<QDoubleSpinBox*>("rxHeightM")->setValue(3);
        dialog.findChild<QDoubleSpinBox*>("txGainDbi")->setValue(7);
        dialog.findChild<QDoubleSpinBox*>("rxGainDbi")->setValue(2);
        dialog.findChild<QDoubleSpinBox*>("txLossDb")->setValue(4);
        dialog.findChild<QDoubleSpinBox*>("rxLossDb")->setValue(1);
        dialog.findChild<QDoubleSpinBox*>("powerW")->setValue(12);
        dialog.findChild<QPushButton*>("swapPropagationEndpoints")->click();
        check(map->userMarkerList()[0].longitude==beforeSwap[1].longitude&&map->userMarkerList()[1].longitude==beforeSwap[0].longitude,"Swap exchanges coordinates");
        check(distance->text()==distanceBeforeSwap,"Endpoint swap preserves distance");
        check(dialog.findChild<QDoubleSpinBox*>("txHeightM")->value()==3&&dialog.findChild<QDoubleSpinBox*>("rxHeightM")->value()==22&&
              dialog.findChild<QDoubleSpinBox*>("txGainDbi")->value()==2&&dialog.findChild<QDoubleSpinBox*>("rxGainDbi")->value()==7&&
              dialog.findChild<QDoubleSpinBox*>("txLossDb")->value()==1&&dialog.findChild<QDoubleSpinBox*>("rxLossDb")->value()==4,"Swap exchanges site antenna parameters");
        check(dialog.findChild<QDoubleSpinBox*>("powerW")->value()==12&&dialog.findChild<QDoubleSpinBox*>("sensitivityDbm")->value()==-110,"Swap preserves TX power and RX sensitivity roles");
        auto drag=[&](const QPointF &from,const QPointF &to){
            QMouseEvent press(QEvent::MouseButtonPress,from,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(map,&press);
            QMouseEvent move(QEvent::MouseMove,to,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(map,&move);
            QMouseEvent release(QEvent::MouseButtonRelease,to,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(map,&release);
        };
        const auto beforeDrag=map->userMarkerList();
        const auto viewBeforeDrag=map->mapCenter();
        const QString distanceBeforeDrag=distance->text();
        markerClass->setCurrentIndex(1);
        click(Qt::LeftButton,point-QPointF(70,0));
        check(map->userMarkerList()[1].longitude==beforeDrag[1].longitude,"Clicking RX in TX mode does not replace an endpoint");
        drag(point-QPointF(70,0),point+QPointF(-35,40));
        const auto afterDrag=map->userMarkerList();
        check(afterDrag.size()==2&&afterDrag[0].longitude==beforeDrag[0].longitude&&
              afterDrag[1].longitude>beforeDrag[1].longitude&&afterDrag[1].latitude<beforeDrag[1].latitude,
              "Left drag moves selected RX even in TX placement mode");
        check(map->mapCenter().latitude==viewBeforeDrag.latitude&&map->mapCenter().longitude==viewBeforeDrag.longitude,
              "Dragging a marker does not pan the map");
        check(distance->text()!=distanceBeforeDrag&&dialog.findChild<QDoubleSpinBox*>("rxLatitude")->value()==afterDrag[1].latitude,
              "Marker drag updates distance and coordinate controls");
        markerClass->setCurrentIndex(0);
        drag(point+QPointF(70,0),point+QPointF(95,-20));
        check(map->userMarkerList()[0].longitude>afterDrag[0].longitude&&map->userMarkerList()[0].latitude>afterDrag[0].latitude,
              "TX marker also draggable in Pan mode");
        const auto locked=map->userMarkerList();
        map->setMarkerEditingEnabled(false);
        drag(point+QPointF(95,-20),point+QPointF(130,-20));
        check(map->userMarkerList()[0].longitude==locked[0].longitude&&map->userMarkerList()[0].latitude==locked[0].latitude,
              "Editing lock prevents marker dragging");
        map->setMarkerEditingEnabled(true);
        dialog.findChild<QDoubleSpinBox*>("frequencyMHz")->setValue(433.92);
        dialog.findChild<QSlider*>("coverageOpacity")->setValue(83);
        auto *grid=dialog.findChild<QComboBox*>("gridSize");
        check(grid&&grid->findData(256)>=0&&grid->findData(512)>=0,"Detailed grid choices available in UI");
        grid->setCurrentIndex(grid->findData(512));
        auto *colorLow=dialog.findChild<QDoubleSpinBox*>("rxColorMinimum");
        auto *colorHigh=dialog.findChild<QDoubleSpinBox*>("rxColorMaximum");
        check(colorLow&&colorHigh&&colorLow->value()==-150&&colorHigh->value()==-20,"Wider RX color defaults");
        colorLow->setValue(-10);
        check(colorHigh->value()==-9,"Low color boundary cannot exceed high boundary");
        colorHigh->setValue(-160);
        check(colorLow->value()==-161,"High color boundary cannot fall below low boundary");
        colorLow->setValue(-145);colorHigh->setValue(-35);
        dialog.findChild<QDoubleSpinBox*>("marginColorMinimum")->setValue(-45);
        dialog.findChild<QDoubleSpinBox*>("marginColorMaximum")->setValue(55);
        markerClass->setCurrentIndex(0);
        QElapsedTimer autosave;autosave.start();
        while(!QFile::exists(settingsFile)&&autosave.elapsed()<3000){app.processEvents();QThread::msleep(10);}
        QSettings saved(settingsFile,QSettings::IniFormat);
        check(saved.value("propagationLab/frequencyMHz").toDouble()==433.92&&saved.value("propagationLab/opacity").toInt()==83,"Autosave writes settings while dialog remains open");
        {
            PropagationLabDialog restored(uk,nullptr,settingsFile);
            restored.setOnlineMapSource("Updated provider","https://example.invalid/other/{z}/{x}/{y}","Attribution",0);
            check(restored.findChild<QDoubleSpinBox*>("frequencyMHz")->value()==433.92&&restored.findChild<QDoubleSpinBox*>("txHeightM")->value()==3&&
                  restored.findChild<QSlider*>("coverageOpacity")->value()==83,"Settings restored into controls");
            check(restored.findChild<QComboBox*>("gridSize")->currentData().toInt()==512&&
                  restored.findChild<QDoubleSpinBox*>("rxColorMinimum")->value()==-145&&
                  restored.findChild<QDoubleSpinBox*>("rxColorMaximum")->value()==-35&&
                  restored.findChild<QDoubleSpinBox*>("marginColorMinimum")->value()==-45&&
                  restored.findChild<QDoubleSpinBox*>("marginColorMaximum")->value()==55,"Detailed grid and overlay ranges restored");
            const auto restoredPoints=restored.findChild<QthMapWidget*>()->userMarkerList(),points=map->userMarkerList();
            check(restoredPoints.size()==2&&restoredPoints[0].latitude==points[0].latitude&&restoredPoints[0].longitude==points[0].longitude&&
                  restoredPoints[1].latitude==points[1].latitude&&restoredPoints[1].longitude==points[1].longitude,"TX/RX points restored");
            check(restored.findChild<QLabel*>("propagationDistance")->text()==distance->text(),"Restored points recompute distance");
            check(restored.findChild<QComboBox*>("propagationMapSource")->currentIndex()==3,"QTH provider does not overwrite saved map choice");
            for(auto *box:restored.findChildren<QCheckBox*>())check(!box->isChecked(),"Restoring settings does not enable online requests");
        }
        QImage shot(dialog.size(),QImage::Format_ARGB32);shot.fill(Qt::transparent);dialog.render(&shot);
        check(shot.save(output+(uk?"/propagation-uk.png":"/propagation-en.png")),"Dialog render");
        dialog.resize(800,600);app.processEvents();QImage small(dialog.size(),QImage::Format_ARGB32);dialog.render(&small);small.save(output+(uk?"/propagation-small-uk.png":"/propagation-small-en.png"));
        auto *tabs=dialog.findChild<QTabWidget*>();tabs->setCurrentIndex(3);app.processEvents();
        QImage overlay(dialog.size(),QImage::Format_ARGB32);dialog.render(&overlay);overlay.save(output+(uk?"/overlay-small-uk.png":"/overlay-small-en.png"));
        tabs->setCurrentIndex(0);
        for(auto *combo:dialog.findChildren<QComboBox*>())if(combo->itemText(0)=="SRTM HGT")combo->setCurrentIndex(1);
        grid->setCurrentIndex(grid->findData(32));
        auto *calculate=dialog.findChild<QPushButton*>("calculateCoverage");calculate->click();
        check(!useTx->isEnabled()&&!useRx->isEnabled(),"QTH import cannot mutate endpoints during calculation");
        QElapsedTimer timer;timer.start();
        while(!calculate->isEnabled()&&timer.elapsed()<15000){app.processEvents();QThread::msleep(5);}
        check(calculate->isEnabled(),"Asynchronous calculation completes without blocking GUI");
        check(useTx->isEnabled()&&useRx->isEnabled(),"QTH import enabled after calculation");
        auto *legend=dialog.findChildren<QLabel*>().front();
        for(auto *label:dialog.findChildren<QLabel*>())if(label->text().contains("-145.0 .. -35.0 dBm"))legend=label;
        check(legend->text().contains("-145.0 .. -35.0 dBm"),"Legend reports actual RX color range");
        colorHigh->setValue(-30);
        check(calculate->isEnabled()&&legend->text().contains("-145.0 .. -30.0 dBm"),"Changing palette reuses completed coverage without starting worker or clearing result");
        QImage computed(dialog.size(),QImage::Format_ARGB32);dialog.render(&computed);computed.save(output+(uk?"/coverage-uk.png":"/coverage-en.png"));
        dialog.hide();dialog.findChild<QPushButton*>("showPathProfile")->click();
        check(calculate->isEnabled(),"Profile button reopens cached path without duplicate calculation");
        const auto profiles=dialog.findChildren<QDialog*>();
        check(!profiles.isEmpty(),"Separate profile window created");
        if(!profiles.isEmpty()){QImage rendered(profiles.front()->size(),QImage::Format_ARGB32);profiles.front()->render(&rendered);rendered.save(output+(uk?"/profile-uk.png":"/profile-en.png"));}
        dialog.findChild<QDoubleSpinBox*>("frequencyMHz")->setValue(434);
        dialog.findChild<QPushButton*>("showPathProfile")->click();
        check(!calculate->isEnabled(),"Profile button calculates invalidated result");
        timer.restart();
        while(!calculate->isEnabled()&&timer.elapsed()<15000){app.processEvents();QThread::msleep(5);}
        check(calculate->isEnabled(),"Profile-only calculation completes");
        dialog.close();
        {
            PropagationLabDialog removal(uk,nullptr,settingsFile);
            removal.setAttribute(Qt::WA_DontShowOnScreen);removal.show();app.processEvents();
            removal.findChild<QDoubleSpinBox*>("txLatitude")->setValue(0);
            removal.findChild<QDoubleSpinBox*>("txLongitude")->setValue(0);
            auto *removalMap=removal.findChild<QthMapWidget*>();removalMap->centerOn(0,0);
            QMouseEvent remove(QEvent::MouseButtonPress,QRectF(removalMap->rect().adjusted(18,18,-18,-34)).center(),Qt::RightButton,Qt::RightButton,Qt::NoModifier);
            QApplication::sendEvent(removalMap,&remove);
            check(removalMap->userMarkerList().size()==1,"Removal clears TX before save");
            check(removal.findChild<QLabel*>("propagationDistance")->text().contains("--"),"Missing marker clears distance");
            removal.close();
        }
        PropagationLabDialog removed(uk,nullptr,settingsFile);
        check(removed.findChild<QthMapWidget*>()->userMarkerList().size()==1&&removed.findChild<QthMapWidget*>()->userMarkerList().front().name=="RX","Deleted marker stays deleted after reopening");
        QFile privateSettings(settingsFile);privateSettings.open(QIODevice::ReadOnly);
        check(!privateSettings.readAll().contains("TEST_ONLY"),"Resolved provider API key is never written to propagation settings");
    }
    if(argc>2&&QString::fromLocal8Bit(argv[2])=="--online"){
        SolarConditionsWidget panel(true);panel.resize(330,600);
        auto *refresh=panel.findChildren<QPushButton*>().front();refresh->click();QElapsedTimer timer;timer.start();
        while(!refresh->isEnabled()&&timer.elapsed()<35000){app.processEvents();QThread::msleep(10);}
        check(refresh->isEnabled(),"NOAA fetch finishes");
        bool populated=false;for(auto *label:panel.findChildren<QLabel*>())if(label->text().contains("<h3>F10.7:")&&!label->text().contains("F10.7: --"))populated=true;
        check(populated,"NOAA live HTTPS flux displayed");
        QImage rendered(panel.size(),QImage::Format_ARGB32);panel.render(&rendered);rendered.save(output+"/solar-live.png");
    }
    std::cout<<checks<<" checks; "<<failures<<" failures\n";return failures?1:0;
}
