#include "remotewebserver.h"

#include "remotevfoservice.h"

#include <QAbstractSocket>
#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>

#include <algorithm>

namespace {
constexpr qint64 MaxHttpHeader = 64 * 1024;
constexpr quint64 MaxWebSocketPayload = 1024 * 1024;

QByteArray websocketAccept(const QByteArray &key) {
    return QCryptographicHash::hash(
               key.trimmed() + QByteArrayLiteral("258EAFA5-E914-47DA-95CA-C5AB0DC85B11"),
               QCryptographicHash::Sha1)
        .toBase64();
}
QByteArray headerValue(const QList<QByteArray> &lines, const QByteArray &name) {
    const QByteArray prefix = name.toLower() + ':';
    for (const QByteArray &line : lines) {
        if (line.toLower().startsWith(prefix)) return line.mid(prefix.size()).trimmed();
    }
    return {};
}
}

RemoteWebServer::RemoteWebServer(RemoteVfoService *service, QObject *parent)
    : QObject(parent), service_(service), server_(new QTcpServer(this)) {
    connect(server_, &QTcpServer::newConnection, this, &RemoteWebServer::acceptConnections);
    if (service_) {
        connect(service_, &RemoteVfoService::jsonFrameReady,
                this, &RemoteWebServer::sendJsonToOwner);
        connect(service_, &RemoteVfoService::binaryFrameReady,
                this, &RemoteWebServer::sendBinaryToOwner);
    }
}

RemoteWebServer::~RemoteWebServer() {
    stop();
}

bool RemoteWebServer::start(const QString &bindAddress, quint16 portValue) {
    stop();
    const QHostAddress address(bindAddress.trimmed().isEmpty()
                                   ? QStringLiteral("127.0.0.1")
                                   : bindAddress.trimmed());
    if (!server_->listen(address, portValue)) {
        status_ = QStringLiteral("Browser server failed: %1").arg(server_->errorString());
        emit statusChanged(status_);
        return false;
    }
    status_ = QStringLiteral("Browser UI: http://%1:%2/")
                  .arg(server_->serverAddress().toString())
                  .arg(server_->serverPort());
    emit statusChanged(status_);
    return true;
}

void RemoteWebServer::stop() {
    const bool statusChangedValue = status_ != QStringLiteral("Browser server stopped");
    if (server_->isListening()) server_->close();
    const auto sockets = buffers_.keys();
    for (QTcpSocket *socket : sockets) closeSocket(socket);
    buffers_.clear();
    owners_.clear();
    ownerSockets_.clear();
    status_ = QStringLiteral("Browser server stopped");
    if (statusChangedValue) emit statusChanged(status_);
}

bool RemoteWebServer::isListening() const {
    return server_->isListening();
}

quint16 RemoteWebServer::port() const {
    return server_->isListening() ? server_->serverPort() : 0;
}

QString RemoteWebServer::statusText() const {
    return status_;
}

