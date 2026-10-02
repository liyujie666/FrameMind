#pragma once
#include <QByteArray>
#include <QList>
#include <QSize>

struct EncodedRequestImage {
    QByteArray jpeg;
    QSize size;
};

// Legacy callers keep 1024/JPEG80. Grid callers supply final, background-encoded bytes.
struct ImageEncodingOptions {
    int maxEdge = 1024;
    int jpegQuality = 80;
    bool requirePrepared = false;
    QList<EncodedRequestImage> preparedImages;
};
