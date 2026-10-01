#pragma once
#include "model/videoinfo.h"
#include <atomic>
class MediaProbe {
public:
    static VideoInfo inspect(const QString& path, const std::atomic_bool* cancelled=nullptr, QString* error=nullptr);
    static QString fingerprint(const QString& path);
};
