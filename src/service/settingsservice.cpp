#include "service/settingsservice.h"

#include "infrastructure/databasemanager.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <wincrypt.h>
#endif

SettingsService::SettingsService(DatabaseManager* db, QObject* parent)
    : QObject(parent)
    , m_db(db)
{
}

// ---------------------------- 非敏感配置 ----------------------------
QString SettingsService::get(const QString& key, const QString& defaultValue) const
{
    if (!m_db) return defaultValue;
    const auto rows = m_db->query(
        QStringLiteral("SELECT value FROM settings WHERE key = ?"), { key });
    if (rows.isEmpty()) return defaultValue;
    return rows.first().value(QStringLiteral("value")).toString();
}

void SettingsService::set(const QString& key, const QString& value)
{
    if (!m_db) return;
    m_db->exec(
        QStringLiteral("INSERT INTO settings(key, value) VALUES(?, ?) "
                       "ON CONFLICT(key) DO UPDATE SET value = excluded.value"),
        { key, value });
    emit settingChanged(key, value);
}

// ---------------------------- 敏感凭证 ----------------------------
QString SettingsService::secretFilePath(const QString& name)
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(base + QStringLiteral("/secrets"));
    // 文件名用 hash，避免凭证名本身泄露语义到文件系统
    const QString safe = QString::fromLatin1(
        QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
    return base + QStringLiteral("/secrets/") + safe + QStringLiteral(".bin");
}

#ifdef Q_OS_WIN

QString SettingsService::secretGet(const QString& name) const
{
    QFile f(secretFilePath(name));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray blob = f.readAll();
    f.close();
    if (blob.isEmpty()) return {};

    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(blob.constData()));
    in.cbData = static_cast<DWORD>(blob.size());
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
        return {};
    }
    const QByteArray plain(reinterpret_cast<const char*>(out.pbData),
                           static_cast<int>(out.cbData));
    LocalFree(out.pbData);
    return QString::fromUtf8(plain);
}

bool SettingsService::secretSet(const QString& name, const QString& value)
{
    const QByteArray plain = value.toUtf8();
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB out{};
    // CRYPTPROTECT_LOCAL_MACHINE = false（仅当前用户可解密）
    if (!CryptProtectData(&in, L"FrameMind secret", nullptr, nullptr, nullptr,
                          0, &out)) {
        return false;
    }
    const QByteArray blob(reinterpret_cast<const char*>(out.pbData),
                          static_cast<int>(out.cbData));
    LocalFree(out.pbData);

    QFile f(secretFilePath(name));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(blob);
    f.close();
    return true;
}

#else  // 非 Windows：M2 暂以明文文件兜底（M5 接 Keychain / libsecret）

QString SettingsService::secretGet(const QString& name) const
{
    QFile f(secretFilePath(name));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray data = f.readAll();
    return QString::fromUtf8(data);
}

bool SettingsService::secretSet(const QString& name, const QString& value)
{
    QFile f(secretFilePath(name));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(value.toUtf8());
    return true;
}

#endif

bool SettingsService::secretDelete(const QString& name)
{
    return QFile::remove(secretFilePath(name));
}
