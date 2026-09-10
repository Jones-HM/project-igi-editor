#include "pch.h"
#include "res_compiler.h"
#include "../logger.h"
#include "../level/qsc_lexer.h"
#include "../level/qsc_parser.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cctype>

static const uint32_t FOURCC_ILFF = 0x46464C49; // "ILFF"
static const uint32_t FOURCC_IRES = 0x53455249; // "IRES"
static const uint32_t FOURCC_NAME = 0x454D414E; // "NAME"
static const uint32_t FOURCC_BODY = 0x59444F42; // "BODY"

static void WriteU32LE(std::ostream& os, uint32_t val) {
    uint8_t buf[4] = {
        static_cast<uint8_t>(val & 0xFF),
        static_cast<uint8_t>((val >> 8) & 0xFF),
        static_cast<uint8_t>((val >> 16) & 0xFF),
        static_cast<uint8_t>((val >> 24) & 0xFF)
    };
    os.write(reinterpret_cast<const char*>(buf), 4);
}

static void WriteFourCC(std::ostream& os, uint32_t val) {
    WriteU32LE(os, val);
}

// Shared ILFF chunk encoder used by RES_WriteEntries and RES_StreamAppend so the
// two functions never drift on the on-disk chunk format. Writes:
//   fourcc | uint32 dataSize | uint32 align(4) | uint32 skip | data | pad-to-4.
// `lastBody` forces skip=0 (end-of-archive sentinel) for the very last BODY chunk.
static void WriteResChunk(std::ostream& os, uint32_t fourcc,
                          const uint8_t* data, uint32_t dataSize, bool lastBody) {
    uint32_t padding = (4 - (dataSize % 4)) % 4;
    uint32_t skip = lastBody ? 0 : (16 + dataSize + padding);
    WriteFourCC(os, fourcc);
    WriteU32LE(os, dataSize);
    WriteU32LE(os, 4);     // align
    WriteU32LE(os, skip);
    if (dataSize) os.write(reinterpret_cast<const char*>(data), dataSize);
    if (padding) { char pad[3] = {0}; os.write(pad, padding); }
}

bool RES_GenerateQSC(const std::string& inputDir, const std::string& outQscPath, const std::string& outResName, std::string& error) {
    std::ofstream out(outQscPath);
    if (!out) {
        error = "Failed to create " + outQscPath;
        return false;
    }

    out << "BeginResource(\"" << outResName << "\");\n";

    namespace fs = std::filesystem;
    try {
        if (!fs::exists(inputDir)) {
            error = "Input directory does not exist: " + inputDir;
            return false;
        }

        std::string qscBase = fs::path(outQscPath).filename().string();
        for (const auto& entry : fs::recursive_directory_iterator(inputDir)) {
            if (!entry.is_regular_file()) continue;
            // Skip the generated QSC itself
            if (entry.path().filename().string() == qscBase) continue;

            std::string relPath = fs::relative(entry.path(), inputDir).string();
            for (auto& c : relPath) if (c == '\\') c = '/';
            out << "AddResource(\"" << relPath << "\", \"LOCAL:" << relPath << "\");\n";
        }
    } catch(const std::exception& e) {
        error = "Filesystem error: " + std::string(e.what());
        return false;
    }

    out << "EndResource();\n";
    return true;
}

