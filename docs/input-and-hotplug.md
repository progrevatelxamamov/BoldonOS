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

## Диагностика зависания на реальном железе

Если загрузка останавливается после `[XHCI] Controller Initialized`, соберите
последние строки экрана — теперь на каждом этапе есть маркер:

```
[BOOT] PCI scan...
[XHCI] probing controller <bus>:<slot>:<func>
[XHCI] waiting for ports (<N>)
[XHCI] port <N> connected
[USB] device VID=0x... PID=0x...
[XHCI] Controller Initialized
[BOOT] PCI scan done
[BOOT] partitions scan...
[BOOT] partitions scan done
[BOOT] mounting filesystems...
[BOOT] filesystems mounted
[BOOT] devfs ready
[HOTPLUG] disks found: <N>
[BOOT] hotplug started
[BOOT] input ready
[BOOT] enabling interrupts
[BOOT] interrupts enabled, scheduler running
```

Что сделано для защиты от «вечного» ожидания железа:

* В `xhci_init()` проверяются BAR0/CAPLENGTH/HCIVERSION, а ожидание HCRST
  ограничено; неотвечающий контроллер пропускается с сообщением, а не вешает ядро.
* Все спинлоки xHCI (`evt_lock`, `ep_lock`, `ep0_lock`, `cmd_exec_lock`,
  `report_lock`) получили таймауты и периодический `schedule_yield()`:
  взаимная блокировка задач больше не останавливает систему.
* Перечисление USB до `sti` имеет бюджет в 40 таймаутов
  (`[XHCI] boot timeout budget exhausted, deferring to hotplug`). Исчерпавшие
  бюджет устройства перечисляются повторно задачей hotplug после включения
  прерываний — загрузка всегда продолжается.
* `xhci_register_controller()` вызывается до перечисления портов; повторные
  попытки hotplug для одного порта ограничены одной попыткой в секунду.
* Задержки в `init_usb_mass_storage()` уменьшены на два порядка
  (было 150 млн итераций `pause` на каждую попытку INQUIRY).
* В `nvme_init()` исправлено чтение 64-битного BAR (раньше старшая половина
  бралась из BAR1 даже для 32-битного BAR), добавлены таймауты ожидания
  CSTS.RDY/CSTS.CFS, таймауты completion queue уменьшены до 20 млн итераций,
  исключён бесконечный цикл при нулевом размере сектора.

Обратите внимание: в дереве нет драйвера AHCI/SATA, поэтому загрузочный диск
на реальном ПК должен быть USB (или NVMe). Устройства за внешним USB-хабом
пока не перечисляются — если флешка/мышь/клавиатура висят на хабе, их нужно
воткнуть напрямую в порт материнской платы.

## Загрузка до shell на реальном ПК

Симптом: экран доходит до `[MOUNT] Boot Drive C: NOT MOUNTED (FAIL)` и
`[INIT] Failed to start shell`, после чего «зависает». Это не зависание ядра —
ядро работает, но не нашло корневую файловую систему, поэтому некому запустить
`/bin/sh`.

Причины и что делать по порядку.

### 1. Проверьте, что загружен свежий образ

В репозитории лежит готовый `template.iso`, но это **старый бинарный снимок**:
`git pull` обновляет исходники, а не пересобирает ISO. Новое ядро печатает
маркеры `[BOOT] ...`, `[XHCI] probing controller`, `[XHCI] port N connected`.
Если их на экране нет — грузится старое ядро.

```bash
git pull origin main
sh kernel/get-deps
make clean
make -j"$(nproc)"
make check-image
```

`make check-image` проверяет наличие строки `[BOOT] PCI scan` внутри
`kernel/bin/kernel` и `template.iso`. Если проверка ругается — удалите
`template.iso` и повторите сборку. Для сборки нужны `nasm`, `xorriso`, `curl`,
`cc`; для `main.img` ещё `sgdisk`, `mkfs.fat`, `mkfs.ext4`, `mtools`, `sudo`.

### 2. Грузитесь с `main.img`, а не с `template.iso`

`template.iso` содержит только ядро, Limine и заставку. В нём **нет корневой
файловой системы** — `/etc/init.sh` и `/bin/sh` там отсутствуют по определению.
Полный загрузочный диск собирается отдельно:

```bash
make usb-image
sudo dd if=main.img of=/dev/sdX bs=4M status=progress conv=fsync
```

`main.img` — GPT-образ на 1 ГиБ: BIOS Boot, EFI System (FAT32) и `BOLDON_ROOT`
(ext4). Корень ext4 наполняется из каталога `osdata/`, который намеренно
исключён из git (`.gitignore`). Минимально в нём должны быть `bin/sh` и
`etc/init.sh`; без них даже смонтированный `C:` не даст shell.
`make usb-image` и `make check-osdata` предупреждают об отсутствии этих файлов.

Если вы записывали `template.iso` на флешку через Rufus/dd, ядро видит
ISO9660/защитный MBR, не находит ни FAT32, ни ext4 и печатает
`[DEBUG] Total valid candidates found: 0`.

`limine.conf` содержит `boot_serial=D291F02E-...` — это GUID раздела из
исходного `main.img`. При пересоздании образа `sgdisk` генерирует новые GUID,
поэтому совпадения не будет; это не критично, потому что
`mount_all_partitions()` и `partitions_try_mount_root()` имеют запасные сценарии
(любой ext4, затем любой найденный раздел). Если разделов ext4 несколько,
обновите GUID командой `sgdisk -i 3 main.img`.

### 3. Новые диагностические маркеры

После пересборки экран говорит сам, где именно проблема:

