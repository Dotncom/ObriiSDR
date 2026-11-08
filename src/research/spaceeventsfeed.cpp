#include "spaceeventsfeed.h"
#include "publicdatafetch.h"
#include "appsettingsutils.h"
#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextBrowser>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QXmlStreamReader>
#include <algorithm>

namespace {
QString text(bool uk, const char *en, const char *ua) { return QString::fromUtf8(uk ? ua : en); }
bool safeUrl(const QUrl &url) { return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() && (url.scheme() == "https" || url.scheme() == "http"); }
QDateTime parseUtc(const QString &value) {
    auto date = QDateTime::fromString(value, Qt::RFC2822Date);
    if (!date.isValid()) date = QDateTime::fromString(value, Qt::ISODate);
    return date.toUTC();
}
QVector<spaceevents::Source> defaults() {
    return {{"noaa_flux", "NOAA F10.7", "flux", "https://services.swpc.noaa.gov/json/f107_cm_flux.json", {}, true},
            {"noaa_kp", "NOAA Kp", "kp", "https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json", {}, true},
            {"comets", "NASA/JPL comet orbit updates", "comets", "https://ssd-api.jpl.nasa.gov/sbdb_query.api?sb-kind=c&fields=full_name,pdes,soln_date,tp_cal&sort=-soln_date&limit=20", {}, true},
            {"meteors", "eMetN Meteor Journal", "rss", "https://www.emeteornews.net/feed/", {}, true},
            {"fireballs", "NASA/JPL fireballs", "fireballs", "https://ssd-api.jpl.nasa.gov/fireball.api?limit=20", {}, true},
            {"comet_news", "NASA/JPL comet news", "rss", "https://www.jpl.nasa.gov/feeds/news/", "comet", true}};
}
}
QVector<spaceevents::Source> spaceevents::sources() {
    auto result = defaults(); QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    for (auto &entry : result) {
        const QString prefix = "publicSources/" + entry.id + '/';
        entry.url = settings.value(prefix + "url", entry.url).toString();
        entry.enabled = settings.value(prefix + "enabled", entry.enabled).toBool();
    }
    return result;
}
spaceevents::Source spaceevents::source(const QString &id) {
    for (const auto &entry : sources()) if (entry.id == id) return entry;
    return {};
}
void spaceevents::configureSources(QWidget *parent, bool uk) {
    QDialog dialog(parent); dialog.setWindowTitle(text(uk, "Public data sources", u8"Джерела онлайн-даних")); dialog.resize(850, 370);
    auto *layout = new QVBoxLayout(&dialog); auto *table = new QTableWidget(&dialog);
    const auto configuration = sources();
    table->setColumnCount(3); table->setRowCount(configuration.size());
    table->setHorizontalHeaderLabels({text(uk,"Use",u8"Вик."),text(uk,"Source",u8"Джерело"),"URL"});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    for (int row = 0; row < configuration.size(); ++row) {
        auto *enabled = new QTableWidgetItem; enabled->setCheckState(configuration[row].enabled ? Qt::Checked : Qt::Unchecked);
        enabled->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable); table->setItem(row, 0, enabled);
        auto *name = new QTableWidgetItem(configuration[row].name); name->setFlags(Qt::ItemIsEnabled); table->setItem(row, 1, name);
        table->setItem(row, 2, new QTableWidgetItem(configuration[row].url));
    }
    layout->addWidget(table, 1);
    auto *autoRefresh = new QCheckBox(text(uk,"Auto-refresh events while the feed is visible",u8"Автооновлення подій, поки стрічка видима"), &dialog);
    QSettings settings(persistentSettingsFilePath(), QSettings::IniFormat);
    autoRefresh->setChecked(settings.value("publicSources/autoRefresh", false).toBool());
    auto *minutes = new QSpinBox(&dialog); minutes->setRange(15, 1440); minutes->setSuffix(text(uk," min",u8" хв"));
    minutes->setValue(settings.value("publicSources/intervalMinutes",60).toInt());
    auto *row = new QHBoxLayout; row->addWidget(autoRefresh); row->addWidget(minutes); layout->addLayout(row);
    auto *notice = new QLabel(text(uk,"URLs must return the same RSS/Atom or JSON schema. Comet orbit updates are not discovery alerts; fireballs are reported past events. Downloads send no station coordinates. Sources see your network IP.",u8"Адреси мають повертати той самий формат RSS/Atom або JSON. Оновлення орбіти комети не є повідомленням про відкриття; боліди — зареєстровані минулі події. Координати станції не надсилаються, але джерела бачать вашу мережеву IP-адресу."), &dialog);
    notice->setWordWrap(true); layout->addWidget(notice);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, &dialog); layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Save)->setText(text(uk,"Save",u8"Зберегти"));
    buttons->button(QDialogButtonBox::Cancel)->setText(text(uk,"Cancel",u8"Скасувати"));
    buttons->button(QDialogButtonBox::RestoreDefaults)->setText(text(uk,"Defaults",u8"Типові"));
    QObject::connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog, [=] {
        const auto initial = defaults();
        for (int i=0;i<initial.size();++i) {table->item(i,0)->setCheckState(Qt::Checked); table->item(i,2)->setText(initial[i].url);}
        autoRefresh->setChecked(false); minutes->setValue(60);
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        for (int i=0;i<configuration.size();++i) {
            if (!safeUrl(QUrl(table->item(i,2)->text().trimmed()))) {
                QMessageBox::warning(&dialog, dialog.windowTitle(), text(uk,"Enter a valid HTTP(S) URL without embedded credentials.",u8"Введіть коректну HTTP(S)-адресу без вбудованого логіна чи пароля.")); return;
            }
        }
        for (int i=0;i<configuration.size();++i) {
            const QString prefix = "publicSources/" + configuration[i].id + '/';
            settings.setValue(prefix+"url",table->item(i,2)->text().trimmed());
            settings.setValue(prefix+"enabled",table->item(i,0)->checkState()==Qt::Checked);
        }
        settings.setValue("publicSources/autoRefresh",autoRefresh->isChecked());
        settings.setValue("publicSources/intervalMinutes",minutes->value()); settings.sync();
        if (settings.status()!=QSettings::NoError) { QMessageBox::warning(&dialog,dialog.windowTitle(),text(uk,"Cannot save source settings.",u8"Не вдалося зберегти налаштування джерел.")); return; }
        dialog.accept();
    });
    dialog.exec();
}

