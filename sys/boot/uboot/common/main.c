/*-
 * Copyright (c) 2000 Benno Rice <benno@jeamland.net>
 * Copyright (c) 2000 Stephane Potvin <sepotvin@videotron.ca>
 * Copyright (c) 2007-2008 Semihalf, Rafal Jaworowski <raj@semihalf.com>
 * Copyright (c) 2016 Vladimir Belian <fate10@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");
#include <sys/param.h>

#include <stand.h>

#include "api_public.h"
#include "bootstrap.h"
#include "glue.h"
#include "libuboot.h"

#ifdef LOADER_ZFS_SUPPORT
#include <libzfs.h>
#endif

#ifndef nitems
#define	nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#endif

#ifdef DEBUG
#define	PRINT_DEBUG_INFO()	do { \
	printf("signature:\n"); \
	printf("  version\t= %d\n", sig->version); \
	printf("  checksum\t= 0x%08x\n", sig->checksum); \
	printf("  sc entry\t= 0x%08x\n", sig->syscall); \
	printf("\naddresses info:\n"); \
	printf(" _etext (sdata)   = 0x%08x\n", (uint32_t)_etext); \
	printf(" _edata           = 0x%08x\n", (uint32_t)_edata); \
	printf(" __sbss_start     = 0x%08x\n", (uint32_t)__sbss_start); \
	printf(" __sbss_end       = 0x%08x\n", (uint32_t)__sbss_end); \
	printf(" __bss_start      = 0x%08x\n", (uint32_t)__bss_start); \
	printf(" _end             = 0x%08x\n", (uint32_t)_end); \
	printf(" syscall entry    = 0x%08x\n", (uint32_t)syscall_ptr); \
	printf(" uboot_heap_start = 0x%08x\n", (uint32_t)uboot_heap_start); \
	printf(" uboot_heap_end   = 0x%08x\n", (uint32_t)uboot_heap_end); } while (0)
#else
#define	PRINT_DEBUG_INFO()
#endif

struct arch_switch archsw;		/* MI/MD interface boundary */

uintptr_t uboot_heap_start;
uintptr_t uboot_heap_end;

extern char end[];
extern char bootprog_name[];
extern char bootprog_rev[];
extern char bootprog_date[];
extern char bootprog_maker[];

extern unsigned char _etext[];
extern unsigned char _edata[];
extern unsigned char __bss_start[];
extern unsigned char __sbss_start[];
extern unsigned char __sbss_end[];
extern unsigned char _end[];

#ifdef LOADER_FDT_SUPPORT
extern int command_fdt_internal(int argc, char *argv[]);
#endif

#ifdef LOADER_ZFS_SUPPORT
static uint64_t zfs_pools[UB_MAX_DEV];

static void
uboot_zfs_probe(void)
{
	char dname[SPECNAMELEN + 1];
	uint64_t id;
	int p, n = 0, i = 0, maxp = 0;

	bzero(&zfs_pools, sizeof(zfs_pools));

	while (ub_dev_get(DEV_TYP_STOR, &i) != NULL) {
		snprintf(dname, sizeof(dname), "disk%d:", n++);
		id = 0;
		if (zfs_probe_dev(dname, &id) == 0) {
			if (id == 0)
				continue;
				
			/* skip pool guid duplicates if found */
			for (p = 0; (p < maxp) && (zfs_pools[p] != id); p++) ;
			if (p < maxp)
				continue;

			zfs_pools[maxp++] = id;
		}
	}
}

static const char *
probe_zfs(const char *dstr)
{
	struct zfs_devdesc dev;
	char *p = NULL, *buf;
	int i, l;

	if ((dstr != NULL) && (strncasecmp(dstr, "zfs", 3) != 0))
		return (NULL);

	if ((dstr != NULL) && ((dstr = strchr(dstr, ' ')) != NULL))
		dstr++;

	for (i = 0; zfs_pools[i] != 0; i++) {
		bzero(&dev, sizeof(dev));
		dev.d_dev = &zfs_dev;
		dev.d_type = dev.d_dev->dv_type;
		dev.pool_guid = zfs_pools[i];
		buf = zfs_fmtdev(&dev);

		p = strchr(buf, ':') + 1;
		l = strcspn(p, ":/");
		if ((buf != NULL) && ((dstr == NULL) || (*dstr == '\0') ||
			((l == strlen(dstr)) && (strncmp(dstr, p, l) == 0))))
			break;
		buf = NULL;
	}
	if (buf != NULL) {
		init_zfs_bootenv(buf);
		return (zfs_fmtdev(&dev));
	}
	printf("could not find %s zfs pool\n",
		(((dstr == NULL) || (*dstr == '\0'))) ? "any" : dstr);
	return (NULL);
}
#endif

