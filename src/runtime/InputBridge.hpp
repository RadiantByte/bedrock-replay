#pragma once

#include <functional>
#include <optional>
#include <string>

namespace pl::input {

struct TouchEvent {
    int action{};
    int pointerId{};
    float x{};
    float y{};
};

struct KeyEvent {
    int keyCode{};
    unsigned int unicodeChar{};
    bool isKeyDown{};
};

struct TextInputEvent {
    std::string text;
};

struct MouseEvent {
    int button{};
    bool isDown{};
};

using TouchCallback = std::function<bool(const TouchEvent&)>;
using KeyCallback = std::function<bool(const KeyEvent&)>;
using TextInputCallback = std::function<bool(const TextInputEvent&)>;
using MouseCallback = std::function<bool(const MouseEvent&)>;

}

namespace bedrockreplay::input {

bool registerTouchCallback(pl::input::TouchCallback callback);
bool registerKeyCallback(pl::input::KeyCallback callback);
bool registerTextInputCallback(pl::input::TextInputCallback callback);
bool registerMouseCallback(pl::input::MouseCallback callback);
bool showKeyboard();
bool hideKeyboard();
std::optional<bool> hudScreenOpen();
std::optional<bool> showingMenu();
std::optional<bool> pauseMenuOpen();

}
