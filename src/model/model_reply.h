#pragma once
#include <QString>
#include <QJsonObject>

// A failed request must not be indistinguishable from an empty model answer.
struct ModelReply {
    QString content;
    QString error;
    int httpStatus = 0;
    qint64 retryAfterMs = -1; // -1: server did not supply Retry-After
    QJsonObject diagnostics;
};
