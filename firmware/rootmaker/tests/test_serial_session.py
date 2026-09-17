"""Host-side ordering contract; actual reset behavior is tested on hardware."""
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
# This contract replaces pyserial below and must not require/open a real port.
serial_stub = types.ModuleType('serial')
serial_stub.Serial = None
with patch.dict(sys.modules, {'serial': serial_stub}):
    import board_lua
Board = board_lua.Board


class Port:
    def __init__(self, **kwargs):
        self.opened = False
        self.transitions = []
        self.dtr = self.rts = True

    def __setattr__(self, name, value):
        if name in ('dtr', 'rts') and getattr(self, 'opened', False):
            self.transitions.append((name, value))
        object.__setattr__(self, name, value)

    def open(self):
        self.opened = True
        # pyserial applies DTR first, then RTS during open().
        self.transitions.extend([('dtr', self.dtr), ('rts', self.rts)])

    def close(self):
        self.opened = False


class SerialSessionTest(unittest.TestCase):
    def test_open_releases_rts_before_dtr(self):
        with patch.object(board_lua.serial, 'Serial', Port), patch.object(Board, 'synchronize'):
            board = Board('/dev/test')
            self.assertEqual(board.serial.transitions,
                             [('dtr', True), ('rts', False), ('dtr', False)])
            board.close()


if __name__ == '__main__':
    unittest.main()
