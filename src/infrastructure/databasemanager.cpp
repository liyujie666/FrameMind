#include "infrastructure/databasemanager.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QFileInfo>
#include <QDir>
#include <QVariant>

DatabaseManager* DatabaseManager::instance()
{
    static DatabaseManager s_instance;
    return &s_instance;
}

DatabaseManager::~DatabaseManager()
{
    if (m_db.isOpen()) {
        m_db.close();
    }
}

bool DatabaseManager::initialize(const QString& dbPath)
{
    QFileInfo fi(dbPath);
    QDir().mkpath(fi.absolutePath());

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));
    m_db.setDatabaseName(dbPath);
    if (!m_db.open()) {
        m_lastError = m_db.lastError().text();
        return false;
    }

    // 开启外键约束（默认关闭）
    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON;"));

    createTables();
    return true;
}

bool DatabaseManager::isOpen() const
{
    return m_db.isOpen();
}

void DatabaseManager::createTables()
{
    static const char* kStatements[] = {
        // 对话表
        "CREATE TABLE IF NOT EXISTS conversations ("
        "  id TEXT PRIMARY KEY,"
        "  title TEXT NOT NULL DEFAULT '新对话',"
        "  video_path TEXT,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP)",
        // 消息表
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id TEXT PRIMARY KEY,"
        "  conversation_id TEXT NOT NULL,"
        "  role TEXT NOT NULL CHECK(role IN ('user','assistant','system')),"
        "  content TEXT NOT NULL,"
        "  attached_frames TEXT,"
        "  timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE)",
        // 视频分析缓存
        "CREATE TABLE IF NOT EXISTS analysis_cache ("
        "  video_path TEXT NOT NULL,"
        "  analysis_type TEXT NOT NULL,"
        "  data TEXT NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  PRIMARY KEY (video_path, analysis_type))",
        // 设置表（仅非敏感配置）
        "CREATE TABLE IF NOT EXISTS settings ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL)",
        // 最近文件
        "CREATE TABLE IF NOT EXISTS recent_files ("
        "  path TEXT PRIMARY KEY,"
        "  last_opened DATETIME DEFAULT CURRENT_TIMESTAMP)",
        // 索引
        "CREATE INDEX IF NOT EXISTS idx_messages_conv "
        "  ON messages(conversation_id, timestamp)",
        "CREATE INDEX IF NOT EXISTS idx_recent_files_time "
        "  ON recent_files(last_opened DESC)",
    };

    for (const char* sql : kStatements) {
        QSqlQuery q(m_db);
        if (!q.exec(QString::fromUtf8(sql))) {
            m_lastError = q.lastError().text();
        }
    }
}

bool DatabaseManager::exec(const QString& sql, const QVariantList& bindings)
{
    QSqlQuery q(m_db);
    if (!q.prepare(sql)) {
        m_lastError = q.lastError().text();
        return false;
    }
    for (const QVariant& v : bindings) {
        q.addBindValue(v);
    }
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

QList<QVariantMap> DatabaseManager::query(const QString& sql, const QVariantList& bindings)
{
    QList<QVariantMap> rows;
    QSqlQuery q(m_db);
    if (!q.prepare(sql)) {
        m_lastError = q.lastError().text();
        return rows;
    }
    for (const QVariant& v : bindings) {
        q.addBindValue(v);
    }
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return rows;
    }
    while (q.next()) {
        QVariantMap row;
        const QSqlRecord rec = q.record();
        for (int i = 0; i < rec.count(); ++i) {
            row.insert(rec.fieldName(i), q.value(i));
        }
        rows.append(row);
    }
    return rows;
}
