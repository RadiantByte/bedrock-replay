#include <bedrocktools/memory/Signatures.hpp>

#include "runtime/Scanner.hpp"

namespace bedrocktools::memory {

bool resolveAll(std::string_view) {
    return bedrockreplay::scanner::resolveAll();
}

std::uintptr_t resolve(SignatureId id) {
    return bedrockreplay::scanner::resolve(static_cast<std::size_t>(id));
}

void clear() {
    bedrockreplay::scanner::clearResolved();
}

}
