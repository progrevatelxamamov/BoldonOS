"""The boot framebuffer maps 4 KiB PTEs; bit 7 is PAT at that level."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
fb = (root / 'kernel/src/dev/fbdev.c').read_text()
vmm = (root / 'kernel/src/mm/vmm.c').read_text()
fn = fb[fb.index('static void *fb_dev_mmap('):fb.index('static int fb_dev_close(')]
assert 'vmm_map_page(process->pml4, phys_start + i * 4096' in fn
assert 'PTE_PRESENT | PTE_USER | PTE_PWT;' in fn
assert 'PTE_HUGE' not in fn.split('uint64_t page_flags =', 1)[1]
assert 'pt->entries[pt_indx] = pte_create(phys, flags);' in vmm
print('Framebuffer mmap uses 4 KiB PTEs without the PAT/huge bit')
