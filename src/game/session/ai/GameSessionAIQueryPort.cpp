#include "game/session/ai/GameSessionAIQueryPort.h"

#include "game/session/ai/GameSessionAIDomain.h"

namespace engine {

ObjectAISimulationDigest
GameSessionAIQueryPort::objectSimulationDigest() const {
    return m_domain->objectAISimulationDigest();
}

} // namespace engine
