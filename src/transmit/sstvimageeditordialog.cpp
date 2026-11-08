#include "sstvimageeditordialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QTransform>
#include <QVBoxLayout>

SstvImageEditorDialog::SstvImageEditorDialog(const QImage &image,
                                             const QSize &targetSize,
                                             const QString &language,
                                             QWidget *parent)
    : QDialog(parent),
      originalImage(image.convertToFormat(QImage::Format_RGB32)),
      outputSize(targetSize),
      language(language) {
    setWindowTitle(text(QStringLiteral("Prepare transmission image"),
                        QString::fromUtf8(u8"Підготовка зображення для передачі")));
    resize(650, 560);

    auto *root = new QVBoxLayout(this);
    auto *tools = new QHBoxLayout();
    auto *rotateLeft = new QPushButton(text(QStringLiteral("Rotate left"),
                                            QString::fromUtf8(u8"Повернути ліворуч")), this);
    auto *rotateRight = new QPushButton(text(QStringLiteral("Rotate right"),
                                             QString::fromUtf8(u8"Повернути праворуч")), this);
    auto *reset = new QPushButton(text(QStringLiteral("Reset"),
                                      QString::fromUtf8(u8"Скинути")), this);
    fitModeCombo = new QComboBox(this);
    fitModeCombo->addItem(text(QStringLiteral("Fit with black borders"),
                               QString::fromUtf8(u8"Вписати з чорними полями")),
                          QStringLiteral("fit"));
    fitModeCombo->addItem(text(QStringLiteral("Crop to fill frame"),
                               QString::fromUtf8(u8"Обрізати до заповнення")),
                          QStringLiteral("crop"));
    tools->addWidget(rotateLeft);
    tools->addWidget(rotateRight);
    tools->addWidget(reset);
    tools->addSpacing(12);
    tools->addWidget(fitModeCombo, 1);

    previewLabel = new QLabel(this);
    previewLabel->setAlignment(Qt::AlignCenter);
    previewLabel->setMinimumSize(480, 360);
    previewLabel->setStyleSheet(QStringLiteral("background: #080a0c; border: 1px solid #3b424a;"));
    sizeLabel = new QLabel(this);
    sizeLabel->setAlignment(Qt::AlignCenter);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(
        text(QStringLiteral("Use image"), QString::fromUtf8(u8"Використати зображення")));
    buttons->button(QDialogButtonBox::Cancel)->setText(
        text(QStringLiteral("Cancel"), QString::fromUtf8(u8"Скасувати")));

    root->addLayout(tools);
    root->addWidget(previewLabel, 1);
    root->addWidget(sizeLabel);
    root->addWidget(buttons);

    connect(rotateLeft, &QPushButton::clicked, this, [this]() {
        quarterTurns = (quarterTurns + 3) % 4;
        updatePreparedImage();
    });
    connect(rotateRight, &QPushButton::clicked, this, [this]() {
        quarterTurns = (quarterTurns + 1) % 4;
        updatePreparedImage();
    });
    connect(reset, &QPushButton::clicked, this, [this]() {
        quarterTurns = 0;
        fitModeCombo->setCurrentIndex(0);
        updatePreparedImage();
    });
    connect(fitModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { updatePreparedImage(); });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updatePreparedImage();
}

QImage SstvImageEditorDialog::preparedImage() const {
    return outputImage;
}

QString SstvImageEditorDialog::text(const QString &english, const QString &ukrainian) const {
    return language.toLower().startsWith(QStringLiteral("uk")) ? ukrainian : english;
}

void SstvImageEditorDialog::updatePreparedImage() {
    if (originalImage.isNull() || outputSize.isEmpty()) {
        outputImage = QImage();
        previewLabel->clear();
        return;
    }
    QTransform transform;
    transform.rotate(quarterTurns * 90.0);
    const QImage rotated = originalImage.transformed(transform, Qt::SmoothTransformation);
    const bool crop = fitModeCombo->currentData().toString() == QStringLiteral("crop");
    outputImage = QImage(outputSize, QImage::Format_RGB32);
    outputImage.fill(Qt::black);
    const QImage scaled = rotated.scaled(outputSize,
                                         crop ? Qt::KeepAspectRatioByExpanding
                                              : Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation);
    QPainter painter(&outputImage);
    const QPoint topLeft((outputSize.width() - scaled.width()) / 2,
                         (outputSize.height() - scaled.height()) / 2);
    painter.drawImage(topLeft, scaled);
    painter.end();

    const QPixmap preview = QPixmap::fromImage(outputImage).scaled(
        previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    previewLabel->setPixmap(preview);
    sizeLabel->setText(text(QStringLiteral("Output: %1 x %2 pixels"),
                            QString::fromUtf8(u8"Вихід: %1 x %2 пікселів"))
                           .arg(outputSize.width())
                           .arg(outputSize.height()));
}