static uint64_t
memsize(struct sys_info *si, int flags)
{
	uint64_t size;
	int i;

	size = 0;
	for (i = 0; i < si->mr_no; i++)
		if (si->mr[i].flags == flags && si->mr[i].size)
			size += (si->mr[i].size);

	return (size);
}

static void
meminfo(void)
{
	uint64_t size;
	struct sys_info *si;
	int t[3] = { MR_ATTR_DRAM, MR_ATTR_FLASH, MR_ATTR_SRAM };
	int i;

	if ((si = ub_get_sys_info()) == NULL)
		panic("could not retrieve system info");

	for (i = 0; i < 3; i++) {
		size = memsize(si, t[i]);
		if (size > 0)
			printf("%s: %juMB\n", ub_mem_type(t[i]),
			        (uintmax_t)(size / 1024 / 1024));
	}
}

static struct devsw *
search_devsw(int type)
{
	int i;

	for (i = 0; devsw[i] != NULL; i++) 
		if (devsw[i]->dv_type == type)
			return (devsw[i]);
	return (NULL);
}

/*
 * Parse a device string into unit, slice and partition numbers.
 *
 * The returned values for slice and partition are interpreted by
 * disk_open().
 *
 * Valid device strings:
 *
 * <unit>
 * <unit>:
 * <unit>:<slice>
 * <unit>:<slice>.
 * <unit>:<slice>.<partition>
 *
 * Slice numbers are 1-based.  0 is a wildcard.
 */
static int
decode_devstr(const char *dstr, int *unit, int *slice, int *partition)
{
	const char *p;
	char *endp;

	/* Ignore optional spaces after the device name. */
	while (*dstr == ' ')
		dstr++;

	/* No unit number present. */
	if (*dstr == '\0') {
		return (-1);
	}
	/* Malformed unit number. */
	if (!isdigit(*dstr))
		return (0);

	/* Guaranteed to extract a number from the string, as *dstr is a digit. */
	*unit = strtol(dstr, &endp, 10);
	dstr = endp;

	/* Unit number and nothing else. */
	if (*dstr == '\0')
		return (1);

	/* Device string is malformed beyond unit number. */
	if (*dstr != ':')
		return (0);
	dstr++;

	/* No slice and partition specification. */
	if (*dstr == '\0')
		return (1);

	*slice = strtoul(dstr, &endp, 10);

	/* Malformed slice number. */
	if (dstr == endp)
		return (0);
	dstr = endp;
	
	/* No partition specification. */
	if (*dstr == '\0')
		return (1);

	/* Device string is malformed beyond slice number. */
	if (*dstr != '.')
		return (0);
	dstr++;

	/* No partition specification. */
	if (*dstr == '\0')
		return (1);

	*partition = strtol(dstr, &endp, 10);

	/* Full, valid device string. */
	if (*endp == '\0')
		return (1);

	/* Junk beyond partition number. */
	return (0);
} 

static void
print_disk_probe_info(struct uboot_devdesc dev)
{

	printf("  Checking unit=%d ", dev.d_unit);
	
	if (dev.d_disk.slice > 0)
		printf("slice=%d ", dev.d_disk.slice);
	else
		printf("slice=<auto> ");
	if (dev.d_disk.partition >= 0)
		printf("partition=%d...", dev.d_disk.partition);
	else
		printf("partition=<auto>...");
}

