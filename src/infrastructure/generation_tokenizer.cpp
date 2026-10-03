#include "infrastructure/generation_tokenizer.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <algorithm>
#include <limits>
#include <queue>

namespace {
QByteArray pairKey(const QByteArray& left, const QByteArray& right) {
    return QByteArray::number(left.size()) + ':' + left + right;
}
std::optional<QStringList> isolated(const QString& text, const QRegularExpression& pattern) {
    QStringList out;
    qsizetype offset = 0;
    auto matches = pattern.globalMatch(text);
    if (!matches.isValid()) return std::nullopt;
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (!match.isValid()) return std::nullopt;
        if (match.capturedStart() > offset) out.append(text.mid(offset, match.capturedStart() - offset));
        if (!match.captured().isEmpty()) out.append(match.captured());
        offset = match.capturedEnd();
    }
    if (!matches.isValid()) return std::nullopt;
    if (offset < text.size()) out.append(text.mid(offset));
    return out;
}
}

bool GenerationTokenizer::addStage(const QJsonObject& object) {
    const auto type = object["type"].toString();
    if (type == "Sequence") {
        if (!object["pretokenizers"].isArray()) return false;
        for (const auto& value : object["pretokenizers"].toArray())
            if (!value.isObject() || !addStage(value.toObject())) return false;
        return true;
    }
    Stage stage;
    if (type == "Split") {
        if (object["behavior"] != "Isolated" || !object["invert"].isBool() || object["invert"].toBool() ||
            !object["pattern"].toObject()["Regex"].isString()) return false;
        stage.pattern = QRegularExpression(object["pattern"].toObject()["Regex"].toString(),
            QRegularExpression::UseUnicodePropertiesOption);
        if (stage.pattern.pattern().isEmpty() || !stage.pattern.isValid() || stage.pattern.match("").hasMatch()) return false;
    } else if (type == "ByteLevel") {
        for (const auto& key : {"add_prefix_space", "use_regex"})
            if (object.contains(key) && !object[key].isBool()) return false;
        stage.byteLevel = true;
        stage.prefixSpace = object["add_prefix_space"].toBool(true);
        stage.useRegex = object["use_regex"].toBool(true);
        stage.pattern = QRegularExpression(QStringLiteral(R"('s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+)"),
            QRegularExpression::UseUnicodePropertiesOption);
    } else return false;
    // Byte-to-Unicode encoding must be the last stage; a subsequent regex over
    // byte-encoded characters would not have the same semantics.
    if (!m_stages.isEmpty() && m_stages.last().byteLevel) return false;
    m_stages.append(stage); return true;
}

