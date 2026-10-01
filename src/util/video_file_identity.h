#pragma once
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
class VideoFileIdentity {
  public:
    static QString legacyId(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QString::fromLatin1(
                QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex());
        QCryptographicHash hash(QCryptographicHash::Sha1);
        hash.addData(QByteArray::number(file.size()));
        hash.addData(file.read(1024 * 1024));
        return QString::fromLatin1(hash.result().toHex()).left(16);
    }
    static QString fingerprint(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        const QFileInfo info(file);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(QByteArray::number(file.size()));
        hash.addData(QByteArray::number(info.lastModified().toMSecsSinceEpoch()));
        for (int i = 0; i < 5; ++i) {
            file.seek(qMax<qint64>(0, (file.size() - 65536) * i / 4));
            hash.addData(file.read(65536));
        }
        return QString::fromLatin1(hash.result().toHex());
    }
};
