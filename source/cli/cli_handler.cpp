#include "pch.h"
#include "cli_handler.h"
#include "cli_tests.h"
#include "common.h"
#include "logger.h"
#include "renderer/res_writer.h"
#include "renderer/renderer_objects.h"
#include "utils.h"
#include <filesystem>
#include <algorithm>

#include <iostream>

bool CLIHandler::IsCLICommand(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "--run-tests" ||
        arg == "--extract-level" || arg == "--verify-level" ||
        arg == "--import-model") {
      return true;
    }
  }
  return false;
}

void CLIHandler::PrintHelp() {
  std::cout
      << "IGI Editor v3.4.1-pre - Game Editor\n\n"
      << "GUI Editor Mode Options:\n"
      << "  -level <num>            Load specific level (1-14)\n"
      << "  -w <width> -h <height>  Set window dimensions\n"
      << "  -draw_parts <bitmask>   Selective loading (1=terrain, 2=sky, 4=objects, 8=flat_sky, 16=buildings, 32=props, 64=AI)\n"
      << "  -stick_to_ground        Enable ground sticking on load\n\n"
      << "Editor CLI Commands:\n"
      << "  --help                  Show this message\n"
      << "  --run-tests             Run all native C++ parser unit tests\n"
      << "  --verify-level --level N [--level N ...]  Verify level output\n"
      << "      --skip-launch       Skip launching editor\n"
      << "      --timeout <sec>     Kill editor after N seconds\n"
      << "      --game-path <path>  IGI1 install path\n"
      << "      --log <path>        Override log file path\n"
      << "      --report-json <f>   Write aggregated JSON report\n"
      << "      --report-md <f>     Write aggregated Markdown report\n"
      << "      --report-dir <dir>  Write per-level reports to dir\n"
      << "      --delay <sec>       Delay between levels (default: 5)\n"
      << "  --extract-level <N> [outdir]  Extract level N resources\n\n"
      << "  --import-model <N> <model> [--source-level <N>]  Import a model family into level N\n\n"
      << "For asset conversion, use igi1conv.exe (in editor/tools/):\n"
      << "  igi1conv tex / mef / qsc / qvm / res / mtp / dat / terrain / fnt / graph\n"
      << "  Run: igi1conv --help\n";
}

