#pragma once

namespace bedrocktools::replay {

class ReplayRuntime {
public:
    static ReplayRuntime& get();
    bool install();
    void uninstall();
    void setEnabled(bool enabled);
    bool enabled() const noexcept;

private:
    bool mInstalled{};
};

}
