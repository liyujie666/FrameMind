#include "service/rag/video_presentation_policy_registry.h"

QVector<VideoPresentationPolicy> VideoPresentationPolicyRegistry::all() {
    QVector<VideoPresentationPolicy> result;
    {
        VideoPresentationPolicy p;
        p.id = "generic_v1"; p.primaryType = VideoContentType::Unknown;
        p.guidance = QStringLiteral("保守整理重要主题、事件与变化，不推断未展示的背景。");
        p.primaryKind = "highlight"; p.primaryTitle = QStringLiteral("值得回看");
        p.secondaryKind = "explore"; p.secondaryTitle = QStringLiteral("继续探索");
        p.primaryEntryKinds = {"topic", "event", "change"}; p.pointRoles = {"topic", "event", "change", "explanation"};
        p.intents = {"understand", "compare", "explore"}; p.targetEntries = 5;
        result.append(p);
    }
    {
        VideoPresentationPolicy p;
        p.id = "tutorial_v1"; p.primaryType = VideoContentType::Tutorial;
        p.guidance = QStringLiteral("只有真实操作证据才写操作步骤，区分讲述、可见操作与前后状态；追问围绕已展示问题。");
        p.primaryKind = "operation_notes"; p.primaryTitle = QStringLiteral("操作笔记");
        p.secondaryKind = "practice"; p.secondaryTitle = QStringLiteral("实践与追问");
        p.primaryEntryKinds = {"goal", "prerequisite", "step", "caution", "result"}; p.pointRoles = {"goal", "prerequisite", "operation", "caution", "result", "explanation"};
        p.intents = {"explain_step", "execution_checklist", "analyze_problem"}; p.targetEntries = 10;
        result.append(p);
    }
    {
        VideoPresentationPolicy p;
        p.id = "educational_v1"; p.primaryType = VideoContentType::Educational;
        p.guidance = QStringLiteral("组织概念、解释、例子与推导；自测答案必须能从视频获得，公式不清晰时不补造。");
        p.primaryKind = "knowledge_notes"; p.primaryTitle = QStringLiteral("知识笔记");
        p.secondaryKind = "self_test"; p.secondaryTitle = QStringLiteral("复习与自测");
        p.primaryEntryKinds = {"concept", "explanation", "example", "derivation", "relationship"}; p.pointRoles = {"concept", "definition", "explanation", "example", "derivation", "relationship"};
        p.intents = {"understand", "compare", "apply"}; p.targetEntries = 10;
        result.append(p);
    }
    {
        VideoPresentationPolicy p;
        p.id = "interview_v1"; p.primaryType = VideoContentType::Interview;
        p.guidance = QStringLiteral("保留问题与回答、观点与论据的关系；未知说话人身份不猜测，不生成会议待办。");
        p.primaryKind = "viewpoints"; p.primaryTitle = QStringLiteral("观点整理");
        p.secondaryKind = "discussion"; p.secondaryTitle = QStringLiteral("深入讨论");
        p.primaryEntryKinds = {"question_answer", "viewpoint", "argument", "case", "disagreement"}; p.pointRoles = {"question", "answer", "viewpoint", "reason", "case", "disagreement"};
        p.intents = {"probe_evidence", "conditions", "compare_views"}; p.targetEntries = 5;
        result.append(p);
    }
    {
        VideoPresentationPolicy p;
        p.id = "presentation_v1"; p.primaryType = VideoContentType::Presentation;
        p.guidance = QStringLiteral("围绕核心结论、支撑材料、数据和例子整理；不能将推测作为演讲者结论。");
        p.primaryKind = "arguments"; p.primaryTitle = QStringLiteral("核心论点");
        p.secondaryKind = "inquiry"; p.secondaryTitle = QStringLiteral("理解与追问");
        p.primaryEntryKinds = {"conclusion", "argument", "data", "example"}; p.pointRoles = {"conclusion", "argument", "evidence", "data", "example"};
        p.intents = {"understand", "probe_evidence", "open_question"}; p.targetEntries = 5;
        result.append(p);
    }
    {
        VideoPresentationPolicy p;
        p.id = "meeting_v1"; p.primaryType = VideoContentType::Meeting;
        p.guidance = QStringLiteral("区分议题、明确决策、分歧与待确认事项；只有明确任务才生成 action_item，未明确 owner/deadline 留空，不创建外部待办。");
        p.primaryKind = "minutes"; p.primaryTitle = QStringLiteral("会议纪要");
        p.secondaryKind = "follow_up"; p.secondaryTitle = QStringLiteral("行动与跟进");
        p.primaryEntryKinds = {"topic", "decision", "disagreement", "open_issue"}; p.pointRoles = {"topic", "viewpoint", "decision", "disagreement", "open_issue", "action"};
        p.intents = {"clarify", "follow_up", "open_question"}; p.targetEntries = 10;
        p.secondaryEntryKinds = {"action_item"}; p.attributeKeys.insert("action_item", {"owner", "deadline"});
        result.append(p);
    }
    {
        VideoPresentationPolicy p;
        p.id = "documentary_v1"; p.primaryType = VideoContentType::Documentary;
        p.guidance = QStringLiteral("整理对象、背景、事件与信息联系；未明确的原因只作为有来源前提的追问。");
        p.primaryKind = "context"; p.primaryTitle = QStringLiteral("内容脉络");
        p.secondaryKind = "exploration"; p.secondaryTitle = QStringLiteral("延伸探索");
        p.primaryEntryKinds = {"subject", "background", "event", "relationship"}; p.pointRoles = {"subject", "background", "event", "relationship", "evidence"};
        p.intents = {"causes", "relationships", "probe_evidence"}; p.targetEntries = 5;
        result.append(p);
    }
    {
        VideoPresentationPolicy p;
        p.id = "drama_v1"; p.primaryType = VideoContentType::Drama;
        p.guidance = QStringLiteral("梳理人物、事件、冲突与转折；未明确动机、伏笔不能写成事实。");
        p.primaryKind = "plot"; p.primaryTitle = QStringLiteral("剧情梳理");
        p.secondaryKind = "plot_discussion"; p.secondaryTitle = QStringLiteral("剧情讨论");
        p.primaryEntryKinds = {"character", "event", "conflict", "turning_point"}; p.pointRoles = {"character", "event", "conflict", "turning_point"};
        p.intents = {"motivation", "foreshadowing", "relationships"}; p.targetEntries = 5;
        result.append(p);
    }
    {
        VideoPresentationPolicy p;
        p.id = "news_v1"; p.primaryType = VideoContentType::News;
        p.guidance = QStringLiteral("区分已报告事实、事件进展和各方表述，保留归属，未明确信息不补全。");
        p.primaryKind = "news_points"; p.primaryTitle = QStringLiteral("事件要点");
        p.secondaryKind = "background_inquiry"; p.secondaryTitle = QStringLiteral("背景与追问");
        p.primaryEntryKinds = {"event", "development", "statement"}; p.pointRoles = {"event", "development", "statement", "background"};
        p.intents = {"chronology", "compare_statements", "unknown_information"}; p.targetEntries = 5;
        result.append(p);
    }
    {
        VideoPresentationPolicy p;
        p.id = "vlog_v1"; p.primaryType = VideoContentType::Vlog;
        p.guidance = QStringLiteral("整理地点、活动、体验与明确建议；个人体验不能写成普遍事实。");
        p.primaryKind = "experiences"; p.primaryTitle = QStringLiteral("经历与发现");
        p.secondaryKind = "exploration"; p.secondaryTitle = QStringLiteral("继续探索");
        p.primaryEntryKinds = {"place", "activity", "experience", "advice"}; p.pointRoles = {"place", "activity", "experience", "advice", "change"};
        p.intents = {"organize_experience", "practical_information", "explore"}; p.targetEntries = 5;
        result.append(p);
    }
    VideoPresentationPolicy demo;
    demo.id = "demo_v1"; demo.mode = "demo";
    demo.guidance = QStringLiteral("只整理可见功能、状态变化和展示结果；仅运行效果不能写成开发或操作教程，不推断未展示步骤。");
    demo.primaryKind = "features"; demo.primaryTitle = QStringLiteral("功能与表现");
    demo.secondaryKind = "exploration"; demo.secondaryTitle = QStringLiteral("继续探索");
    demo.primaryEntryKinds = {"feature", "state_change", "result"};
    demo.pointRoles = {"feature", "state", "change", "result"};
    demo.intents = {"feature_list", "display_effect", "visible_change"};
    result.append(demo);
    return result;
}
VideoPresentationPolicy VideoPresentationPolicyRegistry::resolve(VideoContentType type, bool demo) {
    const QString policyId = demo ? QStringLiteral("demo_v1")
        : type == VideoContentType::Unknown ? QStringLiteral("generic_v1") : contentTypeKey(type) + "_v1";
    auto p = byId(policyId);
    p.primaryType = type; // demo preserves the user/classifier primary type
    return p;
}
VideoPresentationPolicy VideoPresentationPolicyRegistry::byId(const QString &id) {
    for (const auto &p : all()) if (p.id == id) return p;
    return {}; // unknown IDs must fail validation, never silently become generic
}
QJsonObject VideoPresentationPolicy::toJson() const {
    QJsonObject attrs;
    for (auto it = attributeKeys.cbegin(); it != attributeKeys.cend(); ++it) attrs[it.key()] = QJsonArray::fromStringList(it.value());
    return {{"policy_id", id}, {"policy_version", version}, {"mode", mode}, {"guidance", guidance},
        {"primary_type", contentTypeKey(primaryType)}, {"primary_kind", primaryKind}, {"primary_title", primaryTitle},
        {"secondary_kind", secondaryKind}, {"secondary_title", secondaryTitle},
        {"primary_entry_kinds", QJsonArray::fromStringList(primaryEntryKinds)},
        {"secondary_entry_kinds", QJsonArray::fromStringList(secondaryEntryKinds)},
        {"point_roles", QJsonArray::fromStringList(pointRoles)}, {"intents", QJsonArray::fromStringList(intents)},
        {"attribute_keys", attrs}, {"target_entries", targetEntries}, {"target_questions", targetQuestions}};
}
QString VideoPresentationPolicy::validationError(const VideoPresentation &p, const VideoPresentationValidationContext &c) const {
    auto error = p.validationError(c); if (!error.isEmpty()) return error;
    if (id.isEmpty() || p.policyId != id || p.policyVersion != version) return "unsupported_presentation_policy";
    for (const auto *section : {&p.primarySection, &p.secondarySection}) {
        const bool primary = section == &p.primarySection;
        const bool active = section->state != ArtifactState::Pending || !section->entries.isEmpty() || !section->questions.isEmpty();
        if (active && (section->kind != (primary ? primaryKind : secondaryKind) || section->title != (primary ? primaryTitle : secondaryTitle))) return "invalid_controlled_section";
        if (primary && !section->questions.isEmpty()) return "questions_in_primary_section";
        for (const auto &entry : section->entries) {
            if (!(primary ? primaryEntryKinds : secondaryEntryKinds).contains(entry.kind)) return "entry_kind_not_allowed";
            for (const auto &point : entry.points) if (!pointRoles.contains(point.role)) return "point_role_not_allowed";
            const auto keys = attributeKeys.value(entry.kind);
            for (auto it = entry.attributes.begin(); it != entry.attributes.end(); ++it) {
                if (!keys.contains(it.key()) || (!it.value().isString() && !it.value().isNull())) return "attribute_not_allowed";
            }
        }
        for (const auto &question : section->questions) if (!intents.contains(question.intent)) return "question_intent_not_allowed";
    }
    return {};
}
