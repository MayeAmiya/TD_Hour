#pragma once

#include "core/container/container_types.h"

namespace engine::script {

// Weather.ini 在加载边界冻结出的纯表现配置；它不携带脚本运行时、渲染器
// 或 VFS 对象。保留既有命名空间是为了避免改变公开会话值协议。
struct ScriptWeatherSnowSettings final {
    container::String texture = "EXSnowFlake.tga";
    float frequencyScaleX = 0.0533f;
    float frequencyScaleY = 0.0275f;
    float amplitude = 5.0f;
    float pointSize = 1.0f;
    float maximumPointSize = 64.0f;
    float minimumPointSize = 0.0f;
    float quadSize = 0.5f;
    float boxDimensions = 200.0f;
    float boxDensity = 1.0f;
    float velocity = 4.0f;
    bool usePointSprites = true;
    bool enabled = false;
};

} // namespace engine::script
