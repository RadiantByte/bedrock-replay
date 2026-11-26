#pragma once

#include <cstddef>
#include <cstdint>

namespace bedrockreplay::scanner {

bool resolveAll();
std::uintptr_t resolve(std::size_t id);
void clearResolved();

}
