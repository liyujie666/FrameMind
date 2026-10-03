#pragma once
#include <QCache>
#include <QHash>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <optional>

// Counts ordinary text without truncation, padding or a chat template. Message
// framing and image tokens remain separate reserved budgets. Supported assets:
// HF tokenizer.json, byte-level BPE, with isolated regex splits and ByteLevel.
class GenerationTokenizer final {
public:
    bool load(const QString& path, const QString& expectedSha256 = {});
    std::optional<qint64> tokenCount(const QString& text) const;
    QString fingerprint() const { return m_fingerprint; }
    QString error() const { return m_error; }
private:
    struct Stage { bool byteLevel = false, prefixSpace = false, useRegex = false; QRegularExpression pattern; };
    bool addStage(const QJsonObject&);
    std::optional<qint64> countOrdinary(const QString&) const;
    std::optional<qint64> countPiece(const QByteArray&) const;
    QVector<Stage> m_stages;
    QHash<ushort, unsigned char> m_byteDecoder;
    QHash<QByteArray, int> m_mergeRanks;
    QSet<QByteArray> m_vocabulary;
    QStringList m_addedTokens;
    QString m_fingerprint, m_error;
    bool m_loaded = false, m_ignoreMerges = false;
    mutable QCache<QString, qint64> m_counts{1024 * 1024};
};
