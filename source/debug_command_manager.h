#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include "debug_command_queue.h"

class App; // Forward declaration

class DebugCommandManager {
public:
    DebugCommandManager(App* app);
    ~DebugCommandManager();

    void Start();
    void Stop();
    void Update();

private:
    void WatcherThread();
    void ProcessCommand(const DebugCommand& cmd);
    void GotoModel(const DebugCommand& cmd);
    void SetCamera(const DebugCommand& cmd);
    void CaptureModel(const DebugCommand& cmd);
    void CaptureSplineTrace(const DebugCommand& cmd);
    void DeleteModel(const DebugCommand& cmd);

    App* app_;
    std::thread watcher_thread_;
    std::atomic<bool> running_;
    DebugCommandQueue command_queue_;
    std::string commands_file_path_;
};