bool spaceevents::parse(const QByteArray &payload, const Source &source, QVector<Entry> *entries, QString *error) {
    if (error) error->clear(); if (!entries || payload.size()>2*1024*1024) return false;
    QVector<Entry> parsed;
    if (source.type=="rss") {
        QXmlStreamReader xml(payload); Entry item; bool inside=false;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.tokenType()==QXmlStreamReader::DTD) { if(error)*error="DTD is not allowed in news feeds."; return false; }
            const auto name=xml.name();
            if (xml.isStartElement()) {
                if(name=="item" || name=="entry") { item={}; item.source=source.name; inside=true; }
                else if(inside && name=="title") item.title=xml.readElementText(QXmlStreamReader::IncludeChildElements).left(300);
                else if(inside && (name=="pubDate"||name=="published"||name=="updated"||name=="date")) { item.utc=parseUtc(xml.readElementText()); item.dateText=item.utc.isValid()?item.utc.toString("yyyy-MM-dd HH:mm 'UTC'"):QString(); }
                else if(inside && name=="link") {
                    const auto attributes=xml.attributes();
                    if(attributes.hasAttribute("href")) { if(!attributes.hasAttribute("rel")||attributes.value("rel")=="alternate") item.link=QUrl(attributes.value("href").toString()); }
                    else item.link=QUrl(xml.readElementText().trimmed());
                }
            } else if(xml.isEndElement() && inside && (name=="item"||name=="entry")) {
                item.link=QUrl(source.url).resolved(item.link);
                if(!safeUrl(item.link)) item.link=QUrl();
                if(!item.title.isEmpty()&&(source.filter.isEmpty()||item.title.contains(source.filter,Qt::CaseInsensitive))&&parsed.size()<30) parsed<<item;
                inside=false;
            }
        }
        if(xml.hasError()) {if(error)*error="Invalid RSS/Atom feed.";return false;}
        const auto root=QString::fromUtf8(payload.left(1024));
        if(!root.contains("<rss")&&!root.contains("<feed")&&!root.contains("<rdf:RDF")) {if(error)*error="Expected RSS/Atom.";return false;}
    } else {
        QJsonParseError failure; const auto document=QJsonDocument::fromJson(payload,&failure);
        const auto object=document.object();
        const QString expected=source.type=="fireballs"?"1.2":"1.0";
        if(failure.error!=QJsonParseError::NoError || object.value("signature").toObject().value("version").toString()!=expected) {
            if(error)*error="Unsupported NASA/JPL API format/version."; return false;
        }
        const auto fields=object.value("fields").toArray();
        const auto rows=object.value("data").toArray();
        if(rows.isEmpty() && object.value("count").toVariant().toInt()!=0) {if(error)*error="Missing API data.";return false;}
        for(const auto &row:rows) {
            const auto values=row.toArray();
            const auto value=[&](const QString &key) {
                for(int i=0;i<fields.size()&&i<values.size();++i) if(fields[i].toString()==key) return values[i].isString()?values[i].toString():values[i].isDouble()?QString::number(values[i].toDouble(),'g',12):QString();
                return QString();
            };
            Entry item; item.source=source.name;
            if(source.type=="fireballs") {
                item.utc=QDateTime::fromString(value("date"),"yyyy-MM-dd HH:mm:ss"); item.utc.setTimeSpec(Qt::UTC);
                if(!item.utc.isValid()) continue;
                item.dateText=item.utc.toString("yyyy-MM-dd HH:mm 'UTC'");
                item.title=QStringLiteral("Fireball: radiated energy %1 x 10^10 J; impact estimate %2 kt").arg(value("energy"),value("impact-e"));
                item.link=QUrl("https://cneos.jpl.nasa.gov/fireballs/");
            } else if(source.type=="comets") {
                if(value("full_name").isEmpty()) continue;
                item.title=value("full_name")+QStringLiteral(" | orbit solution: ")+value("soln_date")+QStringLiteral(" PT | perihelion (TDB): ")+value("tp_cal");
                item.link=QUrl("https://ssd.jpl.nasa.gov/tools/sbdb_lookup.html#/?sstr="+QString::fromLatin1(QUrl::toPercentEncoding(value("pdes"))));
            } else {if(error)*error="Unknown source schema.";return false;}
            parsed<<item; if(parsed.size()>=30)break;
        }
    }
    *entries=std::move(parsed); return true;
}

