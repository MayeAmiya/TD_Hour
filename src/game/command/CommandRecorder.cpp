#include "core/container/container_types.h"
#include "CommandRecorder.h"
#include "CommandStream.h"

#include <utility>

namespace engine {

void CommandRecorder::clear()
{
    m_commands.clear();
}

void CommandRecorder::record(const GameCommand& command)
{
    m_commands.push_back(command);
}

container::Vector<uint8_t> CommandRecorder::encodeStream() const
{
    return CommandStream::encode(m_commands);
}

bool CommandRecorder::loadStream(const container::Vector<uint8_t>& data, container::String* error)
{
    auto decoded = CommandStream::decode(data);
    if (!decoded.ok) {
        if (error) {
            *error = std::move(decoded.error);
        }
        return false;
    }
    m_commands = std::move(decoded.commands);
    return true;
}

} // namespace engine
