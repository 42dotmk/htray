/* htray - a toggleable overlay bar with a system tray and status readout
 * (battery, speaker/mic volume with mute state, clock).
 *
 * Owns the _NET_SYSTEM_TRAY_S<n> selection so applets (nm-applet, blueman,
 * ...) dock their icons here via XEmbed. The bar is an override-redirect
 * window in a screen corner (see atbottom); SIGUSR1 toggles it
 * (bind `pkill -USR1 -x htray` in your window manager). The applets keep
 * running while hidden - only the bar window is unmapped. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>
#include <X11/Xft/Xft.h>

#ifndef HTRAY_VERSION
#define HTRAY_VERSION "dev"
#endif

#define STB_DS_IMPLEMENTATION
#include <stb_ds.h>

#define SYSTEM_TRAY_REQUEST_DOCK 0
#define XEMBED_EMBEDDED_NOTIFY 0

#include "config.h"

static Display *dpy;
static int screen, haverandr;
static int mx, my, mw, mh; /* monitor the bar sits on */
static Window root, barwin, selwin;
static GC gc;
static XftFont *font;
static XftDraw *xd;
static XftColor xftfg, xftdim;
static unsigned long bgpx, fgpx, borderpx_col;
static Atom xembed, manager, trayatom, trayopcode, trayorient, netcurdesk;
static Window *icons; /* stb_ds array, in docking order */
static int visible;
static int barw;
static char batpath[288], stattxt[128];
static char battxt[16], voltxt[8], mictxt[8], clockstr[48], desktxt[16];
static char memtxt[16], cputxt[16];
static int cpupct = -1;
static long cpuprevtot, cpuprevidle;
static int batcap = -1, volpct = -1, micpct = -1, volmute, micmute;
static int desknum = -1;
static volatile sig_atomic_t togglereq, focusreq;
static char inputbuf[256];       /* command being typed */
static char cmdname[256];        /* the running command, notification title */
static char cmdout[2048];        /* raw output of the running command */
static size_t cmdoutlen;
static int inputactive;          /* keyboard grabbed, keys go to the box */
static pid_t cmdpid;
static int cmdfd = -1;           /* read end of the running command's pipe */

static void
die(const char *msg)
{
	fputs(msg, stderr);
	exit(1);
}

static void
envuint(const char *name, unsigned int *dst)
{
	const char *s = getenv(name);
	char *end;
	long v;

	if (!s || !*s)
		return;
	v = strtol(s, &end, 10);
	if (!*end && v >= 0)
		*dst = (unsigned int)v;
}

static void
envint(const char *name, int *dst)
{
	const char *s = getenv(name);
	char *end;
	long v;

	if (!s || !*s)
		return;
	v = strtol(s, &end, 10);
	if (!*end)
		*dst = (int)v;
}

static void
envstr(const char *name, const char **dst)
{
	const char *s = getenv(name);

	if (s && *s)
		*dst = s;
}

/* apply HTRAY_* environment overrides to the config.h defaults */
static void
loadconfig(void)
{
	envuint("HTRAY_HEIGHT", &barh);
	envuint("HTRAY_BORDERPX", &borderpx);
	envuint("HTRAY_PADDING", &padding);
	envuint("HTRAY_HPAD", &hpad);
	envuint("HTRAY_ICONSIZE", &iconsize);
	envuint("HTRAY_SPACING", &spacing);
	envuint("HTRAY_FONTSIZE", &fontsize);
	envuint("HTRAY_CMDTIMEOUT", &cmdtimeout);
	envuint("HTRAY_INPUTW", &inputw);
	envint("HTRAY_HIDDEN", &starthidden);
	envint("HTRAY_BOTTOM", &atbottom);
	envstr("HTRAY_BG", &col_bg);
	envstr("HTRAY_FG", &col_fg);
	envstr("HTRAY_BORDER", &col_border);
	envstr("HTRAY_FONT", &fontname);
	envstr("HTRAY_TIMEFMT", &timefmt);
	if (barh < 8)
		barh = 8;
}

/* tray icons come and go at will; ignore errors from vanished windows */
static int
xerror(Display *d, XErrorEvent *ee)
{
	(void)d; (void)ee;
	return 0;
}

/* SIGUSR1 toggles the bar, SIGUSR2 toggles input-box focus */
static void
sighandler(int sig)
{
	if (sig == SIGUSR2)
		focusreq = 1;
	else
		togglereq = 1;
}

