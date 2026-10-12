#ifndef DMRPRIVACYUTILS_H
#define DMRPRIVACYUTILS_H

#include <QString>
#include <QVector>

constexpr int DMR_PRIVACY_NONE = 0;
constexpr int DMR_PRIVACY_ARC4 = 1;
constexpr int DMR_PRIVACY_AES256 = 2;
constexpr int DMR_PRIVACY_HYTERA_BASIC = 3;

struct DmrPrivacyKeyEntry {
    bool active = false;
    int mode = 0;
    int keyId = 0;
    QString keyHex;
    QString note;
};

QString dmrPrivacyModeId(int mode);
int dmrPrivacyModeFromText(const QString &text);
int normalizedDmrPrivacyMode(int mode);
QString normalizedDmrPrivacyKeyHex(const QString &text);
bool isValidHyteraBasicKey(const QString &text);
int findDmrPrivacyKeyIndexById(const QVector<DmrPrivacyKeyEntry> &keys, int keyId);
int findDmrPrivacyKeyIndexByModeAndId(const QVector<DmrPrivacyKeyEntry> &keys, int mode, int keyId);

#endif // DMRPRIVACYUTILS_H