int CLIHandler::Process(int argc, char **argv) {
  std::string argsStr;
  for (int i = 0; i < argc; ++i) {
    if (i > 0)
      argsStr += " ";
    argsStr += argv[i];
  }
  Logger::Get().Log(LogLevel::INFO,
                    "[CLI] Process called with arguments: " + argsStr);

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help") {
      Logger::Get().Log(LogLevel::INFO, "[CLI] Help command requested.");
      PrintHelp();
      return 0;
    } else if (arg == "--run-tests") {
      Logger::Get().Log(LogLevel::INFO, "[CLI] Run tests command requested.");
      return RunAllTests();
    } else if (arg == "--import-model" && i + 2 < argc) {
      int levelNo = std::stoi(argv[++i]);
      std::string modelId = argv[++i];
      int sourceLevel = 0;
      if (i + 2 < argc && std::string(argv[i + 1]) == "--source-level") {
        sourceLevel = std::stoi(argv[i + 2]);
        i += 2;
      }
      Renderer_Objects robj;
      robj.SetLevel(levelNo);
      bool ok = robj.AddModelToLevelRes(modelId, [](size_t done, size_t total) {
        if (total > 0 && (done == 0 || done == total || done % 5 == 0)) {
          std::cout << "[Import] Packing textures: " << done << "/" << total << std::endl;
        }
      }, sourceLevel);
      if (ok) {
        std::cout << "[Import] SUCCESS: Model '" << modelId << "' imported into level " << levelNo << std::endl;
        return 0;
      } else {
        std::cerr << "[Import] FAILED: Could not import model '" << modelId << "' into level " << levelNo << std::endl;
        return 1;
      }
    } else if (arg == "--extract-level" && i + 1 < argc) {
      int levelNo = std::stoi(argv[++i]);
      std::string outDir = (i + 1 < argc)
                               ? argv[++i]
                               : Utils::GetExeDirectory() + "\\levels\\level" +
                                     std::to_string(levelNo);
      return ExtractLevelResources(levelNo, outDir);
    } else if (arg == "--verify-level") {
      VerifyLevelParams params;
      // Consume all remaining arguments that belong to --verify-level
      while (i + 1 < argc) {
        std::string nx = argv[i + 1];
        if (nx == "--level" && i + 2 < argc) {
          params.levels.push_back(std::stoi(argv[i + 2])); i += 2;
        } else if (nx == "--skip-launch") {
          params.skipLaunch = true; ++i;
        } else if (nx == "--timeout" && i + 2 < argc) {
          params.timeout = std::stoi(argv[i + 2]); i += 2;
        } else if (nx == "--game-path" && i + 2 < argc) {
          params.gamePath = argv[i + 2]; i += 2;
        } else if (nx == "--log" && i + 2 < argc) {
          params.logPath = argv[i + 2]; i += 2;
        } else if (nx == "--report-json" && i + 2 < argc) {
          params.reportJson = argv[i + 2]; i += 2;
        } else if (nx == "--report-md" && i + 2 < argc) {
          params.reportMd = argv[i + 2]; i += 2;
        } else if (nx == "--report-dir" && i + 2 < argc) {
          params.reportDir = argv[i + 2]; i += 2;
        } else if (nx == "--delay" && i + 2 < argc) {
          params.delay = std::stoi(argv[i + 2]); i += 2;
        } else {
          // Bare integer treated as a level number (backwards-compat: --verify-level 1)
          try { params.levels.push_back(std::stoi(nx)); ++i; }
          catch (...) { break; }
        }
      }
      if (params.levels.empty()) {
        std::cerr << "[VerifyLevel] ERROR: no levels specified. Use --level N.\n";
        return 1;
      }
      return VerifyLevel(params);
    }
  }
  Logger::Get().Log(LogLevel::WARNING,
                    "[CLI] Unknown or invalid arguments processed.");
  PrintHelp();
  return 1;
}