static void
findbattery(void)
{
	DIR *dir;
	struct dirent *de;

	batpath[0] = '\0';
	if (!(dir = opendir("/sys/class/power_supply")))
		return;
	while ((de = readdir(dir))) {
		if (!strncmp(de->d_name, "BAT", 3)) {
			snprintf(batpath, sizeof batpath,
			         "/sys/class/power_supply/%s", de->d_name);
			break;
		}
	}
	closedir(dir);
}

static long
nowms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* run argv, capturing its stdout into buf; the child is killed once
 * cmdtimeout ms pass, so a hung command (e.g. wpctl against a wedged
 * pipewire) cannot block the bar. Returns bytes read, -1 if none. */
static int
readcmd(char *const argv[], char *buf, size_t size)
{
	fd_set fds;
	struct timeval tv;
	pid_t pid;
	long deadline, left;
	ssize_t n;
	size_t len = 0;
	int fd[2], devnull;

	buf[0] = '\0';
	if (pipe(fd) < 0)
		return -1;
	switch ((pid = fork())) {
	case -1:
		close(fd[0]);
		close(fd[1]);
		return -1;
	case 0:
		close(fd[0]);
		dup2(fd[1], 1);
		close(fd[1]);
		if ((devnull = open("/dev/null", O_WRONLY)) >= 0)
			dup2(devnull, 2);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fd[1]);
	deadline = nowms() + (long)cmdtimeout;
	while (len < size - 1) {
		if ((left = deadline - nowms()) <= 0)
			break;
		FD_ZERO(&fds);
		FD_SET(fd[0], &fds);
		tv.tv_sec = left / 1000;
		tv.tv_usec = (left % 1000) * 1000;
		n = select(fd[0] + 1, &fds, NULL, NULL, &tv);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		n = read(fd[0], buf + len, size - 1 - len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		len += (size_t)n;
	}
	close(fd[0]);
	kill(pid, SIGKILL); /* no-op if it already exited */
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;
	buf[len] = '\0';
	return len ? (int)len : -1;
}

/* parse "Volume: 0.90 [MUTED]" from wpctl; -1 if unavailable */
static int
getvolume(const char *target, int *muted)
{
	char *argv[] = { "wpctl", "get-volume", (char *)target, NULL };
	char line[64];
	float v;
	int pct = -1;

	*muted = 0;
	if (readcmd(argv, line, sizeof line) < 0)
		return -1;
	if (sscanf(line, "Volume: %f", &v) == 1) {
		pct = (int)(v * 100.0f + 0.5f);
		if (pct > 999)
			pct = 999;
		if (strstr(line, "MUTED"))
			*muted = 1;
	}
	return pct;
}

/* current workspace from _NET_CURRENT_DESKTOP, shown as-is (hwm is 0-based) */
static void
updatedesktop(void)
{
	Atom type;
	int fmt;
	unsigned long n, after;
	unsigned char *data = NULL;

	desknum = -1;
	if (XGetWindowProperty(dpy, root, netcurdesk, 0, 1, False,
	                       XA_CARDINAL, &type, &fmt, &n, &after,
	                       &data) == Success && data) {
		if (type == XA_CARDINAL && fmt == 32 && n >= 1)
			desknum = (int)*(long *)data;
		XFree(data);
	}
	if (desknum >= 0)
		snprintf(desktxt, sizeof desktxt, "%d", desknum);
	else
		desktxt[0] = '\0';
}

/* refresh battery/volume/mic/clock; returns nonzero if anything changed.
 * stattxt is only a snapshot of all values for change detection. */
static int
updatestatus(void)
{
	char path[512], status[32], old[sizeof stattxt];
	FILE *f;
	int cap = -1;
	long mt = 0, ma = 0, ms = 0, tenths;
	long us, ni, sy, id, io, hi, si, st, tot, idle, dt, di;
	time_t t;
	struct tm *tm;

	strcpy(old, stattxt);
	cpupct = -1;
	if ((f = fopen("/proc/stat", "r"))) {
		if (fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld %ld",
		           &us, &ni, &sy, &id, &io, &hi, &si, &st) == 8) {
			tot = us + ni + sy + id + io + hi + si + st;
			idle = id + io;
			dt = tot - cpuprevtot;
			di = idle - cpuprevidle;
			if (cpuprevtot && dt > 0) {
				cpupct = (int)((dt - di) * 100 / dt);
				if (cpupct < 0)
					cpupct = 0;
				if (cpupct > 100)
					cpupct = 100;
			}
			cpuprevtot = tot;
			cpuprevidle = idle;
		}
		fclose(f);
	}
	if (cpupct >= 0)
		snprintf(cputxt, sizeof cputxt, "%d%%", cpupct);
	memtxt[0] = '\0';
	if ((f = fopen("/proc/meminfo", "r"))) {
		while (fgets(path, sizeof path, f)
		       && (!mt || !ma || !ms)) {
			sscanf(path, "MemTotal: %ld kB", &mt);
			sscanf(path, "MemAvailable: %ld kB", &ma);
			sscanf(path, "Shmem: %ld kB", &ms);
		}
		fclose(f);
	}
	if (mt > 0 && ma > 0 && ma <= mt) {
		/* shmem (tmpfs, X/graphics buffers) excluded: show process+kernel use */
		tenths = (mt - ma - ms) * 10 / 1048576; /* tenths of GiB */
		if (tenths < 0)
			tenths = 0;
		if (tenths > 9999)
			tenths = 9999;
		snprintf(memtxt, sizeof memtxt, "%ld.%ldG",
		         tenths / 10, tenths % 10);
	}
	battxt[0] = '\0';
	if (batpath[0]) {
		snprintf(path, sizeof path, "%s/capacity", batpath);
		if ((f = fopen(path, "r"))) {
			if (fscanf(f, "%d", &cap) != 1)
				cap = -1;
			else if (cap > 100)
				cap = 100;
			fclose(f);
		}
		status[0] = '\0';
		snprintf(path, sizeof path, "%s/status", batpath);
		if ((f = fopen(path, "r"))) {
			if (fscanf(f, "%31s", status) != 1)
				status[0] = '\0';
			fclose(f);
		}
		if (cap >= 0)
			snprintf(battxt, sizeof battxt, "%d%%%s", cap,
			         strcmp(status, "Charging") ? "" : "+");
	}
	batcap = cap;
	volpct = getvolume("@DEFAULT_AUDIO_SINK@", &volmute);
	if (volpct >= 0)
		snprintf(voltxt, sizeof voltxt, "%d%%", volpct);
	micpct = getvolume("@DEFAULT_AUDIO_SOURCE@", &micmute);
	if (micpct >= 0)
		snprintf(mictxt, sizeof mictxt, "%d%%", micpct);
	clockstr[0] = '\0';
	t = time(NULL);
	if ((tm = localtime(&t)))
		strftime(clockstr, sizeof clockstr, timefmt, tm);
	snprintf(stattxt, sizeof stattxt, "%s|%s|%s|%s%d|%s%d|%s",
	         cpupct >= 0 ? cputxt : "", memtxt, battxt,
	         volpct >= 0 ? voltxt : "", volmute,
	         micpct >= 0 ? mictxt : "", micmute, clockstr);
	return strcmp(old, stattxt) != 0;
}

