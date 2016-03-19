/*-
 * Copyright (c) 1998 Michael Smith <msmith@freebsd.org>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
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

#include <stand.h>
#include <string.h>
#ifdef LOADER_ZFS_SUPPORT
#include <libzfs.h>
#endif

#include "bootstrap.h"
#include "disk.h"
#include "libuboot.h"

static int uboot_parsedev(struct uboot_devdesc **, const char *, const char **);

/*
 * Point (dev) at an allocated device specifier for the device matching the
 * path in (devspec). If it contains an explicit device specification,
 * use that.  If not, use the default device.
 */
int
uboot_getdev(void **vdev, const char *devspec, const char **path)
{
	struct uboot_devdesc **dev = (struct uboot_devdesc **)vdev;
	int rv;

	/*
	 * If it looks like this is just a path and no
	 * device, go with the current device.
	 */
	if ((devspec == NULL) || (devspec[0] == '/') ||
		(strchr(devspec, ':') == NULL)) {

		rv = uboot_parsedev(dev, getenv("currdev"), NULL);
		if ((rv == 0) && (path != NULL))
			*path = devspec;
		return(rv);
	}
	/*
	 * Try to parse the device name off the beginning of the devspec.
	 */
	return (uboot_parsedev(dev, devspec, path));
}

/*
 * Point (dev) at an allocated device specifier matching the string version
 * at the beginning of (devspec).  Return a pointer to the remaining
 * text in (path).
 *
 * In all cases, the beginning of (devspec) is compared to the names
 * of known devices in the device switch, and then any following text
 * is parsed according to the rules applied to the device type.
 *
 * For disk-type devices, the syntax is:
 *
 * disk<unit>[<partition>]:
 *
 */
static int
uboot_parsedev(struct uboot_devdesc **dev, const char *devspec, const char **path)
{
	struct uboot_devdesc *idev;
	struct devsw *dv;
	const char *np;
	char *cp;
	int i, err = 0;

	/* minimum length check */
	if (strlen(devspec) < 2)
		return(EINVAL);

	/* look for a device that matches */
	for (i = 0; devsw[i] != NULL; i++) {
		dv = devsw[i];
		if (!strncmp(devspec, dv->dv_name, strlen(dv->dv_name)))
			break;
	}
	if (dv == NULL)
		return (ENOENT);
	if ((idev = malloc(sizeof(struct uboot_devdesc))) == NULL)
		return (ENOMEM);

	np = devspec + strlen(dv->dv_name);

	switch(dv->dv_type) {
#ifdef LOADER_ZFS_SUPPORT
	case DEVT_ZFS:
		err = zfs_parsedev((struct zfs_devdesc *)idev, np, path);
		break;
#endif
#ifdef LOADER_DISK_SUPPORT
	case DEVT_DISK:
		err = disk_parsedev((struct disk_devdesc *)idev, np, path);
		break;
#endif
	case DEVT_NET:
		idev->d_unit = 0;
		if (*np && (*np != ':')) {
			idev->d_unit = strtol(np, &cp, 0);
			if (cp == np) {
				idev->d_unit = -1;
				err = EINVAL;
				break;
			}
		}
		else {
		    cp = (char *)np;
		}
		if (*cp && (*cp != ':')) {
			err = EINVAL;
			break;
		}
		if (path != NULL)
			*path = (*cp == 0) ? cp : cp + 1;
		break;
	case DEVT_NONE:
		free(idev);
		return (0);
	default:
		err = EINVAL;
	}
	if (err) {
		free(idev);
		return (err);
	}
	idev->d_dev = dv;
	idev->d_type = dv->dv_type;

	if (dev != NULL)
		*dev = idev;
	else
		free(idev);

	return (0);
}

char *
uboot_fmtdev(void *vdev)
{
	struct uboot_devdesc *dev = (struct uboot_devdesc *)vdev;
	static char buf[UB_MAXNAMELEN];

	switch(dev->d_type) {
#ifdef LOADER_ZFS_SUPPORT
	case DEVT_ZFS:
	        return (zfs_fmtdev(vdev));
#endif
	case DEVT_NONE:
		strcpy(buf, "(no device)");
		break;
	case DEVT_DISK:
#ifdef LOADER_DISK_SUPPORT
		return (disk_fmtdev(vdev));
#endif
	case DEVT_NET:
		sprintf(buf, "%s%d:", dev->d_dev->dv_name, dev->d_unit);
		break;
	}
	return(buf);
}

/*
 * Set currdev to suit the value being supplied in (value).
 */
int
uboot_setcurrdev(struct env_var *ev, int flags, const void *value)
{
	struct uboot_devdesc *ncurr;
	int rv;

	if ((rv = uboot_parsedev(&ncurr, value, NULL)) != 0)
		return (rv);
	free(ncurr);
	env_setenv(ev->ev_name, flags | EV_NOHOOK, value, NULL, NULL);
	return (0);
}
