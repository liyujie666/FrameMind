#include "service/rag/unit_carry_context.h"
#include <QJsonDocument>
#include <QSet>

namespace {
int chars(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)).size();
}
bool strings(const QJsonValue& value, int count, int length) {
    if (!value.isArray() || value.toArray().size() > count) return false;
    for (const auto v : value.toArray())
        if (!v.isString() || v.toString().size() > length) return false;
    return true;
}
}

QJsonObject UnitCarryContext::input() const {
    auto out = m_carry;
    if (!out.contains("topic")) out["topic"] = "";
    if (!out.contains("current_state")) out["current_state"] = QJsonObject{{"text", ""}, {"fact_refs", QJsonArray{}}};
    if (!out.contains("key_facts")) out["key_facts"] = QJsonArray{};
    if (!out.contains("pending_threads")) out["pending_threads"] = QJsonArray{};
    if (!out.contains("uncertainties")) out["uncertainties"] = QJsonArray{};
    out["last_successful_page"] = m_lastSuccessfulPage;
    out["gap_count"] = m_gapCount;
    fitBudget(out);
    return out;
}

void UnitCarryContext::fitBudget(QJsonObject& out) {
    // Remove whole low-priority items; never truncate facts or serialize broken JSON.
    for (const auto field : {"uncertainties", "pending_threads", "key_facts"}) {
        auto items = out[field].toArray();
        while (chars(out) > CharacterBudget && !items.isEmpty()) {
            items.removeLast();
            out[field] = items;
        }
    }
    if (chars(out) > CharacterBudget) {
        out["current_state"] = QJsonObject{{"text", ""}, {"fact_refs", QJsonArray{}}};
    }
    // State references must have an expanded, unmodified accepted fact in this input.
    QSet<QString> expanded;
    for (const auto v : out["key_facts"].toArray()) expanded.insert(v.toObject()["ref"].toString());
    const auto state = out["current_state"].toObject();
    for (const auto v : state["fact_refs"].toArray())
        if (!expanded.contains(v.toString())) {
            out["current_state"] = QJsonObject{{"text", ""}, {"fact_refs", QJsonArray{}}};
            break;
        }
}

bool UnitCarryContext::acceptPage(int ordinal, const QJsonArray& accepted, const QJsonValue& carry) {
    for (int i = 0; i < accepted.size(); ++i)
        m_facts.insert(QString("P%1.F%2").arg(ordinal).arg(i), accepted[i].toObject());
    const auto obj = carry.toObject();
    const auto state = obj["current_state"].toObject();
    bool valid = carry.isObject() && obj["topic"].isString() && obj["topic"].toString().size() <= 80 &&
                 obj["current_state"].isObject() && state["text"].isString() && state["text"].toString().size() <= 160 &&
                 strings(state["fact_refs"], 4, 40) && strings(obj["key_fact_refs"], 4, 40) &&
                 strings(obj["pending_threads"], 2, 80) && strings(obj["uncertainties"], 2, 80);
    if (!state["text"].toString().trimmed().isEmpty() && state["fact_refs"].toArray().isEmpty()) valid = false;
    QStringList refs;
    // Include state refs first so pruning prefers the newest confirmed state.
    for (const auto field : {state["fact_refs"].toArray(), obj["key_fact_refs"].toArray()})
        for (const auto v : field) {
            const auto ref = v.toString();
            if (!m_facts.contains(ref)) valid = false;
            if (!refs.contains(ref)) refs << ref;
        }
    if (refs.size() > 4) valid = false;
    QJsonObject next;
    if (valid) {
        QJsonArray expanded;
        for (const auto& ref : refs) expanded.append(QJsonObject{{"ref", ref}, {"text", m_facts[ref]["text"]}});
        const QJsonObject cleanState{{"text", state["text"]}, {"fact_refs", state["fact_refs"]}};
        next = {{"topic", obj["topic"]}, {"current_state", cleanState}, {"key_facts", expanded},
                {"pending_threads", obj["pending_threads"]}, {"uncertainties", obj["uncertainties"]}};
        auto full = next;
        full["last_successful_page"] = ordinal;
        full["gap_count"] = 0;
        if (chars(full) > CharacterBudget) valid = false;
    }
    if (!valid) {
        next = input();
        if (!state["text"].toString().trimmed().isEmpty()) {
            auto uncertain = next["uncertainties"].toArray();
            const QString marker = QStringLiteral("本页新状态未通过上下文校验，不能作为已确认状态");
            if (uncertain.size() < 2 && !uncertain.contains(marker)) uncertain.append(marker);
            next["uncertainties"] = uncertain;
        }
        auto expanded = next["key_facts"].toArray();
        for (int i = 0; i < accepted.size() && expanded.size() < 4; ++i) {
            const auto ref = QString("P%1.F%2").arg(ordinal).arg(i);
            expanded.append(QJsonObject{{"ref", ref}, {"text", m_facts[ref]["text"]}});
        }
        next["key_facts"] = expanded;
    }
    m_lastSuccessfulPage = ordinal;
    m_gapCount = 0;
    next["last_successful_page"] = m_lastSuccessfulPage;
    next["gap_count"] = m_gapCount;
    fitBudget(next);
    m_carry = next;
    return valid;
}
void UnitCarryContext::failPage() { ++m_gapCount; }