static int
textw(const char *s)
{
	XGlyphInfo ext;

	if (!s[0])
		return 0;
	XftTextExtentsUtf8(dpy, font, (FcChar8 *)s, (int)strlen(s), &ext);
	return ext.xOff;
}

/* reserved width of a readout: its template, or the text if it overflows */
static int
slotw(const char *s, const char *tmpl)
{
	int w = textw(s), tw = textw(tmpl);

	return w > tw ? w : tw;
}

/* width of the workspace segment: number, gap, divider, gap */
static int
wsw(void)
{
	if (!desktxt[0])
		return 0;
	return textw(desktxt) + (int)spacing + 1 + (int)spacing;
}

/* total width of the status area: icon+text segments, then the clock */
static int
statusw(void)
{
	int w = 0;

	if (cpupct >= 0)
		w += (int)(cpuw + icgap) + slotw(cputxt, cputmpl)
		     + (int)spacing;
	if (memtxt[0])
		w += (int)(memw + icgap) + slotw(memtxt, memtmpl)
		     + (int)spacing;
	if (volpct >= 0)
		w += (int)(spkw + icgap) + slotw(voltxt, voltmpl)
		     + (int)spacing;
	if (micpct >= 0)
		w += (int)(micw + icgap) + slotw(mictxt, mictmpl)
		     + (int)spacing;
	if (batcap >= 0)
		w += (int)(batw + batnubw + icgap) + textw(battxt)
		     + (int)spacing;
	w += textw(clockstr);
	if (w && !clockstr[0])
		w -= (int)spacing;
	return w;
}

/* pin the bar to the second monitor when there is one; RandR lists the
 * primary first. Whole screen without the extension. */