static const char *
probe_disks(const char *dstr)
{
	struct device_info *di;
	struct uboot_devdesc dev;
	struct open_file f;
	int stlist[] = {
		DT_STOR_IDE, DT_STOR_SCSI, DT_STOR_USB, DT_STOR_MMC,
		DT_STOR_SATA
	};
	int type = DEV_TYP_STOR;
	int nunits = 0, res = -1, valid = -1;
	int i, v;
	char *s;

	bzero(&dev, sizeof(dev));

	if ((dev.d_dev = search_devsw(DEVT_DISK)) == NULL)
		return (NULL);

	/* parse devstr if any */
	if (dstr != NULL) {
		for (i = 0; i < nitems(stlist); i++) {
			v = strlen(s = ub_stor_type(stlist[i]));
			if (strncasecmp(dstr, s, v) == 0) {
				type = stlist[i];
				break;
			}
		}
		/* if device type is none above may be it's genereal disk type */
		if ((strncasecmp(dstr, "disk", 4) != 0) && (type == DEV_TYP_STOR))
			return (NULL);
		if ((res = decode_devstr(&dstr[v], &dev.d_unit,
			&dev.d_disk.slice, &dev.d_disk.partition)) == 0)
			return (NULL);
	}
	/* calculate real unit address, offset and number of units */
	for (v = i = 0; (di = ub_dev_get(DEV_TYP_STOR, &i)) != NULL; v++)
		if ((di->type & type) && (nunits++ == dev.d_unit))
			valid = dev.d_unit += v;
	if (valid == -1)
		return (NULL);

	printf("Found U-Boot device: %s\n", dev.d_dev->dv_name);
	dev.d_type = dev.d_dev->dv_type;
	dev.d_disk.partition = -1;
	f.f_devdata = &dev;

	/* probe only one device unit if unit number is specified */
	if (res > 0)
		nunits = 1;
	while (nunits--) {
		print_disk_probe_info(dev);
		if ((dev.d_dev)->dv_open(&f, &dev) == 0) {
			printf(" good.\n");
			return (uboot_fmtdev(&dev));
		}
		printf("\n");
		dev.d_unit++;
	}
	return (NULL);
}

static const char *
probe_net(const char *dstr)
{
	struct uboot_devdesc dev;
	int i;

	bzero(&dev, sizeof(dev));

	if ((dstr != NULL) && (strncasecmp(dstr, "net", 3) != 0))
		return (NULL);
	if ((dev.d_dev = search_devsw(DEVT_NET)) == NULL)
		return (NULL);
	dev.d_type = dev.d_dev->dv_type;

	return (uboot_fmtdev(&dev));
}

