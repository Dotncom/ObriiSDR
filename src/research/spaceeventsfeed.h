#ifndef SPACEEVENTSFEED_H
#define SPACEEVENTSFEED_H
#include <QDateTime>
#include <QJsonArray>
#include <QWidget>
#include <QUrl>
#include <atomic>
class QTextBrowser;
class QLabel;
class QPushButton;
class QThread;
class QTimer;
namespace spaceevents {
struct Source { QString id, name, type, url, filter; bool enabled = true; };
struct Entry { QString title, dateText, source; QUrl link; QDateTime utc; };
QVector<Source> sources();
Source source(const QString &id);
void configureSources(QWidget *parent, bool ukrainian);
bool parse(const QByteArray &payload, const Source &source, QVector<Entry> *entries, QString *error);
}
class SpaceEventsFeedWidget : public QWidget {
    Q_OBJECT
public:
    explicit SpaceEventsFeedWidget(bool ukrainian, QWidget *parent = nullptr);
    ~SpaceEventsFeedWidget() override;
protected:
    void showEvent(QShowEvent *event) override;
private:
    void refresh();
    void render();
    bool uk;
    QTextBrowser *feed;
    QLabel *status;
    QPushButton *button;
    QThread *worker = nullptr;
    QTimer *timer;
    std::atomic_bool cancel{false};
    QVector<spaceevents::Entry> entries;
    QDateTime lastRefresh;
};
#endif
