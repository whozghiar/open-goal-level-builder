#!/usr/bin/env python3
"""Generates data/<game>/game-data.json: what the editor knows about a game beyond the extracted
files. It reads the decompiled game code of a jak-project checkout (goal_src/<game>, read only)
and the extractor's type table (extractor/data/<game>/all-types.gc).

  actors.roles        actor types that are creatures or logic (the others are objects), from
                      their base types
  actors.effects      the particle effects: part-spawner and its subtypes (an actor's art-name
                      names its particle group)
  actors.unlisted     the logic the catalog does not offer: it does nothing, brings creatures,
                      runs a cutscene or a demo
  actors.water_looks  the model of each "look" of the water-anim actors (water, dark eco, lava)
  oceans              the ocean maps: corner, height and the cells the ocean covers
  levels              per level: task level, base task mask, ocean, layer or companion levels
  story               the task nodes in game order (task masks, borrowed layers) and what the
                      story shows or hides: decor prototypes of a level, actor types

Usage: python tools/gen_game_data.py <jak-project>/goal_src [jak1 jak2 jak3]
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# Actor roles. An actor type is a creature when it is or derives from a type of `creatures`
# (unless it is or derives from one of `objects`), logic when it is or derives from a type of
# `logic` or is a process that draws nothing (not a process-drawable). Types ending with one of
# `creature_suffixes` are creatures even when the type table does not know them (the cutscene
# characters "jak-highres"...).
GAMES = {
    'jak1': {
        'creatures': ['nav-enemy', 'process-taskable', 'pelican', 'seagull', 'lurkerworm', 'village-fish',
                      'robotboss', 'plant-boss', 'ogreboss', 'ogreboss-village2', 'flying-lurker', 'bully',
                      'puffer', 'yeti', 'yeti-slave', 'mother-spider', 'spider-egg', 'swamp-rat-nest',
                      'swamp-rat-nest-dummy', 'lightning-mole', 'junglefish', 'sidekick', 'target'],
        'objects': [],
        'logic': ['part-spawner', 'water-vol', 'battlecontroller', 'pov-camera', 'camera-tracker', 'manipy',
                  'touch-tracker', 'voicebox', 'viewer', 'anim-tester', 'process-drawable-reserved'],
        # logic not offered by the catalog (the water volumes need the planes of their level)
        'unlisted': ['water-vol', 'battlecontroller', 'pov-camera', 'camera-tracker', 'manipy', 'touch-tracker',
                     'viewer', 'anim-tester', 'process-drawable-reserved'],
    },
    'jak2': {
        'creatures': ['enemy', 'process-taskable', 'mech', 'wren', 'sidekick', 'target',
                      # characters that are neither: Samos at the stadium, a grunt of a cutscene;
                      # what makes guards talk to each other
                      'stad-samos', 'stad-youngsamos', 'sig-intro-grunt', 'guard-conversation'],
        # enemy subtypes that are machines
        'objects': ['fort-turret', 'sew-gunturret', 'hip-whack-a-metal'],
        'logic': ['part-spawner', 'process-hidden', 'battle', 'training-path', 'parking-spot', 'manipy',
                  'touch-tracker', 'viewer', 'process-drawable-reserved'],
        # process-hidden: the types of Jak 1 the game keeps and kills at once (water-vol, ecovent,
        # fuel-cell...); the fights, formations and spawners of enemies; cutscenes, demo, training
        'unlisted': ['process-hidden', 'battle', 'hover-formation', 'flying-formation', 'jellyfish-formation',
                     'nestb-formation', 'metalhead-spawner', 'predator-manager', 'forest-hover-manager',
                     'hoverboard-training-manager', 'training-manager', 'training-path', 'demo-control',
                     'scene-stage', 'manipy', 'touch-tracker', 'viewer', 'process-drawable-reserved'],
    },
    'jak3': {
        'creatures': ['enemy', 'process-taskable', 'mech', 'sidekick', 'target'],
        'objects': [],
        'logic': ['part-spawner', 'process-hidden', 'w-parking-spot', 'manipy', 'touch-tracker', 'viewer',
                  'process-drawable-reserved', 'editable-player', 'nav-mesh-editor'],
        'unlisted': ['process-hidden', 'manipy', 'touch-tracker', 'viewer', 'process-drawable-reserved',
                     'editable-player', 'nav-mesh-editor'],
    },
}
CREATURE_SUFFIXES = ['-highres']

# The families of the objects in the editor's catalog: the first base type of an object's
# ancestry found here, else the first word of FAMILY_WORDS in its name, else "other".
FAMILY_BASES = [
    ('water-anim', 'water'), ('crate', 'crate'), ('collectable', 'collectable'), ('eco', 'collectable'),
    ('basebutton', 'button'), ('com-airlock', 'door'), ('eco-door', 'door'), ('elevator', 'platform'),
    ('base-plat', 'platform'), ('baseplat', 'platform'), ('plat', 'platform'), ('drop-plat', 'platform'),
    ('rigid-body-platform', 'platform'), ('conveyor', 'platform'), ('vehicle', 'vehicle'),
    ('elec-gate', 'hazard'), ('fire-floor', 'hazard'), ('strip-hazard', 'hazard'), ('sew-blade', 'hazard'),
    ('bouncer', 'movement'), ('swingpole', 'movement'), ('launcher', 'movement'), ('warp-gate', 'movement'),
    ('base-turret', 'weapon'), ('breakaway', 'breakable'), ('process-focusable', 'other'),
]
FAMILY_WORDS = [('door', 'door'), ('gate', 'door'), ('plat', 'platform'), ('lift', 'platform'),
                ('elevator', 'platform'), ('bridge', 'platform'), ('crate', 'crate'), ('button', 'button'),
                ('switch', 'button'), ('lamp', 'light'), ('light', 'light'), ('turret', 'weapon'),
                ('water', 'water'), ('pool', 'water'), ('lava', 'water'), ('break', 'breakable'),
                ('barrel', 'breakable')]

# ------------------------------------------------------------------------------------------------
# GOAL source reading
# ------------------------------------------------------------------------------------------------


class Sym(str):
    __slots__ = ()


class Str(str):
    __slots__ = ()


DELIMS = set('()\'`,";') | set(' \t\r\n')


def atom(tok):
    if tok == '#t':
        return True
    if tok == '#f':
        return False
    try:
        if tok.startswith('#x'):
            return int(tok[2:], 16)
        if tok.startswith('-#x'):
            return -int(tok[3:], 16)
        if tok.startswith('#b'):
            return int(tok[2:], 2)
        return int(tok)
    except ValueError:
        pass
    if tok[0] in '0123456789-+.' and any(c.isdigit() for c in tok):
        try:
            return float(tok)
        except ValueError:
            pass
    return Sym(tok)


def form_end(text, i):
    """The end of the balanced form starting at text[i] == '('."""
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == '\\' else 1
        elif c == ';':
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        elif c == '#' and text.startswith('#|', i):
            j = text.find('|#', i)
            i = n if j < 0 else j + 2
            continue
        elif c == '#' and text.startswith('#\\', i):
            i += 3
            continue
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def parse(text):
    """The forms of GOAL source: lists, Sym, Str, int, float and bool; 'x reads as (quote x)."""
    out = []
    stack = [(out, [])]  # items, prefixes waiting for the next item
    i, n = 0, len(text)

    def add(x):
        items, prefixes = stack[-1]
        while prefixes:
            x = [Sym(prefixes.pop()), x]
        items.append(x)

    while i < n:
        c = text[i]
        if c in ' \t\r\n':
            i += 1
        elif c == ';':
            j = text.find('\n', i)
            i = n if j < 0 else j + 1
        elif c == '#' and text.startswith('#|', i):
            j = text.find('|#', i + 2)
            i = n if j < 0 else j + 2
        elif c == '(':
            stack.append(([], []))
            i += 1
        elif c == ')':
            if len(stack) > 1:
                items, _ = stack.pop()
                add(items)
            i += 1
        elif c == "'":
            stack[-1][1].append('quote')
            i += 1
        elif c == '`':
            stack[-1][1].append('quasiquote')
            i += 1
        elif c == ',':
            at = text.startswith(',@', i)
            stack[-1][1].append('unquote-splicing' if at else 'unquote')
            i += 2 if at else 1
        elif c == '"':
            j = i + 1
            buf = []
            while j < n and text[j] != '"':
                if text[j] == '\\' and j + 1 < n:
                    buf.append(text[j + 1])
                    j += 2
                else:
                    buf.append(text[j])
                    j += 1
            add(Str(''.join(buf)))
            i = j + 1
        else:
            j = i + 3 if text.startswith('#\\', i) else i
            while j < n and text[j] not in DELIMS:
                j += 1
            add(atom(text[i:j]))
            i = j
    while len(stack) > 1:  # unbalanced end of file
        items, _ = stack.pop()
        add(items)
    return out