int
main(int argc, char **argv)
{
	struct api_signature *sig = NULL;
	int i, devs_no;
	const char *devstr, *ldev;

	/*
	 * We first check if a command line argument was passed to us containing
	 * API's signature address. If it wasn't then we try to search for the
	 * API signature via the usual hinted address.
	 * If we can't find the magic signature and related info, exit with a
	 * unique error code that U-Boot reports as "## Application terminated,
	 * rc = 0xnnbadab1". Hopefully 'badab1' looks enough like "bad api" to
	 * provide a clue. It's better than 0xffffffff anyway.
	 */
	if (!api_parse_cmdline_sig(argc, argv, &sig) && !api_search_sig(&sig))
		return (0x01badab1);

	syscall_ptr = sig->syscall;
	if (syscall_ptr == NULL)
		return (0x02badab1);

	if (sig->version > API_SIG_VERSION)
		return (0x03badab1);

        /* Clear BSS sections */
	bzero(__sbss_start, __sbss_end - __sbss_start);
	bzero(__bss_start, _end - __bss_start);

	/*
	 * Initialise the heap as early as possible.  Once this is done,
	 * alloc() is usable. The stack is buried inside us, so this is safe.
	 */
	uboot_heap_start = round_page((uintptr_t)end);
#ifndef LOADER_ZFS_SUPPORT
	uboot_heap_end   = uboot_heap_start + 512 * 1024;
#else
	uboot_heap_end   = uboot_heap_start + 2 * 1024 * 1024;
#endif
	setheap((void *)uboot_heap_start, (void *)uboot_heap_end);

	/*
	 * Set up console.
	 */
	cons_probe();
	printf("Compatible U-Boot API signature found @%p\n", sig);

	printf("\n");
	printf("%s, Revision %s\n", bootprog_name, bootprog_rev);
	printf("(%s, %s)\n", bootprog_maker, bootprog_date);
	printf("\n");

	PRINT_DEBUG_INFO();
	meminfo();

	archsw.arch_loadaddr = uboot_loadaddr;
	archsw.arch_getdev = uboot_getdev;
	archsw.arch_copyin = uboot_copyin;
	archsw.arch_copyout = uboot_copyout;
	archsw.arch_readin = uboot_readin;
	archsw.arch_autoload = uboot_autoload;
#ifdef LOADER_ZFS_SUPPORT
	/* Note this needs to be set before ZFS init. */
	archsw.arch_zfs_probe = uboot_zfs_probe;
#endif
	/*
	 * Enumerate U-Boot devices
	 */
	if ((devs_no = ub_dev_enum()) == 0)
		panic("no U-Boot devices found");
	printf("Number of U-Boot devices: %d\n", devs_no);

	devstr = ub_env_get("loaderdev");
	if (devstr == NULL)
		printf("U-Boot env: loaderdev not set, will probe all devices.\n");
	else
		printf("U-Boot env: loaderdev='%s'\n", devstr);

	/*
	 * March through the device switch probing for things.
	 */
	for (i = 0; devsw[i] != NULL; i++) 
		if (devsw[i]->dv_init != NULL)
			(devsw[i]->dv_init)();

	do {
#ifdef LOADER_ZFS_SUPPORT
		if ((ldev = probe_zfs(devstr)) != NULL)
			break;
#endif
		if ((ldev = probe_disks(devstr)) != NULL)
			break;

		if ((ldev = probe_net(devstr)) != NULL)
			break;
		/*
		 * If we couldn't find a boot device, return an error to u-boot.
		 * U-boot may be running a boot script that can try something
		 * different so returning an error is better than forcing a reboot.
		 */
		printf("No boot device found!\n");
		return (0xbadef1ce);
	} while (0);

	printf("Booting from %s\n", ldev);

	env_setenv("currdev", EV_VOLATILE, ldev, uboot_setcurrdev, env_nounset);
	env_setenv("loaddev", EV_VOLATILE, ldev, env_noset, env_nounset);
	
	setenv("LINES", "24", 1);		/* optional */
	setenv("prompt", "loader>", 1);

	interact(NULL);				/* doesn't return */

	return (0);
}

COMMAND_SET(heap, "heap", "show heap usage", command_heap);
static int
command_heap(int argc, char *argv[])
{

	printf("heap base at %p, top at %p, used %td\n", end, sbrk(0),
		sbrk(0) - end);

	return (CMD_OK);
}

COMMAND_SET(reboot, "reboot", "reboot the system", command_reboot);
static int
command_reboot(int argc, char *argv[])
{

	printf("Resetting...\n");
	ub_reset();

	printf("Reset failed!\n");
	while(1);
}

COMMAND_SET(devinfo, "devinfo", "show U-Boot devices", command_devinfo);
static int
command_devinfo(int argc, char *argv[])
{
	struct device_info *di;
	int i = 0;

	if (ub_dev_enum() == 0) {
		command_errmsg = "no U-Boot devices found!?";
		return (CMD_ERROR);
	}
	printf("U-Boot devices:\n");
	while ((di = ub_dev_get(DEV_TYP_NET | DEV_TYP_STOR, &i)) != NULL) {
		printf("device info (%d):\n", i - 1);
		ub_dump_di(di);
		printf("\n");
	}
	return (CMD_OK);
}

COMMAND_SET(sysinfo, "sysinfo", "show U-Boot system info", command_sysinfo);
static int
command_sysinfo(int argc, char *argv[])
{
	struct sys_info *si;

	if ((si = ub_get_sys_info()) == NULL) {
		command_errmsg = "could not retrieve U-Boot sys info!?";
		return (CMD_ERROR);
	}
	printf("U-Boot system info:\n");
	ub_dump_si(si);
	return (CMD_OK);
}

enum ubenv_action {
	UBENV_UNKNOWN,
	UBENV_SHOW,
	UBENV_IMPORT
};

