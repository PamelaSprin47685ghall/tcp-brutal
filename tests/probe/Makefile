# Probe-only build; nothing here ships and tests/probe is not in SRCS.
KDIR ?= $(HOME)/.cache/tbtun-build/linux72-headers/usr/lib/modules/7.2.9-1-MANJARO/build
PAHOLE ?= $(HOME)/.cache/tbtun-build/pahole/usr/bin/pahole
obj-m += tbt_probe.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) PAHOLE=$(PAHOLE) modules
	$(CC) -std=gnu99 -O2 -Wall -Wextra -Werror -o probe_user probe_user.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) probe_user probe_user_san
