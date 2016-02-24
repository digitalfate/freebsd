/*-
 * Copyright (c) _____________
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

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/time.h>
#include <sys/rman.h>
#include <sys/clock.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/resource.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <arm/allwinner/allwinner_machdep.h>

#include "clock_if.h"

#define TMR_LOSC_CTRL_REG		0x00
#define TMR_RTC_DATE_REG		0x04
#define TMR_RTC_TIME_REG		0x08

#define TMR_LOSC_OSC_SRC		0x00000001
#define TMR_LOSC_GSM			0x00000008
#define TMR_LOSC_AUTO_SW_EN		0x00004000
#define TRM_LOSC_MAGIC			0x16aa0000
#define TMR_LOSC_BUSY_MASK		0x00000380

#define YEAR_MIN				(sc->sun7i ? 1970 : 2010)
#define YEAR_MAX				(sc->sun7i ? 2100 : 2073)
#define YEAR_OFFSET				(sc->sun7i ? 1900 : 2010)
#define YEAR_MASK				(sc->sun7i ? 0xff : 0x3f)
#define LEAP_BIT				(sc->sun7i ? 24 : 22)

#define TIME_MASK				0x001f3f3f

#define GET_SEC_VALUE(x)		((x)  & 0x0000003f)
#define GET_MIN_VALUE(x)		(((x) & 0x00003f00) >> 8)
#define GET_HOUR_VALUE(x)		(((x) & 0x001f0000) >> 16)
#define GET_DAY_VALUE(x)		((x)  & 0x0000001f)
#define GET_MON_VALUE(x)		(((x) & 0x00000f00) >> 8)
#define GET_YEAR_VALUE(x)		(((x) >> 16) & YEAR_MASK)

#define SET_DAY_VALUE(x)		GET_DAY_VALUE(x)
#define SET_MON_VALUE(x)		(((x) & 0x0000000f) << 8)
#define SET_YEAR_VALUE(x)		(((x) & YEAR_MASK)  << 16)
#define SET_LEAP_VALUE(x)		(((x) & 0x00000001) << LEAP_BIT)
#define SET_SEC_VALUE(x)		GET_SEC_VALUE(x)
#define SET_MIN_VALUE(x)		(((x) & 0x0000003f) << 8)
#define SET_HOUR_VALUE(x)		(((x) & 0x0000001f) << 16)

#define	HALF_OF_SEC_NS			500000000
#define RTC_TIMEOUT				70

#define rtc_read_4(sc, reg) \
	bus_space_read_4((sc)->bst, (sc)->bsh, (reg))
#define rtc_write_4(sc, reg, val) \
	bus_space_write_4((sc)->bst, (sc)->bsh, (reg), (val))

#define	IS_LEAP_YEAR(y) \
	(((y) % 400) == 0 || (((y) % 100) != 0 && ((y) % 4) == 0))


static struct ofw_compat_data compat_data[] = {
	{ "allwinner,sun4i-a10-rtc", true },
	{ "allwinner,sun7i-a20-rtc", true },
	{ NULL, false }
};

static struct resource_spec sunxi_rtc_spec[] = {
	{ SYS_RES_MEMORY,	0,	RF_ACTIVE },
	{ -1, 0 }
};
struct sunxi_rtc_softc {
	struct resource				*res[1];
	bus_space_tag_t				bst;
	bus_space_handle_t			bsh;
	u_int						sun7i;
};

static int sunxi_rtc_probe(device_t dev);
static int sunxi_rtc_attach(device_t dev);

static int sunxi_rtc_gettime(device_t dev, struct timespec *ts);
static int sunxi_rtc_settime(device_t dev, struct timespec *ts);

static device_method_t sunxi_rtc_methods[] = {
	DEVMETHOD(device_probe,		sunxi_rtc_probe),
	DEVMETHOD(device_attach,	sunxi_rtc_attach),

	DEVMETHOD(clock_gettime,	sunxi_rtc_gettime),
	DEVMETHOD(clock_settime,	sunxi_rtc_settime),

	DEVMETHOD_END
};

static driver_t sunxi_rtc_driver = {
	"rtc",
	sunxi_rtc_methods,
	sizeof(struct sunxi_rtc_softc),
};

static devclass_t sunxi_rtc_devclass;

EARLY_DRIVER_MODULE(sunxi_rtc, simplebus, sunxi_rtc_driver, sunxi_rtc_devclass, 0, 0,
    BUS_PASS_TIMER + BUS_PASS_ORDER_MIDDLE);


static int
sunxi_rtc_probe(device_t dev)
{
	struct sunxi_rtc_softc *sc  = device_get_softc(dev);
	u_int soc_family;

	if (!ofw_bus_search_compatible(dev, compat_data)->ocd_data)
		return (ENXIO);

	soc_family = allwinner_soc_family();
	if (soc_family != ALLWINNERSOC_SUN7I && 
		soc_family != ALLWINNERSOC_SUN4I) return (ENXIO);
		
	sc->sun7i = (soc_family == ALLWINNERSOC_SUN7I) ? 1 : 0;

	device_set_desc(dev, "Allwinner RTC");
	return (BUS_PROBE_DEFAULT);
}

static int
sunxi_rtc_attach(device_t dev)
{
	struct sunxi_rtc_softc *sc  = device_get_softc(dev);
	uint32_t val;

	if (bus_alloc_resources(dev, sunxi_rtc_spec, sc->res)) {
		device_printf(dev, "could not allocate resources\n");
		return (ENXIO);
	}

	sc->bst = rman_get_bustag(sc->res[0]);
	sc->bsh = rman_get_bushandle(sc->res[0]);

	val = rtc_read_4(sc, TMR_LOSC_CTRL_REG);
	val &= ~TMR_LOSC_AUTO_SW_EN;
	val |= TRM_LOSC_MAGIC | TMR_LOSC_GSM | TMR_LOSC_OSC_SRC;
	rtc_write_4(sc, TMR_LOSC_CTRL_REG, val);
	
	DELAY(100);
	
	val = rtc_read_4(sc, TMR_LOSC_CTRL_REG);
	if (!(val & TMR_LOSC_OSC_SRC)) {
		bus_release_resources(dev, sunxi_rtc_spec, sc->res);
		device_printf(dev, "set LOSC to external failed\n");
		return (ENXIO);
	}
	
	clock_register(dev, 1000000);
	
	return (0);
}

static int
sunxi_rtc_gettime(device_t dev, struct timespec *ts)
{
	struct sunxi_rtc_softc *sc  = device_get_softc(dev);
	struct clocktime ct;
	uint32_t rdate, rtime;

	rdate = rtc_read_4(sc, TMR_RTC_DATE_REG);
	rtime = rtc_read_4(sc, TMR_RTC_TIME_REG);
	
	if ((rtime & TIME_MASK) == 0)
		rdate = rtc_read_4(sc, TMR_RTC_DATE_REG);

	ct.sec = GET_SEC_VALUE(rtime);
	ct.min = GET_MIN_VALUE(rtime);
	ct.hour = GET_HOUR_VALUE(rtime);
	ct.day = GET_DAY_VALUE(rdate);
	ct.mon = GET_MON_VALUE(rdate);
	ct.year = GET_YEAR_VALUE(rdate) + YEAR_OFFSET;
	ct.dow = -1;
	/* RTC resolution is 1 sec */
	ct.nsec = 0;
	
	return clock_ct_to_ts(&ct, ts);
}

