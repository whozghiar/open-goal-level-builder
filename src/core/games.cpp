#include "core/games.h"

namespace ogle {

const char* game_name(Game g) {
  switch (g) {
    case Game::Jak1:
      return "jak1";
    case Game::Jak2:
      return "jak2";
    default:
      return "jak3";
  }
}

Game game_from_name(const std::string& name, Game fallback) {
  if (name == "jak1") return Game::Jak1;
  if (name == "jak2") return Game::Jak2;
  if (name == "jak3") return Game::Jak3;
  return fallback;
}

}  // namespace ogle