bool RES_Compile(const std::string& scriptPath, std::string& error) {
    std::ifstream f(scriptPath, std::ios::binary);
    if (!f) {
        error = "Failed to open script: " + scriptPath;
        return false;
    }
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto lr = qsc::Lex(src);
    if (!lr.ok) { error = "Lex error: " + lr.error; return false; }
    auto pr = qsc::Parse(lr.tokens);
    if (!pr.ok) { error = "Parse error: " + pr.error; return false; }

    if (!pr.program || pr.program->kind != qsc::NodeKind::Program) {
        error = "Invalid script format";
        return false;
    }

    std::string outResName;
    std::vector<std::pair<std::string, std::string>> resources;

    for (const auto& stmt : pr.program->children) {
        if (stmt->kind == qsc::NodeKind::ExprStmt && !stmt->children.empty()) {
            auto call = stmt->children[0].get();
            if (call->kind == qsc::NodeKind::Call) {
                if (call->s_val == "BeginResource" && call->children.size() == 1) {
                    if (call->children[0]->kind == qsc::NodeKind::StringLit) {
                        outResName = call->children[0]->s_val;
                    }
                } else if (call->s_val == "AddResource" && call->children.size() == 2) {
                    if (call->children[0]->kind == qsc::NodeKind::StringLit && call->children[1]->kind == qsc::NodeKind::StringLit) {
                        resources.push_back({call->children[0]->s_val, call->children[1]->s_val});
                    }
                }
            }
        }
    }

    if (outResName.empty()) {
        error = "BeginResource(\"out\") not found in script";
        return false;
    }

    namespace fs = std::filesystem;
    fs::path baseDir = fs::path(scriptPath).parent_path();
    fs::path outPath = baseDir / outResName;

    std::ofstream os(outPath, std::ios::binary);
    if (!os) {
        error = "Failed to create output file: " + outPath.string();
        return false;
    }

    // ILFF header (20 bytes), matching the real game .res files exactly:
    //   "ILFF" | uint32 size (TOTAL file size) | uint32 align (4) | uint32 skip (0) | "IRES"
    // The size field is patched to the final file size at the end. (The old code
    // wrote size = finalSize-8 and align = 0, which the game's loader rejected.)
    WriteFourCC(os, FOURCC_ILFF);
    WriteU32LE(os, 0);   // size — patched below
    WriteU32LE(os, 4);   // align = 4 (as in every shipped .res)
    WriteU32LE(os, 0);   // skip = 0 for the top-level ILFF
    WriteFourCC(os, FOURCC_IRES);

    for (const auto& res : resources) {
        std::string internalName = res.first;
        std::string localPath = res.second;
        
        if (localPath.substr(0, 6) == "LOCAL:") {
            localPath = localPath.substr(6);
        }
        fs::path fullLocalPath = baseDir / localPath;
        
        std::ifstream srcF(fullLocalPath, std::ios::binary | std::ios::ate);
        if (!srcF) {
            Logger::Get().Log(LogLevel::WARNING, "[RES] Could not read file, skipping: " + fullLocalPath.string());
            continue;
        }
        std::streamsize dataSize = srcF.tellg();
        srcF.seekg(0, std::ios::beg);
        
        // Chunk NAME
        uint32_t nameSize = internalName.length() + 1; // include null
        uint32_t namePadding = (4 - (nameSize % 4)) % 4;
        uint32_t nameSkip = 16 + nameSize + namePadding;
        
        WriteFourCC(os, FOURCC_NAME);
        WriteU32LE(os, nameSize);
        WriteU32LE(os, 4); // align = 4 (matches shipped .res chunks)
        WriteU32LE(os, nameSkip);
        os.write(internalName.c_str(), nameSize);
        if (namePadding > 0) {
            char pad[3] = {0};
            os.write(pad, namePadding);
        }

        // Chunk BODY — last entry must have skip=0 (end-of-archive sentinel).
        uint32_t bodySize = dataSize;
        uint32_t bodyPadding = (4 - (bodySize % 4)) % 4;
        bool isLastRes = (&res == &resources.back());
        uint32_t bodySkip = isLastRes ? 0 : (16 + bodySize + bodyPadding);

        WriteFourCC(os, FOURCC_BODY);
        WriteU32LE(os, bodySize);
        WriteU32LE(os, 4); // align = 4 (matches shipped .res chunks)
        WriteU32LE(os, bodySkip);
        
        char buf[8192];
        while (srcF.read(buf, sizeof(buf))) {
            os.write(buf, srcF.gcount());
        }
        os.write(buf, srcF.gcount());
        
        if (bodyPadding > 0) {
            char pad[3] = {0};
            os.write(pad, bodyPadding);
        }
    }

    // Patch the ILFF size field (offset 4) to the TOTAL file size, exactly as the
    // shipped archives store it (verified against SOUNDS/SPRITES/COMMON/TEXTURES.res).
    uint32_t finalSize = static_cast<uint32_t>(os.tellp());
    os.seekp(4, std::ios::beg);
    WriteU32LE(os, finalSize);

    return true;
}

bool RES_WriteEntries(const std::vector<RESEntry>& entries, const std::string& outPath, std::string& error) {
    std::ofstream os(outPath, std::ios::binary);
    if (!os) { error = "Failed to create output file: " + outPath; return false; }

    // ILFF header (size patched at end).
    WriteFourCC(os, FOURCC_ILFF);
    WriteU32LE(os, 0);   // size — patched below
    WriteU32LE(os, 4);   // align
    WriteU32LE(os, 0);   // skip
    WriteFourCC(os, FOURCC_IRES);

    for (size_t ei = 0; ei < entries.size(); ++ei) {
        const auto& e = entries[ei];
        bool isLast = (ei == entries.size() - 1);

        // NAME chunk: null-terminated name.
        std::vector<uint8_t> nameBytes(e.name.begin(), e.name.end());
        nameBytes.push_back(0);
        WriteResChunk(os, FOURCC_NAME, nameBytes.data(), (uint32_t)nameBytes.size(), false);

        // BODY chunk. The LAST BODY must have skip=0 — the game parser uses
        // this as the end-of-archive sentinel. All earlier BODY chunks carry
        // the normal skip so the parser advances to the next NAME chunk.
        WriteResChunk(os, FOURCC_BODY, e.data.data(), (uint32_t)e.data.size(), isLast);
    }

    // Verify the stream survived all writes before patching the header.
    if (!os.good()) { error = "write failed (stream error) for " + outPath; return false; }

    uint32_t finalSize = static_cast<uint32_t>(os.tellp());
    os.seekp(4, std::ios::beg);
    WriteU32LE(os, finalSize);
    os.flush();
    if (!os.good()) { error = "header patch/flush failed for " + outPath; return false; }
    return true;
}

