import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
kernel = root / 'kernel'
obj = 'obj/src/mm/vmm.c.o'
subprocess.run(['make', '-C', str(kernel), obj], check=True)
with tempfile.TemporaryDirectory() as directory:
    binary = pathlib.Path(directory) / 'vmm-test'
    subprocess.run(['cc', '-std=gnu11', '-no-pie', '-ffunction-sections',
                    '-fdata-sections', '-Wl,--gc-sections',
                    '-I' + str(kernel / 'include'),
                    '-I' + str(kernel / 'limine-protocol/include'),
                    str(root / 'tests/vmm_translation.c'), str(kernel / obj),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