void RemoteWebServer::acceptConnections() {
    while (server_->hasPendingConnections()) {
        QTcpSocket *socket = server_->nextPendingConnection();
        if (!socket) continue;
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        buffers_.insert(socket, {});
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { readSocket(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() { closeSocket(socket); });
        connect(socket, &QTcpSocket::errorOccurred, this,
                [this, socket](QAbstractSocket::SocketError) { closeSocket(socket); });
    }
}

void RemoteWebServer::readSocket(QTcpSocket *socket) {
    if (!buffers_.contains(socket)) return;
    QByteArray &buffer = buffers_[socket];
    buffer.append(socket->readAll());
    if (owners_.contains(socket)) {
        while (processWebSocketFrames(socket, buffer)) {}
    } else {
        processHttpRequest(socket, buffer);
    }
}

void RemoteWebServer::closeSocket(QTcpSocket *socket) {
    if (!socket) return;
    const QString owner = owners_.take(socket);
    if (!owner.isEmpty()) {
        ownerSockets_.remove(owner);
        if (service_) service_->closeEndpoint(owner);
    }
    buffers_.remove(socket);
    socket->disconnect(this);
    if (socket->state() != QAbstractSocket::UnconnectedState) socket->disconnectFromHost();
    socket->deleteLater();
}

bool RemoteWebServer::processHttpRequest(QTcpSocket *socket, QByteArray &buffer) {
    if (buffer.size() > MaxHttpHeader) {
        sendHttp(socket, 431, QByteArrayLiteral("text/plain"), QByteArrayLiteral("Header too large"));
        return false;
    }
    const int end = buffer.indexOf("\r\n\r\n");
    if (end < 0) return false;
    const QByteArray request = buffer.left(end + 4);
    buffer.remove(0, end + 4);
    const QList<QByteArray> lines = request.split('\n');
    if (lines.isEmpty()) return false;
    const QList<QByteArray> requestParts = lines.front().trimmed().split(' ');
    if (requestParts.size() < 2 || requestParts[0] != QByteArrayLiteral("GET")) {
        sendHttp(socket, 405, QByteArrayLiteral("text/plain"), QByteArrayLiteral("GET only"));
        return false;
    }
    const QByteArray path = requestParts[1].split('?').front();
    if (path == QByteArrayLiteral("/ws")) {
        const QByteArray key = headerValue(lines, QByteArrayLiteral("Sec-WebSocket-Key"));
        const QByteArray upgrade = headerValue(lines, QByteArrayLiteral("Upgrade")).toLower();
        if (key.isEmpty() || upgrade != QByteArrayLiteral("websocket")) {
            sendHttp(socket, 400, QByteArrayLiteral("text/plain"), QByteArrayLiteral("WebSocket upgrade required"));
            return false;
        }
        QByteArray response = QByteArrayLiteral("HTTP/1.1 101 Switching Protocols\r\n")
            + QByteArrayLiteral("Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ")
            + websocketAccept(key) + QByteArrayLiteral("\r\n\r\n");
        socket->write(response);
        const QString owner = QStringLiteral("web-%1").arg(nextOwner_++);
        owners_.insert(socket, owner);
        ownerSockets_.insert(owner, socket);
        if (service_) {
            service_->openEndpoint(owner,
                QStringLiteral("Browser %1:%2").arg(socket->peerAddress().toString()).arg(socket->peerPort()));
        }
        return true;
    }
    if (path == QByteArrayLiteral("/api/status")) {
        const QByteArray body = QJsonDocument(service_ ? service_->statusJson() : QJsonObject())
                                    .toJson(QJsonDocument::Compact);
        sendHttp(socket, 200, QByteArrayLiteral("application/json; charset=utf-8"), body);
        return true;
    }
    if (path == QByteArrayLiteral("/") || path == QByteArrayLiteral("/index.html")) {
        sendHttp(socket, 200, QByteArrayLiteral("text/html; charset=utf-8"), browserPage());
        return true;
    }
    sendHttp(socket, 404, QByteArrayLiteral("text/plain"), QByteArrayLiteral("Not found"));
    return true;
}

bool RemoteWebServer::processWebSocketFrames(QTcpSocket *socket, QByteArray &buffer) {
    if (buffer.size() < 2) return false;
    const uchar first = static_cast<uchar>(buffer[0]);
    const uchar second = static_cast<uchar>(buffer[1]);
    const quint8 opcode = first & 0x0f;
    const bool masked = (second & 0x80) != 0;
    quint64 length = second & 0x7f;
    int offset = 2;
    if (length == 126) {
        if (buffer.size() < 4) return false;
        length = (static_cast<quint64>(static_cast<uchar>(buffer[2])) << 8) |
                 static_cast<quint64>(static_cast<uchar>(buffer[3]));
        offset = 4;
    } else if (length == 127) {
        if (buffer.size() < 10) return false;
        length = 0;
        for (int i = 2; i < 10; ++i) length = (length << 8) | static_cast<uchar>(buffer[i]);
        offset = 10;
    }
    if (!masked || length > MaxWebSocketPayload) {
        closeSocket(socket);
        return false;
    }
    if (buffer.size() < offset + 4 + static_cast<qint64>(length)) return false;
    const QByteArray mask = buffer.mid(offset, 4);
    offset += 4;
    QByteArray payload = buffer.mid(offset, static_cast<int>(length));
    buffer.remove(0, offset + static_cast<int>(length));
    for (int i = 0; i < payload.size(); ++i) payload[i] = payload[i] ^ mask[i & 3];

    if (opcode == 0x8) {
        sendWebSocketFrame(socket, 0x8, {});
        closeSocket(socket);
        return false;
    }
    if (opcode == 0x9) {
        sendWebSocketFrame(socket, 0xA, payload);
        return !buffer.isEmpty();
    }
    if (opcode != 0x1) return !buffer.isEmpty();

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error == QJsonParseError::NoError && document.isObject() && service_) {
        QJsonObject command = document.object();
        command.insert(QStringLiteral("type"), QStringLiteral("remoteVfo"));
        service_->handleCommand(owners_.value(socket), command);
    }
    return !buffer.isEmpty();
}

