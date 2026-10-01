#include "infrastructure/bert_tokenizer.h"
#include <QTemporaryDir>
#include <QtTest>
class TokenizerTests : public QObject {
    Q_OBJECT
  private slots:
    void losslessBoundedPassages() {
        QTemporaryDir dir;
        QFile file(dir.filePath("vocab.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        for (int i = 0; i < 110; ++i) {
            QString token = QString("unused_%1").arg(i);
            if (i == 0)
                token = "[PAD]";
            if (i == 100)
                token = "[UNK]";
            if (i == 101)
                token = "[CLS]";
            if (i == 102)
                token = "[SEP]";
            if (i == 103)
                token = QStringLiteral("中");
            file.write(token.toUtf8() + "\n");
        }
        file.close();
        BertTokenizer tokenizer;
        QVERIFY(tokenizer.load(file.fileName()));
        const QString text = QString(3000, QChar(0x4E2D)) + QStringLiteral("后半段精确事实42万元");
        QCOMPARE(tokenizer.tokenCount(QString(3000, QChar(0x4E2D))), 3002);
        const auto pages = tokenizer.splitText(text, 500);
        QVERIFY(pages.size() > 6);
        QCOMPARE(pages.join(""), text);
        for (const auto &page : pages)
            QVERIFY(tokenizer.tokenCount(page) <= 500);
        const QString mixed = QStringLiteral("中文 budget42 😀 \n") + text;
        QCOMPARE(tokenizer.splitText(mixed, 500).join(""), mixed);
    }
};
QTEST_GUILESS_MAIN(TokenizerTests)
#include "video_tokenizer_tests.moc"
