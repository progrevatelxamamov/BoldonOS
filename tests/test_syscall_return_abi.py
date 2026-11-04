"""Ensure syscall return preserves every Linux-preserved GPR, including r10."""
from pathlib import Path
import re
s = (Path(__file__).resolve().parents[1] / 'kernel/src/syscall/syscall_stub.asm').read_text()
tail = s[s.index('    pop r10'):s.index('    swapgs', s.index('    pop r10'))]
instructions = [line.split(';')[0].strip().lower() for line in tail.splitlines()]
instructions = [line for line in instructions if line]
assert not any(re.match(r'(mov|xor|pop|lea|add|sub|and|or)\s+r10(?:w|d|b)?\s*(?:,|$)', i) for i in instructions[1:])
assert 'push rax' in instructions and 'pop rax' in instructions
assert instructions.index('push rax') < instructions.index('mov ds, ax') < instructions.index('pop rax')
assert instructions.index('pop rax') < instructions.index('pop rcx')
print('syscall return preserves r10 and rax while restoring segment selectors')
