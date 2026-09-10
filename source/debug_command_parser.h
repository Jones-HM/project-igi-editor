#pragma once

#include "debug_command.h"

#include <cmath>
#include <optional>
#include <sstream>

inline std::optional<DebugCommand> ParseDebugCommand(const std::string& line) {
    std::istringstream input(line);
    DebugCommand command;
    if (!(input >> command.type) ||
        (command.type != "goto" && command.type != "set-camera" && command.type != "capture-model" && command.type != "capture-spline" && command.type != "delete" &&
         command.type != "wireframe" && command.type != "draw-parts" && command.type != "reset-level")) {
        return std::nullopt;
    }
    std::string token;
    try {
        while (input >> token) {
            if (token.starts_with("level=")) command.level = std::stoi(token.substr(6));
            else if (token.starts_with("model=")) command.modelId = token.substr(6);
            else if (token.starts_with("task=")) command.taskId = token.substr(5);
            else if (token.starts_with("path=")) command.path = token.substr(5);
            else if (token.starts_with("val=")) command.val = std::stoi(token.substr(4));
            else if (token.starts_with("x=")) { command.x = std::stod(token.substr(2)); command.has_pos = true; }
            else if (token.starts_with("y=")) { command.y = std::stod(token.substr(2)); command.has_pos = true; }
            else if (token.starts_with("z=")) { command.z = std::stod(token.substr(2)); command.has_pos = true; }
            else if (token.starts_with("yaw=")) command.yaw = std::stod(token.substr(4));
            else if (token.starts_with("pitch=")) command.pitch = std::stod(token.substr(6));
            else if (token.starts_with("orbit_frames=")) command.orbit_frames = std::stoi(token.substr(13));
            else if (token.starts_with("video_fps=")) command.video_fps = std::stoi(token.substr(10));
            else if (token.starts_with("orbit=")) command.orbit_frames = std::stoi(token.substr(6));
        }
    } catch (const std::exception&) { return std::nullopt; }
    if (command.type == "set-camera" &&
        (!command.has_pos || !std::isfinite(command.x) || !std::isfinite(command.y) ||
         !std::isfinite(command.z) || !std::isfinite(command.yaw) ||
         !std::isfinite(command.pitch))) return std::nullopt;
    if (command.type == "capture-spline" && command.path.empty()) return std::nullopt;
    return command;
}
