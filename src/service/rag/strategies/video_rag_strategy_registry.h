#pragma once
#include "service/rag/strategies/video_rag_build_strategy.h"
class VideoRAGStrategyRegistry {
public: static VideoRAGBuildPlan resolve(const VideoContentProfile&,const AvailableCapabilities&);
};
