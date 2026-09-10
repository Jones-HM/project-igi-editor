#include "model_texture_resolution.h"

#include <algorithm>
#include <cctype>

bool IsCompleteModelImportMetadata(const ModelImportMetadataStatus& status) {
    return status.datPublished && status.mtpPublished;
}

const ModelTextureSource* FindExactTextureSource(
    const std::vector<ModelTextureSource>& sources,
    int sourceLevel,
    const std::string& modelId) {
    for (const auto& source : sources) {
        if (source.level == sourceLevel && source.modelId == modelId) {
            return &source;
        }
    }
    return nullptr;
}

std::string StripTextureFormatSuffix(const std::string& textureId) {
    static const char* const kSuffixes[] = {
        "_argb8888", "_rgb565", "_argb1555", "_argb4444",
        "_a8r8g8b8", "_r5g6b5", "_a1r5g5b5", "_a4r4g4b4"
    };

    for (const char* suffix : kSuffixes) {
        const size_t suffixLength = std::strlen(suffix);
        if (textureId.size() <= suffixLength) continue;

        bool matches = true;
        const size_t start = textureId.size() - suffixLength;
        for (size_t i = 0; i < suffixLength; ++i) {
            const unsigned char actual = static_cast<unsigned char>(textureId[start + i]);
            const unsigned char expected = static_cast<unsigned char>(suffix[i]);
            if (std::tolower(actual) != std::tolower(expected)) {
                matches = false;
                break;
            }
        }
        if (matches) return textureId.substr(0, start);
    }
    return textureId;
}

bool IsTextureMappingCompatible(const std::vector<int>& materialSlots,
                                std::size_t orderedMappingSize) {
    if (orderedMappingSize == 0 || materialSlots.empty()) return false;
    return std::all_of(materialSlots.begin(), materialSlots.end(),
        [orderedMappingSize](int slot) {
            return slot >= 0 && static_cast<std::size_t>(slot) < orderedMappingSize;
        });
}

int SelectUnambiguousModelSourceLevel(
    const std::vector<ModelSourceBundle>& candidates) {
    if (candidates.empty()) return 0;
    const auto& first = candidates.front();
    for (const auto& candidate : candidates) {
        if (candidate.textureIds != first.textureIds ||
            candidate.meshBytes != first.meshBytes ||
            candidate.textureBytes != first.textureBytes) {
            return 0;
        }
    }
    return first.level;
}
