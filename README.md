# BoldonOS
An OS created from scratch by a 13-year-old

## Сборка и запуск

```bash
git pull origin main
sh kernel/get-deps
make clean
make -j"$(nproc)"       # template.iso — только ядро, без корневой ФС
make check-image         # убедиться, что ISO содержит свежее ядро
make usb-image           # main.img — полный диск с корнем ext4 (для реального ПК)
sudo dd if=main.img of=/dev/sdX bs=4M status=progress conv=fsync
```

`template.iso`, лежащий в git — это старый бинарный снимок. `git pull` его не
пересобирает: после любого изменения исходников нужен `make`. Проверить образ
можно командой `make check-image` (ищет маркер `[BOOT] PCI scan`).

`main.img` наполняется из каталога `osdata/` (он в `.gitignore`, поэтому в
репозитории его нет). Минимально там должны быть `bin/sh` и `etc/init.sh`,
иначе ядро смонтирует `C:`, но shell не запустится.

Подробная диагностика загрузки на реальном железе — в
[docs/input-and-hotplug.md](docs/input-and-hotplug.md).
