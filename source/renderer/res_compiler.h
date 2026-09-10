#pragma once
#include <string>

// Generates a QSC resource script by scanning the given input directory for MEF models.
// outResName is the relative path that will be written into the BeginResource("...") call.
bool RES_GenerateQSC(const std::string& inputDir, const std::string& outQscPath, const std::string& outResName, std::string& error);

// Compiles a QSC resource script into a binary .res (ILFF IRES) archive.
bool RES_Compile(const std::string& scriptPath, std::string& error);

// Repacks a set of in-memory resources into a binary .res (ILFF IRES) archive.
// Used to patch an existing archive (parse → modify one entry → write all back)
// without needing every loose file on disk. Preserves entry order and names.
#include "res_writer.h"
#include <functional>
bool RES_WriteEntries(const std::vector<RESEntry>& entries, const std::string& outPath, std::string& error);

// Stream `srcResPath` to `outPath`, appending `newEntries` at the end, WITHOUT loading the
// whole archive into memory (safe for 200MB+ archives in the 32-bit editor). Writes a valid
// ILFF/IRES archive (the previously-final BODY gets a normal skip; the very last appended
// BODY gets skip=0). Returns false (with error) on any IO/stream failure. Does NOT modify
// srcResPath. onProgress(done,total) may be null; total = source entry count + newEntries.
bool RES_StreamAppend(const std::string& srcResPath,
                      const std::vector<RESEntry>& newEntries,
                      const std::string& outPath,
                      std::string& error,
                      const std::function<void(size_t,size_t)>& onProgress = nullptr);

// Stream `srcResPath` to `outPath`, replacing entries whose names match the
// supplied entries case-insensitively and appending supplied entries absent
// from the source. The source is never modified.
bool RES_StreamMerge(const std::string& srcResPath,
                     const std::vector<RESEntry>& entries,
                     const std::string& outPath,
                     std::string& error,
                     const std::function<void(size_t,size_t)>& onProgress = nullptr);
