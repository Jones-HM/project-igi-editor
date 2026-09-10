/******************************************************************************
 * @file    dat_parser.cpp
 * @brief   IGI level DAT model-texture mapping parser
 *****************************************************************************/

#include "dat_writer.h"
#include "../logger.h"
#include "../utils.h"
#include <fstream>
#include <sstream>
#include <algorithm>

// Maximum plausible material count for a single model.
// The DAT has a texture-manifest section after all model entries; its header
// is the total texture count (e.g. "393"), which stoi() would parse as a huge
// number from the first texture name ("207_01_1" → 207). Stopping here
// prevents the parser from drifting into the manifest section.
static constexpr int kMaxMaterialsPerModel = 64;

DATFile DAT_Parse(const std::string& filepath) {
    DATFile result;

    std::ifstream file(filepath);
    if (!file.is_open()) {
        result.error = "Cannot open file: " + filepath;
        Logger::Get().Log(LogLevel::ERR, "[DAT] " + result.error);
        return result;
    }

    // Tokenise: skip blank lines, *** comments, and // comments
    std::vector<std::string> tokens;
    std::string line;
    while (std::getline(file, line)) {
        line = Utils::Trim(line);
        if (line.empty() || line.rfind("***", 0) == 0 || line.rfind("//", 0) == 0)
            continue;
        tokens.push_back(line);
    }

    if (tokens.empty()) {
        result.error = "Empty file: " + filepath;
        Logger::Get().Log(LogLevel::ERR, "[DAT] " + result.error);
        return result;
    }

    // token[0] = declared total model count
    try {
        result.declaredModelCount = std::stoi(tokens[0]);
    } catch (...) {
        result.error = "Failed to parse model count token: " + tokens[0];
        Logger::Get().Log(LogLevel::ERR, "[DAT] " + result.error);
        return result;
    }

    size_t cursor = 1;

    // --- 0. Model section ---
    for (int m = 0; m < result.declaredModelCount && cursor < tokens.size(); ++m) {
        if (cursor >= tokens.size()) break;
        const std::string modelName = tokens[cursor++];
        
        int matCount = 0;
        if (cursor < tokens.size()) {
            try { matCount = std::stoi(tokens[cursor++]); }
            catch (...) { break; }
            if (matCount > kMaxMaterialsPerModel) matCount = kMaxMaterialsPerModel;
        }

        DATModelEntry entry;
        entry.modelName = modelName;
        for (int i = 0; i < matCount && cursor < tokens.size(); ++i) {
            entry.textures.push_back(tokens[cursor++]);
        }
        result.models.push_back(std::move(entry));
    }

    // Backwards-compatibility for older files that might be malformed or lack strict bounds:
    // If the next token isn't a valid integer count for the texture manifest, we might have to scan.
    // However, since we now strictly parse by counts like the Python script, we continue sequentially.

    // --- 1. Texture manifest section ---
    if (cursor < tokens.size()) {
        try {
            result.declaredTextureCount = std::stoi(tokens[cursor++]);
            // Read exactly declaredTextureCount textures so VNAM/BANM/shadow sections can follow.
            // level1.dat declares 426 but has 427 entries; the extra token is consumed harmlessly
            // as a failed VNAM count parse and silently caught below.
            int texRemaining = (int)(tokens.size() - cursor);
            int texLimit = result.declaredTextureCount < texRemaining
                         ? result.declaredTextureCount : texRemaining;
            for (int i = 0; i < texLimit; ++i) {
                result.allTextures.push_back(tokens[cursor++]);
            }
        } catch (...) {
            // Recover if needed
        }
    }

    // --- 2. VNAM section (Optional) ---
    if (cursor < tokens.size()) {
        try {
            int vnamCount = std::stoi(tokens[cursor++]);
            for (int v = 0; v < vnamCount && cursor < tokens.size(); ++v) {
                DATVnamEntry ve;
                if (cursor < tokens.size()) ve.mainModelName = tokens[cursor++];
                if (cursor < tokens.size()) ve.virModelName = tokens[cursor++];
                
                int texCount = 0;
                if (cursor < tokens.size()) texCount = std::stoi(tokens[cursor++]);
                
                for (int t = 0; t < texCount && cursor < tokens.size(); ++t) {
                    ve.textures.push_back(tokens[cursor++]);
                }
                result.vnam_models.push_back(std::move(ve));
            }
        } catch (...) {}
    }

    // --- 3. Bone Animations section (Optional) ---
    if (cursor < tokens.size()) {
        try {
            int animCount = std::stoi(tokens[cursor++]);
            for (int a = 0; a < animCount && cursor < tokens.size(); ++a) {
                result.animations.push_back(tokens[cursor++]);
            }
        } catch (...) {}
    }

    // --- 4. Shadow Models section (Optional) ---
    if (cursor < tokens.size()) {
        try {
            int shadowCount = std::stoi(tokens[cursor++]);
            for (int s = 0; s < shadowCount && cursor < tokens.size(); ++s) {
                result.shadows.push_back(tokens[cursor++]);
            }
        } catch (...) {}
    }

    Logger::Get().Log(LogLevel::INFO,
        "[DAT] Parsed " + filepath +
        " | models=" + std::to_string(result.models.size()) +
        " | textures=" + std::to_string(result.allTextures.size()) +
        " | vnam=" + std::to_string(result.vnam_models.size()) +
        " | anims=" + std::to_string(result.animations.size()) +
        " | shadows=" + std::to_string(result.shadows.size()));

    result.valid = true;
    return result;
}