```
[DISK] usb0 sectors=... bps=512
[DISK] usb0 partitions=3
[DISK] block devices at boot: 1
[XHCI] port 3 connected
[USB] device VID=0x... PID=0x...
[MSD] Init USB Mass Storage...
[MSD] Registered successfully as usb0
[XHCI] devices connected on this controller: 2
[ROOT] C: mounted from usb0 partition 3 (ext4)
[HOTPLUG] root filesystem became available as C:
[INIT] Starting /etc/init.sh...
```

| Маркер | Что означает |
|---|---|
| `[DISK] block devices at boot: 0` | USB-флешка не определена как накопитель |
| `[XHCI] devices connected on this controller: 0` | контроллер не видит ни одного порта с устройством |
| `[USB] ... failed: address device rejected` | устройство не отвечает на перечисление |
| `[MSD] SCSI INQUIRY failed` | флешка есть, но SCSI-команды не проходят |
| `[DISK] usb0 partitions=0` | диск виден, но таблица разделов не читается |
| `[INIT] root filesystem C: is still not mounted` | ждём диск; hotplug повторяет попытку каждые 2 с |
| `[INIT] C: is mounted but /bin/sh or /etc/init.sh is missing` | диск смонтирован, но `osdata/` пуст |

### 4. Если диски всё ещё не видны

* В BIOS включите **xHCI Hand-off** и **EHCI Hand-off**, а режим xHCI поставьте
  в `Enabled`, не `Auto`/`Smart Auto`. На платах времён Windows 7 USB 2.0 порты
  часто остаются за EHCI, и xHCI их не видит — тогда будет
  `[XHCI] no ports report a device`.
* Вставьте флешку напрямую в порт материнской платы (хабы не поддерживаются).
* SATA-диск с Windows не определяется: драйвера AHCI в ядре нет. Это ожидаемо.
* Два `[XHCI] Controller Initialized` — это два контроллера на плате, норма.

### 5. Позднее появление диска

Перечисление USB до `sti` ограничено бюджетом таймаутов, поэтому накопитель
может появиться уже после включения прерываний. Раньше такой диск монтировался
только в `/media/...`, а `C:` оставался пустым и init падал. Теперь hotplug при
появлении нового диска и дополнительно раз в 2 секунды пытается смонтировать
корень через `partitions_try_mount_root()`, а задача init ждёт `C:` вместо того,
чтобы сразу завершиться ошибкой. Если вставить флешку позже, система
продолжит загрузку сама.

### Остановка на `waiting for ports (4)`

Ранее при отсутствии подключённых устройств выполнялось до 30 миллионов
проходов по MMIO-регистрам всех портов. На старом контроллере это давало
огромную задержку даже при пустых портах. Теперь этот цикл удалён: загрузка
сразу проверяет порты один раз, а поздние подключения обнаруживает существующая
задача `xhci-hotplug`, пересканирующая порты после запуска планировщика.
После `waiting for ports` выводится `scanning ports now; late connections
handled by hotplug`. Это устраняет длительный опрос, но не подтверждает
работоспособность конкретного контроллера без проверки на реальном ПК.

## Исправление регрессий ImPS/2 и таймаутов

В upstream Microwindows `src/drivers/mou_devmice.c` проверяет ответ на
шестибайтовую последовательность ImPS/2 как `0xF4`. Это не ACK: ACK имеет
значение `0xFA`, а `0xF4` — команда включения передачи данных. Предыдущее
описание проверки Nano-X как ожидающей `0xFA` было неточным. Новый mousedev
возвращает `0xFA`, поэтому такой Nano-X печатает `Failed to switch to ImPS/2`,
даже когда режим уже переключился. Не следует возвращать неправильный ACK
в ядре ради подавления сообщения.

Для исходного дерева Microwindows, содержащего именно эту проверку:

```bash
cd /path/to/microwindows
git apply /path/to/BoldonOS/patches/microwindows-mousedev-ack.patch
```

После этого пересоберите Nano-X и обновите его бинарник в `osdata/` и на
загрузочном диске. Изменение ядра само по себе не обновляет Nano-X. Если в
вашей версии уже проверяется `0xFA`, этот патч не нужен и причину ошибки
необходимо искать отдельно. Источник проверки:
https://github.com/ghaerr/microwindows/blob/master/src/drivers/mou_devmice.c

В аппаратном PS/2-парсере устранено отбрасывание байтов X с установленными
битами 6/7: это допустимые данные, а не признаки рассинхронизации. Знак X/Y
теперь берётся из заголовка пакета (9-битное PS/2-смещение). Дренирование
контроллера ограничено также количеством прочитанных байтов, чтобы движение
мыши не продлевало его бесконечно. Ошибки записи команд мыши возвращаются
вызывающему коду; сброс состояния больше не включает неинициализированный
поток.

Ожидание событий xHCI теперь проверяет HPET, работающий до `sti`: 100 мс на
ожидание при загрузке, 1000 мс после неё, суммарный бюджет ожиданий при
перечислении одного контроллера — 750 мс. Если HPET отсутствует, остаётся
ограничение итераций (при загрузке снижено до 200 тысяч). Это не жёсткая
граница времени всей загрузки: остальные операции и сами MMIO-доступы могут
занимать дополнительное время. Повторное обнаружение портов и подавление
диагностики хранятся отдельно для каждого контроллера, а не совместно для
портов разных контроллеров с одинаковыми номерами.

Проверки извлекают фактические функции из исходников и запускают host-тесты:

```bash
python3 tests/test_mouse_protocols.py
```

Они проверяют ImPS/2/Explorer ID и ACK, отрицательные и большие положительные
PS/2-смещения, дедлайны ожидания xHCI и резервный путь без HPET. Это не заменяет
проверку на реальном оборудовании и не подтверждает устранение сбоя старого ПК.
