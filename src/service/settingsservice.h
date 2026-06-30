#ifndef FRAMEMIND_SETTINGSSERVICE_H
#define FRAMEMIND_SETTINGSSERVICE_H

#include <QObject>
#include <QString>
#include <QVariant>

class DatabaseManager;

/**
 * 配置与密钥管理（M2 最小版，M5-T2 扩展为完整三平台实现）。
 *
 * - 非敏感配置：SQLite settings 表（key/value）。
 * - 敏感凭证（API Key 等）：走 OS 密钥服务，禁止入 SQLite / 配置 / 日志。
 *     Windows：DPAPI（CryptProtectData），密文落 AppData/secrets/<name>.bin。
 *     其他平台：M2 暂以同样的 DPAPI-缺省 stub 兜底（M5 接 Keychain/libsecret）。
 */
class SettingsService : public QObject {
    Q_OBJECT
public:
    explicit SettingsService(DatabaseManager* db, QObject* parent = nullptr);

    // ---- 非敏感配置 ----
    QString get(const QString& key, const QString& defaultValue = {}) const;
    void    set(const QString& key, const QString& value);

    // ---- 敏感凭证 ----
    QString secretGet(const QString& name) const;
    bool    secretSet(const QString& name, const QString& value);
    bool    secretDelete(const QString& name);

signals:
    void settingChanged(const QString& key, const QString& value);

private:
    static QString secretFilePath(const QString& name);

    DatabaseManager* m_db = nullptr;
};

#endif // FRAMEMIND_SETTINGSSERVICE_H
