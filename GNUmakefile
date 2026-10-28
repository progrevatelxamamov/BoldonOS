# Nuke built-in rules.
.SUFFIXES:

# Delete the target of a failed recipe.
.DELETE_ON_ERROR:

# Default user QEMU flags. These are appended to the QEMU command calls.
# -drive file=nvme.img,if=none,id=nvme0,format=raw -device nvme,drive=nvme0,serial=deadbeef 
# -d int,cpu_reset -no-reboot -no-shutdown \

QEMUFLAGS := -m 8G -smp 4 \
	-serial stdio \
	-no-reboot -no-shutdown \
	-device qemu-xhci,id=xhci \
	-device usb-mouse,bus=xhci.0 \
	-drive file=main.img,if=none,id=usbstick,format=raw \
	-device usb-storage,drive=usbstick,bus=xhci.0 \
	-device usb-net,netdev=net0 -netdev user,id=net0

# Internal QEMU flags that should not be changed by the user.
override QEMU_MACHINE_FLAGS := \
    -M q35
override QEMU_UEFI_FLAGS := \
    -drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-x86_64.fd,readonly=on

override IMAGE_NAME := template

# User controllable size of the HDD image, in MiB.
HDD_SIZE := 64

# Internal HDD geometry that should not be changed by the user. Older mtools
# require one; 64 heads of 32 sectors make a cylinder exactly 1 MiB in size.
override HDD_HEADS := 64
override HDD_SECTORS_PER_TRACK := 32
override HDD_CYLINDER_SECTORS := $(shell echo $$(( $(HDD_HEADS) * $(HDD_SECTORS_PER_TRACK) )))

# Internal HDD partition layout that should not be changed by the user. sgdisk
# lays the partition out as GPT before converting the table to MBR, so the
# first and last cylinders are left to the GPT structures.
override HDD_PART_START := $(HDD_CYLINDER_SECTORS)
override HDD_PART_SECTORS := $(shell echo $$(( ($(HDD_SIZE) - 2) * $(HDD_CYLINDER_SECTORS) )))
override HDD_PART_END := $(shell echo $$(( $(HDD_PART_START) + $(HDD_PART_SECTORS) - 1 )))
override HDD_PART_OFFSET := $(shell echo $$(( $(HDD_PART_START) * 512 )))

# Toolchain for building the 'limine' executable for the host.
HOST_CC := cc
HOST_CFLAGS := -g -O2 -pipe
HOST_CPPFLAGS :=
HOST_LDFLAGS :=
HOST_LIBS :=

.PHONY: all
all: $(IMAGE_NAME).iso

.PHONY: all-hdd
all-hdd: $(IMAGE_NAME).hdd

.PHONY: run
run: $(IMAGE_NAME).iso
	qemu-system-x86_64 \
		$(QEMU_MACHINE_FLAGS) \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS)

.PHONY: run-uefi
run-uefi: edk2-ovmf-bins $(IMAGE_NAME).iso
	qemu-system-x86_64 \
		$(QEMU_MACHINE_FLAGS) \
		$(QEMU_UEFI_FLAGS) \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS)

.PHONY: run-hdd
run-hdd: $(IMAGE_NAME).hdd
	qemu-system-x86_64 \
		$(QEMU_MACHINE_FLAGS) \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

.PHONY: run-hdd-uefi
run-hdd-uefi: edk2-ovmf-bins $(IMAGE_NAME).hdd
	qemu-system-x86_64 \
		$(QEMU_MACHINE_FLAGS) \
		$(QEMU_UEFI_FLAGS) \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

# Путь до EFI-раздела (там, где лежит limine.conf).
# Поменяйте, если у вас он смонтирован в другое место.
EFI_DIR := /boot

.PHONY: run-real-pc
run-real-pc: kernel
	sudo cp -v kernel/bin/kernel $(EFI_DIR)/boldonoskernel
	@echo
	@echo "Ядро скопировано в $(EFI_DIR)/boldonoskernel"
	@echo "Убедитесь, что в limine.conf есть запись:"
	@echo "  /BoldonOS"
	@echo "      protocol: limine"
	@echo "      path: boot():/boldonoskernel"
	@echo
	@echo "Перезагрузитесь и выберите BoldonOS в меню Limine."

.INTERMEDIATE: edk2-ovmf-bins.tar.gz
edk2-ovmf-bins.tar.gz:
	curl -fL -o $@ https://github.com/osdev0/edk2-ovmf-stable-bins/releases/latest/download/edk2-ovmf-bins.tar.gz