def is_list(x):
    return isinstance(x, list)


def head(x):
    return x[0] if is_list(x) and x else None


def unquote(x):
    return x[1] if is_list(x) and len(x) == 2 and x[0] == 'quote' else x


def is_new(x, type_name):
    """(new 'static 'type_name ...)"""
    return (is_list(x) and len(x) >= 3 and x[0] == 'new' and unquote(x[1]) == 'static'
            and unquote(x[2]) == type_name)


def kwargs(form):
    """The :key value pairs of a form."""
    d = {}
    i = 0
    while i < len(form):
        x = form[i]
        if isinstance(x, Sym) and x.startswith(':') and i + 1 < len(form):
            d[str(x[1:])] = form[i + 1]
            i += 2
        else:
            i += 1
    return d


def walk(x):
    """Every list in x, x first, in source order."""
    stack = [x]
    while stack:
        y = stack.pop()
        if is_list(y):
            yield y
            stack.extend(reversed(y))


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def sources(root):
    for base, dirs, files in os.walk(root):
        dirs.sort()
        for f in sorted(files):
            if f.endswith('.gc'):
                yield os.path.join(base, f)


def forms_matching(paths, pattern, need=None):
    """(path, form) of the forms starting where `pattern` matches in the files (holding `need`)."""
    rx = re.compile(pattern, re.M)
    for path in paths:
        text = read(path)
        if need and need not in text:
            continue
        for m in rx.finditer(text):
            start = m.start()
            forms = parse(text[start:form_end(text, start)])
            if forms:
                yield path, forms[0]


