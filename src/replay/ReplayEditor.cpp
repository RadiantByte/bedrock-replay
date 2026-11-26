#include "ReplayEditor.hpp"

namespace bedrocktools::replay {

ReplayEditor& ReplayEditor::get() {
    static ReplayEditor editor;
    return editor;
}

bool ReplayEditor::install() {
    return true;
}

void ReplayEditor::closeSession() {
}

}
