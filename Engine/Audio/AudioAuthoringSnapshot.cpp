#include "Audio/AudioAuthoringSnapshot.h"
#include <algorithm>
namespace TerraEngine::Audio { AudioAuthoringSnapshot AudioAuthoringSnapshot::Create(AudioAuthoringSnapshotData data) { std::sort(data.listeners.begin(), data.listeners.end(), [](const auto& a, const auto& b) { return a.objectId == b.objectId ? a.componentGeneration < b.componentGeneration : a.objectId < b.objectId; }); return AudioAuthoringSnapshot(std::make_shared<const AudioAuthoringSnapshotData>(std::move(data))); } }