def enum_values(paths, name):
    """name -> value of a defenum (bit indices for a bitfield enum)."""
    for _, form in forms_matching(paths, r'^\(defenum ' + re.escape(name) + r'\s', '(defenum ' + name):
        values = {}
        nxt = 0
        items = form[2:]
        k = 0
        while k < len(items):
            it = items[k]
            if isinstance(it, Sym) and it.startswith(':'):
                k += 2
                continue
            if is_list(it) and it:
                v = it[1] if len(it) > 1 and isinstance(it[1], int) and not isinstance(it[1], bool) else nxt
                values[str(it[0])] = v
                nxt = v + 1
            k += 1
        return values
    return {}


# ------------------------------------------------------------------------------------------------
# game data
# ------------------------------------------------------------------------------------------------


def bits_value(expr, bits):
    """(task-mask task0 primary0) -> the mask."""
    if isinstance(expr, int) and not isinstance(expr, bool):
        return expr
    if is_list(expr) and len(expr) >= 1:
        v = 0
        for name in expr[1:]:
            if str(name) in bits:
                v |= 1 << bits[str(name)]
        return v
    return 0


def negate(e):
    if e is True or e is False:
        return not e
    return e[1] if is_list(e) and e[0] == 'not' else ['not', e]


def story_expr(x):
    """A story condition of the game code as JSON, None when it is not only about the story:
    ["closed", node], ["open", node], ["complete", task], ["not", e], ["and", e...], ["or", e...]
    In an "or", a test of the actor's own saved state (a crate already broken...) counts as false:
    at a given point of the story, the story terms decide."""
    if x is True or x is False:
        return x
    h = head(x)
    if h == 'not' and len(x) == 2:
        e = story_expr(x[1])
        return None if e is None else negate(e)
    if h in ('and', 'or') and len(x) > 1:
        parts = [story_expr(a) for a in x[1:]]
        if h == 'or':
            parts = [p for p in parts if p is not None and p is not False]
            if not parts:
                return None if any(story_expr(a) is None for a in x[1:]) else False
            if True in parts:
                return True
        elif any(p is None for p in parts):
            return None
        return parts[0] if len(parts) == 1 else [str(h)] + parts
    if h in ('task-node-closed?', 'task-node-open?') and len(x) == 2 and head(x[1]) == 'game-task-node':
        return ['closed' if h == 'task-node-closed?' else 'open', str(x[1][1])]
    if h in ('task-closed?', 'task-open?') and len(x) == 2 and isinstance(x[1], Str):
        return ['closed' if h == 'task-closed?' else 'open', str(x[1])]
    if h == 'task-complete?' and len(x) == 3 and head(x[2]) == 'game-task':
        return ['complete', str(x[2][1])]
    if h == 'demo?' and len(x) == 1:
        return False
    return None


def expr_refs(e, out):
    if is_list(e):
        if e and e[0] in ('closed', 'open', 'complete'):
            out.add((e[0], e[1]))
        else:
            for a in e[1:]:
                expr_refs(a, out)
    return out


SELF = ('this', 'self', 'obj')


def removes_itself(code):
    """Code that stops drawing its actor or removes it (not following state changes)."""
    no_draw = False
    for f in walk(code):
        h = head(f)
        if (h in ('logior!', 'logclear!') and len(f) == 3 and head(f[2]) == 'draw-control-status'
                and 'no-draw' in f[2][1:] and head(f[1]) == '->' and f[1][1] in SELF):
            if h == 'logclear!':
                return False  # drawn again
            no_draw = True
        if h in ('cleanup-for-death', 'deactivate') and len(f) == 2 and f[1] in SELF:
            return True
        if h == 'go' and len(f) >= 2 and f[1] == 'empty-state':
            return True
    return no_draw