static void
updatemon(void)
{
	XRRMonitorInfo *info;
	int i, n;

	mx = 0;
	my = 0;
	mw = DisplayWidth(dpy, screen);
	mh = DisplayHeight(dpy, screen);
	if (!haverandr)
		return;
	if (!(info = XRRGetMonitors(dpy, root, True, &n)))
		return;
	if (n > 0) {
		i = n > 1 ? 1 : 0;
		mx = info[i].x;
		my = info[i].y;
		mw = info[i].width;
		mh = info[i].height;
	}
	XRRFreeMonitors(info);
}

/* recompute bar geometry and slot every icon; safe to call while hidden */
static void
layout(void)
{
	int i, x, tw, n = (int)arrlen(icons);

	updatemon();
	tw = statusw();
	barw = (int)(2 * hpad) + wsw() + (int)(inputw + spacing)
	       + n * (int)(iconsize + spacing) + tw;
	if (n && !tw)
		barw -= (int)spacing;
	if (barw < (int)barh)
		barw = (int)barh;
	XMoveResizeWindow(dpy, barwin,
	                  mx + mw - barw - 2 * (int)borderpx - (int)padding,
	                  my + (atbottom ? mh - (int)barh - 2 * (int)borderpx
	                                   - (int)padding
	                                 : (int)padding),
	                  (unsigned int)barw, barh);
	x = (int)hpad + wsw() + (int)(inputw + spacing);
	for (i = 0; i < n; i++) {
		XMoveResizeWindow(dpy, icons[i], x,
		                  ((int)barh - (int)iconsize) / 2,
		                  iconsize, iconsize);
		x += (int)(iconsize + spacing);
	}
}

/* outline body, nub on the right, fill level proportional to charge */
static void
drawbattery(int x, int y)
{
	int fw = ((int)batw - 4) * batcap / 100;

	XDrawRectangle(dpy, barwin, gc, x, y, batw - 1, bath - 1);
	XFillRectangle(dpy, barwin, gc, x + (int)batw,
	               y + ((int)bath - (int)batnubh) / 2, batnubw, batnubh);
	if (fw > 0)
		XFillRectangle(dpy, barwin, gc, x + 2, y + 2,
		               (unsigned int)fw, bath - 4);
}

/* CPU package: outlined die, filled core, pins on all four sides */
static void
drawcpu(int x, int y)
{
	int i, o;

	XDrawRectangle(dpy, barwin, gc, x + 2, y + 2, cpuw - 5, cpuh - 5);
	XFillRectangle(dpy, barwin, gc, x + 5, y + 5, cpuw - 10, cpuh - 10);
	for (i = 0; i < 3; i++) {
		o = 4 + i * 2;
		XDrawLine(dpy, barwin, gc, x + o, y, x + o, y + 1);
		XDrawLine(dpy, barwin, gc, x + o, y + (int)cpuh - 2,
		          x + o, y + (int)cpuh - 1);
		XDrawLine(dpy, barwin, gc, x, y + o, x + 1, y + o);
		XDrawLine(dpy, barwin, gc, x + (int)cpuw - 2, y + o,
		          x + (int)cpuw - 1, y + o);
	}
}

/* RAM chip: body with a die notch, pins along top and bottom edges */
static void
drawmem(int x, int y)
{
	int i;

	XDrawRectangle(dpy, barwin, gc, x, y + 2, memw - 1, memh - 5);
	XDrawRectangle(dpy, barwin, gc, x + 5, y + 5, 3, 2);
	for (i = 0; i < 4; i++) {
		XDrawLine(dpy, barwin, gc, x + 2 + i * 3, y,
		          x + 2 + i * 3, y + 1);
		XDrawLine(dpy, barwin, gc, x + 2 + i * 3, y + (int)memh - 2,
		          x + 2 + i * 3, y + (int)memh - 1);
	}
}

/* driver box + cone, then sound waves, or an x when muted */
static void
drawspeaker(int x, int y, int muted)
{
	XPoint cone[4];

	cone[0].x = (short)(x + 3); cone[0].y = (short)(y + 3);
	cone[1].x = (short)(x + 8); cone[1].y = (short)y;
	cone[2].x = (short)(x + 8); cone[2].y = (short)(y + 9);
	cone[3].x = (short)(x + 3); cone[3].y = (short)(y + 6);
	XFillRectangle(dpy, barwin, gc, x, y + 3, 3, 4);
	XFillPolygon(dpy, barwin, gc, cone, 4, Convex, CoordModeOrigin);
	if (muted) {
		XDrawLine(dpy, barwin, gc, x + 10, y + 2, x + 14, y + 8);
		XDrawLine(dpy, barwin, gc, x + 14, y + 2, x + 10, y + 8);
	} else {
		XDrawArc(dpy, barwin, gc, x + 9, y + 2, 4, 6,
		         -60 * 64, 120 * 64);
		XDrawArc(dpy, barwin, gc, x + 9, y, 6, 10,
		         -60 * 64, 120 * 64);
	}
}

