"""Helpers for host-side tests that compile a slice of the kernel sources.

The kernel is freestanding, so a test never links the whole kernel: it takes
specific functions out of the production sources and supplies minimal stubs for
everything those functions call. That keeps the asserted code identical to what
the kernel actually executes, which is the whole point of these tests.

Extraction is by function name with brace matching instead of by text offsets,
so moving a function inside its source file never breaks a test.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def source(rel_path):
    return (ROOT / rel_path).read_text()


def function(text, name, occurrence=0):
    """Return the full definition of `name`, including a leading comment block.

    Brace matching is quote/comment aware, so string literals and comments that
    contain braces cannot truncate the result.
    """
    pattern = re.compile(
        r'^[A-Za-z_][A-Za-z0-9_ \t\*]*\b' + re.escape(name) +
        r'\s*\([^;{]*\)\s*\{', re.M)
    matches = list(pattern.finditer(text))
    assert matches, f'definition of {name} not found'
    start = matches[occurrence].start()
    brace = text.index('{', matches[occurrence].start())
    depth = 0
    i = brace
    in_str = in_chr = in_line = in_block = False
    while i < len(text):
        c = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ''
        if in_line:
            if c == '\n':
                in_line = False
        elif in_block:
            if c == '*' and nxt == '/':
                in_block = False
                i += 1
        elif in_str:
            if c == '\\':
                i += 1
            elif c == '"':
                in_str = False
        elif in_chr:
            if c == '\\':
                i += 1
            elif c == "'":
                in_chr = False
        else:
            if c == '/' and nxt == '/':
                in_line = True
                i += 1
            elif c == '/' and nxt == '*':
                in_block = True
                i += 1
            elif c == '"':
                in_str = True
            elif c == "'":
                in_chr = True
            elif c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    return text[start:i + 1]
        i += 1
    raise AssertionError(f'unterminated definition of {name}')


def defines(text, *names):
    """Return the `#define` lines (with continuations) for the given names."""
    out = []
    for name in names:
        m = re.search(r'^#\s*define\s+' + re.escape(name) + r'\b[^\n]*'
                      r'(?:\\\n[^\n]*)*', text, re.M)
        assert m, f'#define {name} not found'
        out.append(m.group(0))
    return '\n'.join(out)


def slice_between(text, start, end):
    """Return text[start_marker:end_marker]; both markers must be present."""
    assert start in text, f'missing start marker: {start[:60]}'
    begin = text.index(start)
    assert end in text[begin:], f'missing end marker: {end[:60]}'
    return text[begin:text.index(end, begin)]


def compile_and_run(code, defines_=None, std='c11', extra_flags=()):
    """Compile `code` with -Wall -Wextra -Werror and run it."""
    flags = ['cc', f'-std={std}', '-Wall', '-Wextra', '-Werror']
    flags.extend(extra_flags)
    for d in (defines_ or ()):
        flags.append(f'-D{d}')
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'test.c'
        exe = Path(tmp) / 'test'
        src.write_text(code)
        flags.extend([str(src), '-o', str(exe)])
        result = subprocess.run(flags, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        return subprocess.run([str(exe)], check=True, capture_output=True)


def compile_only(code, std='c11'):
    """Compile `code` to an object file, failing the test on any diagnostic."""
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'test.c'
        src.write_text(code)
        result = subprocess.run(
            ['cc', f'-std={std}', '-Wall', '-Wextra', '-Werror', '-c',
             str(src), '-o', str(Path(tmp) / 'test.o')],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        return result