def hides(branch, states):
    """Code that removes the actor or stops drawing it, directly or by going to a state that does
    (`states`: the state name -> whether it does, for the actor's type and its parents)."""
    if removes_itself(branch):
        return True
    for f in walk(branch):
        h = head(f)
        target = None
        if h == 'go' and len(f) >= 2:
            target = f[1]
            if head(target) == 'method-of-object' and len(target) == 3 and target[1] in SELF:
                target = target[2]
        elif h == 'go-virtual' and len(f) >= 2:
            target = f[1]
        if isinstance(target, Sym) and states(str(target)):
            return True
    return False


def visibility_rules(body, states):
    """When the actor of an init-from-entity! method is shown, from the story tests guarding the
    code that hides it; None without such a test."""
    rules = []
    for f in walk(body):
        h = head(f)
        if h in ('if', 'when', 'unless') and len(f) >= 3:
            test = story_expr(f[1])
            if test is None or test is True or test is False:
                continue
            if h == 'if':
                then, other = f[2], (f[3] if len(f) > 3 else None)
            elif h == 'when':
                then, other = ['begin'] + f[2:], None
            else:
                then, other = None, ['begin'] + f[2:]
            then_hides = then is not None and hides(then, states)
            other_hides = other is not None and hides(other, states)
            if then_hides and not other_hides:
                rules.append(negate(test))
            elif other_hides and not then_hides:
                rules.append(test)
        elif h == 'cond':
            # a clause runs when its test holds and the earlier ones do not
            earlier = []
            for clause in f[1:]:
                if not is_list(clause) or not clause:
                    break
                test = True if clause[0] == 'else' else story_expr(clause[0])
                if test is None:
                    break
                reached = earlier + ([] if test is True else [test])
                if hides(['begin'] + clause[1:], states) and reached:
                    rules.append(negate(reached[0] if len(reached) == 1 else ['and'] + reached))
                if test is True:
                    break
                earlier.append(negate(test))
    if not rules:
        return None
    return rules[0] if len(rules) == 1 else ['and'] + rules