/* capsule + U-shaped stand; a slash across it when muted */
static void
drawmic(int x, int y, int muted)
{
	XFillRectangle(dpy, barwin, gc, x + 3, y, 3, 7);
	XDrawArc(dpy, barwin, gc, x + 1, y + 2, 7, 7, 180 * 64, 180 * 64);
	XDrawLine(dpy, barwin, gc, x + 4, y + 9, x + 4, y + 11);
	XDrawLine(dpy, barwin, gc, x + 2, y + 11, x + 7, y + 11);
	if (muted)
		XDrawLine(dpy, barwin, gc, x, y + 11, x + 8, y);
}

static void
drawbar(void)
{
	const char *s;
	int x, w, boxy = ((int)barh - (int)iconsize) / 2;
	int ty = ((int)barh + font->ascent - font->descent) / 2;

	XClearWindow(dpy, barwin);
	x = (int)hpad;
	if (desktxt[0]) {
		XftDrawStringUtf8(xd, &xftfg, font, x, ty,
		                  (FcChar8 *)desktxt, (int)strlen(desktxt));
		x += textw(desktxt) + (int)spacing;
		XSetForeground(dpy, gc, borderpx_col);
		XDrawLine(dpy, barwin, gc, x, ((int)barh - (int)divh) / 2,
		          x, ((int)barh + (int)divh) / 2);
		XSetForeground(dpy, gc, fgpx);
		x += 1 + (int)spacing;
	}
	/* input box; show the tail of the line when it overflows */
	XSetForeground(dpy, gc, inputactive ? fgpx : borderpx_col);
	XDrawRectangle(dpy, barwin, gc, x, boxy, inputw - 1, iconsize - 1);
	XSetForeground(dpy, gc, fgpx);
	s = inputbuf;
	while (*s && textw(s) > (int)inputw - 14)
		s++;
	if (*s)
		XftDrawStringUtf8(xd, &xftfg, font, x + 5, ty,
		                  (FcChar8 *)s, (int)strlen(s));
	else if (!inputactive)
		XftDrawStringUtf8(xd, &xftdim, font, x + 5, ty,
		                  (FcChar8 *)"$", 1);
	if (inputactive)
		XFillRectangle(dpy, barwin, gc, x + 6 + textw(s), boxy + 4,
		               2, iconsize - 8);
	x = barw - (int)hpad - statusw();
	if (cpupct >= 0) {
		drawcpu(x, ((int)barh - (int)cpuh) / 2);
		x += (int)(cpuw + icgap);
		w = slotw(cputxt, cputmpl);
		XftDrawStringUtf8(xd, &xftfg, font, x + w - textw(cputxt),
		                  ty, (FcChar8 *)cputxt, (int)strlen(cputxt));
		x += w + (int)spacing;
	}
	if (memtxt[0]) {
		drawmem(x, ((int)barh - (int)memh) / 2);
		x += (int)(memw + icgap);
		w = slotw(memtxt, memtmpl);
		XftDrawStringUtf8(xd, &xftfg, font, x + w - textw(memtxt),
		                  ty, (FcChar8 *)memtxt, (int)strlen(memtxt));
		x += w + (int)spacing;
	}
	if (volpct >= 0) {
		drawspeaker(x, ((int)barh - (int)spkh) / 2, volmute);
		x += (int)(spkw + icgap);
		w = slotw(voltxt, voltmpl);
		XftDrawStringUtf8(xd, &xftfg, font, x + w - textw(voltxt),
		                  ty, (FcChar8 *)voltxt, (int)strlen(voltxt));
		x += w + (int)spacing;
	}
	if (micpct >= 0) {
		drawmic(x, ((int)barh - (int)mich) / 2, micmute);
		x += (int)(micw + icgap);
		w = slotw(mictxt, mictmpl);
		XftDrawStringUtf8(xd, &xftfg, font, x + w - textw(mictxt),
		                  ty, (FcChar8 *)mictxt, (int)strlen(mictxt));
		x += w + (int)spacing;
	}
	if (batcap >= 0) {
		drawbattery(x, ((int)barh - (int)bath) / 2);
		x += (int)(batw + batnubw + icgap);
		XftDrawStringUtf8(xd, &xftfg, font, x, ty,
		                  (FcChar8 *)battxt, (int)strlen(battxt));
		x += textw(battxt) + (int)spacing;
	}
	if (clockstr[0])
		XftDrawStringUtf8(xd, &xftfg, font, x, ty,
		                  (FcChar8 *)clockstr, (int)strlen(clockstr));
}

