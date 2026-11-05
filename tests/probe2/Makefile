# Probe-only build; not in SRCS, not in the DKMS tarball.
KDIR ?= $(HOME)/.cache/tbtun-build/linux72-headers/usr/lib/modules/7.2.9-1-MANJARO/build
PAHOLE ?= $(HOME)/.cache/tbtun-build/pahole/usr/bin/pahole
obj-m += tbt_probe2.o

# Builds only this module. M=$(CURDIR) is passed explicitly because an exported
# M in the environment (a parent make that set it for the main module) would
# otherwise redirect kbuild at the repository and compile tbtun/ by accident.
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) PAHOLE=$(PAHOLE) modules
	$(CC) -std=gnu99 -O2 -Wall -Wextra -Werror -o probe2_user probe2_user.c

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(RM) probe2_user
