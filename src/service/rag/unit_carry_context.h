#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>

// Unit-local accepted facts only. No model-owned progress or source identities.
class UnitCarryContext {
public:
    static constexpr int CharacterBudget = 1000;
    QJsonObject input() const;
    // Called only after the whole page's facts passed validatedFacts(). Returns false on fallback.
    bool acceptPage(int pageOrdinal, const QJsonArray& acceptedFacts, const QJsonValue& carry);
    void failPage();
private:
    QHash<QString, QJsonObject> m_facts;
    QJsonObject m_carry;
    int m_lastSuccessfulPage = -1;
    int m_gapCount = 0;
    static void fitBudget(QJsonObject&);
};
