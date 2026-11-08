#ifndef SSTVIMAGEEDITORDIALOG_H
#define SSTVIMAGEEDITORDIALOG_H

#include <QDialog>
#include <QImage>
#include <QSize>

class QComboBox;
class QLabel;

class SstvImageEditorDialog final : public QDialog {
public:
    SstvImageEditorDialog(const QImage &image,
                          const QSize &targetSize,
                          const QString &language,
                          QWidget *parent = nullptr);

    QImage preparedImage() const;

private:
    QString text(const QString &english, const QString &ukrainian) const;
    void updatePreparedImage();

    QImage originalImage;
    QImage outputImage;
    QSize outputSize;
    QString language;
    int quarterTurns = 0;
    QLabel *previewLabel = nullptr;
    QLabel *sizeLabel = nullptr;
    QComboBox *fitModeCombo = nullptr;
};

#endif // SSTVIMAGEEDITORDIALOG_H
