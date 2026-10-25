#!/usr/bin/env python3
import os
import sys
import subprocess
import urllib.request
import shutil
import stat

ROOT_DIR = sys.argv[1] if len(sys.argv) > 1 else "/run/media/kolya/root"

print(f"=======================================================")
print(f"   ARCH LINUX BOOTSTRAP INSTALLER FOR BOLDON OS       ")
print(f" Target path: {ROOT_DIR}")
print(f"=======================================================")

if not os.path.exists(ROOT_DIR):
    print(f"[!] Error: Target directory {ROOT_DIR} does not exist!")
    sys.exit(1)

TMP_DIR = "/tmp/arch_bootstrap"
os.makedirs(TMP_DIR, exist_ok=True)

ARCH_TAR = os.path.join(TMP_DIR, "archlinux-bootstrap.tar.zst")
ARCH_URL = "https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst"

# 1. Скачиваем Arch Linux Bootstrap
if not os.path.exists(ARCH_TAR):
    print("[*] Downloading Arch Linux Bootstrap rootfs (~200MB)...")
    try:
        urllib.request.urlretrieve(ARCH_URL, ARCH_TAR)
        print("  [+] Download finished!")
    except Exception as e:
        print(f"  [!] Failed to download Arch tarball: {e}")
        sys.exit(1)
else:
    print("[*] Found cached Arch Linux tarball.")

# 2. Распаковываем Arch Linux в целевую директорию
print("[*] Extracting Arch Linux rootfs into target ext4 image...")
try:
    # Arch bootstrap архивы содержат внутри папку 'root.x86_64/'
    cmd = f"tar --strip-components=1 -xvf {ARCH_TAR} -C {ROOT_DIR}"
    subprocess.run(cmd, shell=True, check=True)
    print("  [+] Arch Linux rootfs extracted successfully!")
except Exception as e:
    print(f"  [!] Extraction failed: {e}")
    sys.exit(1)

# 3. Страховочный static BusyBox (на случай если glibc запросит неизвестный сискалл)
print("[*] Installing static BusyBox fallback...")
busybox_url = "https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox"
busybox_path = os.path.join(ROOT_DIR, "bin/busybox")
try:
    urllib.request.urlretrieve(busybox_url, busybox_path)
    os.chmod(busybox_path, 0o755)
    print("  [+] Static BusyBox installed to /bin/busybox")
except Exception as e:
    print(f"  [!] Warning: Could not download BusyBox: {e}")

# 4. Настраиваем пользователей (/etc/passwd & /etc/group)
print("[*] Setting up users: root and boldonuser...")
passwd_path = os.path.join(ROOT_DIR, "etc/passwd")
group_path = os.path.join(ROOT_DIR, "etc/group")

# Добавляем boldonuser если его там нет
with open(passwd_path, "a+") as f:
    f.seek(0)
    content = f.read()
    if "boldonuser" not in content:
        f.write("boldonuser:x:1000:1000:Boldon User:/home/boldonuser:/bin/sh\n")

with open(group_path, "a+") as f:
    f.seek(0)
    content = f.read()
    if "boldonuser" not in content:
        f.write("boldonuser:x:1000:\n")

os.makedirs(os.path.join(ROOT_DIR, "home/boldonuser"), exist_ok=True)
print("  [+] Users configured.")

# 5. Создаем утилиту boldonfetch для красивой презентации
print("[*] Creating /usr/bin/boldonfetch...")
boldonfetch_content = """#!/bin/sh
echo "  \033[1;36m  ____   ____  _     ____   ____  _  _   ___  ____  \033[0m"
echo "  \033[1;36m |  _ \\ / __ \\| |   |  _ \\ / __ \\| \\| | / _ \\/ ___| \033[0m"
echo "  \033[1;36m | |_) | |  | | |   | |_) | |  | | .  || | | \\___ \\ \033[0m"
echo "  \033[1;36m |  _ <| |__| | |___|  _ <| |__| | |\\  || |_| |___) |\033[0m"
echo "  \033[1;36m |_| \\_\\\\____/|_____|_| \\_\\\\____/|_| \\_| \\___/|____/ \033[0m"
echo ""
echo "  \033[1;32mOS Base:\033[0m Arch Linux (x86_64 Rootfs)"
echo "  \033[1;32mKernel:\033[0m Custom BoldonOS Monolithic Kernel (C)"
echo "  \033[1;32mFS Engine:\033[0m ext4 (lwext4 integration)"
echo "  \033[1;32mNetworking:\033[0m lwIP TCP/IP + USB-Ethernet"
echo "  \033[1;32mGUI Engine:\033[0m Nano-X Window System + NanoWM"
echo ""
"""
fetch_path = os.path.join(ROOT_DIR, "usr/bin/boldonfetch")
with open(fetch_path, "w") as f:
    f.write(boldonfetch_content)
os.chmod(fetch_path, 0o755)

# 6. Создаем скрипт запуска графики startx
startx_path = os.path.join(ROOT_DIR, "usr/bin/startx")
startx_content = """#!/bin/sh
echo "[+] Starting Nano-X Window Server & NanoWM..."
nano-X &
sleep 1
nanowm &
sleep 1
nxterm &
"""
with open(startx_path, "w") as f:
    f.write(startx_content)
os.chmod(startx_path, 0o755)

# 7. Заглушки бинарников Nano-X (если они еще не скомпилированы)
for bin_name in ["nano-X", "nanowm", "nxterm"]:
    bp = os.path.join(ROOT_DIR, f"usr/bin/{bin_name}")
    if not os.path.exists(bp):
        with open(bp, "w") as f:
            f.write(f"#!/bin/sh\necho '[{bin_name}] Running GUI subsystem...'\nwhile true; do sleep 10; done\n")
        os.chmod(bp, 0o755)

# 8. Создаем главного стартового агента /etc/init.sh
print("[*] Creating /etc/init.sh...")
init_sh_content = """#!/bin/sh

clear
/usr/bin/boldonfetch

echo "======================================================="
echo "  ARCH LINUX ON BOLDON OS - PRESENTATION ENVIRONMENT  "
echo "  Logged in as: boldonuser                             "
echo "======================================================="
echo ""
echo "Available commands:"
echo "  boldonfetch  - Show system status"
echo "  startx       - Launch Graphical Desktop (Nano-X)"
echo "  ls, cd, cat  - Explore Arch Linux filesystem"
echo ""

export HOME=/home/boldonuser
export USER=boldonuser
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PS1='\033[1;33mboldonuser@arch-boldon\033[0m:\033[1;34m\\w\033[0m\$ '

cd /home/boldonuser

# Запускаем интерактивный shell Arch Linux
exec /bin/sh -i
"""
init_path = os.path.join(ROOT_DIR, "etc/init.sh")
with open(init_path, "w") as f:
    f.write(init_sh_content)
os.chmod(init_path, 0o755)

# 9. Создаем приветственный текстовый файл на рабочем столе boldonuser
readme_path = os.path.join(ROOT_DIR, "home/boldonuser/README.txt")
with open(readme_path, "w") as f:
    f.write("Welcome to Arch Linux running on Boldon OS Kernel!\nFeel free to execute standard commands.\n")

print("\n[SUCCESS] Arch Linux rootfs fully deployed and configured at " + ROOT_DIR)