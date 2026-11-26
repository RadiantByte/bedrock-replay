#pragma once

#include <array>

namespace bedrocktools::replay {

inline std::array<float, 16> cameraClipMatrix(const float* projection, const float* view) {
    std::array<float, 16> result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            for (int index = 0; index < 4; ++index) {
                result[column * 4 + row] += projection[index * 4 + row] * view[column * 4 + index];
            }
        }
    }
    return result;
}

}
