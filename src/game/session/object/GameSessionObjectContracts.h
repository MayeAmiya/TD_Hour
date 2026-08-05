#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
#include <optional>

namespace engine {

struct GameSessionObjectSpawnResult final {
    ObjectId object = INVALID_OBJECT_ID;
    std::optional<ecs::entity> entity;
    bool scriptNameRequested = false;
    bool scriptNameBound = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return object && entity.has_value();
    }
};

struct GameSessionProductionCommandResult final {
    bool accepted = false;
    uint32_t productionId = 0;
    // Set only for ObjectProductionRejectionReason::InsufficientFunds. The
    // stringified message above cannot be matched safely, and the confirmed
    // command port needs this one reason to reproduce RefCode's
    // EVA_InsufficientFunds announcement without importing the production
    // simulation vocabulary into this contract header.
    bool insufficientFunds = false;
    container::String message;
};

struct GameSessionCaptionCommandResult final {
    bool accepted = false;
    size_t actorCount = 0;
    size_t changedCount = 0;
    container::String message;
};

} // namespace engine
