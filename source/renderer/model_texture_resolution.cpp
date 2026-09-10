#include "model_texture_resolution.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

std::vector<ModelPickerEntry> FilterModelPickerEntries(
    const std::vector<ModelPickerEntry>& entries,
    const std::string& filter) {
    std::string lowerFilter = filter;
    std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::vector<ModelPickerEntry> filtered;
    for (const auto& entry : entries) {
        if (lowerFilter.empty()) {
            filtered.push_back(entry);
            continue;
        }
        std::string lowerLabel = entry.label;
        std::transform(lowerLabel.begin(), lowerLabel.end(), lowerLabel.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerLabel.find(lowerFilter) != std::string::npos) {
            filtered.push_back(entry);
        }
    }
    return filtered;
}

bool ModelSourceRequiresImport(bool destinationInventoryLoaded,
                               bool destinationContainsModel,
                               int destinationLevel,
                               int selectedSourceLevel) {
    const bool explicitForeignSource = selectedSourceLevel > 0 &&
        selectedSourceLevel != destinationLevel;
    if (!destinationInventoryLoaded) return explicitForeignSource;
    if (!destinationContainsModel) return true;
    return explicitForeignSource;
}

int ModelPickerSelectionAt(
    const std::vector<ModelPickerEntry>& entries,
    const std::string& filter,
    int mouseX,
    int mouseY,
    int viewportWidth,
    int viewportHeight,
    int scrollOffset,
    int rowHeight) {
    constexpr int kPanelWidth = 280;
    constexpr int kHeaderHeight = 50;
    constexpr int kFooterHeight = 20;
    if (rowHeight <= 0 || mouseX < viewportWidth - kPanelWidth ||
        mouseX > viewportWidth || mouseY < kHeaderHeight ||
        mouseY >= viewportHeight - kFooterHeight || scrollOffset < 0) {
        return -1;
    }

    const auto filtered = FilterModelPickerEntries(entries, filter);
    const int row = (mouseY - kHeaderHeight) / rowHeight;
    const int selected = scrollOffset + row;
    return selected >= 0 && selected < static_cast<int>(filtered.size()) ? selected : -1;
}

std::vector<ModelPickerEntry> DedupeModelPickerEntries(
    const std::vector<ModelPickerEntry>& entries,
    int currentLevel) {
    std::map<std::string, std::set<int>> levelsByModel;
    std::vector<std::string> order;
    for (const auto& entry : entries) {
        auto levelsIt = levelsByModel.find(entry.modelId);
        if (levelsIt == levelsByModel.end()) {
            levelsByModel.emplace(entry.modelId, std::set<int>{});
            order.push_back(entry.modelId);
            levelsIt = levelsByModel.find(entry.modelId);
        }
        if (entry.sourceLevel > 0) levelsIt->second.insert(entry.sourceLevel);
    }

    std::vector<ModelPickerEntry> unique;
    unique.reserve(order.size());
    for (const auto& modelId : order) {
        const auto& levels = levelsByModel[modelId];
        int sourceLevel = 0;
        if (!levels.empty()) {
            sourceLevel = levels.count(currentLevel) > 0
                ? currentLevel
                : *levels.begin();
        }
        unique.push_back({modelId, sourceLevel, modelId});
    }
    return unique;
}

std::optional<ModelPickerCommit> ResolveModelPickerCommit(
    const std::vector<ModelPickerEntry>& entries,
    const std::string& filter,
    int selected) {
    const auto filtered = FilterModelPickerEntries(entries, filter);
    if (selected < 0 || selected >= static_cast<int>(filtered.size())) {
        return std::nullopt;
    }
    return ModelPickerCommit{filtered[selected].modelId,
                             filtered[selected].sourceLevel};
}

int MoveModelPickerSelection(int selected, int count, int delta) {
    if (count <= 0) return 0;
    return std::max(0, std::min(count - 1, selected + delta));
}

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

std::vector<uint8_t> FindModelSourceEntry(
    const std::array<ModelSourceArchiveIndex, 2>& bundle,
    const std::string& entryId,
    bool isTexture,
    const std::function<std::vector<uint8_t>(const std::string& resPath,
                                              const ResEntryInfo& info)>& readEntry) {
    auto equalsCI = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    };
    auto lookup = [&](const ModelSourceArchiveIndex& archive,
                      const std::string& id) -> std::vector<uint8_t> {
        const std::string entryName =
            id + (isTexture ? ".tex" : ".mef");
        for (const auto& item : archive.entries) {
            if (equalsCI(item.first, entryName))
                return readEntry(archive.resPath, item.second);
        }
        return {};
    };
    for (const auto& archive : bundle) {
        auto bytes = lookup(archive, entryId);
        if (!bytes.empty()) return bytes;
        // Format-suffixed texture ids fall back to the stripped name in the
        // same archive before the search continues to the common archive.
        if (isTexture) {
            const std::string stripped = StripTextureFormatSuffix(entryId);
            if (stripped != entryId) {
                bytes = lookup(archive, stripped);
                if (!bytes.empty()) return bytes;
            }
        }
    }
    return {};
}
