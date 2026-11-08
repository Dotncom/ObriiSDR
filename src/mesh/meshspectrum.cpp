#include "meshspectrum.h"
#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace Mesh {
SpectrumView::SpectrumView(QWidget *parent):QWidget(parent) {
    setMinimumSize(260,150);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
    setObjectName("nodeSpectrum");
}
bool SpectrumView::setTrace(const QJsonObject &trace) {
    const auto bins=trace.value("levelsDbfs").toArray();
    const auto start=trace.value("startHz"),step=trace.value("binHz");
    bool valid=bins.size()>=2&&bins.size()<=1024&&start.isDouble()&&step.isDouble()&&
        std::isfinite(start.toDouble())&&std::isfinite(step.toDouble())&&step.toDouble()>0&&
        trace.value("offset").toInt(-1)==0&&trace.value("totalBins").toInt(-1)==bins.size()&&
        !trace.value("truncated").toBool(true);
    QVector<double> values;values.reserve(bins.size());
    for(const auto &v:bins) {
        valid=valid&&v.isDouble()&&std::isfinite(v.toDouble());values.append(v.toDouble());
    }
    if(!valid){clear(QString::fromUtf8("Очікування повного спектра"));return false;}
    levels=std::move(values);firstHz=start.toDouble();binHz=step.toDouble();message.clear();update();return true;
}
void SpectrumView::clear(const QString &text){levels.clear();message=text;update();}
void SpectrumView::setRange(double low,double high) {
    if(!std::isfinite(low)||!std::isfinite(high)||high-low<1)return;
    minimumDb=low;maximumDb=high;update();
}
void SpectrumView::setListener(double hz,double width){listenerHz=hz;bandwidthHz=width;update();}
void SpectrumView::paintEvent(QPaintEvent *) {
    QPainter p(this);p.fillRect(rect(),QColor("#101419"));
    const QRectF plot(54,26,std::max(1,width()-72),std::max(1,height()-59));
    p.setFont(QFont("Segoe UI",9));
    p.setPen(QColor("#b6bdc5"));p.drawText(QRectF(2,0,48,16),Qt::AlignCenter,"dBFS");
    for(int i=0;i<=4;++i) {
        const double y=plot.top()+plot.height()*i/4;
        p.setPen(QColor("#30373e"));p.drawLine(QPointF(plot.left(),y),QPointF(plot.right(),y));
        p.setPen(QColor("#b6bdc5"));p.drawText(QRectF(0,y-9,48,18),Qt::AlignRight|Qt::AlignVCenter,
            QString::number(maximumDb-(maximumDb-minimumDb)*i/4,'f',0));
    }
    if(levels.isEmpty()) {p.drawText(plot,Qt::AlignCenter|Qt::TextWordWrap,message);return;}
    const double span=binHz*(levels.size()-1);
    const auto x=[&](double hz){return plot.left()+(hz-firstHz)/span*plot.width();};
    const int ticks=std::clamp(int(plot.width()/145),1,4);
    for(int i=0;i<=ticks;++i) {
        const double f=firstHz+span*i/ticks;
        p.setPen(QColor("#30373e"));p.drawLine(QPointF(x(f),plot.top()),QPointF(x(f),plot.bottom()));
        p.setPen(QColor("#b6bdc5"));const double labelX=std::clamp(x(f)-62.0,0.0,std::max(0.0,width()-124.0));
        p.drawText(QRectF(labelX,plot.bottom()+4,124,22),Qt::AlignCenter,QString::number(f/1e6,'f',4)+" MHz");
    }
    p.save();p.setClipRect(plot);
    p.fillRect(QRectF(QPointF(x(listenerHz-bandwidthHz/2),plot.top()),
                     QPointF(x(listenerHz+bandwidthHz/2),plot.bottom())),QColor(92,162,220,28));
    QPainterPath path;
    for(int i=0;i<levels.size();++i) {
        const QPointF point(plot.left()+plot.width()*i/(levels.size()-1),
            plot.bottom()-(levels[i]-minimumDb)/(maximumDb-minimumDb)*plot.height());
        if(i==0)path.moveTo(point);else path.lineTo(point);
    }
    p.setRenderHint(QPainter::Antialiasing);p.setPen(QPen(QColor("#72deaa"),1.2));p.drawPath(path);
    p.setPen(QPen(QColor("#efba69"),1,Qt::DashLine));p.drawLine(QPointF(x(listenerHz),plot.top()),QPointF(x(listenerHz),plot.bottom()));
    p.restore();
}
}