SpaceEventsFeedWidget::SpaceEventsFeedWidget(bool ukrainian,QWidget *parent):QWidget(parent),uk(ukrainian) {
    auto *layout=new QVBoxLayout(this); auto *row=new QHBoxLayout;
    button=new QPushButton(text(uk,"Refresh space events",u8"Оновити космічні події"),this);
    auto *settings=new QPushButton(text(uk,"Sources...",u8"Джерела..."),this); row->addWidget(button);row->addWidget(settings);layout->addLayout(row);
    status=new QLabel(text(uk,"Not downloaded yet",u8"Дані ще не завантажені"),this);status->setWordWrap(true);layout->addWidget(status);
    feed=new QTextBrowser(this);feed->setOpenLinks(false);feed->setOpenExternalLinks(false);feed->setMinimumHeight(150);layout->addWidget(feed,1);
    connect(feed,&QTextBrowser::anchorClicked,this,[](const QUrl &url){if(safeUrl(url))QDesktopServices::openUrl(url);});
    connect(button,&QPushButton::clicked,this,&SpaceEventsFeedWidget::refresh);
    connect(settings,&QPushButton::clicked,this,[this]{spaceevents::configureSources(this,uk);});
    timer=new QTimer(this);timer->setInterval(60000);
    connect(timer,&QTimer::timeout,this,[this]{
        QSettings settings(persistentSettingsFilePath(),QSettings::IniFormat);
        if(isVisible()&&settings.value("publicSources/autoRefresh",false).toBool()&&
            (!lastRefresh.isValid()||lastRefresh.secsTo(QDateTime::currentDateTimeUtc())>=60*(std::clamp)(settings.value("publicSources/intervalMinutes",60).toInt(),15,1440)))refresh();
    }); timer->start();
}
SpaceEventsFeedWidget::~SpaceEventsFeedWidget(){cancel=true;if(worker){worker->wait();delete worker;}}
void SpaceEventsFeedWidget::showEvent(QShowEvent *event){QWidget::showEvent(event);}
void SpaceEventsFeedWidget::refresh(){
    if(worker)return;cancel=false;button->setEnabled(false);lastRefresh=QDateTime::currentDateTimeUtc();
    status->setText(text(uk,"Fetching public feeds...",u8"Завантаження відкритих джерел..."));
    const auto configuration=spaceevents::sources();const auto previous=entries;
    worker=QThread::create([this,configuration,previous]{
        QVector<spaceevents::Entry> combined;QStringList failures;
        for(const auto &source:configuration){
            if(cancel||!source.enabled||source.type=="flux"||source.type=="kp")continue;
            QString error;const auto bytes=fetchPublicData(QUrl(source.url),cancel,&error);QVector<spaceevents::Entry> found;
            if(!cancel&&spaceevents::parse(bytes,source,&found,&error))combined+=found;
            else if(!cancel){failures<<source.name+": "+error;for(const auto &entry:previous)if(entry.source==source.name)combined<<entry;}
        }
        if(cancel)return;
        QMetaObject::invokeMethod(this,[this,combined,failures]{entries=combined;render();status->setText(
            text(uk,"Fetched %1 items at %2 UTC",u8"Отримано %1 повідомлень, %2 UTC").arg(entries.size()).arg(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm"))+
            (failures.isEmpty()?QString():"\n"+text(uk,"Failed sources (previous items retained): ",u8"Помилки джерел (попередні повідомлення збережено): ")+failures.join("; ")));button->setEnabled(true);},Qt::QueuedConnection);
    });
    connect(worker,&QThread::finished,this,[this]{auto *finished=worker;worker=nullptr;finished->deleteLater();button->setEnabled(true);});worker->start(QThread::LowPriority);
}
void SpaceEventsFeedWidget::render(){
    QString html;
    for(const auto &entry:entries){
        const QString title=entry.title.toHtmlEscaped();
        html+="<p><b>"+(entry.link.isEmpty()?title:"<a href=\""+entry.link.toString(QUrl::FullyEncoded).toHtmlEscaped()+"\">"+title+"</a>")+"</b><br>"+
            entry.source.toHtmlEscaped()+" "+entry.dateText.toHtmlEscaped()+"</p>";
    }
    feed->setHtml(html.isEmpty()?text(uk,"No matching items returned.",u8"Відповідних повідомлень немає."):html);
}
