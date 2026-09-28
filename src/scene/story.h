#pragma once

// The story view: a level as the game shows it at a point of the story (Jak II, Jak 3). The
// actors, decor prototypes and layers the game does not show at that point are hidden
// (Node::story_hidden); nothing is removed, the document keeps every state.
//
// The levels loaded with the one opened hang under a group telling what they are:
//   extras.og_layer      a layer: the actors (and decor) a mission adds, shown while the story
//                        lends it to the level
//   extras.og_companion  a level the game shows with it (the hut of Dead Town)

#include <vector>

#include "io/game_data.h"
#include "scene/scene.h"

namespace ogle {

struct StoryView {
  bool enabled = true;  // off: every state at once
  int step = 1;         // the story nodes before it are closed (GameData)
};

// Sets Node::story_hidden for that view; clears it when the view is off or the game has no story.
void apply_story(Scene& scene, const GameData* data, const StoryView& view);

// The steps at which what the document shows changes, in order.
std::vector<int> story_changes(const Scene& scene, const GameData& data);

}  // namespace ogle
