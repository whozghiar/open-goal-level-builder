#include "io/catalog.h"

#include <algorithm>
#include <map>
#include <set>

#include "io/library_index.h"

namespace ogle {

namespace {

// the order of the families in the catalog
const char* kFamilies[] = {"crate", "collectable", "platform", "door",  "button", "movement", "hazard", "breakable",
                           "light", "weapon",      "vehicle",  "water", "other",  "effect",   "logic"};

int family_rank(const std::string& f) {
  for (int i = 0; i < (int)(sizeof(kFamilies) / sizeof(kFamilies[0])); i++)
    if (f == kFamilies[i]) return i;
  return (int)(sizeof(kFamilies) / sizeof(kFamilies[0]));
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// the pieces an object leaves when it breaks: not a model of its own
bool debris(const std::string& model) {
  for (const char* w : {"-explode", "-debris", "-chunk"})
    if (model.find(w) != std::string::npos) return true;
  const auto b = model.rfind("-break");
  return b != std::string::npos && (b + 6 == model.size() || model[b + 6] == '-');
}

}  // namespace

const ModelAsset* resolve_actor_model(const AssetLibrary& library, const GameData* data, const std::string& etype,
                                      const json& lump, const std::string& group) {
  auto named = [&](const std::string& n) { return n.empty() ? nullptr : library.model_named(n, group); };
  if (lump.is_object()) {
    if (data && data->is_water(etype)) {
      auto look = lump.find("look");
      if (look != lump.end() && look->is_number())
        if (const ModelAsset* m = named(data->water_model((int)look->get<double>()))) return m;
    }
    // "skel-" + its art-name
    auto art = lump.find("art-name");
    if (art != lump.end() && art->is_string()) {
      const std::string name = art->get<std::string>();
      if (data)
        if (const ModelAsset* m = named(data->skeleton_model(name))) return m;
      if (const ModelAsset* m = named(name)) return m;
    }
  }
  if (data)
    if (const auto* models = data->type_models(etype))
      for (const auto& n : *models)
        if (const ModelAsset* m = named(n)) return m;
  return library.model_for_etype(etype, group);
}

json portable_lump(const json& lump) {
  json out = json::object();
  if (!lump.is_object()) return out;
  for (const auto& [key, value] : lump.items()) {
    if (key == "name" || key == "kill-mask" || key == "visvol" || key == "task" ||
        key.find("actor") != std::string::npos || key.find("path") != std::string::npos ||
        key.find("group") != std::string::npos)
      continue;
    out[key] = value;
  }
  return out;
}

Node make_catalog_actor(const CatalogObject& object, const std::string& name) {
  Node n;
  n.kind = NodeKind::Actor;
  n.name = name;
  json lump = object.lump.is_object() ? object.lump : json::object();
  lump["name"] = name;
  n.extras = json{{"etype", object.etype}, {"game_task", 0}, {"lump", lump}};
  return n;
}

std::vector<CatalogObject> build_catalog(const AssetLibrary& library, const GameData* data, const LibraryIndex* index) {
  // the actors of the extracted levels, by type
  struct Art {
    int actors = 0;     // known from the index
    std::string level;  // the first level with one
    json lump;          // the data of the first one (from the index)
  };
  struct Seen {
    int count = 0;
    json lump;
    std::vector<std::string> levels;
    std::map<std::string, Art> arts;  // the art-names of its actors: the models, the particle groups they choose
    bool plain = false;               // some of its actors choose none
  };
  std::map<std::string, Seen> seen;
  for (const auto& level : library.levels()) {
    if (!level.actor_types.is_object()) continue;
    for (const auto& [etype, info] : level.actor_types.items()) {
      Seen& s = seen[etype];
      s.count += info.value("count", 0);
      if (s.lump.is_null() && info.contains("lump")) s.lump = info["lump"];
      if (s.levels.size() < 8) s.levels.push_back(level.name);
      auto note = [&](const json& art) {
        if (!art.is_string()) return;
        Art& a = s.arts[art.get<std::string>()];
        if (a.level.empty()) a.level = level.name;
      };
      auto lump = info.find("lump");
      if (lump != info.end() && lump->is_object() && lump->contains("art-name")) note((*lump)["art-name"]);
      else s.plain = true;
      auto names = info.find("art_names");
      if (names != info.end() && names->is_array())
        for (const auto& a : *names) note(a);
    }
  }
  // every actor of every level (the files of the levels hold them all, level.json the first of
  // each type): how many actors have each art-name, and whether some have none
  const bool indexed = index && index->ready();
  if (indexed) {
    std::map<std::string, int> with_art;
    for (const auto& li : index->levels())
      for (const auto& [etype, names] : li.art_names) {
        Seen& s = seen[etype];
        for (const auto& [name, n] : names) {
          Art& a = s.arts[name];
          if (a.level.empty()) a.level = li.level;
          if (a.lump.is_null()) {
            auto lumps = li.art_lumps.find(etype);
            if (lumps != li.art_lumps.end() && lumps->second.count(name)) a.lump = lumps->second.at(name);
          }
          a.actors += n;
          with_art[etype] += n;
        }
      }
    for (auto& [etype, s] : seen) s.plain = with_art[etype] < s.count;
  }
  std::set<std::string> types;
  if (data)
    for (const auto& [type, family] : data->objects()) types.insert(type);
  for (const auto& [type, s] : seen)
    if (!data || data->role(type) == ActorRole::Object) types.insert(type);

  std::vector<CatalogObject> out;
  auto add_seen = [&](CatalogObject& o) {
    auto it = seen.find(o.etype);
    if (it == seen.end()) return;
    o.count = it->second.count;
    o.levels = it->second.levels;
    if (o.lump.is_null() || o.lump.empty()) o.lump = portable_lump(it->second.lump);
  };
  // what an art-name of a type gives: its actors, its level, its data
  auto add_art = [&](CatalogObject& o, const Seen& s, const std::string& name) {
    auto a = s.arts.find(name);
    if (a != s.arts.end()) {
      if (indexed) o.count = a->second.actors;
      if (!a->second.level.empty()) o.levels = {a->second.level};
      if (a->second.lump.is_object()) o.lump = portable_lump(a->second.lump);
    }
    o.lump["art-name"] = name;
  };
  for (const auto& type : types) {
    if (data && data->is_water(type)) continue;  // listed by look below
    auto s = seen.find(type);
    json lump = s != seen.end() ? s->second.lump : json::object();
    const bool known = data && (data->objects().count(type) || data->type_models(type));
    // the types that choose their model by name (art-name): one entry per model, and the one of
    // the actors that choose none
    const auto* models = data ? data->type_models(type) : nullptr;
    const bool by_name = s != seen.end() && !s->second.arts.empty();
    bool keep_art = false;
    if (by_name) {
      std::vector<std::pair<const ModelAsset*, std::string>> variants;  // model, art-name
      auto add_variant = [&](const std::string& model, const std::string& art) {
        const ModelAsset* m = model.empty() || debris(model) ? nullptr : library.model_named(model, "");
        if (!m) return;
        for (const auto& v : variants)
          if (v.first->name == m->name) return;
        variants.push_back({m, art});
      };
      for (const auto& [art, a] : s->second.arts) {
        const std::string skeleton = data ? data->skeleton_model(art) : std::string();
        add_variant(skeleton.empty() ? art : skeleton, art);
      }
      if (models && !variants.empty())
        for (const auto& name : *models) {
          const std::string skeleton = data->skeleton_of_model(name);
          add_variant(name, skeleton.empty() ? name : skeleton);
        }
      for (const auto& [m, art] : variants) {
        CatalogObject o;
        o.etype = type;
        o.name = m->name;
        o.family = data ? data->object_family(type) : "other";
        o.model = m->name;
        o.model_path = m->path;
        add_seen(o);
        add_art(o, s->second, art);
        out.push_back(std::move(o));
      }
      if (!variants.empty() && !s->second.plain) continue;
      // no art-name names a model (it names something else: a particle group, the actor): one
      // entry that keeps it; else the entry of the actors without one
      if (variants.empty()) keep_art = true;
      else lump.erase("art-name");
    }
    const ModelAsset* m = resolve_actor_model(library, data, type, lump, "");
    // an object the game draws without a model (trick points, fire, a manager of the level's
    // objects...): one the levels have
    if (!m && (!known || s == seen.end())) continue;
    CatalogObject o;
    o.etype = o.name = type;
    o.family = data ? data->object_family(type) : "other";
    if (m) {
      o.model = m->name;
      o.model_path = m->path;
    }
    o.lump = json::object();
    add_seen(o);
    if (!keep_art) o.lump.erase("art-name");
    out.push_back(std::move(o));
  }
  if (data) {
    // water, dark eco and lava: one per look, each with its model and the type of the actors
    // that have it (the water of a level adds its ripples and color)
    const std::string dark_eco = data->is_water("dark-eco-pool") ? "dark-eco-pool" : "water-anim";
    const std::vector<std::string> water_types = data->water_types();
    auto water_type = [&](const std::string& model) {
      if (model.find("dark-eco") != std::string::npos) return dark_eco;
      std::string best = "water-anim";
      for (const auto& t : water_types)
        if (t.size() > best.size() && model.size() > t.size() && model.compare(0, t.size(), t) == 0 &&
            model[t.size()] == '-')
          best = t;
      return best;
    };
    for (int look = 0; look < data->water_look_count(); look++) {
      const std::string& model = data->water_model(look);
      const ModelAsset* m = model.empty() ? nullptr : library.model_named(model, "");
      if (!m) continue;
      CatalogObject o;
      o.etype = water_type(model);
      o.name = model;
      o.family = "water";
      o.model = m->name;
      o.model_path = m->path;
      o.lump = json{{"look", look}};
      add_seen(o);
      out.push_back(std::move(o));
    }
    // the particle effects (lights, neon signs, steam, sparks...): one per particle group the
    // actors of the levels name, with the type of the actors that use it most
    struct Group {
      std::string etype;
      int actors = -1;
    };
    std::map<std::string, Group> groups;
    for (const auto& [type, s] : seen) {
      if (!data->is_effect(type) || data->is_unlisted(type)) continue;
      for (const auto& [name, a] : s.arts) {
        Group& g = groups[name];
        const int n = indexed ? a.actors : s.count;
        if (n > g.actors) g = {type, n};
      }
    }
    for (const auto& [name, g] : groups) {
      CatalogObject o;
      o.etype = g.etype;
      o.name = name.rfind("group-", 0) == 0 ? name.substr(6) : name;
      o.family = "effect";
      add_seen(o);
      add_art(o, seen[g.etype], name);
      out.push_back(std::move(o));
    }
    // the other logic of the levels (a neon sign, the boats of a port, a parking spot): a marker
    for (const auto& [type, s] : seen) {
      if (data->role(type) != ActorRole::Logic || data->is_effect(type) || data->is_unlisted(type) ||
          data->is_water(type) || !s.count)
        continue;
      CatalogObject o;
      o.etype = o.name = type;
      o.family = "logic";
      add_seen(o);
      out.push_back(std::move(o));
    }
  }
  std::sort(out.begin(), out.end(), [](const CatalogObject& a, const CatalogObject& b) {
    const int fa = family_rank(a.family), fb = family_rank(b.family);
    return fa != fb ? fa < fb : a.name < b.name;
  });
  return out;
}

std::vector<CatalogModel> list_catalog_models(const AssetLibrary& library, const GameData* data,
                                              const std::vector<CatalogObject>& objects) {
  std::set<std::string> taken;
  for (const auto& o : objects)
    if (!o.model.empty()) taken.insert(o.model);
  // the characters: the models of the creatures, the ones named after a creature (its parts: a
  // boss's pieces) or after an NPC's name, the ones of the cutscenes
  std::set<std::string> characters, creature_names;
  if (data) {
    for (const auto& [type, models] : data->all_type_models())
      if (data->role(type) == ActorRole::Creature) characters.insert(models.begin(), models.end());
    for (const auto& [type, role] : data->roles()) {
      if (role != ActorRole::Creature) continue;
      creature_names.insert(type);
      for (const char* suffix : {"-npc", "-highres"})
        if (ends_with(type, suffix)) creature_names.insert(type.substr(0, type.size() - std::string(suffix).size()));
    }
  }
  auto named_after_creature = [&](const std::string& model) {
    for (size_t dash = model.size(); dash != std::string::npos; dash = dash ? model.rfind('-', dash - 1) : std::string::npos) {
      if (creature_names.count(model.substr(0, dash))) return true;
      if (!dash) break;
    }
    return false;
  };
  std::vector<CatalogModel> out;
  for (const auto& m : library.models()) {
    if (!out.empty() && out.back().name == m.name) continue;  // its other lods and copies
    if (taken.count(m.name) || characters.count(m.name) || named_after_creature(m.name) ||
        m.name.find("-highres") != std::string::npos || ends_with(m.name, "-lowres") || ends_with(m.name, "-fma") ||
        ends_with(m.name, "-shadow") || m.name.find("-collision") != std::string::npos || m.name == "scenecamera")
      continue;
    out.push_back({m.name, m.path, m.group});  // the first copy is the lowest lod
  }
  return out;
}

std::vector<CatalogPart> list_parts(const Scene& level) {
  std::vector<CatalogPart> prototypes, pieces;
  std::map<uint32_t, std::vector<const Node*>> children;
  for (const auto& n : level.nodes)
    if (n.parent) children[n.parent].push_back(&n);
  for (const auto& n : level.nodes) {
    const Node* p = n.parent ? level.find(n.parent) : nullptr;
    if (!p) continue;
    if (p->name.find("-tie-") != std::string::npos || p->name.find("-shrub-") != std::string::npos) {
      // a prototype: its instances have the mesh (or the node itself)
      CatalogPart part;
      part.kind = CatalogPart::Kind::Prototype;
      part.name = n.name;
      if (n.mesh) {
        part.mesh_node = n.name;
        part.triangles = n.mesh->tri_count();
        part.copies = 1;
      } else {
        for (const Node* c : children[n.id]) {
          if (!c->mesh) continue;
          if (part.mesh_node.empty()) {
            part.mesh_node = c->name;
            part.triangles = c->mesh->tri_count();
          }
          part.copies++;
        }
      }
      if (!part.mesh_node.empty()) prototypes.push_back(std::move(part));
    } else if (p->name.find("-tfrag-") != std::string::npos && n.mesh) {
      // a piece of the shell; the terrain and the older extractions (a mesh per texture) are not
      auto terrain = n.extras.find("og_terrain");
      if (terrain == n.extras.end() || !terrain->is_boolean() || terrain->get<bool>()) continue;
      CatalogPart part;
      part.kind = CatalogPart::Kind::Piece;
      part.name = part.mesh_node = n.name;
      part.triangles = n.mesh->tri_count();
      part.copies = 1;
      pieces.push_back(std::move(part));
    }
  }
  auto by_name = [](const CatalogPart& a, const CatalogPart& b) { return a.name < b.name; };
  std::sort(prototypes.begin(), prototypes.end(), by_name);
  std::sort(pieces.begin(), pieces.end(), by_name);
  prototypes.insert(prototypes.end(), pieces.begin(), pieces.end());
  return prototypes;
}

std::shared_ptr<Mesh> part_mesh(const Scene& level, const CatalogPart& part) {
  for (const auto& n : level.nodes) {
    if (n.name != part.mesh_node || !n.mesh) continue;
    auto mesh = std::make_shared<Mesh>(*n.mesh);
    const AABB b = mesh->bounds();
    if (b.valid()) {
      const Vec3 origin{b.center().x, b.lo.y, b.center().z};
      for (auto& prim : mesh->prims)
        for (auto& p : prim.pos) p = p - origin;
      mesh->touch();
    }
    return mesh;
  }
  return nullptr;
}

}  // namespace ogle