void RemoteWebServer::sendHttp(QTcpSocket *socket,
                               int status,
                               const QByteArray &contentType,
                               const QByteArray &body) {
    const QByteArray reason = status == 200 ? QByteArrayLiteral("OK") :
                              status == 404 ? QByteArrayLiteral("Not Found") :
                              status == 405 ? QByteArrayLiteral("Method Not Allowed") :
                              status == 431 ? QByteArrayLiteral("Request Header Fields Too Large") :
                                              QByteArrayLiteral("Bad Request");
    QByteArray response = QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status) + ' ' + reason +
        QByteArrayLiteral("\r\nContent-Type: ") + contentType +
        QByteArrayLiteral("\r\nContent-Length: ") + QByteArray::number(body.size()) +
        QByteArrayLiteral("\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n") + body;
    socket->write(response);
    socket->disconnectFromHost();
}

void RemoteWebServer::sendWebSocketFrame(QTcpSocket *socket, quint8 opcode, const QByteArray &payload) {
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) return;
    QByteArray frame;
    frame.append(static_cast<char>(0x80 | (opcode & 0x0f)));
    const quint64 size = static_cast<quint64>(payload.size());
    if (size < 126) {
        frame.append(static_cast<char>(size));
    } else if (size <= 0xffff) {
        frame.append(static_cast<char>(126));
        frame.append(static_cast<char>((size >> 8) & 0xff));
        frame.append(static_cast<char>(size & 0xff));
    } else {
        frame.append(static_cast<char>(127));
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.append(static_cast<char>((size >> shift) & 0xff));
    }
    frame.append(payload);
    socket->write(frame);
}

