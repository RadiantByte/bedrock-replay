#pragma once

namespace bedrocktools::replay {

class ReplayVisuals {
public:
    static ReplayVisuals& get();
    bool install();
    void uninstall();

private:
    bool mInstalled{};
};

}