def game_data(goal_src, game):
    cfg = GAMES[game]
    root = os.path.join(goal_src, game)
    engine = list(sources(os.path.join(root, 'engine')))
    everything = list(sources(root))
    out = {'format': 1, 'game': game,
           'source': 'tools/gen_game_data.py, from goal_src/%s of open-goal/jak-project and extractor/data/%s/all-types.gc'
                     % (game, game)}

    # --- actor roles -----------------------------------------------------------------------------
    parent = {}
    types_text = read(os.path.join(REPO, 'extractor', 'data', game, 'all-types.gc'))
    for m in re.finditer(r'^\(deftype ([^\s()]+)\s+\(([^\s()]+)\)', types_text, re.M):
        parent[m.group(1)] = m.group(2)

    def lineage(t):
        out_ = [t]
        while t in parent and len(out_) < 64:
            t = parent[t]
            out_.append(t)
        return out_

    roles = {}
    for t in parent:
        line = lineage(t)
        if 'process' not in line:
            continue  # not an actor
        if any(x in cfg['objects'] for x in line):
            continue
        if any(x in cfg['creatures'] for x in line):
            roles[t] = 'creature'
        elif any(x in cfg['logic'] for x in line) or 'process-drawable' not in line:
            roles[t] = 'logic'
    # the objects (actors drawn with a model that are neither creatures nor logic) and their family
    bases = dict(FAMILY_BASES)
    objects = {}
    for t in parent:
        line = lineage(t)
        if t in roles or 'process-drawable' not in line or t in ('process-drawable', 'process-focusable'):
            continue
        family = next((bases[x] for x in line if x in bases and bases[x] != 'other'), None)
        if family is None:
            family = next((f for word, f in FAMILY_WORDS if word in t), 'other')
        objects[t] = family
    out['actors'] = {'roles': dict(sorted(roles.items())), 'creature_suffixes': CREATURE_SUFFIXES,
                     'water_types': sorted(t for t in parent if 'water-anim' in lineage(t)),
                     'objects': dict(sorted(objects.items()))}

    # --- water looks -----------------------------------------------------------------------------
    skel_models = {}
    for _, form in forms_matching(everything, r'^\(defskelgroup ', '(defskelgroup'):
        # (defskelgroup skel-x x x-lod0-jg -1 ((x-lod0-mg (meters 999999))) ...); Jak 3 names the
        # meshes by their index in the art group: the model is then named after the group
        if len(form) > 5 and is_list(form[5]) and form[5] and is_list(form[5][0]) and form[5][0]:
            mg = form[5][0][0]
            if isinstance(mg, Sym) and mg.endswith('-mg'):
                skel_models[str(form[1])] = re.sub(r'-lod\d+$', '', str(mg)[:-3])
            elif len(form) > 2:
                skel_models[str(form[1])] = re.sub(r'^skel-|^\*|-sg\*$', '', str(form[1]))

    # --- the models of each type: the skeleton groups its methods, states and behaviors load
    # ("skel-x" in Jak II and 3, *x-sg* in Jak 1), in the order of the code (the first one is
    # usually what init-from-entity! draws). A type without any takes its parent's.
    rx_skel = re.compile(r'"(skel-[^"\s]+)"|(\*[\w-]+-sg\*)')
    rx_owner = re.compile(r'^\((?:defmethod ([^\s()]+) \(\((?:this|obj|self) ([^\s()]+)\)|'
                          r'defmethod ([^\s()]+) ([^\s()]+) \(|defstate [^\s()]+ \(([^\s()]+)\)|'
                          r'defbehavior ([^\s()]+) ([^\s()]+) )', re.M)
    rx_deftype = re.compile(r'^\(deftype ([^\s()]+) ', re.M)
    rx_string = re.compile(r'"([^"\s]+)"')
    found = {}  # type -> model -> (rank, order)

    def note(owner, model, init, order):
        # the models an init method loads come first, pieces of debris last
        rank = (0 if init else 1) + (2 if re.search(r'-(explode|debris|break|chunk)(-|$)', model) else 0)
        old = found.setdefault(owner, {}).get(model)
        if old is None or (rank, order) < old:
            found[owner][model] = (rank, order)

    order = 0
    for path in everything:
        text = read(path)
        if 'skel-' not in text and '-sg*' not in text:
            continue
        for m in rx_owner.finditer(text):
            g = m.groups()
            method, owner = (g[0], g[1]) if g[1] else (g[2], g[3]) if g[3] else ('', g[4]) if g[4] else (g[5], g[6])
            init = 'init' in (method or '')
            body = text[m.start():form_end(text, m.start())]
            for sm in rx_skel.finditer(body):
                model = skel_models.get(sm.group(1) or sm.group(2))
                if model:
                    order += 1
                    note(owner, model, init, order)
        # tables of skeleton group names ("ruins-breakable-wall-1"): the type of the file whose
        # name begins the group's
        types_here = sorted(rx_deftype.findall(text), key=len, reverse=True)
        for sm in rx_string.finditer(text):
            key = 'skel-' + sm.group(1)
            if key not in skel_models:
                continue
            owner = next((t for t in types_here if sm.group(1).startswith(t)), None)
            if owner:
                order += 1
                note(owner, skel_models[key], False, order)
    own_models = {t: [m for m, _ in sorted(ms.items(), key=lambda kv: kv[1])] for t, ms in found.items()}
    type_models = {}
    for t in set(parent) | set(own_models):
        for up in lineage(t):
            if up in own_models:
                type_models[t] = own_models[up]
                break

    # the objects whose models are all characters are creatures too (the NPCs of a mission...)
    character_models = {m for t, ms in type_models.items() if roles.get(t) == 'creature' for m in ms}
    for t, ms in type_models.items():
        if t not in roles and ms and all(m in character_models or m.endswith(('-highres', '-lowres'))
                                         for m in ms):
            roles[t] = 'creature'
    out['actors']['roles'] = dict(sorted(roles.items()))
    out['actors']['effects'] = sorted(t for t in parent if 'part-spawner' in lineage(t))
    out['actors']['unlisted'] = sorted(t for t in parent
                                       if roles.get(t) == 'logic' and any(x in cfg['unlisted'] for x in lineage(t))
                                       and 'water-anim' not in lineage(t))  # Jak 1's water is a water-vol
    for t in list(out['actors']['objects']):
        if t in roles:
            del out['actors']['objects'][t]
    out['actors']['models'] = {t: ms for t, ms in sorted(type_models.items()) if t in parent or t in own_models}
    # the models of the skeleton groups by name, for the actors that choose theirs by name
    # (art-name: "skel-" + its value)
    out['actors']['skeletons'] = {re.sub(r'^skel-', '', k): v for k, v in sorted(skel_models.items())
                                  if k.startswith('skel-') and re.sub(r'^skel-', '', k) != v}
    looks = []
    for path, form in forms_matching(engine, r'^\(define \*water-anim-look\*', 'water-anim-look'):
        for f in walk(form):
            if is_new(f, 'water-anim-look'):
                group = unquote(kwargs(f).get('skel-group'))
                group = str(group) if group else ''
                key = group if group.startswith('*') else 'skel-' + group
                fallback = re.sub(r'^\*|-sg\*$', '', group)
                looks.append(skel_models.get(key, fallback))
        break
    out['actors']['water_looks'] = looks

    # --- oceans ----------------------------------------------------------------------------------
    defs = {}  # *ocean-...* -> form
    links = {}  # (map, field) -> table symbol
    for path in engine + [p for p in everything if p.endswith('-ocean.gc')]:
        text = read(path)
        if '*ocean-map' not in text and '*ocean-mid-' not in text:
            continue
        for m in re.finditer(r'^\((define|set!) ', text, re.M):
            start = m.start()
            forms = parse(text[start:form_end(text, start)])
            if not forms:
                continue
            f = forms[0]
            if head(f) == 'define' and len(f) == 3 and str(f[1]).startswith('*ocean-'):
                defs[str(f[1])] = f[2]
            elif (head(f) == 'set!' and len(f) == 3 and head(f[1]) == '->' and len(f[1]) == 3
                  and str(f[1][1]).startswith('*ocean-map')):
                links[(str(f[1][1]), str(f[1][2]))] = str(f[2])
    oceans = {}
    for name, value in defs.items():
        if not name.startswith('*ocean-map') or not is_new(value, 'ocean-map'):
            continue
        kw = kwargs(value)
        corner = kwargs(kw.get('start-corner', []))
        table = lambda field: defs.get(links.get((name, field)) or str(unquote(kw.get(field, ''))))
        indices = table('ocean-mid-indices')
        masks = table('ocean-mid-masks')
        if indices is None or masks is None:
            continue
        idx = next((f[4:] for f in walk(indices) if head(f) == 'new' and len(f) > 4 and unquote(f[2]) == 'array'), [])
        mask_list = []
        for f in walk(masks):
            if is_new(f, 'ocean-mid-mask'):
                m = kwargs(f).get('mask')
                mask_list.append(list(m[4:12]) if is_list(m) else [0] * 8)
        rows = []
        for r in range(48):
            row = []
            for c in range(48):
                block = (r // 8) * 6 + c // 8
                mi = idx[block] if block < len(idx) else 0
                bits = mask_list[mi] if mi < len(mask_list) else [0xff] * 8
                row.append('0' if bits[r % 8] & (1 << (c % 8)) else '1')
            rows.append(''.join(row))
        # the color of the water: the average of its color table (0x80 is 1.0, the PS2's scale)
        color = [0.1, 0.3, 0.35]
        colors = table('ocean-colors')
        if colors is not None:
            rgb = [kwargs(f) for f in walk(colors) if is_new(f, 'rgba')]
            if rgb:
                color = [round(min(sum(float(c.get(k, 0)) for c in rgb) / len(rgb) / 128.0, 1.0), 3)
                         for k in ('r', 'g', 'b')]
        oceans[name] = {'corner': [round(float(corner.get(k, 0.0)) / 4096.0, 4) for k in ('x', 'y', 'z')],
                        'cell': 96.0, 'cells': rows, 'color': color}
    out['oceans'] = dict(sorted(oceans.items()))

    # --- levels ----------------------------------------------------------------------------------
    task_bits = enum_values(engine, 'task-mask')
    levels = {}
    info_path = os.path.join(root, 'engine', 'level', 'level-info.gc')
    info_text = read(info_path)
    # Jak 3 levels set the height of their ocean
    ocean_height_field = ':ocean-height' in info_text
    for form in parse(info_text):
        if head(form) != 'define' or len(form) != 3 or not is_new(form[2], 'level-load-info'):
            continue
        kw = kwargs(form[2])
        name = str(unquote(kw.get('name', form[1])))
        ocean = unquote(kw.get('ocean', False))
        lev = {'taskname': str(unquote(kw['taskname'])) if kw.get('taskname') else name,
               'base_mask': bits_value(kw.get('base-task-mask', 0), task_bits),
               'ocean': str(ocean) if isinstance(ocean, Sym) and str(ocean) in oceans else None}
        if lev['ocean'] and ocean_height_field:
            lev['ocean_height'] = round(float(kw.get('ocean-height', 0.0)) / 4096.0, 4)
        wants = set()
        for f in walk(unquote(kw.get('continues', []))):
            if is_new(f, 'level-buffer-state'):
                k = kwargs(f)
                if unquote(k.get('display?')) in ('display', 'special') and unquote(k.get('name')):
                    wants.add(str(unquote(k['name'])))
        lev['wants'] = sorted(wants - {name})
        borrow, display = kw.get('borrow-level'), kw.get('borrow-display?')
        slots = [str(x) if isinstance(x, Sym) else None for x in borrow[4:]] if is_list(borrow) else []
        modes = [str(unquote(x)) if isinstance(unquote(x), Sym) else None for x in display[4:]] if is_list(display) else []
        lev['borrow'] = [[lv, modes[i] if i < len(modes) else None] for i, lv in enumerate(slots)]
        levels[name] = lev

    # --- story -----------------------------------------------------------------------------------
    task_dir = os.path.join(root, 'engine', 'game', 'task')
    node_index = enum_values(engine, 'game-task-node')
    story = None
    if node_index and os.path.isdir(task_dir):
        node_forms = []
        for form in parse(read(os.path.join(task_dir, 'game-task.gc'))):
            for f in walk(form):
                if is_new(f, 'game-task-node-info'):
                    node_forms.append(f)
        nodes = [None] * (max(node_index.values()) + 1)
        for f in node_forms:
            kw = kwargs(f)
            name = str(kw.get('name', ''))
            if name not in node_index:
                continue
            flags = [str(x) for x in kw.get('flags', [])[1:]] if is_list(kw.get('flags')) else []
            op = next((o for o, flag in (('abs', 'abs-task-mask'), ('set', 'set-task-mask'),
                                        ('clear', 'clear-task-mask')) if flag in flags), None)
            parents = []
            for p in walk(kw.get('parent-node', [])):
                if head(p) == 'game-task-node' and len(p) == 2 and str(p[1]) in node_index and p[1] != 'none':
                    parents.append(node_index[str(p[1])])
            borrow = []
            for b in unquote(kw.get('borrow', [])) or []:
                if is_list(b) and len(b) == 4:
                    borrow.append([str(b[0]), int(b[1]), str(b[2]) if b[2] else None, str(b[3]) if b[3] else None])
            task = kw.get('task')
            node = {'name': name, 'task': str(task[1]) if head(task) == 'game-task' else '',
                    'level': str(unquote(kw.get('level', ''))) if kw.get('level') else ''}
            if op:
                node['mask_op'] = op
                node['mask'] = bits_value(kw.get('task-mask', 0), task_bits)
            if 'close-task' in flags:
                node['close_task'] = True
            if 'closed' in flags:
                node['closed'] = True  # from the start of the game
            if 'utility-node' in flags:
                node['utility'] = True
            if parents:
                node['parents'] = parents
            if borrow:
                node['borrow'] = borrow
            nodes[node_index[name]] = node
        for i, n in enumerate(nodes):
            if n is None:
                nodes[i] = {'name': 'none' if i == 0 else 'node-%d' % i, 'task': '', 'level': ''}

        # the node whose borrow list resets the borrowed layers before the open nodes add theirs
        reset = None
        control = read(os.path.join(task_dir, 'task-control.gc'))
        m = re.search(r'\(borrow-eval \(-> \*game-info\* sub-task-list \(game-task-node ([\w-]+)\) borrow\)\)', control)
        if m and m.group(1) in node_index:
            reset = m.group(1)

        # per level: the decor prototypes shown or hidden by the story, the ocean height set by
        # the story (the sewer drained)
        prototypes = {}
        ocean_heights = {}
        for form in parse(control):
            if 'prototypes-game-visible-set!' not in json.dumps(form):
                continue
            for f in walk(form):
                if head(f) != 'case' or len(f) < 3 or f[1] != ['->', 'this', 'name']:
                    continue
                for clause in f[2:]:
                    if not is_list(clause) or not clause or clause[0] == 'else':
                        continue
                    keys = [str(unquote(k)) for k in clause[0]] if is_list(clause[0]) and clause[0] and \
                        head(clause[0]) != 'quote' else [str(unquote(clause[0]))]
                    for g in walk(clause[1:]):
                        if head(g) == 'prototypes-game-visible-set!' and len(g) >= 3:
                            names = [re.sub(r'\.mb$', '', str(s)) for s in unquote(g[1]) if isinstance(s, Str)]
                            when = story_expr(g[2])
                            if names and when is not None:
                                for key in keys:
                                    prototypes.setdefault(key, []).append({'names': names, 'when': when})
                        elif head(g) == 'cond':
                            earlier = []
                            for cc in g[1:]:
                                if not is_list(cc) or not cc:
                                    break
                                test = True if cc[0] == 'else' else story_expr(cc[0])
                                if test is None:
                                    continue  # not about the story (the debug mode): never in the editor
                                for s in walk(cc[1:]):
                                    if head(s) == 'set-height!' and len(s) == 3 and isinstance(s[2], (int, float)):
                                        reached = earlier + ([] if test is True else [test])
                                        when = True if not reached else (reached[0] if len(reached) == 1 else ['and'] + reached)
                                        for key in keys:
                                            ocean_heights.setdefault(key, []).append(
                                                {'when': when, 'height': round(float(s[2]) / 4096.0, 4)})
                                if test is True:
                                    break
                                earlier.append(negate(test))

        # actor types shown or hidden by the story (their init-from-entity! method)
        state_hides = {}  # (type, state) -> the state removes the actor or stops drawing it
        for _, form in forms_matching(everything, r'^\(defstate ', '(defstate'):
            if len(form) > 2 and is_list(form[2]) and form[2]:
                state_hides[(str(form[2][0]), str(form[1]))] = removes_itself(form[3:])
        types = {}
        pattern = r'^\(defmethod init-from-entity! \(\((?:this|obj) ([^\s()]+)\)'
        own_init = set()
        rx_init = re.compile(pattern, re.M)
        for path in everything:
            text = read(path)
            if 'init-from-entity!' in text:
                own_init.update(rx_init.findall(text))
        for path, form in forms_matching(everything, pattern, 'init-from-entity!'):
            text = json.dumps(form)
            if 'task-node-closed?' not in text and 'task-complete?' not in text and 'task-closed?' not in text:
                continue
            tname = str(form[2][0][1]) if is_list(form[2]) and form[2] and is_list(form[2][0]) else None
            if not tname:
                continue

            def states(state, line=lineage(tname)):
                return next((state_hides[(t, state)] for t in line if (t, state) in state_hides), False)

            when = visibility_rules(form[3:], states)
            if when is not None:
                types[tname] = when
        for t in parent:
            if t in types or t in own_init:
                continue
            for up in lineage(t)[1:]:
                if up in types:
                    types[t] = types[up]  # the parent's init-from-entity!
                    break
                if up in own_init:
                    break

        # references to unknown nodes or tasks make a rule unusable
        known_tasks = {n['task'] for n in nodes}

        def usable(e):
            return all((kind == 'complete' and ref in known_tasks) or (kind != 'complete' and ref in node_index)
                       for kind, ref in expr_refs(e, set()))

        prototypes = {k: [r for r in v if usable(r['when'])] for k, v in sorted(prototypes.items())}
        ocean_heights = {k: [r for r in v if usable(r['when'])] for k, v in sorted(ocean_heights.items())}
        story = {'reset_node': reset, 'nodes': nodes,
                 'prototypes': {k: v for k, v in prototypes.items() if v},
                 'types': {k: v for k, v in sorted(types.items()) if usable(v)},
                 'ocean_heights': {k: v for k, v in ocean_heights.items() if v},
                 'mask_bits': {k: v for k, v in task_bits.items() if not k.startswith('tm')}}

    # layers: the levels borrowed by the story or by default; companions: the levels a level's
    # continue points also show, when they belong to its task level (the hut of Dead Town)
    layers = set()
    if story:
        for n in story['nodes']:
            for host, slot, lev, disp in n.get('borrow', []):
                if lev:
                    layers.add(lev)
    for lev in levels.values():
        layers.update(lv for lv, _ in lev['borrow'] if lv)
    for name, lev in levels.items():
        lev['layer'] = name in layers
        lev['companions'] = [w for w in lev['wants'] if w in levels and w not in layers and w != name
                             and levels[w]['taskname'] == name]
        # the city clears the "ctywide" bit of the task mask (its traffic manager)
        lev['traffic'] = name == 'ctywide' or 'ctywide' in lev['wants']
    for lev in levels.values():
        del lev['wants']
        if not any(lv for lv, _ in lev['borrow']):
            del lev['borrow']
    out['levels'] = dict(sorted(levels.items()))
    out['story'] = story
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    goal_src = sys.argv[1]
    for game in sys.argv[2:] or ['jak1', 'jak2', 'jak3']:
        data = game_data(goal_src, game)
        path = os.path.join(REPO, 'data', game, 'game-data.json')
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(data, f, indent=1, sort_keys=False)
            f.write('\n')
        story = data['story']
        print('%s: %d actor roles, %d water looks, %d oceans, %d levels (%d layers), %s' % (
            game, len(data['actors']['roles']), len(data['actors']['water_looks']), len(data['oceans']),
            len(data['levels']), sum(1 for l in data['levels'].values() if l['layer']),
            'no story' if not story else '%d story nodes, %d prototype levels, %d actor types' % (
                len(story['nodes']), len(story['prototypes']), len(story['types']))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
