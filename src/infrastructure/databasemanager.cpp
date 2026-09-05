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
        "  video_id TEXT,"
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
        "  last_opened DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  duration_ms INTEGER NOT NULL DEFAULT 0)",
        // 视频元数据表
        "CREATE TABLE IF NOT EXISTS video_metadata ("
        "  video_id TEXT PRIMARY KEY,"
        "  video_path TEXT NOT NULL,"
        "  video_summary TEXT,"
        "  index_level INTEGER NOT NULL DEFAULT 0,"
        "  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP)",
        // 场景描述表
        "CREATE TABLE IF NOT EXISTS scene_descriptions ("
        "  video_id TEXT NOT NULL,"
        "  scene_id INTEGER NOT NULL,"
        "  description TEXT,"
        "  visual_description TEXT,"
        "  PRIMARY KEY (video_id, scene_id))",
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

    // 兼容升级：旧库可能没有 duration_ms；缺则 ALTER 加上
    ensureColumn(QStringLiteral("recent_files"),
                 QStringLiteral("duration_ms"),
                 QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
    // 兼容升级：旧库可能没有 video_id；缺则 ALTER 加上
    ensureColumn(QStringLiteral("conversations"),
                 QStringLiteral("video_id"),
                 QStringLiteral("TEXT"));
    // 兼容升级：添加 elapsed_ms 字段用于记录消息耗时
    ensureColumn(QStringLiteral("messages"),
                 QStringLiteral("elapsed_ms"),
                 QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
}

void DatabaseManager::ensureColumn(const QString& table,
                                   const QString& column,
                                   const QString& definition)
{
    // PRAGMA 不支持 ? 绑定，需要拼接（此处 table/column/definition 均为内部硬编码常量）
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) return;
    bool exists = false;
    while (q.next()) {
        if (q.value(1).toString().compare(column, Qt::CaseInsensitive) == 0) {
            exists = true;
            break;
        }
    }
    if (exists) return;

    QSqlQuery alter(m_db);
    alter.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3")
               .arg(table, column, definition));
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

bool DatabaseManager::saveVideoMetadata(const QString& videoId,
                                        const QString& videoPath,
                                        const QString& summary,
                                        int indexLevel)
{
    return exec(QStringLiteral(
        "INSERT INTO video_metadata (video_id, video_path, video_summary, index_level, updated_at) "
        "VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP) "
        "ON CONFLICT(video_id) DO UPDATE SET "
        "  video_path = excluded.video_path, "
        "  video_summary = excluded.video_summary, "
        "  index_level = excluded.index_level, "
        "  updated_at = CURRENT_TIMESTAMP"),
        {videoId, videoPath, summary, indexLevel});
}

bool DatabaseManager::saveSceneDescription(const QString& videoId,
                                           int sceneId,
                                           const QString& description,
                                           const QString& visualDescription)
{
    return exec(QStringLiteral(
        "INSERT INTO scene_descriptions (video_id, scene_id, description, visual_description) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(video_id, scene_id) DO UPDATE SET "
        "  description = excluded.description, "
        "  visual_description = excluded.visual_description"),
        {videoId, sceneId, description, visualDescription});
}

QString DatabaseManager::loadVideoSummary(const QString& videoId)
{
    const auto rows = query(
        QStringLiteral("SELECT video_summary FROM video_metadata WHERE video_id = ?"),
        {videoId});
    if (rows.isEmpty()) return {};
    return rows.first().value(QStringLiteral("video_summary")).toString();
}

QMap<int, QString> DatabaseManager::loadSceneDescriptions(const QString& videoId)
{
    QMap<int, QString> result;
    const auto rows = query(
        QStringLiteral("SELECT scene_id, description FROM scene_descriptions WHERE video_id = ?"),
        {videoId});
    for (const auto& row : rows) {
        const int sceneId = row.value(QStringLiteral("scene_id")).toInt();
        const QString desc = row.value(QStringLiteral("description")).toString();
        if (!desc.isEmpty()) {
            result.insert(sceneId, desc);
        }
    }
    return result;
}

QMap<int, QString> DatabaseManager::loadSceneVisualDescriptions(const QString& videoId)
{
    QMap<int, QString> result;
    const auto rows = query(
        QStringLiteral("SELECT scene_id, visual_description FROM scene_descriptions WHERE video_id = ?"),
        {videoId});
    for (const auto& row : rows) {
        const int sceneId = row.value(QStringLiteral("scene_id")).toInt();
        const QString desc = row.value(QStringLiteral("visual_description")).toString();
        if (!desc.isEmpty()) {
            result.insert(sceneId, desc);
        }
    }
    return result;
}
