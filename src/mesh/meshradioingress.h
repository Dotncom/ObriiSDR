#pragma once
#include "meshports.h"
#include <QJsonArray>
#include <functional>
#include <memory>

namespace Mesh {
class RadioIngress {
public:
    using Consumer=std::function<void(const QString &,const float *,int,bool)>;
    RadioIngress();
    ~RadioIngress();
    bool configure(const QVector<PortBinding> &,QString *error);
    void clear();
    void poll(const Consumer &);
    QJsonArray status() const;
    bool active() const;
    static bool anyActive();
private:
    struct Private;
    std::unique_ptr<Private> d;
};
}
