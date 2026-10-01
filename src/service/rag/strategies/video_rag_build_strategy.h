#pragma once
#include "model/video_rag_types.h"
class IVideoRAGBuildStrategy {
public:
    virtual ~IVideoRAGBuildStrategy()=default;
    virtual VideoRAGBuildPlan makePlan(const AvailableCapabilities&) const=0;
};
class GenericStrategy final:public IVideoRAGBuildStrategy { public:VideoRAGBuildPlan makePlan(const AvailableCapabilities&) const override; };
class DialogueStrategy final:public IVideoRAGBuildStrategy {
public:explicit DialogueStrategy(bool meeting):m_meeting(meeting){} VideoRAGBuildPlan makePlan(const AvailableCapabilities&) const override;
private:bool m_meeting;
};
class LectureStrategy final:public IVideoRAGBuildStrategy { public:VideoRAGBuildPlan makePlan(const AvailableCapabilities&) const override; };
class TutorialStrategy final:public IVideoRAGBuildStrategy { public:VideoRAGBuildPlan makePlan(const AvailableCapabilities&) const override; };
