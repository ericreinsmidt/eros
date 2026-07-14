IMAGE := ghcr.io/loveretro/tg5040-toolchain:latest
BRICK ?= 192.168.1.100
# The device has no scp/sftp binary; push files with tar over ssh.
SSH := sshpass -p 'tina' ssh -o StrictHostKeyChecking=no root@$(BRICK)

.PHONY: all clean deploy run kill payload tools minarch

all: build/eros.elf

# One container build produces the launcher plus the boot-brightness helper and
# the BT-switch reader. Both ship in the payload, so a clean `make` generates them.
build/eros.elf: src/*.c src/*.h tools/setbright.c tools/swread.c mk/cross.mk
	docker run --rm -v $(CURDIR):/work -w /work $(IMAGE) \
		/bin/bash -c 'source ~/.bashrc && make -f mk/cross.mk build/eros.elf build/setbright build/swread'

# Cross-build the dev input tools (evinject/evread) + setbright into build/.
tools:
	docker run --rm -v $(CURDIR):/work -w /work $(IMAGE) \
		/bin/bash -c 'source ~/.bashrc && make -f mk/cross.mk tools'

clean:
	rm -rf build

# Push the binary + configs to a device already carrying the payload tree.
deploy: build/eros.elf
	$(SSH) 'mkdir -p /mnt/SDCARD/eros/icons'
	tar -cf - -C build eros.elf swread -C ../config systems.cfg eros.cfg \
	    -C ../sd/eros launch.sh -C ../res/fonts menu.ttf | $(SSH) 'tar -xf - -C /mnt/SDCARD/eros'
	tar -cf - -C res/icons . | $(SSH) 'tar -xf - -C /mnt/SDCARD/eros/icons'

# Push the from-source minarch (EROS-branded/restyled menu) + its font.
deploy-minarch: vendor/minarch.elf res/fonts/menu.ttf
	tar -cf - -C vendor minarch.elf -C ../res/fonts menu.ttf | $(SSH) 'tar -xf - -C /mnt/SDCARD/eros && chmod +x /mnt/SDCARD/eros/minarch.elf'

payload:
	./mk/payload.sh

minarch:
	./minarch/build.sh
