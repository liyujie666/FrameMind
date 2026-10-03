#pragma once
#include "model/video_rag_types.h"
#include <QHash>

struct VideoPresentationPolicy {
    QString id, version = QStringLiteral("policy_v1"), mode;
    VideoContentType primaryType = VideoContentType::Unknown;
    QString primaryKind, primaryTitle, secondaryKind, secondaryTitle, guidance;
    QStringList primaryEntryKinds, secondaryEntryKinds, pointRoles, intents;
    QHash<QString, QStringList> attributeKeys;
    int targetEntries = 5, targetQuestions = 3;
    QString validationError(const VideoPresentation &, const VideoPresentationValidationContext &) const;
    QJsonObject toJson() const;
};
class VideoPresentationPolicyRegistry {
public:
    // demo must be selected from evidence by the coordinator in P6, not guessed here.
    static VideoPresentationPolicy resolve(VideoContentType, bool demo = false);
    static VideoPresentationPolicy byId(const QString &);
    static QVector<VideoPresentationPolicy> all();
};