#define TMR_LOSC_BUSY \
	(rtc_read_4(sc, TMR_LOSC_CTRL_REG) & TMR_LOSC_BUSY_MASK)

static int
sunxi_rtc_settime(device_t dev, struct timespec *ts)
{
	struct sunxi_rtc_softc *sc  = device_get_softc(dev);
	struct clocktime ct;
	uint32_t clk = 0, rdate, rtime;

	clock_ts_to_ct(ts, &ct);
	
	if ((ct.year < YEAR_MIN) || (ct.year > YEAR_MAX)) {
		device_printf(dev, "could not set time, year out of range\n");
		return (EINVAL);
	}
	/* 
	 * write all zeros to the time register to avoid unxpected 
	 * date increment
	 */
	while (TMR_LOSC_BUSY && (clk++ < RTC_TIMEOUT)) DELAY(1);
	if (clk >= RTC_TIMEOUT) {
		device_printf(dev, "could not set time, RTC busy\n");
		return (EINVAL);
	}
	rtc_write_4(sc, TMR_RTC_TIME_REG, 0);

	/* RTC resolution is 1 sec */
	if (ts->tv_nsec >= HALF_OF_SEC_NS) ts->tv_sec++;
	ts->tv_nsec = 0;

	rdate = SET_DAY_VALUE(ct.day) | SET_MON_VALUE(ct.mon) | \
		SET_YEAR_VALUE(ct.year - YEAR_OFFSET) | 
		SET_LEAP_VALUE(IS_LEAP_YEAR(ct.year));
			
	rtime = SET_SEC_VALUE(ct.sec) | SET_MIN_VALUE(ct.min) | \
		SET_HOUR_VALUE(ct.hour);

	clk = 0;
	while (TMR_LOSC_BUSY && (clk++ < RTC_TIMEOUT)) DELAY(1);
	if (clk >= RTC_TIMEOUT) {
		device_printf(dev, "could not set date, RTC busy\n");
		return (EINVAL);
	}
	rtc_write_4(sc, TMR_RTC_DATE_REG, rdate);

	clk = 0;
	while (TMR_LOSC_BUSY && (clk++ < RTC_TIMEOUT)) DELAY(1);
	if (clk >= RTC_TIMEOUT) {
		device_printf(dev, "could not set time, RTC busy\n");
		return (EINVAL);
	}
	rtc_write_4(sc, TMR_RTC_TIME_REG, rtime);

	DELAY(RTC_TIMEOUT);

	return (0);
}
