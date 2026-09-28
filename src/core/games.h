#pragma once

// The three games the editor reads levels of.

#include <string>

namespace ogle {

enum class Game { Jak1 = 1, Jak2 = 2, Jak3 = 3 };

const char* game_name(Game g);  // "jak1", "jak2", "jak3" (folder names of the library)
Game game_from_name(const std::string& name, Game fallback = Game::Jak2);

}  // namespace ogle