/* hand the finished command's output to the notification daemon */
static void
notifycmd(void)
{
	char out[64];
	char *argv[] = { "notify-send", "-a", "htray", cmdname,
	                 cmdoutlen ? cmdout : "(no output)", NULL };

	cmdout[cmdoutlen] = '\0';
	readcmd(argv, out, sizeof out);
}

static void
cmdkill(void)
{
	if (cmdfd < 0)
		return;
	close(cmdfd);
	cmdfd = -1;
	kill(cmdpid, SIGKILL); /* no-op if it already exited */
	while (waitpid(cmdpid, NULL, 0) < 0 && errno == EINTR)
		;
	cmdpid = 0;
}

/* run the typed line via sh -c; output is read back in run()'s select
 * loop, so a slow command never blocks the bar */
static void
runinput(void)
{
	int fd[2];

	cmdkill();
	cmdoutlen = 0;
	if (!inputbuf[0] || pipe(fd) < 0)
		return;
	strcpy(cmdname, inputbuf);
	switch ((cmdpid = fork())) {
	case -1:
		close(fd[0]);
		close(fd[1]);
		cmdpid = 0;
		return;
	case 0:
		close(fd[0]);
		dup2(fd[1], 1);
		dup2(fd[1], 2);
		close(fd[1]);
		execl("/bin/sh", "sh", "-c", inputbuf, (char *)NULL);
		_exit(127);
	}
	close(fd[1]);
	cmdfd = fd[0];
}

/* drain command output; once it finishes, notify with what it printed */
static void
cmdread(void)
{
	char buf[256];
	ssize_t n;
	size_t i;

	n = read(cmdfd, buf, sizeof buf);
	if (n < 0 && errno == EINTR)
		return;
	if (n > 0) {
		for (i = 0; i < (size_t)n && cmdoutlen < sizeof cmdout - 1;
		     i++)
			cmdout[cmdoutlen++] = buf[i];
	} else {
		cmdkill();
		notifycmd();
	}
}

static void
startinput(void)
{
	struct timespec ts = { 0, 10 * 1000000L };
	int i;

	if (inputactive)
		return;
	/* the WM's passive grab on the keybinding that signalled us stays
	 * active until its keys are released; retry like dmenu does */
	for (i = 0; i < 100; i++) {
		if (XGrabKeyboard(dpy, barwin, True, GrabModeAsync,
		                  GrabModeAsync, CurrentTime) == GrabSuccess) {
			inputactive = 1;
			drawbar();
			return;
		}
		nanosleep(&ts, NULL);
	}
}

static void
stopinput(void)
{
	if (!inputactive)
		return;
	inputactive = 0;
	XUngrabKeyboard(dpy, CurrentTime);
}

static void
keypress(XKeyEvent *kev)
{
	char c[8];
	KeySym ks = NoSymbol;
	size_t len = strlen(inputbuf);
	int n = XLookupString(kev, c, sizeof c, &ks, NULL);

	switch (ks) {
	case XK_Return:
	case XK_KP_Enter:
		stopinput();
		runinput();
		inputbuf[0] = '\0';
		drawbar();
		break;
	case XK_Escape:
		inputbuf[0] = '\0';
		stopinput();
		drawbar();
		break;
	case XK_BackSpace:
		if (len)
			inputbuf[len - 1] = '\0';
		drawbar();
		break;
	default:
		if (n != 1)
			break;
		if (c[0] == 0x15) /* ^U clears the line */
			inputbuf[0] = '\0';
		else if ((unsigned char)c[0] >= 0x20 && c[0] != 0x7f
		         && len < sizeof inputbuf - 1) {
			inputbuf[len] = c[0];
			inputbuf[len + 1] = '\0';
		} else
			break;
		drawbar();
	}
}

/* left click on the box focuses it; any other click unfocuses */
static void
buttonpress(XButtonEvent *bev)
{
	int bx = (int)hpad + wsw();

	if (bev->button == Button1 && bev->x >= bx
	    && bev->x < bx + (int)inputw) {
		startinput();
	} else if (inputactive) {
		stopinput();
		drawbar();
	}
}

static void
toggle(void)
{
	if (visible) {
		stopinput();
		XUnmapWindow(dpy, barwin);
		visible = 0;
	} else {
		updatedesktop();
		updatestatus();
		layout();
		XMapRaised(dpy, barwin);
		visible = 1;
	}
}

