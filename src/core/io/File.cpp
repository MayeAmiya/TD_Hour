#include "container/container_types.h"
#include "File.h"
#include <cstring>

namespace io {

container::String File::readAll()
{
    auto pos = tell();
    seek(0, FileSeek::End);
    auto fileSize = tell();
    seek(pos, FileSeek::Start);

    container::String result;
    if (fileSize > 0)
    {
        result.resize(static_cast<size_t>(fileSize));
        read(result.data(), result.size());
    }
    return result;
}

size_t File::writeString(container::StringView str)
{
    return write(str.data(), str.size());
}

} // namespace io