bool DAT_WriteNative(const DATFile& dat, const std::string& outPath, std::string& err) {
    // Open binary so we control CRLF exactly (no platform newline translation).
    std::ofstream f(outPath, std::ios::binary);
    if (!f.is_open()) {
        err = "Cannot open output file: " + outPath;
        Logger::Get().Log(LogLevel::ERR, "[DAT] " + err);
        return false;
    }

    auto emit = [&f](const std::string& s) { f << s << "\r\n"; };

    // Machine-generated header (two *** lines + a blank line — all skipped on read).
    emit("*** This file is machine generated");
    emit("*** DO NOT EDIT!");
    emit("");

    // Total model count
    emit(std::to_string(dat.models.size())); // Use actual count

    for (const auto& m : dat.models) {
        emit(m.modelName);
        emit(std::to_string(m.textures.size()));
        for (const auto& t : m.textures)
            emit(t);
    }

    // Texture manifest: total count, then each texture name.
    emit(std::to_string(dat.allTextures.size()));
    for (const auto& t : dat.allTextures)
        emit(t);

    // Optional sections: VNAM, animations, shadows.
    emit(std::to_string(dat.vnam_models.size()));
    for (const auto& v : dat.vnam_models) {
        emit(v.mainModelName);
        emit(v.virModelName);
        emit(std::to_string(v.textures.size()));
        for (const auto& t : v.textures)
            emit(t);
    }
    emit(std::to_string(dat.animations.size()));
    for (const auto& a : dat.animations)
        emit(a);
    emit(std::to_string(dat.shadows.size()));
    for (const auto& s : dat.shadows)
        emit(s);

    if (!f.good()) {
        err = "Write error to: " + outPath;
        Logger::Get().Log(LogLevel::ERR, "[DAT] " + err);
        return false;
    }
    Logger::Get().Log(LogLevel::INFO, "[DAT] Wrote native DAT to: " + outPath);
    return true;
}

void DAT_AddModel(DATFile& dat, const std::string& modelName,
                  const std::vector<std::string>& textureNames, bool& alreadyPresent) {
    alreadyPresent = false;

    // Idempotency check (ignore the `waypoint` sentinel).
    for (const auto& m : dat.models) {
        if (m.modelName == "waypoint")
            continue;
        if (m.modelName == modelName) {
            alreadyPresent = true;
            return;
        }
    }

    DATModelEntry entry;
    entry.modelName = modelName;
    entry.textures  = textureNames;

    // Insert before the trailing `waypoint` entry so the writer emits the new model
    // ahead of waypoint/0 (preserving the manifest-boundary structure DAT_Parse needs).
    auto wp = std::find_if(dat.models.begin(), dat.models.end(),
                           [](const DATModelEntry& m) { return m.modelName == "waypoint"; });
    dat.models.insert(wp, std::move(entry));
    dat.declaredModelCount += 1;

    // Add any new textures to the manifest, bumping the declared count per addition.
    for (const auto& t : textureNames) {
        if (std::find(dat.allTextures.begin(), dat.allTextures.end(), t) == dat.allTextures.end()) {
            dat.allTextures.push_back(t);
            dat.declaredTextureCount += 1;
        }
    }
}

