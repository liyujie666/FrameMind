#include "infrastructure/clip_tokenizer.h"

#ifdef FRAMEMIND_HAS_ONNXRUNTIME

#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <algorithm>
#include <set>
#include <codecvt>
#include <locale>

// CLIP BPE byte-to-unicode table (same as openai/clip simple_tokenizer.py)
void ClipTokenizer::buildByteEncoder()
{
    m_byteToUnicode.clear();
    m_unicodeToByte.clear();

    // Printable byte ranges that map to themselves
    // '!' (33) .. '~' (126), '¡' (161) .. '¬' (172), '®' (174) .. 'ÿ' (255)
    std::vector<int> bs;
    for (int i = 33; i <= 126; ++i)  bs.push_back(i);
    for (int i = 161; i <= 172; ++i) bs.push_back(i);
    for (int i = 174; i <= 255; ++i) bs.push_back(i);

    std::vector<int> cs(bs.begin(), bs.end());

    int n = 0;
    for (int b = 0; b < 256; ++b) {
        if (std::find(bs.begin(), bs.end(), b) == bs.end()) {
            bs.push_back(b);
            cs.push_back(256 + n);
            ++n;
        }
    }

    for (size_t i = 0; i < bs.size(); ++i) {
        m_byteToUnicode[static_cast<uint8_t>(bs[i])] = static_cast<char32_t>(cs[i]);
        m_unicodeToByte[static_cast<char32_t>(cs[i])] = static_cast<uint8_t>(bs[i]);
    }
}

ClipTokenizer::ClipTokenizer()
{
    buildByteEncoder();
}

bool ClipTokenizer::load(const QString& mergesPath, const QString& vocabJsonPath)
{
    // --- 1. Load merge rules ---
    QFile mergesFile(mergesPath);
    if (!mergesFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[ClipTokenizer] 无法打开 merges 文件:" << mergesPath;
        return false;
    }

    QTextStream in(&mergesFile);
    in.setEncoding(QStringConverter::Utf8);
    in.readLine();  // skip "#version: ..." header

    int rank = 0;
    QVector<QPair<std::string, std::string>> merges;
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.isEmpty()) continue;
        int spaceIdx = line.indexOf(' ');
        if (spaceIdx < 0) continue;
        std::string a = line.left(spaceIdx).toStdString();
        std::string b = line.mid(spaceIdx + 1).toStdString();
        m_bpeRanks[a + " " + b] = rank++;
        merges.append({a, b});
    }
    mergesFile.close();

    // --- 2. Build encoder ---
    if (!vocabJsonPath.isEmpty()) {
        // Load encoder directly from clip_vocab.json  {"token": id, ...}
        QFile vocabFile(vocabJsonPath);
        if (!vocabFile.open(QIODevice::ReadOnly)) {
            qWarning() << "[ClipTokenizer] 无法打开 vocab JSON:" << vocabJsonPath;
            // fall through to self-build below
        } else {
            QByteArray raw = vocabFile.readAll();
            vocabFile.close();

            // Minimal JSON object parser: "key": number pairs
            // Uses Qt's QJsonDocument for correctness
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
            if (doc.isObject()) {
                m_encoder.clear();
                QJsonObject obj = doc.object();
                for (auto it = obj.begin(); it != obj.end(); ++it) {
                    m_encoder[it.key().toStdString()] =
                        static_cast<int64_t>(it.value().toInt());
                }
                m_loaded = true;
                qDebug() << "[ClipTokenizer] 加载成功 (vocab JSON), vocab size:"
                         << m_encoder.size() << "merges:" << m_bpeRanks.size();
                return true;
            }
            qWarning() << "[ClipTokenizer] vocab JSON 解析失败:" << err.errorString()
                       << "— 回退到自建 encoder";
        }
    }

    // --- 3. Self-build encoder from byte table + merges (fallback) ---
    m_encoder.clear();
    int idx = 0;
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv;
    for (auto it = m_byteToUnicode.begin(); it != m_byteToUnicode.end(); ++it) {
        m_encoder[conv.to_bytes(it->second)] = idx++;
    }
    for (const auto& [a, b] : merges) {
        m_encoder[a + b] = idx++;
    }
    m_encoder["<|startoftext|>"] = SOS_TOKEN;
    m_encoder["<|endoftext|>"]   = EOS_TOKEN;

    m_loaded = true;
    qDebug() << "[ClipTokenizer] 加载成功 (自建 encoder), vocab size:"
             << m_encoder.size() << "merges:" << m_bpeRanks.size();
    return true;
}