static void
dock(Window w)
{
	XEvent ev;

	if (w == None)
		return;
	XAddToSaveSet(dpy, w);
	XSelectInput(dpy, w, StructureNotifyMask);
	XReparentWindow(dpy, w, barwin, 0, 0);
	memset(&ev, 0, sizeof ev);
	ev.xclient.type = ClientMessage;
	ev.xclient.window = w;
	ev.xclient.message_type = xembed;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = CurrentTime;
	ev.xclient.data.l[1] = XEMBED_EMBEDDED_NOTIFY;
	ev.xclient.data.l[3] = (long)barwin;
	XSendEvent(dpy, w, False, NoEventMask, &ev);
	XMapWindow(dpy, w);
	arrput(icons, w);
	layout();
	if (visible)
		drawbar();
}

static int
iconindex(Window w)
{
	int i;

	for (i = 0; i < (int)arrlen(icons); i++)
		if (icons[i] == w)
			return i;
	return -1;
}

static void
undock(Window w)
{
	int i = iconindex(w);

	if (i < 0)
		return;
	arrdel(icons, i);
	layout();
	if (visible)
		drawbar();
}

static void
handle(XEvent *ev)
{
	switch (ev->type) {
	case ClientMessage:
		if (ev->xclient.message_type == trayopcode
		    && ev->xclient.data.l[1] == SYSTEM_TRAY_REQUEST_DOCK)
			dock((Window)ev->xclient.data.l[2]);
		break;
	case Expose:
		if (ev->xexpose.window == barwin && ev->xexpose.count == 0)
			drawbar();
		break;
	case KeyPress:
		if (inputactive)
			keypress(&ev->xkey);
		break;
	case ButtonPress:
		if (ev->xbutton.window == barwin)
			buttonpress(&ev->xbutton);
		break;
	case DestroyNotify:
		undock(ev->xdestroywindow.window);
		break;
	case ReparentNotify:
		if (ev->xreparent.parent != barwin)
			undock(ev->xreparent.window);
		break;
	case ConfigureNotify:
		/* icons that resize themselves get forced back into their slot */
		if (iconindex(ev->xconfigure.window) >= 0
		    && (ev->xconfigure.width != (int)iconsize
		        || ev->xconfigure.height != (int)iconsize))
			layout();
		/* keep the bar on top when other windows restack above it */
		else if (visible && ev->xconfigure.event == root
		         && ev->xconfigure.window != barwin)
			XRaiseWindow(dpy, barwin);
		break;
	case MapNotify:
		if (visible && ev->xmap.event == root
		    && ev->xmap.window != barwin)
			XRaiseWindow(dpy, barwin);
		break;
	case PropertyNotify:
		if (ev->xproperty.window == root
		    && ev->xproperty.atom == netcurdesk) {
			updatedesktop();
			layout();
			if (visible)
				drawbar();
		}
		break;
	case SelectionClear:
		if (ev->xselectionclear.window == selwin)
			die("htray: lost tray selection, exiting\n");
		break;
	}
}

static unsigned long
getcolor(const char *name)
{
	XColor c, dummy;

	if (!XAllocNamedColor(dpy, DefaultColormap(dpy, screen),
	                      name, &c, &dummy))
		die("htray: cannot allocate color\n");
	return c.pixel;
}

