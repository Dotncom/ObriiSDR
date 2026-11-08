#ifndef DMRPRIVACYCODEC_H
#define DMRPRIVACYCODEC_H

#include <QByteArray>
#include <QString>
#include <array>

namespace DmrPrivacyCodec {
struct PiHeader {
    int algorithm = 0;
    int manufacturer = 0;
    int keyId = 0;
    quint32 mi = 0;
    int correctedBits = 0;
};
bool decodePi(const QString &dibits, PiHeader &header);
int lateEntryNibble(const QString &rawAmbe72);
bool decodeLateEntry(const std::array<int, 18> &fragments, quint32 &mi);
quint32 nextMi(quint32 mi, bool aes);
QByteArray aesIv(quint32 mi);
QByteArray aes256Ofb(const QByteArray &key, const QByteArray &iv, int bytes);
QByteArray rc4(const QByteArray &key, int drop, int bytes);
QByteArray voiceKeystream(int mode, const QByteArray &key, quint32 mi);
}

#endif