bool GenerationTokenizer::load(const QString& path, const QString& expectedSha256) {
    m_loaded = false; m_error.clear(); m_fingerprint.clear(); m_counts.clear();
    m_stages.clear(); m_vocabulary.clear(); m_mergeRanks.clear(); m_addedTokens.clear(); m_byteDecoder.clear();
    auto reject = [&](const QString& error) { m_error = error; return false; };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 1 || file.size() > 64 * 1024 * 1024)
        return reject("tokenizer_file_unavailable_or_too_large");
    const auto data = file.readAll();
    if (data.size() != file.size()) return reject("tokenizer_file_read_failed");
    const auto sha = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    if (!expectedSha256.isEmpty() && sha != expectedSha256.toLower()) return reject("tokenizer_sha256_mismatch");
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(data, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return reject("invalid_tokenizer_json");
    const auto root = document.object(), model = root["model"].toObject();
    for (const auto& key : {"byte_fallback", "ignore_merges"})
        if (model.contains(key) && !model[key].isBool()) return reject("invalid_bpe_model_flag");
    for (const auto& key : {"continuing_subword_prefix", "end_of_word_suffix", "unk_token"})
        if (model.contains(key) && !model[key].isNull() && !model[key].isString()) return reject("invalid_bpe_model_field");
    if (model["type"] != "BPE" || (!root["normalizer"].isNull() && !root["normalizer"].isUndefined()) ||
        (!model["dropout"].isNull() && !model["dropout"].isUndefined() && model["dropout"].toDouble(-1) != 0) ||
        !model["continuing_subword_prefix"].toString().isEmpty() || !model["end_of_word_suffix"].toString().isEmpty() ||
        model["byte_fallback"].toBool() || !model["unk_token"].toString().isEmpty())
        return reject("unsupported_generation_tokenizer_model");
    if (!root["pre_tokenizer"].isObject() || !addStage(root["pre_tokenizer"].toObject()) ||
        m_stages.isEmpty() || !m_stages.last().byteLevel) return reject("unsupported_generation_pre_tokenizer");
    int extra = 0;
    for (int byte = 0; byte < 256; ++byte) {
        const bool direct = (byte >= 33 && byte <= 126) || (byte >= 161 && byte <= 172) || byte >= 174;
        m_byteDecoder.insert(ushort(direct ? byte : 256 + extra++), static_cast<unsigned char>(byte));
    }
    auto decode = [&](const QString& token) -> std::optional<QByteArray> {
        QByteArray bytes;
        for (const auto character : token) {
            const auto it = m_byteDecoder.constFind(character.unicode());
            if (it == m_byteDecoder.cend()) return std::nullopt;
            bytes.append(char(it.value()));
        }
        return bytes;
    };
    if (!model["vocab"].isObject() || !model["merges"].isArray()) return reject("invalid_bpe_vocabulary");
    const auto vocabulary = model["vocab"].toObject();
    for (auto it = vocabulary.begin(); it != vocabulary.end(); ++it) {
        if (!it.value().isDouble() || it.value().toDouble() < 0 || it.value().toDouble() != it.value().toInt(-1))
            return reject("invalid_bpe_token_id");
        const auto token = decode(it.key());
        if (!token || token->isEmpty()) return reject("unsupported_bpe_vocabulary_symbol");
        m_vocabulary.insert(*token);
    }
    for (int byte = 0; byte < 256; ++byte)
        if (!m_vocabulary.contains(QByteArray(1, char(byte)))) return reject("incomplete_byte_vocabulary");
    int rank = 0;
    for (const auto& value : model["merges"].toArray()) {
        QString left, right;
        if (value.isString()) {
            const auto text = value.toString(); const auto split = text.indexOf(' ');
            if (split < 1) return reject("invalid_bpe_merge");
            left = text.left(split); right = text.mid(split + 1);
        } else if (value.isArray() && value.toArray().size() == 2 && value.toArray()[0].isString() && value.toArray()[1].isString()) {
            left = value.toArray()[0].toString(); right = value.toArray()[1].toString();
        } else return reject("invalid_bpe_merge");
        const auto a = decode(left), b = decode(right);
        if (!a || !b || a->isEmpty() || b->isEmpty() || !m_vocabulary.contains(*a) || !m_vocabulary.contains(*b) ||
            !m_vocabulary.contains(*a + *b) || m_mergeRanks.contains(pairKey(*a, *b))) return reject("invalid_bpe_merge_symbols");
        m_mergeRanks.insert(pairKey(*a, *b), rank++);
    }
    if (root.contains("added_tokens") && !root["added_tokens"].isArray()) return reject("invalid_added_tokens");
    for (const auto& value : root["added_tokens"].toArray()) {
        const auto token = value.toObject();
        for (const auto& key : {"single_word", "lstrip", "rstrip", "normalized", "special"})
            if (token.contains(key) && !token[key].isBool()) return reject("invalid_added_token_flag");
        if (!value.isObject() || !token["content"].isString() || token["content"].toString().isEmpty() ||
            token["single_word"].toBool() || token["lstrip"].toBool() || token["rstrip"].toBool())
            return reject("unsupported_added_token_behavior");
        m_addedTokens.append(token["content"].toString());
    }
    m_addedTokens.removeDuplicates();
    std::sort(m_addedTokens.begin(), m_addedTokens.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    m_ignoreMerges = model["ignore_merges"].toBool();
    m_fingerprint = "hf_bytelevel_bpe_v1:" + sha; m_loaded = true; return true;
}

std::optional<qint64> GenerationTokenizer::countPiece(const QByteArray& bytes) const {
    if (bytes.isEmpty()) return 0;
    if (m_ignoreMerges && m_vocabulary.contains(bytes)) return 1;
    struct Node { QByteArray text; int previous, next, version = 0; bool live = true; };
    struct Merge { int rank, left, right, leftVersion, rightVersion; };
    struct Later { bool operator()(const Merge& a, const Merge& b) const { return a.rank != b.rank ? a.rank > b.rank : a.left > b.left; } };
    QVector<Node> nodes; nodes.reserve(bytes.size());
    for (int i = 0; i < bytes.size(); ++i) nodes.append({QByteArray(1, bytes[i]), i - 1, i + 1 < bytes.size() ? i + 1 : -1});
    std::priority_queue<Merge, std::vector<Merge>, Later> queue;
    auto push = [&](int left) {
        if (left < 0 || !nodes[left].live || nodes[left].next < 0) return;
        const int right = nodes[left].next;
        const auto it = m_mergeRanks.constFind(pairKey(nodes[left].text, nodes[right].text));
        if (it != m_mergeRanks.cend()) queue.push({it.value(), left, right, nodes[left].version, nodes[right].version});
    };
    for (int i = 0; i < nodes.size(); ++i) push(i);
    qint64 count = nodes.size();
    while (!queue.empty()) {
        const auto merge = queue.top(); queue.pop();
        auto& left = nodes[merge.left]; auto& right = nodes[merge.right];
        if (!left.live || !right.live || left.next != merge.right || left.version != merge.leftVersion || right.version != merge.rightVersion) continue;
        left.text += right.text; left.next = right.next; ++left.version; right.live = false; --count;
        if (left.next >= 0) nodes[left.next].previous = merge.left;
        push(left.previous); push(merge.left);
    }
    for (const auto& node : nodes) if (node.live && !m_vocabulary.contains(node.text)) return std::nullopt;
    return count;
}

std::optional<qint64> GenerationTokenizer::countOrdinary(const QString& text) const {
    if (text.isEmpty()) return 0;
    QStringList pieces{text};
    for (const auto& stage : m_stages) {
        QStringList next;
        for (auto piece : pieces) {
            if (stage.byteLevel && stage.prefixSpace && !piece.startsWith(' ')) piece.prepend(' ');
            if (!stage.byteLevel || stage.useRegex) {
                const auto split = isolated(piece, stage.pattern);
                if (!split) return std::nullopt;
                next += *split;
            }
            else next.append(piece);
        }
        pieces = std::move(next);
    }
    qint64 count = 0;
    for (const auto& piece : pieces) { const auto n = countPiece(piece.toUtf8()); if (!n) return std::nullopt; count += *n; }
    return count;
}
std::optional<qint64> GenerationTokenizer::tokenCount(const QString& text) const {
    if (!m_loaded) return std::nullopt;
    if (const auto cached = m_counts.object(text)) return *cached;
    qint64 count = 0; qsizetype offset = 0;
    while (offset < text.size()) {
        qsizetype first = -1; QString token;
        for (const auto& candidate : m_addedTokens) {
            const auto index = text.indexOf(candidate, offset);
            if (index >= 0 && (first < 0 || index < first)) { first = index; token = candidate; }
        }
        if (first < 0) first = text.size();
        const auto n = countOrdinary(text.mid(offset, first - offset)); if (!n) return std::nullopt;
        count += *n;
        if (token.isEmpty()) break;
        ++count; offset = first + token.size();
    }
    if (text.size() < 32768) m_counts.insert(text, new qint64(count), int(text.size() * 2 + 64));
    return count;
}