static void
setup(void)
{
	XSetWindowAttributes swa;
	struct sigaction sa;
	XEvent ev;
	char selname[32], fontpat[128];
	long orient = 0; /* horizontal */

	int di;

	loadconfig();
	if (!(dpy = XOpenDisplay(NULL)))
		die("htray: cannot open display\n");
	XSetErrorHandler(xerror);
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	haverandr = XRRQueryExtension(dpy, &di, &di);
	bgpx = getcolor(col_bg);
	fgpx = getcolor(col_fg);
	borderpx_col = getcolor(col_border);

	/* fontconfig name; a set fontsize overrides the pattern's size */
	if (fontsize)
		snprintf(fontpat, sizeof fontpat, "%s:pixelsize=%u",
		         fontname, fontsize);
	else
		snprintf(fontpat, sizeof fontpat, "%s", fontname);
	if (!(font = XftFontOpenName(dpy, screen, fontpat))
	    && !(font = XftFontOpenName(dpy, screen, "monospace")))
		die("htray: cannot load font\n");

	xembed = XInternAtom(dpy, "_XEMBED", False);
	manager = XInternAtom(dpy, "MANAGER", False);
	trayopcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
	trayorient = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
	netcurdesk = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
	snprintf(selname, sizeof selname, "_NET_SYSTEM_TRAY_S%d", screen);
	trayatom = XInternAtom(dpy, selname, False);

	if (XGetSelectionOwner(dpy, trayatom) != None)
		die("htray: another system tray is running\n");

	selwin = XCreateSimpleWindow(dpy, root, -1, -1, 1, 1, 0, 0, 0);
	XChangeProperty(dpy, selwin, trayorient, XA_CARDINAL, 32,
	                PropModeReplace, (unsigned char *)&orient, 1);
	XSetSelectionOwner(dpy, trayatom, selwin, CurrentTime);
	if (XGetSelectionOwner(dpy, trayatom) != selwin)
		die("htray: unable to acquire tray selection\n");

	swa.override_redirect = True;
	swa.background_pixel = bgpx;
	swa.border_pixel = borderpx_col;
	swa.event_mask = ExposureMask|SubstructureNotifyMask|ButtonPressMask
	                 |KeyPressMask;
	barwin = XCreateWindow(dpy, root, 0, 0, barh, barh, borderpx,
	                       CopyFromParent, CopyFromParent, CopyFromParent,
	                       CWOverrideRedirect|CWBackPixel|CWBorderPixel
	                       |CWEventMask, &swa);
	gc = XCreateGC(dpy, barwin, 0, NULL);
	XSetForeground(dpy, gc, fgpx);
	xd = XftDrawCreate(dpy, barwin, DefaultVisual(dpy, screen),
	                   DefaultColormap(dpy, screen));
	if (!XftColorAllocName(dpy, DefaultVisual(dpy, screen),
	                       DefaultColormap(dpy, screen), col_fg, &xftfg)
	    || !XftColorAllocName(dpy, DefaultVisual(dpy, screen),
	                          DefaultColormap(dpy, screen), col_border,
	                          &xftdim))
		die("htray: cannot allocate colors\n");

	/* tell waiting applets a tray is now available */
	memset(&ev, 0, sizeof ev);
	ev.xclient.type = ClientMessage;
	ev.xclient.window = root;
	ev.xclient.message_type = manager;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = CurrentTime;
	ev.xclient.data.l[1] = (long)trayatom;
	ev.xclient.data.l[2] = (long)selwin;
	XSendEvent(dpy, root, False, StructureNotifyMask, &ev);
	XSelectInput(dpy, root, PropertyChangeMask|SubstructureNotifyMask);

	findbattery();
	updatedesktop();
	updatestatus();
	layout();
	if (!starthidden) {
		XMapRaised(dpy, barwin);
		visible = 1;
	}

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = sighandler;
	sigemptyset(&sa.sa_mask); /* no SA_RESTART: select must wake up */
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	XSync(dpy, False);
}

static void
run(void)
{
	XEvent ev;
	fd_set fds;
	struct timeval tv;
	int nfds, nready, xfd = ConnectionNumber(dpy);

	for (;;) {
		while (XPending(dpy)) {
			XNextEvent(dpy, &ev);
			handle(&ev);
		}
		if (togglereq) {
			togglereq = 0;
			toggle();
			XSync(dpy, False);
			continue;
		}
		if (focusreq) {
			focusreq = 0;
			if (inputactive) {
				stopinput();
				drawbar();
			} else {
				if (!visible)
					toggle();
				XSync(dpy, False);
				startinput();
			}
			XSync(dpy, False);
			continue;
		}
		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		nfds = xfd;
		if (cmdfd >= 0) {
			FD_SET(cmdfd, &fds);
			if (cmdfd > nfds)
				nfds = cmdfd;
		}
		/* visible: poll every second so volume changes show up fast;
		 * hidden: wake just past the next minute boundary */
		tv.tv_sec = visible ? 1 : 60 - (long)(time(NULL) % 60);
		tv.tv_usec = 0;
		if ((nready = select(nfds + 1, &fds, NULL, NULL, &tv)) < 0) {
			if (errno == EINTR)
				continue;
			die("htray: select failed\n");
		}
		if (cmdfd >= 0 && FD_ISSET(cmdfd, &fds)) {
			cmdread();
			XSync(dpy, False);
		}
		if (nready == 0 && updatestatus() && visible) {
			layout();
			drawbar();
			XSync(dpy, False);
		}
	}
}

int
main(int argc, char *argv[])
{
	if (argc == 2 && !strcmp(argv[1], "-v")) {
		printf("htray %s\n", HTRAY_VERSION);
		return 0;
	}
	if (argc > 1)
		die("usage: htray [-v]\n");
	setup();
	run();
	return 0;
}
