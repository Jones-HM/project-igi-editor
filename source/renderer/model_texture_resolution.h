#pragma once

#include <cstddef>
#include <string>
#include <vector>

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