void RemoteWebServer::sendJsonToOwner(const QString &ownerId, const QJsonObject &frame) {
    QTcpSocket *socket = ownerSockets_.value(ownerId);
    if (!socket) return;
    sendWebSocketFrame(socket, 0x1, QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

void RemoteWebServer::sendBinaryToOwner(const QString &ownerId,
                                        const QJsonObject &frame,
                                        const QByteArray &payload) {
    Q_UNUSED(frame);
    QTcpSocket *socket = ownerSockets_.value(ownerId);
    if (!socket) return;
    if (socket->bytesToWrite() > 256 * 1024) return;
    sendWebSocketFrame(socket, 0x2, payload);
}

QByteArray RemoteWebServer::browserPage() {
    return QByteArrayLiteral(R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Obrii SDR Remote VFO</title>
<style>
html,body{height:100%;margin:0;background:#101317;color:#e5e9ef;font:14px system-ui,sans-serif}body{display:grid;grid-template-rows:auto 1fr auto}
header,footer{padding:8px 12px;background:#191e24;border-bottom:1px solid #343b44}footer{border-top:1px solid #343b44;border-bottom:0;color:#aeb7c2}
.controls{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.controls label{display:flex;gap:4px;align-items:center}input,select,button{background:#222932;color:#f2f5f8;border:1px solid #46515e;padding:5px}button{cursor:pointer}button.run{background:#205d3b}button.stop{background:#652a2a}
.capture{display:grid;grid-template-columns:auto minmax(180px,420px) auto;gap:7px;align-items:center;margin-top:7px;color:#aeb7c2}.capture input{width:100%;padding:0}
main{min-height:0;display:grid;grid-template-rows:minmax(110px,42%) 36px minmax(150px,1fr);padding:8px;gap:4px}canvas{width:100%;height:100%;background:#05070a;border:1px solid #313943;box-sizing:border-box}#scale{cursor:crosshair;background:#111820}#spectrum,#waterfall{cursor:crosshair}
@media(max-width:760px){main{grid-template-rows:minmax(90px,35%) 34px minmax(140px,1fr)}.controls{font-size:12px}.capture{grid-template-columns:auto 1fr auto}}
</style></head><body>
<header><div class="controls">
<strong>Obrii SDR Remote VFO</strong>
<label>MHz <input id="freq" type="number" step="0.000001" value="100.000000"></label>
<label>Mode <select id="mode"><option value="0">AM</option><option value="1" selected>NFM</option><option value="2">SAM</option><option value="3">USB</option><option value="4">LSB</option><option value="5">DSB</option><option value="6">CW</option><option value="7">WFM</option></select></label>
<label>BW Hz <input id="bw" type="number" min="100" max="250000" value="12500"></label>
<label>SQL dB <input id="sql" type="number" min="-200" max="0" value="-160"></label>
<label><input id="audio" type="checkbox" checked> Audio</label>
<button id="start" class="run">Start VFO</button><button id="stop" class="stop">Stop</button>
</div><div class="capture"><span id="capmin">capture min</span><input id="captureTune" type="range" min="0" max="1" step="1"><span id="capmax">capture max</span></div></header>
<main><canvas id="spectrum"></canvas><canvas id="scale"></canvas><canvas id="waterfall"></canvas></main><footer id="status">Connecting...</footer>
<script>
const $=id=>document.getElementById(id),status=$('status'),sc=$('spectrum'),fc=$('scale'),wc=$('waterfall'),sx=sc.getContext('2d'),fx=fc.getContext('2d'),wx=wc.getContext('2d');
let ws,active=false,audioCtx,node,queue=[],lastLevels=[],lastFrame=null,captureMin=0,captureMax=0,sendTimer=0,dragging=false;
function resize(){for(const c of [sc,fc,wc]){const r=c.getBoundingClientRect(),w=Math.max(2,Math.floor(r.width*devicePixelRatio)),h=Math.max(2,Math.floor(r.height*devicePixelRatio));if(c.width!==w||c.height!==h){c.width=w;c.height=h}}if(lastFrame)drawScale(lastFrame)}addEventListener('resize',resize);resize();
function settings(){return{vfoId:'main',settings:{listeningFrequency:Number($('freq').value)*1e6,bandwidth:Number($('bw').value),modulationType:Number($('mode').value),squelchDb:Number($('sql').value),audioEnabled:$('audio').checked,spectrumEnabled:true,fftLength:2048,refreshMs:100}}}
function send(action){if(ws&&ws.readyState===1)ws.send(JSON.stringify(Object.assign({action},settings())))}
function scheduleUpdate(delay=80){clearTimeout(sendTimer);sendTimer=setTimeout(()=>{if(active)send('update')},delay)}
function formatHz(v){const a=Math.abs(v);return a>=1e9?(v/1e9).toFixed(6)+' GHz':a>=1e6?(v/1e6).toFixed(6)+' MHz':a>=1e3?(v/1e3).toFixed(3)+' kHz':v.toFixed(0)+' Hz'}
function tuneBounds(){const half=Math.max(0,Number($('bw').value)||0)/2;return{lo:captureMin+half,hi:captureMax-half}}
function setFrequency(hz,notify=true){if(!Number.isFinite(hz))return;if(captureMax>captureMin){const b=tuneBounds();hz=b.hi>=b.lo?Math.max(b.lo,Math.min(b.hi,hz)):Math.max(captureMin,Math.min(captureMax,hz))}$('freq').value=(hz/1e6).toFixed(6);$('captureTune').value=String(Math.round(hz));if(lastFrame)drawScale(lastFrame);if(notify)scheduleUpdate()}
function commitTypedFrequency(){const hz=Number($('freq').value)*1e6;if(Number.isFinite(hz))setFrequency(hz);else if(lastFrame)setFrequency(Number(lastFrame.centerFrequency),false)}
function setCapture(c){if(!Number.isFinite(c.minFrequency)||!Number.isFinite(c.maxFrequency)||c.maxFrequency<=c.minFrequency)return;captureMin=c.minFrequency;captureMax=c.maxFrequency;const tune=$('captureTune');tune.min=String(Math.ceil(captureMin));tune.max=String(Math.floor(captureMax));tune.step=String(Math.max(1,Math.round((captureMax-captureMin)/200000)));$('capmin').textContent=formatHz(captureMin);$('capmax').textContent=formatHz(captureMax);setFrequency(Number($('freq').value)*1e6,false)}
function connect(){ws=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws');ws.binaryType='arraybuffer';ws.onopen=()=>{status.textContent='Connected';ws.send(JSON.stringify({action:'open',clientName:navigator.userAgent.slice(0,80)}))};ws.onclose=()=>{status.textContent='Disconnected; retrying';active=false;queue=[];setTimeout(connect,1200)};ws.onmessage=e=>{if(typeof e.data==='string'){const m=JSON.parse(e.data);if(m.type==='spectrum')draw(m);else if(m.type==='remoteVfo'){if(m.action==='error')status.textContent=m.message;else if(m.action==='audioReset')queue=[];else if(m.action==='branchStatus'&&m.error)status.textContent=m.error;else if(m.capabilities){const c=m.capabilities;setCapture(c);if(!active&&c.centerFrequency)setFrequency(c.centerFrequency,false);const v=(m.vfos||[]).find(x=>x.vfoId==='main');if(v&&Number.isFinite(v.frequencyHz))setFrequency(v.frequencyHz,false);status.textContent=`${m.role||'listener'} | ${m.server.activeVfos}/${m.server.maxVfos} VFO | RX ${c.receiverRunning?'running':'stopped'} | capture ${formatHz(c.minFrequency)} - ${formatHz(c.maxFrequency)}`}}}else pushAudio(e.data)}}
function ensureAudio(){if(audioCtx)return;audioCtx=new (window.AudioContext||window.webkitAudioContext)({sampleRate:48000});node=audioCtx.createScriptProcessor(4096,0,1);node.onaudioprocess=e=>{const out=e.outputBuffer.getChannelData(0);out.fill(0);let p=0;while(p<out.length&&queue.length){const q=queue[0],n=Math.min(out.length-p,q.length);out.set(q.subarray(0,n),p);p+=n;queue[0]=q.subarray(n);if(!queue[0].length)queue.shift()}};node.connect(audioCtx.destination)}
function pushAudio(b){if(!$('audio').checked)return;ensureAudio();const v=new Int16Array(b),f=new Float32Array(v.length);for(let i=0;i<v.length;i++)f[i]=v[i]/32768;queue.push(f);while(queue.length>24)queue.shift()}
function drawScale(m){const lo=Number(m.minFrequency),hi=Number(m.maxFrequency);if(!Number.isFinite(lo)||!Number.isFinite(hi)||hi<=lo)return;fx.clearRect(0,0,fc.width,fc.height);fx.strokeStyle='#607083';fx.fillStyle='#cbd5df';fx.font=`${Math.max(10,Math.round(11*devicePixelRatio))}px system-ui`;fx.textBaseline='bottom';const ticks=Math.max(3,Math.min(9,Math.floor(fc.width/(135*devicePixelRatio))));for(let i=0;i<ticks;i++){const t=i/(ticks-1),x=t*(fc.width-1),hz=lo+t*(hi-lo);fx.beginPath();fx.moveTo(x,0);fx.lineTo(x,9*devicePixelRatio);fx.stroke();fx.textAlign=i===0?'left':i===ticks-1?'right':'center';fx.fillText(formatHz(hz),x,fc.height-2*devicePixelRatio)}const tuned=Number($('freq').value)*1e6,x=(tuned-lo)/(hi-lo)*fc.width;if(x>=0&&x<=fc.width){fx.fillStyle='#ff4b45';fx.beginPath();fx.moveTo(x,0);fx.lineTo(x-6*devicePixelRatio,8*devicePixelRatio);fx.lineTo(x+6*devicePixelRatio,8*devicePixelRatio);fx.closePath();fx.fill()}}
function draw(m){resize();lastFrame=m;const raw=m.magnitudes||[];if(!raw.length)return;const a=m.magnitudeOrder==='fft-native'?raw.map((_,i)=>raw[(i+Math.floor(raw.length/2))%raw.length]):raw;lastLevels=a;sx.clearRect(0,0,sc.width,sc.height);sx.strokeStyle='#59d98e';sx.lineWidth=Math.max(1,devicePixelRatio);sx.beginPath();for(let i=0;i<a.length;i++){const x=i/(a.length-1)*sc.width,y=(1-Math.max(0,Math.min(1,(a[i]+140)/120)))*sc.height;i?sx.lineTo(x,y):sx.moveTo(x,y)}sx.stroke();drawScale(m);const row=wx.createImageData(wc.width,1);for(let x=0;x<wc.width;x++){const v=a[Math.min(a.length-1,Math.floor(x/wc.width*a.length))],t=Math.max(0,Math.min(1,(v+140)/110)),i=x*4;row.data[i]=Math.floor(255*Math.max(0,(t-.55)*2.2));row.data[i+1]=Math.floor(255*Math.max(0,1-Math.abs(t-.62)*2.3));row.data[i+2]=Math.floor(255*Math.max(0,1-t*1.25));row.data[i+3]=255}wx.drawImage(wc,0,0,wc.width,wc.height,0,1,wc.width,wc.height-1);wx.putImageData(row,0,0)}
function tuneFromPointer(e){if(!lastFrame)return;const r=e.currentTarget.getBoundingClientRect(),t=Math.max(0,Math.min(1,(e.clientX-r.left)/r.width)),lo=Number(lastFrame.minFrequency),hi=Number(lastFrame.maxFrequency);setFrequency(lo+t*(hi-lo))}
function wheelTune(e){e.preventDefault();const base=Math.max(1,Number($('bw').value)/20),factor=e.ctrlKey?0.1:e.shiftKey?10:1;setFrequency(Number($('freq').value)*1e6+(e.deltaY<0?1:-1)*base*factor)}
for(const c of [sc,fc,wc]){c.addEventListener('pointerdown',e=>{dragging=true;c.setPointerCapture(e.pointerId);tuneFromPointer(e)});c.addEventListener('pointermove',e=>{if(dragging)tuneFromPointer(e)});c.addEventListener('pointerup',()=>dragging=false);c.addEventListener('pointercancel',()=>dragging=false);c.addEventListener('wheel',wheelTune,{passive:false})}
$('captureTune').oninput=e=>setFrequency(Number(e.target.value));$('freq').oninput=()=>scheduleUpdate(250);$('freq').onchange=commitTypedFrequency;$('freq').onkeydown=e=>{if(e.key==='Enter'){e.preventDefault();commitTypedFrequency();$('freq').blur()}};$('start').onclick=()=>{ensureAudio();audioCtx.resume();active=true;send('activate')};$('stop').onclick=()=>{active=false;queue=[];send('deactivate')};for(const id of ['mode','sql','audio'])$(id).onchange=scheduleUpdate;$('bw').onchange=()=>{commitTypedFrequency();scheduleUpdate()};connect();
</script></body></html>)HTML");
}
