#ifndef FRAMEMIND_ENTITY_PROFILE_H
#define FRAMEMIND_ENTITY_PROFILE_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMetaType>
#include <cstdint>
#include <vector>

/**
 * 实体档案（agent-core-design.md §6.3）。
 *
 * 用于维护整个视频中同一实体的身份一致性：
 *   场景1的"红衣人" = 场景3的"厨师" = 用户问的"那个人"
 *
 * 由 EntityTracker 建立与维护。
 */
struct EntityAppearance {
    int     sceneId = -1;
    int64_t timestampMs = 0;
    QString description;            // 该次出现的描述
    // 归一化坐标 [0,1]，如果检测器提供
    double bboxX = -1.0, bboxY = -1.0, bboxW = 0.0, bboxH = 0.0;
};

struct EntityProfile {
    /// 实体类型
    enum EntityType {
        Person,
        Object,
        Location,
        Text,             // 画面中的文字/标志
        Unknown
    };

    QString    id;                      // "person_1" / "object_3" 等
    QString    videoId;
    EntityType type = Unknown;

    QString primaryDescription;         // 主描述（如"穿蓝色围裙的中年男性"）
    QStringList aliases;                // 别名（"厨师"、"那个男的"...）

    QVector<EntityAppearance> appearances;

    /// 该实体的语义 embedding（供检索匹配 / 共指消解）
    std::vector<float> descriptionEmbedding;

    static QString typeToString(EntityType t)
    {
        switch (t) {
        case Person:   return QStringLiteral("person");
        case Object:   return QStringLiteral("object");
        case Location: return QStringLiteral("location");
        case Text:     return QStringLiteral("text");
        default:       return QStringLiteral("unknown");
        }
    }

    bool isValid() const { return !id.isEmpty(); }
    int64_t firstAppearMs() const { return appearances.isEmpty() ? -1 : appearances.first().timestampMs; }
    int64_t lastAppearMs()  const { return appearances.isEmpty() ? -1 : appearances.last().timestampMs;  }
};

Q_DECLARE_METATYPE(EntityProfile)

#endif // FRAMEMIND_ENTITY_PROFILE_H
