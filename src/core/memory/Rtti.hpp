#pragma once

#include <cstddef>
#include <string_view>

namespace bedrocktools::core::rtti {

void** vtable(std::string_view typeName);
void* virtualFunction(std::string_view typeName, std::size_t slot);
bool isInstance(const void* object, std::string_view typeName);

}