std::vector<std::string> ClipTokenizer::preTokenize(const QString& text) const
{
    // CLIP tokenizer: lowercase + split on whitespace/punctuation
    // Pattern from simple_tokenizer.py:
    //   <|startoftext|>|<|endoftext|>|'s|'t|'re|'ve|'m|'ll|'d|[\p{L}]+|[\p{N}]|[^\s\p{L}\p{N}]+
    static const QRegularExpression pat(
        QStringLiteral(R"(<\|startoftext\|>|<\|endoftext\|>|'s|'t|'re|'ve|'m|'ll|'d|[\p{L}]+|[\p{N}]|[^\s\p{L}\p{N}]+)"),
        QRegularExpression::CaseInsensitiveOption);

    QString lower = text.toLower().trimmed();
    std::vector<std::string> words;

    auto it = pat.globalMatch(lower);
    while (it.hasNext()) {
        auto match = it.next();
        QString word = match.captured(0);
        // Convert to byte-level unicode representation
        QByteArray utf8 = word.toUtf8();
        std::string encoded;
        std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv;
        for (uint8_t byte : utf8) {
            encoded += conv.to_bytes(m_byteToUnicode.at(byte));
        }
        // Append end-of-word marker (</w> is represented as last char getting Ġ suffix in CLIP)
        // CLIP uses trailing </w> convention: add special char to last character
        words.push_back(encoded);
    }

    return words;
}

std::vector<std::string> ClipTokenizer::bpe(const std::string& token) const
{
    if (token.empty()) return {};

    // Split token into individual UTF-8 characters (each is a BPE symbol)
    std::vector<std::string> word;
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> conv;
    std::u32string chars = conv.from_bytes(token);
    for (size_t i = 0; i < chars.size(); ++i) {
        std::string ch = conv.to_bytes(chars[i]);
        if (i == chars.size() - 1) {
            ch += "</w>";  // CLIP end-of-word marker
        }
        word.push_back(ch);
    }

    if (word.size() == 1) return word;

    while (true) {
        // Find the highest-priority merge pair
        int bestRank = INT_MAX;
        int bestIdx = -1;

        for (size_t i = 0; i + 1 < word.size(); ++i) {
            std::string pair = word[i] + " " + word[i + 1];
            auto it = m_bpeRanks.find(pair);
            if (it != m_bpeRanks.end() && it->second < bestRank) {
                bestRank = it->second;
                bestIdx = static_cast<int>(i);
            }
        }

        if (bestIdx < 0) break;  // No more merges possible

        // Apply the merge
        std::string merged = word[bestIdx] + word[bestIdx + 1];
        std::vector<std::string> newWord;
        for (size_t i = 0; i < word.size(); ++i) {
            if (static_cast<int>(i) == bestIdx) {
                newWord.push_back(merged);
                ++i;  // skip next
            } else {
                newWord.push_back(word[i]);
            }
        }
        word = std::move(newWord);

        if (word.size() == 1) break;
    }

    return word;
}

std::vector<int64_t> ClipTokenizer::encode(const QString& text, int maxLen) const
{
    std::vector<int64_t> tokens(maxLen, 0);
    if (!m_loaded || text.isEmpty()) {
        tokens[0] = SOS_TOKEN;
        tokens[1] = EOS_TOKEN;
        return tokens;
    }

    tokens[0] = SOS_TOKEN;
    int pos = 1;

    auto words = preTokenize(text);
    for (const auto& word : words) {
        auto bpeTokens = bpe(word);
        for (const auto& t : bpeTokens) {
            if (pos >= maxLen - 1) break;
            auto it = m_encoder.find(t);
            if (it != m_encoder.end()) {
                tokens[pos++] = it->second;
            }
        }
        if (pos >= maxLen - 1) break;
    }

    tokens[pos] = EOS_TOKEN;
    return tokens;
}

#endif // FRAMEMIND_HAS_ONNXRUNTIME