edk2-ovmf-bins: edk2-ovmf-bins.tar.gz
	rm -rf edk2-ovmf-bins
	gunzip < edk2-ovmf-bins.tar.gz | tar -xf -

.INTERMEDIATE: limine-binary.tar.gz
limine-binary.tar.gz:
	curl -fL -o $@ https://github.com/Limine-Bootloader/Limine/releases/latest/download/limine-binary.tar.gz

limine-binary/limine: limine-binary.tar.gz
	rm -rf limine-binary
	gunzip < limine-binary.tar.gz | tar -xf -
	$(MAKE) -C limine-binary \
		CC="$(HOST_CC)" \
		CFLAGS="$(HOST_CFLAGS)" \
		CPPFLAGS="$(HOST_CPPFLAGS)" \
		LDFLAGS="$(HOST_LDFLAGS)" \
		LIBS="$(HOST_LIBS)"

kernel/.deps-obtained:
	./kernel/get-deps

.PHONY: kernel
kernel: kernel/.deps-obtained
	$(MAKE) -C kernel

.PHONY: check-image
check-image:
	@if [ ! -f kernel/bin/kernel ]; then \
		echo "ERROR: kernel/bin/kernel is missing. Run: make -j$$(nproc)"; exit 1; \
	fi
	@if ! grep -aq '\[BOOT\] PCI scan' kernel/bin/kernel; then \
		echo "ERROR: kernel/bin/kernel is a stale build without [BOOT] markers."; \
		echo "Run: make clean && make -j$$(nproc)"; exit 1; \
	fi
	@if [ -f template.iso ] && ! grep -aq '\[BOOT\] PCI scan' template.iso; then \
		echo "ERROR: template.iso contains an OLD kernel."; \
		echo "Run: rm -f template.iso && make -j$$(nproc)"; exit 1; \
	fi
	@echo "OK: kernel and image contain the current boot diagnostics"

.PHONY: check-osdata
check-osdata:
	@if [ ! -d osdata ]; then \
		echo "ERROR: osdata/ is missing."; \
		echo "The root filesystem must contain bin/sh and etc/init.sh."; \
		exit 1; \
	fi
	@if [ ! -f osdata/etc/init.sh ]; then \
		echo "WARNING: osdata/etc/init.sh is missing - init will fail"; \
	fi
	@if [ ! -f osdata/bin/sh ]; then \
		echo "WARNING: osdata/bin/sh is missing - init will fail"; \
	fi

.PHONY: usb-image
usb-image:
	@$(MAKE) check-osdata
	@$(MAKE) main.img
	@$(MAKE) check-image
	@echo ""
	@echo "Write main.img to a USB flash drive (replace sdX with the real device):"
	@echo "  sudo dd if=main.img of=/dev/sdX bs=4M status=progress conv=fsync"
	@echo "Then boot from that USB stick in BIOS or UEFI mode."

$(IMAGE_NAME).iso: limine-binary/limine kernel
	rm -rf iso_root
	mkdir -p iso_root/boot
	cp -v boldonsplash.bmp iso_root/boot/
	cp -v kernel/bin/kernel iso_root/boot/
	mkdir -p iso_root/boot/limine
	cp -v limine.conf limine-binary/limine-bios.sys limine-binary/limine-bios-cd.bin limine-binary/limine-uefi-cd.bin iso_root/boot/limine/
	mkdir -p iso_root/EFI/BOOT
	cp -v limine-binary/BOOTX64.EFI iso_root/EFI/BOOT/
	cp -v limine-binary/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
	./limine-binary/limine bios-install $(IMAGE_NAME).iso
	rm -rf iso_root

$(IMAGE_NAME).hdd: limine-binary/limine kernel
	rm -f $(IMAGE_NAME).hdd
	dd if=/dev/zero bs=1024k count=0 seek=$(HDD_SIZE) of=$(IMAGE_NAME).hdd
	PATH=$$PATH:/usr/sbin:/sbin sgdisk $(IMAGE_NAME).hdd -n 1:$(HDD_PART_START):$(HDD_PART_END) -t 1:ef00 -m 1
	./limine-binary/limine bios-install $(IMAGE_NAME).hdd
	mformat -i $(IMAGE_NAME).hdd@@$(HDD_PART_OFFSET) -T $(HDD_PART_SECTORS) -h $(HDD_HEADS) -s $(HDD_SECTORS_PER_TRACK) ::
	mmd -i $(IMAGE_NAME).hdd@@$(HDD_PART_OFFSET) ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $(IMAGE_NAME).hdd@@$(HDD_PART_OFFSET) kernel/bin/kernel ::/boot
	mcopy -i $(IMAGE_NAME).hdd@@$(HDD_PART_OFFSET) limine.conf limine-binary/limine-bios.sys ::/boot/limine
	mcopy -i $(IMAGE_NAME).hdd@@$(HDD_PART_OFFSET) limine-binary/BOOTX64.EFI ::/EFI/BOOT
	mcopy -i $(IMAGE_NAME).hdd@@$(HDD_PART_OFFSET) limine-binary/BOOTIA32.EFI ::/EFI/BOOT