bool RES_StreamAppend(const std::string& srcResPath,
                      const std::vector<RESEntry>& newEntries,
                      const std::string& outPath,
                      std::string& error,
                      const std::function<void(size_t,size_t)>& onProgress) {
    if (newEntries.empty()) {
        error = "RES_StreamAppend requires at least one new entry";
        return false;
    }

    // First pass: count source entries so onProgress totals are accurate. This only
    // streams chunk headers' worth of work via RES_ForEachEntry (peak RAM = one entry).
    size_t srcCount = 0;
    std::string ferr;
    if (!RES_ForEachEntry(srcResPath,
            [&](const std::string&, const uint8_t*, size_t) { ++srcCount; }, ferr)) {
        error = "could not read source archive: " + ferr;
        return false;
    }

    std::ofstream os(outPath, std::ios::binary);
    if (!os) { error = "Failed to create output file: " + outPath; return false; }

    // ILFF header (size patched at end).
    WriteFourCC(os, FOURCC_ILFF);
    WriteU32LE(os, 0);   // size — patched below
    WriteU32LE(os, 4);   // align
    WriteU32LE(os, 0);   // skip
    WriteFourCC(os, FOURCC_IRES);

    const size_t total = srcCount + newEntries.size();
    size_t done = 0;

    // Second pass: stream every source entry through. None of these is the archive's
    // final BODY (the appended newEntries follow), so all get a NORMAL skip.
    bool streamFailed = false;
    if (!RES_ForEachEntry(srcResPath,
            [&](const std::string& name, const uint8_t* data, size_t size) {
                if (streamFailed) return;
                std::vector<uint8_t> nameBytes(name.begin(), name.end());
                nameBytes.push_back(0);
                WriteResChunk(os, FOURCC_NAME, nameBytes.data(), (uint32_t)nameBytes.size(), false);
                WriteResChunk(os, FOURCC_BODY, data, (uint32_t)size, false);
                if (!os.good()) { streamFailed = true; return; }
                if (onProgress) onProgress(++done, total);
            }, ferr)) {
        error = "source stream failed: " + ferr;
        return false;
    }
    if (streamFailed || !os.good()) {
        error = "write failed while streaming source entries to " + outPath;
        return false;
    }

    // Append the new entries; the very LAST BODY overall gets skip=0 (end sentinel).
    for (size_t i = 0; i < newEntries.size(); ++i) {
        const auto& e = newEntries[i];
        bool isLast = (i == newEntries.size() - 1);
        std::vector<uint8_t> nameBytes(e.name.begin(), e.name.end());
        nameBytes.push_back(0);
        WriteResChunk(os, FOURCC_NAME, nameBytes.data(), (uint32_t)nameBytes.size(), false);
        WriteResChunk(os, FOURCC_BODY, e.data.data(), (uint32_t)e.data.size(), isLast);
        if (!os.good()) { error = "write failed appending new entry to " + outPath; return false; }
        if (onProgress) onProgress(++done, total);
    }

    if (!os.good()) { error = "write failed (stream error) for " + outPath; return false; }

    uint32_t finalSize = static_cast<uint32_t>(os.tellp());
    os.seekp(4, std::ios::beg);
    WriteU32LE(os, finalSize);
    os.flush();
    if (!os.good()) { error = "header patch/flush failed for " + outPath; return false; }
    return true;
}

