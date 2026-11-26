#pragma once

namespace bedrocktools::replay {

class ReplayEditor {
public:
    static ReplayEditor& get();
    bool install();
    void closeSession();

private:
    ReplayEditor() = default;
};

}