int CLIHandler::ExtractLevelResources(int levelNo, const std::string &outDir) {
  std::string igiPath = Utils::GetIGIRootPath();
  std::string levelPath =
      igiPath + "\\missions\\location0\\level" + std::to_string(levelNo);

  Logger::Get().Log(LogLevel::INFO, "[CLI] Extracting level " +
                                        std::to_string(levelNo) + " resources");
  Logger::Get().Log(LogLevel::INFO, "[CLI] IGI level path: " + levelPath);
  Logger::Get().Log(LogLevel::INFO, "[CLI] Output dir:     " + outDir);

  namespace fs = std::filesystem;
  try {
    fs::create_directories(outDir + "\\models\\level" + std::to_string(levelNo));
    fs::create_directories(outDir + "\\models");
    fs::create_directories(outDir + "\\textures\\level" + std::to_string(levelNo));
    fs::create_directories(outDir + "\\textures");
    fs::create_directories(outDir + "\\terrain");
  } catch (const std::exception &e) {
    Logger::Get().Log(LogLevel::ERR, "[CLI] Failed to create output dirs: " +
                                         std::string(e.what()));
    return 1;
  }

  int totalExtracted = 0;

  // --- Extract models from level1.res (or loose .mef files) ---
  std::string resFile =
      levelPath + "\\models\\level" + std::to_string(levelNo) + ".res";
  if (fs::exists(resFile)) {
    Logger::Get().Log(LogLevel::INFO,
                      "[CLI] Extracting models from: " + resFile);
    RESFile res = RES_Parse(resFile);
    if (res.valid) {
      for (const auto &entry : res.entries) {
        std::string name = entry.name;
        // Strip path prefix (e.g. "LOCAL:models/xxx.mef" -> "xxx.mef")
        size_t slash = name.find_last_of("/\\");
        std::string filename =
            (slash != std::string::npos) ? name.substr(slash + 1) : name;
        // Write to levelN subdir so FindModelFile's per-level search finds it.
        std::string destPath = outDir + "\\models\\level" + std::to_string(levelNo) + "\\" + filename;
        std::ofstream f(destPath, std::ios::binary);
        if (f) {
          f.write(reinterpret_cast<const char *>(entry.data.data()),
                  entry.data.size());
          totalExtracted++;
        }
      }
      Logger::Get().Log(LogLevel::INFO, "[CLI] Extracted " +
                                            std::to_string(totalExtracted) +
                                            " models from RES");
    } else {
      Logger::Get().Log(LogLevel::WARNING,
                        "[CLI] Could not parse RES: " + res.error);
    }
  } else {
    // Fallback: copy loose .mef files
    std::string modelsDir = levelPath + "\\models";
    if (fs::exists(modelsDir)) {
      for (const auto &entry : fs::directory_iterator(modelsDir)) {
        if (entry.path().extension() == ".mef") {
          std::string dest =
              outDir + "\\models\\level" + std::to_string(levelNo) + "\\" + entry.path().filename().string();
          fs::copy_file(entry.path(), dest,
                        fs::copy_options::overwrite_existing);
          totalExtracted++;
        }
      }
      Logger::Get().Log(LogLevel::INFO, "[CLI] Copied " +
                                            std::to_string(totalExtracted) +
                                            " loose MEF files");
    }
  }

  // --- Extract textures: loose .tex files OR from textures .res archive ---
  int texCount = 0;
  std::string texDir = levelPath + "\\textures";
  // Prefer a per-level subdirectory so FindTextureFile's levelN search works.
  std::string texOutDir = outDir + "\\textures\\level" + std::to_string(levelNo);
  fs::create_directories(texOutDir);

  if (fs::exists(texDir)) {
    // Try .res archive first (e.g. level6/textures/level6.res)
    std::string texResFile = texDir + "\\level" + std::to_string(levelNo) + ".res";
    if (fs::exists(texResFile)) {
      RESFile texRes = RES_Parse(texResFile);
      if (texRes.valid) {
        for (const auto& entry : texRes.entries) {
          size_t slash = entry.name.find_last_of("/\\");
          std::string filename = (slash != std::string::npos) ? entry.name.substr(slash + 1) : entry.name;
          if (filename.size() < 4) continue;
          std::string ext = filename.substr(filename.size() - 4);
          for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
          if (ext != ".tex") continue;
          std::string destPath = texOutDir + "\\" + filename;
          std::ofstream f(destPath, std::ios::binary);
          if (f) { f.write(reinterpret_cast<const char*>(entry.data.data()), entry.data.size()); texCount++; }
        }
        Logger::Get().Log(LogLevel::INFO, "[CLI] Extracted " + std::to_string(texCount) + " TEX files from " + texResFile);
      }
    } else {
      // Fallback: copy loose .tex files
      for (const auto& entry : fs::directory_iterator(texDir)) {
        if (entry.path().extension() == ".tex") {
          std::string dest = texOutDir + "\\" + entry.path().filename().string();
          fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing);
          texCount++;
        }
      }
      Logger::Get().Log(LogLevel::INFO, "[CLI] Copied " + std::to_string(texCount) + " TEX files");
    }
  }

  // --- Copy terrain files ---
  int terrainCount = 0;
  std::string terrainDir = levelPath + "\\terrain";
  if (fs::exists(terrainDir)) {
    for (const auto &entry : fs::directory_iterator(terrainDir)) {
      std::string dest =
          outDir + "\\terrain\\" + entry.path().filename().string();
      fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing);
      terrainCount++;
    }
    Logger::Get().Log(LogLevel::INFO, "[CLI] Copied " +
                                          std::to_string(terrainCount) +
                                          " terrain files");
  }

  std::cout << "[ExtractLevel] Level " << levelNo
            << ": models=" << totalExtracted << " textures=" << texCount
            << " terrain=" << terrainCount << "\n";
  return (totalExtracted + texCount + terrainCount > 0) ? 0 : 1;
}