static void
handle_uboot_env_var(enum ubenv_action action, const char * var)
{
	char ldvar[128];
	const char *val;
	char *wrk;
	int len;

	/*
	 * On an import with the variable name formatted as ldname=ubname,
	 * import the uboot variable ubname into the loader variable ldname,
	 * otherwise the historical behavior is to import to uboot.ubname.
	 */
	if (action == UBENV_IMPORT) { 
		len = strcspn(var, "=");
		if (len == 0) {
			printf("name cannot start with '=': '%s'\n", var);
			return;
		}
		if (var[len] == 0) {
			strcpy(ldvar, "uboot.");
			strncat(ldvar, var, sizeof(ldvar) - 7);
		} else {
			len = MIN(len, sizeof(ldvar) - 1);
			strncpy(ldvar, var, len);
			ldvar[len] = 0;
			var = &var[len + 1];
		}
	}
	/*
	 * If the user prepended "uboot." (which is how they usually see these
	 * names) strip it off as a convenience.
	 */
	if (strncmp(var, "uboot.", 6) == 0) {
		var = &var[6];
	}
	/* If there is no variable name left, punt. */
	if (var[0] == 0) {
		printf("empty variable name\n");
		return;
	}
	val = ub_env_get(var);
	if (action == UBENV_SHOW) {
		if (val == NULL)
			printf("uboot.%s is not set\n", var);
		else
			printf("uboot.%s=%s\n", var, val);
	} else if (action == UBENV_IMPORT) {
		if (val != NULL) {
			setenv(ldvar, val, 1);
		}
	}
}

static int
command_ubenv(int argc, char *argv[])
{
	enum ubenv_action action;
	const char *var;
	int i;

	action = UBENV_UNKNOWN;
	if (argc > 1) {
		if (strcasecmp(argv[1], "import") == 0)
			action = UBENV_IMPORT;
		else if (strcasecmp(argv[1], "show") == 0)
			action = UBENV_SHOW;
	}
	if (action == UBENV_UNKNOWN) {
		command_errmsg = "usage: 'ubenv <import|show> [var ...]";
		return (CMD_ERROR);
	}
	if (argc > 2) {
		for (i = 2; i < argc; i++)
			handle_uboot_env_var(action, argv[i]);
	} else {
		var = NULL;
		for (;;) {
			if ((var = ub_env_enum(var)) == NULL)
				break;
			handle_uboot_env_var(action, var);
		}
	}
	return (CMD_OK);
}
COMMAND_SET(ubenv, "ubenv", "show or import U-Boot env vars", command_ubenv);

#ifdef LOADER_FDT_SUPPORT
/*
 * Since proper fdt command handling function is defined in fdt_loader_cmd.c,
 * and declaring it as extern is in contradiction with COMMAND_SET() macro
 * (which uses static pointer), we're defining wrapper function, which
 * calls the proper fdt handling routine.
 */
static int
command_fdt(int argc, char *argv[])
{

	return (command_fdt_internal(argc, argv));
}

COMMAND_SET(fdt, "fdt", "flattened device tree handling", command_fdt);
#endif

#ifdef LOADER_ZFS_SUPPORT
COMMAND_SET(lszfs, "lszfs", "list child datasets of a zfs dataset",
    command_lszfs);

static int
command_lszfs(int argc, char *argv[])
{
	int err;

	if (argc != 2) {
		command_errmsg = "wrong number of arguments";
		return (CMD_ERROR);
	}
	err = zfs_list(argv[1]);
	if (err != 0) {
		command_errmsg = strerror(err);
		return (CMD_ERROR);
	}
	return (CMD_OK);
}

COMMAND_SET(reloadbe, "reloadbe", "refresh the list of ZFS Boot Environments",
	    command_reloadbe);

static int
command_reloadbe(int argc, char *argv[])
{
	int err;
	char *root;

	if (argc > 2) {
		command_errmsg = "wrong number of arguments";
		return (CMD_ERROR);
	}
	if (argc == 2) {
		err = zfs_bootenv(argv[1]);
	} else {
		root = getenv("zfs_be_root");
		if (root == NULL) {
			return (CMD_OK);
		}
		err = zfs_bootenv(root);
	}
	if (err != 0) {
		command_errmsg = strerror(err);
		return (CMD_ERROR);
	}
	return (CMD_OK);
}
#endif
