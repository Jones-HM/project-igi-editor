#pragma once

#include <string>

struct DebugCommand {
    std::string type;
    int level = -1;
    int val = 0;
    std::string taskId;
    std::string modelId;
    std::string path;
    bool has_pos = false;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double yaw = 0.0;
    double pitch = 0.0;
    int orbit_frames = 0;
    int video_fps = 12;
};
