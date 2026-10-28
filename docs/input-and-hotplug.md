# Устройства ввода, hotplug и диски

## Linux-совместимый ввод

| Путь | Формат |
| --- | --- |
| `/dev/input/event0` | клавиатура, `struct input_event` (EV_KEY / EV_SYN), Linux keycodes |
| `/dev/input/event1` | мышь, `struct input_event` (EV_REL / EV_KEY / EV_SYN) |
| `/dev/input/mice`, `/dev/mouse`, `/dev/psaux`, `/dev/input/mouse0` | mousedev: пакеты PS/2, ImPS/2, ImExPS/2 |
| `/dev/kbd` | сырые scancode Set 1 (для Nano-X и консоли) |
| `/dev/input/mousectl` | скорость курсора |

Поддерживаются ioctl evdev: `EVIOCGVERSION`, `EVIOCGID`, `EVIOCGNAME`,
`EVIOCGBIT`, `EVIOCGKEY`, `EVIOCGLED`, `EVIOCGREP`, `EVIOCSREP`, `EVIOCGRAB`.

Mousedev реализован по образцу `drivers/input/mousedev.c` из Linux:
состояние (протокол, ответы, остаток пакета) хранится **для каждого open()**,
команды подтверждаются байтом `0xFA`, `F3 C8 / F3 64 / F3 50` переключают
в ImPS/2 (4 байта), `F3 C8 / F3 C8 / F3 50` — в ImExPS/2,
`F2` возвращает ID 0/3/4, `FF` — `FA AA 00`, `E9` — статус, `EB` — пакет.
Движение хранится как структурированные дельты, а не как готовые байты:
смена протокола больше не может сдвинуть поток и вызвать «телепорт» курсора.

Nano-X открывает `/dev/input/mice` с `O_RDWR|O_NONBLOCK`, пишет
`F3 C8 F3 64 F3 50`, получает ACK и читает 4-байтные ImPS/2 пакеты.

### Скорость мыши

100 = нормальная скорость (1.0x), допустимо 10..500.

```
echo 150 > /dev/input/mousectl
cat /dev/input/mousectl
```

Либо через ioctl на любом устройстве мыши:

```c
ioctl(fd, 0x4D01, (void *)(intptr_t)150);   /* MOUSE_IOC_SETSPEED */
ioctl(fd, 0x4D02, &value);                  /* MOUSE_IOC_GETSPEED */
```

Масштабирование с дробным остатком: медленная мышь не теряет
одиночные шаги. Оно применяется и к `g_mouse` (курсор GUI), и к mousedev,
и к evdev.

## xHCI

* Событие Transfer Event теперь сопоставляется с конкретным TRB
  (`evt.param == pending_trb_phys`), отчёт копируется в теневую очередь
  из 8 слотов в момент прихода события — гонка между DMA и задачей HID
  и потеря/report-stomping исключены.
* `report_len` вычисляется из residual и ограничивается `report_size`;
  некорректные completion codes больше не превращаются в «отчёт».
* Парсер HID: report ID проверяется строго, короткие/чужие отчёты
  отбрасываются, знаковое расширение работает и для relative-полей с
  `logical_min >= 0` (главная причина броска курсора вправо), дельты
  ограничены ±127.
* Восстановление после halt: сброс endpoint + `CLEAR_FEATURE(ENDPOINT_HALT)`
  на устройстве + переинициализация кольца с корректным cycle bit и
  `SET_TR_DEQUEUE`. Добавлен watchdog 500 мс на зависший transfer.
* Bulk-трансферы больше не отпускают блокировку endpoint до получения
  события; на таймауте восстанавливается TR Dequeue Pointer.

## Диски, mount, umount, fdisk, безопасное извлечение

При подключении диска hotplug-менеджер (`partitions_hotplug_start()`,
задача `hotplug`, опрос 200 мс):

1. читает таблицу разделов (MBR/GPT), а при её отсутствии обрабатывает
   «суперфлоппи» (ФС прямо в LBA 0);
2. создаёт блочные узлы `/dev/<имя>` и Linux-алиасы `/dev/sda`,
   `/dev/sda1`, `/dev/<имя>p1` и т. д.;
3. определяет ФС (FAT32/ext4) и монтирует разделы **через ту же функцию
   `vfs_mount()`, которую использует syscall `mount(2)`**;
   точка монтирования — `/media/<имя узла>`, буква диска выделяется из E..Z;
4. публикует `/proc/partitions`, `/proc/mounts`, `/proc/mountinfo`.

```
mount -t vfat /dev/sda1 /mnt/usb
umount /mnt/usb
fdisk -l /dev/sda
fdisk /dev/sda
cat /proc/partitions
```

Реализованы syscalls `mount` (165), `umount2` (166), `fsync`/`fdatasync`
(74/75), `pread64`/`pwrite64` (17/18). Блочные узлы — `VFS_TYPE_BLOCK_DEV`
с `st_rdev`, `read/write/lseek` и ioctl `BLKGETSIZE`, `BLKGETSIZE64`,
`BLKSSZGET`, `BLKPBSZGET`, `HDIO_GETGEO`, `BLKFLSBUF`, `BLKRRPART`, `BLKPG`
(последние два перечитывают таблицу разделов).

### Безопасное извлечение

`/dev/diskctl` — список дисков и команды:

```
echo "rescan"                     > /dev/diskctl
echo "eject usb0"                 > /dev/diskctl
echo "mount /dev/sda1 /mnt/usb"   > /dev/diskctl
echo "umount /mnt/usb"            > /dev/diskctl
```

`eject` сбрасывает кэш на устройство, размонтирует ФС, удаляет точки
монтирования и узлы разделов. При физическом отключении (xHCI disconnect)
`xhci_destroy_slot()` вызывает `partitions_device_remove()` **до**
`unregister_device()`: ФС размонтируются, записи кэша инвалидируются,
таблица разделов удаляется, devfs-узлы снимаются с регистрации —
обращений к уже исчезнувшему устройству не остаётся.

## Ограничение

lwext4 в дереве использует один общий mount point `/`, поэтому одновременно
может быть смонтирован только один раздел ext4 (обычно корневой `C:`).
Разделы FAT32 на флешках монтируются без ограничений.
