#pragma once
#include <QString>

// A failed request must not be indistinguishable from an empty model answer.
struct ModelReply {
    QString content;
    QString error;
};
