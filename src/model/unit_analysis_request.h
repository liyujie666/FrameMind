#pragma once
#include "model/video_rag_types.h"
#include "model/image_encoding_options.h"

// Every attempt gets a new requestId. All fields travel together across callbacks.
struct UnitAnalysisRequest {
    VideoBuildContext context;
    QString unitId, pageId, requestId;
    int pageOrdinal = 0;
    int attempt = 0;
    int workerId = -1;
    ImageEncodingOptions imageOptions;
    int maxOutputTokens = 0;
};
