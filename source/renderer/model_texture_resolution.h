#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "res_writer.h"

struct ModelTextureSource {
    int level;
    std::string modelId;
    std::vector<std::string> textures;
};

struct ModelPickerEntry {
    std::string modelId;
    int sourceLevel;
    std::string label;
};

// Build the exact model list used by picker drawing and input handling.
std::vector<ModelPickerEntry> FilterModelPickerEntries(
    const std::vector<ModelPickerEntry>& entries,
    const std::string& filter);

// Collapse per-source picker rows to one row per unique model ID. The kept
// row carries the current level when it owns the model, otherwise the
// smallest owning level, so Enter imports a deterministic source bundle.
std::vector<ModelPickerEntry> DedupeModelPickerEntries(
    const std::vector<ModelPickerEntry>& entries,
    int currentLevel);

// The model ID and source level committed by Enter on the filtered picker
// list. Empty when the selection is outside the filtered list.
struct ModelPickerCommit {
    std::string modelId;
    int sourceLevel;
};
std::optional<ModelPickerCommit> ResolveModelPickerCommit(
    const std::vector<ModelPickerEntry>& entries,
    const std::string& filter,
    int selected);

// Import a missing model when the destination inventory is known, and always
// honor an explicit foreign source even when it has the same model ID.
bool ModelSourceRequiresImport(bool destinationInventoryLoaded,
                               bool destinationContainsModel,
                               int destinationLevel,
                               int selectedSourceLevel);

// Return the filtered-list index for a model-picker row click, or -1 when the
// click is outside the item area. Coordinates are top-down screen pixels.
int ModelPickerSelectionAt(
    const std::vector<ModelPickerEntry>& entries,
    const std::string& filter,
    int mouseX,
    int mouseY,
    int viewportWidth,
    int viewportHeight,
    int scrollOffset,
    int rowHeight);

// Move a picker selection within the filtered-list bounds. The list holds one
// row per unique model ID, so every step changes the previewed model.
int MoveModelPickerSelection(int selected, int count, int delta);

struct ModelImportMetadataStatus {
    bool datPublished;
    bool mtpPublished;
};

struct ModelSourceBundle {
    int level;
    std::vector<std::string> textureIds;
    std::vector<uint8_t> meshBytes;
    std::vector<std::vector<uint8_t>> textureBytes;
};

bool IsCompleteModelImportMetadata(const ModelImportMetadataStatus& status);

// Find the ordered material mapping for one model variant in one source level.
// A missing exact source is intentionally not replaced by a prefix match here.
const ModelTextureSource* FindExactTextureSource(
    const std::vector<ModelTextureSource>& sources,
    int sourceLevel,
    const std::string& modelId);

// Remove only known pixel-format tags. Numeric suffixes are part of texture
// identity and must remain untouched.
std::string StripTextureFormatSuffix(const std::string& textureId);

// Every material slot emitted by a MEF must address the selected ordered
// mapping. Slot numbers are indices, not texture IDs, so accepting an
// out-of-range slot would make the importer silently bind the wrong material.
bool IsTextureMappingCompatible(const std::vector<int>& materialSlots,
                                std::size_t orderedMappingSize);

// Untagged imports are safe only when every exact source is byte-equivalent.
// A zero result means the caller must request explicit source provenance.
int SelectUnambiguousModelSourceLevel(
    const std::vector<ModelSourceBundle>& candidates);

// One indexed entry inside a source-bundle archive: the archive file plus the
// name->offset/size table built once with RES_BuildIndex. Level archives are
// searched before the shared common archive, so same-name bytes from an
// unrelated archive can never override the selected source bundle.
struct ModelSourceArchiveIndex {
    std::string resPath;
    std::unordered_map<std::string, ResEntryInfo> entries;
};

// Resolve one entry from a source bundle: the level archive first, then the
// shared common archive. Textures are stored as "<id>.tex", meshes as
// "<id>.mef"; callers pass the bare id with its suffix kind. Format-suffixed
// texture ids fall back to the stripped name in the same archive before the
// search continues to the next archive.
std::vector<uint8_t> FindModelSourceEntry(
    const std::array<ModelSourceArchiveIndex, 2>& bundle,
    const std::string& entryId,
    bool isTexture,
    const std::function<std::vector<uint8_t>(const std::string& resPath,
                                              const ResEntryInfo& info)>& readEntry);
