# Qemu SPARC for NeXT

CG14 with modifications for recent Qemu builds

Still needs some work to build

Sparc32 System emulator

Use the executable qemu-system-sparc to simulate the following Sun4m architecture machines:

- SPARCstation 4
- SPARCstation 5
- SPARCstation 10
- **SPARCstation 20 is recommended for NeXTSTEP 3.3**
- SPARCserver 600MP
- SPARCstation LX
- SPARCstation Voyager
- SPARCclassic
- SPARCbook
The emulation is somewhat complete. SMP up to 16 CPUs is supported, but Linux limits the number of usable CPUs to 4.

QEMU emulates the following sun4m peripherals:

- IOMMU
- TCX, cgthree or cgfourteen Frame buffer
- Lance (Am7990) Ethernet
- Non Volatile RAM M48T02/M48T08
- Slave I/O: timers, interrupt controllers, Zilog serial ports, keyboard and power/reset logic
- ESP SCSI controller with hard disk and CD-ROM support
- Floppy drive (not on SS-600MP)
- CS4231 sound device (only on SS-5, not working yet)


The number of peripherals is fixed in the architecture. Maximum memory size depends on the machine type, for SS-5 it is 256MB and for others 2047MB.

Since version 0.8.2, QEMU uses OpenBIOS http://www.openbios.org/. OpenBIOS is a free (GPL v2) portable firmware implementation. The goal is to implement a 100% IEEE 1275-1994 (referred to as Open Firmware) compliant firmware.

A sample Linux 2.6 series kernel and ram disk image are available on the QEMU web site. There are still issues with NetBSD and OpenBSD, but most kernel versions work. Please note that currently older Solaris kernels don’t work probably due to interface issues between OpenBIOS and Solaris.

The following options are specific to the Sparc32 emulation:

    -g WxHx[xDEPTH]
    
Set the initial graphics mode. For TCX, the default is 1024x768x8 with the option of 1024x768x24. For cgthree, the default is 1024x768x8 with the option of 1152x900x8 for people who wish to use OBP.

    -prom-env string

Set OpenBIOS variables in NVRAM, for example:

    qemu-system-sparc -prom-env 'auto-boot?=false' -prom-env 'boot-device=sd(0,2,0):d' -prom-env 'boot-args=linux single'-M [SS-4|SS-5|SS-10|SS-20|SS-600MP|LX|Voyager|SPARCClassic] [|SPARCbook]

Set the emulated machine type. Default is SS-5.

----
## ROMs
