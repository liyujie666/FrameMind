#include "infrastructure/bert_tokenizer.h"

#ifdef FRAMEMIND_HAS_ONNXRUNTIME

#include <QFile>
#include <QTextStream>
#include <QDebug>

BertTokenizer::BertTokenizer() = default;

bool BertTokenizer::load(const QString& vocabPath)
{
    QFile file(vocabPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[BertTokenizer] 无法打开词表:" << vocabPath;
        return false;
    }

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);

    m_vocab.clear();
    int64_t idx = 0;
    while (!in.atEnd()) {
        QString line = in.readLine();
        std::string token = line.toStdString();
        m_vocab[token] = idx++;
    }

    m_vocabSize = static_cast<int>(m_vocab.size());
    m_loaded = (m_vocabSize > 0);
    qDebug() << "[BertTokenizer] 加载成功, vocab size:" << m_vocabSize;
    return m_loaded;
}

bool BertTokenizer::isCjkChar(QChar ch)
{
    uint32_t cp = ch.unicode();
    return (cp >= 0x4E00 && cp <= 0x9FFF)
        || (cp >= 0x3400 && cp <= 0x4DBF)
        || (cp >= 0x20000 && cp <= 0x2A6DF)
        || (cp >= 0x2A700 && cp <= 0x2B73F)
        || (cp >= 0x2B740 && cp <= 0x2B81F)
        || (cp >= 0x2B820 && cp <= 0x2CEAF)
        || (cp >= 0xF900 && cp <= 0xFAFF)
        || (cp >= 0x2F800 && cp <= 0x2FA1F);
}

bool BertTokenizer::isPunctuation(QChar ch)
{
    uint32_t cp = ch.unicode();
    if ((cp >= 33 && cp <= 47) || (cp >= 58 && cp <= 64)
        || (cp >= 91 && cp <= 96) || (cp >= 123 && cp <= 126)) {
        return true;
    }
    return ch.category() >= QChar::Punctuation_Connector
        && ch.category() <= QChar::Punctuation_Other;
}

QStringList BertTokenizer::basicTokenize(const QString& text) const
{
    QString cleaned;
    cleaned.reserve(text.size() * 2);

    for (const QChar& ch : text) {
        if (ch.isNull() || ch.unicode() == 0xFFFD) continue;

        if (isCjkChar(ch)) {
            // CJK 字符前后加空格，使其成为独立 token
            cleaned += ' ';
            cleaned += ch;
            cleaned += ' ';
        } else if (isPunctuation(ch)) {
            cleaned += ' ';
            cleaned += ch;
            cleaned += ' ';
        } else if (ch.isSpace()) {
            cleaned += ' ';
        } else {
            cleaned += ch.toLower();
        }
    }

    return cleaned.split(' ', Qt::SkipEmptyParts);
}

std::vector<int64_t> BertTokenizer::wordPieceTokenize(const QString& token) const
{
    std::vector<int64_t> result;
    if (token.isEmpty()) return result;

    std::string word = token.toStdString();
    int start = 0;
    int len = static_cast<int>(word.size());

    while (start < len) {
        int end = len;
        bool found = false;

        while (start < end) {
            std::string substr = word.substr(start, end - start);
            if (start > 0) {
                substr = "##" + substr;
            }

            auto it = m_vocab.find(substr);
            if (it != m_vocab.end()) {
                result.push_back(it->second);
                found = true;
                break;
            }
            --end;
        }

        if (!found) {
            result.push_back(UNK_TOKEN);
            ++start;
        } else {
            start = end;
        }
    }

    return result;
}

std::vector<int64_t> BertTokenizer::encode(const QString& text, int maxLen) const
{
    std::vector<int64_t> tokens(maxLen, PAD_TOKEN);

    if (!m_loaded || text.isEmpty()) {
        tokens[0] = CLS_TOKEN;
        tokens[1] = SEP_TOKEN;
        return tokens;
    }

    tokens[0] = CLS_TOKEN;
    int pos = 1;

    QStringList words = basicTokenize(text);
    for (const QString& word : words) {
        auto subTokens = wordPieceTokenize(word);
        for (int64_t id : subTokens) {
            if (pos >= maxLen - 1) break;
            tokens[pos++] = id;
        }
        if (pos >= maxLen - 1) break;
    }

    tokens[pos] = SEP_TOKEN;
    return tokens;
}

std::vector<int64_t> BertTokenizer::attentionMask(const std::vector<int64_t>& inputIds) const
{
    std::vector<int64_t> mask(inputIds.size(), 0);
    for (size_t i = 0; i < inputIds.size(); ++i) {
        if (inputIds[i] != PAD_TOKEN) {
            mask[i] = 1;
        }
    }
    return mask;
}

#endif // FRAMEMIND_HAS_ONNXRUNTIME
