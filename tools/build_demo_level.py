#!/usr/bin/env python3
"""Builds the demo level of the tutorial (docs/captures/README.md): two decks of the industrial
zone, a canal bridge stretched to cross the palace's pool between them, lamps, orbs, crates, the
palace's fountain bowl and a fruit stand. It drives the editor through its remote control, the
connection the MCP server forwards an assistant's tool calls to, with the same tools: this is what
an assistant does when asked to build a level.

The level is made of the game's parts, so it needs a Jak II library extracted with the editor. It
holds no data of the game itself: only names and positions.

Usage (from the repository root, after building the editor):

  python tools/build_demo_level.py [--out build/demo/canal-crossing.glb] [--view view.png]
                                   [--port 47890]

It starts a hidden editor on `--port` (the preferences are left unchanged), builds the level,
saves it as a project to `--out` and, with `--view`, saves the picture the assistant gets from
capture_view.
"""

import argparse
import base64
import json
import os
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EDITOR = os.path.join(REPO, 'build', 'bin', 'open-goal-level-editor' + ('.exe' if os.name == 'nt' else ''))


class Editor:
    """One request per connection: a JSON line {id, tool, args}, answered by a JSON line."""

    def __init__(self, port):
        self.port = port
        self.next_id = 1
        self.proc = subprocess.Popen([EDITOR, '--hidden', '--remote-port', str(port)],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def call(self, tool, **args):
        for _ in range(150):  # the editor takes a moment to start
            try:
                s = socket.create_connection(('127.0.0.1', self.port))
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise SystemExit('the editor did not start')
        with s:
            s.sendall((json.dumps({'id': self.next_id, 'tool': tool, 'args': args}) + '\n').encode())
            self.next_id += 1
            data = b''
            while b'\n' not in data:
                chunk = s.recv(1 << 22)
                if not chunk:
                    break
                data += chunk
        answer = json.loads(data.split(b'\n')[0])
        if not answer.get('ok'):
            raise SystemExit(f'{tool}: {answer.get("error")}')
        print(f'{tool:16s} {json.dumps(args)[:100]}')
        return answer

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def build(e):
    e.call('new_level', discard_changes=True)
    # two decks of catwalk modules of the industrial zone (16 m each), 40 m apart
    for x in (-44, -28, 28, 44):
        for z in (-8, 8):
            e.call('place', kind='decor', name='city-ind-catwalk-main-01', position=[x, 0, z])
    # the pool of the palace between them, and a canal bridge (16 m) stretched to cross it
    e.call('place', kind='object', name='water-anim-ctypal-lrgsqr-pool', position=[0, -1.5, 0])
    bridge = e.call('place', kind='decor', name='city-canal-bridge-span', position=[0, 0.83, 0])
    e.call('transform_nodes', ids=[bridge['result']['nodes'][0]['id']], scale_by=[2.5, 1, 1])
    # lamps at the ends of the bridge, orbs along it
    for x in (-22, 22):
        for z in (-5, 5):
            e.call('place', kind='object', name='ctyn-lamp', position=[x, 10, z], on_ground=True)
    for x in (-12, -6, 0, 6, 12):
        e.call('place', kind='object', name='skill', position=[x, 2, 0])
    # crates, and a corner of barrels and a market crate, on the first deck
    for i, (x, z) in enumerate([(-46, 10), (-43.5, 10), (-46, 12.5)]):
        e.call('place', kind='object', name='crate', position=[x, 10, z], on_ground=True, yaw_deg=12 * i)
    e.call('place', kind='object', name='market-crate', position=[-43.5, 10, -9.5], on_ground=True, yaw_deg=30)
    for x, z in [(-48, -12), (-45.5, -12), (-47, -9.5)]:
        e.call('place', kind='decor', name='cty-tanker-barrel', position=[x, 1.08, z])
    # the fountain bowl of the palace, a fruit stand and ground lamps on the second deck
    e.call('place', kind='decor', name='ctyp-fountain-bowl', position=[40, 1.08, 0])
    e.call('place', kind='object', name='cty-fruit-stand', position=[30, 10, 12.5], on_ground=True, yaw_deg=180)
    for z in (-13, 13):
        e.call('place', kind='decor', name='city-groundlamp-01', position=[50, 1.08, z])
    e.call('select', ids=[])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=os.path.join('build', 'demo', 'canal-crossing.glb'))
    ap.add_argument('--view', help='also save the view the assistant gets from capture_view (PNG)')
    ap.add_argument('--port', type=int, default=47890)
    a = ap.parse_args()
    if not os.path.isfile(EDITOR):
        raise SystemExit(f'build the editor first ({EDITOR} is missing)')
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    e = Editor(a.port)
    try:
        build(e)
        if a.view:
            e.call('set_camera', position=[-20, 42, 62], look_at=[2, 0, 0])
            image = e.call('capture_view', width=1280, height=720)['image']
            with open(a.view, 'wb') as f:
                f.write(base64.b64decode(image))
        e.call('save_project', path=os.path.abspath(a.out))
    finally:
        e.close()
    print('saved', a.out)


if __name__ == '__main__':
    sys.exit(main())