.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).hdd main.img

.PHONY: distclean
distclean: clean
	$(MAKE) -C kernel distclean
	rm -rf limine-binary limine-binary.tar.gz edk2-ovmf-bins edk2-ovmf-bins.tar.gz

MAIN_SIZE_MIB := 1024

main.img: limine-binary/limine kernel
	@if [ ! -d osdata ]; then echo "WARNING: osdata/ is missing - the ext4 root will be empty and /etc/init.sh will not exist"; fi
	@echo "==> Создаём пустой GPT образ $(MAIN_SIZE_MIB) МиБ"
	rm -f main.img
	dd if=/dev/zero bs=1M count=$(MAIN_SIZE_MIB) of=main.img status=none

	@echo "==> Разметка GPT (p1: BIOS Boot, p2: EFI System, p3: BOLDON_ROOT)"
	# 1. Создаем пустую таблицу GPT
	sgdisk -o main.img
	# 2. Раздел 1: BIOS Boot Partition (тип ef02, 1Мб) - нужен для limine bios-install
	sgdisk -n 1:2048:+1M -t 1:ef02 -c 1:"BIOSBOOT" main.img
	# 3. Раздел 2: EFI System Partition (тип ef00, 128Мб) - для UEFI загрузки и ядра
	sgdisk -n 2:0:+128M -t 2:ef00 -c 2:"BOOT" main.img
	# 4. Раздел 3: Системный раздел Ext4 (тип 8300, все оставшееся место)
	sgdisk -n 3:0:0 -t 3:8300 -c 3:"BOLDON_ROOT" main.img

	@echo "==> Форматируем разделы p2 (FAT32) и p3 (Ext4)"
	@LOOP_DEV=$$(sudo losetup -f --show -P main.img); \
	echo "    Loop device: $$LOOP_DEV"; \
	sudo mkfs.fat -F 32 -n "BOOT" $${LOOP_DEV}p2; \
	sudo mkfs.ext4 -F -L BOLDON_ROOT -b 4096 $${LOOP_DEV}p3; \
	mkdir -p .mnt_boot .mnt_root; \
	sudo mount $${LOOP_DEV}p2 .mnt_boot; \
	sudo mount $${LOOP_DEV}p3 .mnt_root; \
	sudo mkdir -p .mnt_boot/boot/limine .mnt_boot/EFI/BOOT; \
	sudo cp -v boldonsplash.bmp .mnt_boot/boot/ 2>/dev/null || true; \
	sudo cp -v kernel/bin/kernel .mnt_boot/boot/; \
	sudo cp -v limine.conf limine-binary/limine-bios.sys .mnt_boot/boot/limine/; \
	sudo cp -v limine-binary/BOOTX64.EFI .mnt_boot/EFI/BOOT/; \
	sudo cp -v limine-binary/BOOTIA32.EFI .mnt_boot/EFI/BOOT/ 2>/dev/null || true; \
	if [ -d osdata ] && [ "$$(ls -A osdata 2>/dev/null)" ]; then \
		sudo cp -a osdata/. .mnt_root/; \
	fi; \
	sync; \
	sudo umount .mnt_boot .mnt_root; \
	rmdir .mnt_boot .mnt_root; \
	sudo losetup -d $$LOOP_DEV

	@echo "==> Устанавливаем BIOS Limine в BIOS Boot Partition"
	./limine-binary/limine bios-install main.img

.PHONY: run-main
run-main: main.img
	qemu-system-x86_64 \
		$(QEMU_MACHINE_FLAGS) \
		$(QEMUFLAGS)

.PHONY: run-main-uefi
run-main-uefi: edk2-ovmf-bins main.img
	qemu-system-x86_64 \
		$(QEMU_MACHINE_FLAGS) \
		$(QEMU_UEFI_FLAGS) \
		$(QEMUFLAGS)