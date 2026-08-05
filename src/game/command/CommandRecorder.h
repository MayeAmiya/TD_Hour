#pragma once

#include "core/container/container_types.h"

#include "GameCommand.h"
namespace engine {

class CommandRecorder {
public:
    void clear();
    void record(const GameCommand& command);
    const container::Vector<GameCommand>& commands() const { return m_commands; }
    container::Vector<uint8_t> encodeStream() const;
    bool loadStream(const container::Vector<uint8_t>& data, container::String* error = nullptr);

private:
    container::Vector<GameCommand> m_commands;
};

} // namespace engine