bool DAT_UpsertModel(DATFile& dat, const std::string& modelName,
                     const std::vector<std::string>& textureNames) {
    for (auto& model : dat.models) {
        if (model.modelName != modelName) continue;
        if (model.textures == textureNames) return false;
        model.textures = textureNames;
        for (const auto& texture : textureNames) {
            if (std::find(dat.allTextures.begin(), dat.allTextures.end(), texture) == dat.allTextures.end()) {
                dat.allTextures.push_back(texture);
            }
        }
        dat.declaredTextureCount = static_cast<int>(dat.allTextures.size());
        return true;
    }

    bool alreadyPresent = false;
    DAT_AddModel(dat, modelName, textureNames, alreadyPresent);
    return !alreadyPresent;
}

std::string DAT_FormatReport(const DATFile& dat, const std::string& modelFilter) {
    std::ostringstream out;

    out << "=== IGI DAT Model-Texture Map ===\n"
        << "Models declared : " << dat.declaredModelCount  << "\n"
        << "Models parsed   : " << dat.models.size()       << "\n"
        << "Textures declared: " << dat.declaredTextureCount << "\n"
        << "Textures in manifest: " << dat.allTextures.size() << "\n"
        << "\n";

    for (const auto& m : dat.models) {
        if (!modelFilter.empty() &&
            m.modelName.find(modelFilter) == std::string::npos)
            continue;

        out << m.modelName << "\n"
            << "  materials: " << m.textures.size() << "\n";
        for (size_t i = 0; i < m.textures.size(); ++i)
            out << "  [" << i << "] " << m.textures[i] << "\n";
    }

    if (!modelFilter.empty())
        return out.str();

    // Append texture manifest
    out << "\n=== Texture Manifest (" << dat.allTextures.size() << " entries) ===\n";
    for (size_t i = 0; i < dat.allTextures.size(); ++i)
        out << "  [" << i << "] " << dat.allTextures[i] << "\n";

    return out.str();
}

bool DAT_WriteReport(const DATFile& dat, const std::string& outPath,
                     const std::string& modelFilter) {
    std::ofstream f(outPath);
    if (!f.is_open()) {
        Logger::Get().Log(LogLevel::ERR, "[DAT] Cannot write report to: " + outPath);
        return false;
    }
    f << DAT_FormatReport(dat, modelFilter);
    Logger::Get().Log(LogLevel::INFO, "[DAT] Report written to: " + outPath);
    return true;
}

// Escape a string for JSON (handles backslashes and double-quotes only;
// all DAT content is plain ASCII so no unicode escaping is needed).
static std::string JsonStr(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    out += '"';
    return out;
}

std::string DAT_FormatJSON(const DATFile& dat, const std::string& modelFilter) {
    std::ostringstream out;
    out << "{\n"
        << "  \"declaredModelCount\": "   << dat.declaredModelCount   << ",\n"
        << "  \"parsedModelCount\": "     << dat.models.size()        << ",\n"
        << "  \"declaredTextureCount\": " << dat.declaredTextureCount << ",\n"
        << "  \"manifestTextureCount\": " << dat.allTextures.size()   << ",\n"
        << "  \"models\": [\n";

    bool firstModel = true;
    for (const auto& m : dat.models) {
        if (!modelFilter.empty() &&
            m.modelName.find(modelFilter) == std::string::npos)
            continue;

        if (!firstModel) out << ",\n";
        firstModel = false;

        out << "    {\n"
            << "      \"modelName\": " << JsonStr(m.modelName) << ",\n"
            << "      \"materialCount\": " << m.textures.size() << ",\n"
            << "      \"textures\": [";

        for (size_t i = 0; i < m.textures.size(); ++i) {
            if (i > 0) out << ", ";
            out << JsonStr(m.textures[i]);
        }
        out << "]\n"
            << "    }";
    }
    out << "\n  ]";

    // Include texture manifest only when not filtering
    if (modelFilter.empty()) {
        out << ",\n  \"textureManifest\": [";
        for (size_t i = 0; i < dat.allTextures.size(); ++i) {
            if (i > 0) out << ", ";
            out << JsonStr(dat.allTextures[i]);
        }
        out << "]";
    }

    out << "\n}\n";
    return out.str();
}

bool DAT_WriteJSON(const DATFile& dat, const std::string& outPath,
                   const std::string& modelFilter) {
    std::ofstream f(outPath);
    if (!f.is_open()) {
        Logger::Get().Log(LogLevel::ERR, "[DAT] Cannot write JSON to: " + outPath);
        return false;
    }
    f << DAT_FormatJSON(dat, modelFilter);
    Logger::Get().Log(LogLevel::INFO, "[DAT] JSON written to: " + outPath);
    return true;
}
