#ifndef FRAMEMIND_DATABASEMANAGER_H
#define FRAMEMIND_DATABASEMANAGER_H

#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QList>
#include <QSqlDatabase>

/**
 * SQLite 数据库封装（architecture-design.md §八）。
 *
 * 安全：所有动态参数一律走绑定（exec/query 的 bindings），禁止字符串拼 SQL。
 */
class DatabaseManager {
public:
    static DatabaseManager* instance();

    /// 打开数据库并幂等建表；dbPath 所在目录会被创建
    bool initialize(const QString& dbPath);
    bool isOpen() const;

    /// 执行写入/DDL，bindings 按顺序绑定到 SQL 中的 '?'
    bool exec(const QString& sql, const QVariantList& bindings = {});

    /// 执行查询，返回行列表（列名 → 值）
    QList<QVariantMap> query(const QString& sql, const QVariantList& bindings = {});

    QString lastError() const { return m_lastError; }

private:
    DatabaseManager() = default;
    ~DatabaseManager();
    Q_DISABLE_COPY(DatabaseManager)

    void createTables();
    /// 兼容旧库：检查表中是否存在某列，缺则 ALTER TABLE 添加
    void ensureColumn(const QString& table, const QString& column,
                      const QString& definition);

    QSqlDatabase m_db;
    QString      m_lastError;
};

#endif // FRAMEMIND_DATABASEMANAGER_H