bool RES_StreamMerge(const std::string& srcResPath,
                     const std::vector<RESEntry>& entries,
                     const std::string& outPath,
                     std::string& error,
                     const std::function<void(size_t,size_t)>& onProgress) {
    if (entries.empty()) {
        error = "RES_StreamMerge requires at least one entry";
        return false;
    }

    auto equalsResourceCI = [](const std::string& a, const std::string& b) {
        const auto leaf = [](const std::string& value) {
            const size_t slash = value.find_last_of("\\/");
            return slash == std::string::npos ? value : value.substr(slash + 1);
        };
        const std::string aLeaf = leaf(a);
        const std::string bLeaf = leaf(b);
        if (aLeaf.size() != bLeaf.size()) return false;
        for (size_t i = 0; i < aLeaf.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(aLeaf[i])) !=
                std::tolower(static_cast<unsigned char>(bLeaf[i]))) return false;
        }
        return true;
    };

    std::vector<bool> present(entries.size(), false);
    size_t sourceCount = 0;
    std::string ferr;
    if (!RES_ForEachEntry(srcResPath,
            [&](const std::string& name, const uint8_t*, size_t) {
                ++sourceCount;
                for (size_t i = 0; i < entries.size(); ++i) {
                    if (equalsResourceCI(name, entries[i].name)) present[i] = true;
                }
            }, ferr)) {
        error = "could not read source archive: " + ferr;
        return false;
    }

    size_t missingCount = 0;
    for (bool found : present) if (!found) ++missingCount;

    std::ofstream os(outPath, std::ios::binary);
    if (!os) {
        error = "Failed to create output file: " + outPath;
        return false;
    }
    WriteFourCC(os, FOURCC_ILFF);
    WriteU32LE(os, 0);
    WriteU32LE(os, 4);
    WriteU32LE(os, 0);
    WriteFourCC(os, FOURCC_IRES);

    const size_t total = sourceCount + missingCount;
    size_t sourceIndex = 0;
    size_t done = 0;
    bool streamFailed = false;
    if (!RES_ForEachEntry(srcResPath,
            [&](const std::string& name, const uint8_t* data, size_t size) {
                if (streamFailed) return;
                size_t replacement = entries.size();
                for (size_t i = 0; i < entries.size(); ++i) {
                    if (equalsResourceCI(name, entries[i].name)) {
                        replacement = i;
                        break;
                    }
                }
                const RESEntry* selected = replacement < entries.size() ? &entries[replacement] : nullptr;
                const bool lastSourceBody = missingCount == 0 && sourceIndex + 1 == sourceCount;
                const std::string& outputName = selected ? selected->name : name;
                const uint8_t* outputData = selected ? selected->data.data() : data;
                const size_t outputSize = selected ? selected->data.size() : size;
                std::vector<uint8_t> nameBytes(outputName.begin(), outputName.end());
                nameBytes.push_back(0);
                WriteResChunk(os, FOURCC_NAME, nameBytes.data(),
                              static_cast<uint32_t>(nameBytes.size()), false);
                WriteResChunk(os, FOURCC_BODY, outputData,
                              static_cast<uint32_t>(outputSize), lastSourceBody);
                if (!os.good()) { streamFailed = true; return; }
                ++sourceIndex;
                if (onProgress) onProgress(++done, total);
            }, ferr)) {
        error = "source stream failed: " + ferr;
        std::error_code ec;
        std::filesystem::remove(outPath, ec);
        return false;
    }
    if (streamFailed || !os.good()) {
        error = "write failed while merging source entries to " + outPath;
        std::error_code ec;
        std::filesystem::remove(outPath, ec);
        return false;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        if (present[i]) continue;
        const auto& entry = entries[i];
        std::vector<uint8_t> nameBytes(entry.name.begin(), entry.name.end());
        nameBytes.push_back(0);
        WriteResChunk(os, FOURCC_NAME, nameBytes.data(),
                      static_cast<uint32_t>(nameBytes.size()), false);
        WriteResChunk(os, FOURCC_BODY, entry.data.data(),
                      static_cast<uint32_t>(entry.data.size()), i + 1 == entries.size() ||
                      std::all_of(present.begin() + i + 1, present.end(), [](bool value) { return value; }));
        if (!os.good()) {
            error = "write failed appending merged entry to " + outPath;
            std::error_code ec;
            std::filesystem::remove(outPath, ec);
            return false;
        }
        if (onProgress) onProgress(++done, total);
    }

    if (!os.good()) {
        error = "write failed (stream error) for " + outPath;
        std::error_code ec;
        std::filesystem::remove(outPath, ec);
        return false;
    }
    const std::streampos end = os.tellp();
    if (end == std::streampos(-1) || static_cast<uint64_t>(end) > UINT32_MAX) {
        error = "output size cannot be represented in RES header: " + outPath;
        std::error_code ec;
        std::filesystem::remove(outPath, ec);
        return false;
    }
    os.seekp(4, std::ios::beg);
    WriteU32LE(os, static_cast<uint32_t>(end));
    os.flush();
    if (!os.good()) {
        error = "header patch/flush failed for " + outPath;
        std::error_code ec;
        std::filesystem::remove(outPath, ec);
        return false;
    }
    return true;
}
