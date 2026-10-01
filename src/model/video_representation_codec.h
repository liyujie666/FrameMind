#pragma once
#include "model/video_representation.h"
QJsonObject representationToJson(const VideoRepresentation &);
VideoRepresentation representationFromJson(const QJsonObject &);
