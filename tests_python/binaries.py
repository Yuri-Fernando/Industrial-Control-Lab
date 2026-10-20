"""Locate the compiled controller / remote I/O node on Linux, WSL or Windows."""
import configparser
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = '.exe' if sys.platform.startswith('win') else ''


def pio_build_dir():
    """PlatformIO build_dir, honouring the optional gitignored platformio_local*.ini override."""
    build_dir = ROOT / '.pio' / 'build'
    for ini in sorted(ROOT.glob('platformio_local*.ini')):
        cfg = configparser.ConfigParser(inline_comment_prefixes=(';', '#'))
        cfg.read(ini, encoding='utf-8')
        value = cfg.get('platformio', 'build_dir', fallback='').strip()
        if value:
            build_dir = Path(value) if Path(value).is_absolute() else ROOT / value
    return build_dir


def _find(env_var, candidates):
    override = os.getenv(env_var)
    if override:
        return Path(override)
    found = [p for p in candidates if p.is_file()]
    if not found:
        raise RuntimeError('Binaries not built. Run `pio run` (PlatformIO) or `python scripts/lab.py build`.')
    return max(found, key=lambda p: p.stat().st_mtime)  # newest build wins


def controller_binary():
    return _find('ICL_CONTROLLER_BIN', [pio_build_dir() / 'native' / f'program{EXE}',
                                        ROOT / 'build' / f'industrial_controller{EXE}'])


def io_node_binary():
    return _find('ICL_IO_NODE_BIN', [pio_build_dir() / 'io_node' / f'program{EXE}',
                                     ROOT / 'build' / f'io_node{EXE}'])
