#pragma once

#include <memory>

namespace app {

class ApplicationHost final {
public:
    ApplicationHost();
    ~ApplicationHost();

    ApplicationHost(const ApplicationHost&) = delete;
    ApplicationHost& operator=(const ApplicationHost&) = delete;

    int run();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace app
