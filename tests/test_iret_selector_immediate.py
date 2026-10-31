"""Guard the executable's initial ring-3 IRET frame against SS=0x18."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
elf = (root / 'kernel/src/sys/elf.c').read_text()
gdt = (root / 'kernel/include/desc/gdt.h').read_text()

assert re.search(r'#define\s+USER_DATA\s+0x18\b', gdt)
assert re.search(r'#define\s+USER_CODE\s+0x20\b', gdt)
assert 'thr->regs->ss     = USER_DATA | 3;' in elf
assert 'thr->regs->cs     = USER_CODE | 3;' in elf
# The first and last IRET frame pushes must include the privilege bits, even
# if the compiler's register allocator or another path later changes.
iret = elf[elf.index('"pushq $0x1B'):elf.index('"iretq\\n\\t"')]
pushes = re.findall(r'"pushq\s+([^\\"]+)', iret)
assert pushes[:5] == ['$0x1B', '%1', '$0x202', '%2', '%3'], pushes
assert '(USER_DATA | 3) == 0x1B' in elf
print('Initial IRET user SS immediate and selector layout passed')
