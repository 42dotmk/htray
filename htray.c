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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#define STB_DS_IMPLEMENTATION
#include <stb_ds.h>

#define SYSTEM_TRAY_REQUEST_DOCK 0
#define XEMBED_EMBEDDED_NOTIFY 0

/* configuration */
static const unsigned int barh     = 32;         /* bar height in px */
static const unsigned int borderpx = 2;          /* border width */
static const unsigned int padding  = 0;         /* gap from screen corner */
static const unsigned int hpad     = 10;         /* inner horizontal padding */
static const unsigned int iconsize = 22;         /* tray icon side in px */
static const unsigned int spacing  = 8;          /* gap between icons/text */
static const char col_bg[]         = "#1a1b26";
static const char col_fg[]         = "#7aa2f7";
static const char col_border[]     = "#3b4261";
static const char fontname[]       = "10x20";    /* "fixed" as fallback */
static const char timefmt[]        = "%d(%a) %H:%M"; /* strftime clock format */
static const int starthidden       = 0;
static const int atbottom          = 1;          /* anchor to bottom edge */
static const unsigned int batw     = 18;         /* battery icon body width */
static const unsigned int bath     = 10;         /* battery icon body height */
static const unsigned int batnubw  = 2;          /* battery nub width */
static const unsigned int batnubh  = 4;          /* battery nub height */
static const unsigned int spkw     = 15;         /* speaker icon width */
static const unsigned int spkh     = 10;         /* speaker icon height */
static const unsigned int micw     = 9;          /* mic icon width */
static const unsigned int mich     = 12;         /* mic icon height */
static const unsigned int icgap    = 5;          /* gap between icon and text */
static const unsigned int divh     = 16;         /* divider height */
static const unsigned int memw     = 14;         /* mem chip body width */
static const unsigned int memh     = 13;         /* mem chip height incl pins */
static const unsigned int cpuw     = 13;         /* cpu chip width incl pins */
static const unsigned int cpuh     = 13;         /* cpu chip height incl pins */
/* reserved readout widths so the bar doesn't resize as values change */
static const char cputmpl[]        = "00%";
static const char memtmpl[]        = "00.0G";
static const char voltmpl[]        = "00%";
static const char mictmpl[]        = "00%";

static Display *dpy;
static int screen, sw, sh;
static Window root, barwin, selwin;
static GC gc;
static XFontStruct *font;
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
static volatile sig_atomic_t togglereq;

static void
die(const char *msg)
{
	fputs(msg, stderr);
	exit(1);
}

/* tray icons come and go at will; ignore errors from vanished windows */
static int
xerror(Display *d, XErrorEvent *ee)
{
	(void)d; (void)ee;
	return 0;
}

static void
sigusr1(int sig)
{
	(void)sig;
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

/* parse "Volume: 0.90 [MUTED]" from wpctl; -1 if unavailable */
static int
getvolume(const char *target, int *muted)
{
	char cmd[64], line[64];
	FILE *p;
	float v;
	int pct = -1;

	*muted = 0;
	snprintf(cmd, sizeof cmd, "wpctl get-volume %s 2>/dev/null", target);
	if (!(p = popen(cmd, "r")))
		return -1;
	if (fgets(line, sizeof line, p)
	    && sscanf(line, "Volume: %f", &v) == 1) {
		pct = (int)(v * 100.0f + 0.5f);
		if (pct > 999)
			pct = 999;
		if (strstr(line, "MUTED"))
			*muted = 1;
	}
	pclose(p);
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
	long mt = 0, ma = 0, tenths;
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
		       && (!mt || !ma)) {
			sscanf(path, "MemTotal: %ld kB", &mt);
			sscanf(path, "MemAvailable: %ld kB", &ma);
		}
		fclose(f);
	}
	if (mt > 0 && ma > 0 && ma <= mt) {
		tenths = (mt - ma) * 10 / 1048576; /* tenths of GiB */
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
	return s[0] ? XTextWidth(font, s, (int)strlen(s)) : 0;
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

/* recompute bar geometry and slot every icon; safe to call while hidden */
static void
layout(void)
{
	int i, x, tw, n = (int)arrlen(icons);

	tw = statusw();
	barw = (int)(2 * hpad) + wsw() + n * (int)(iconsize + spacing) + tw;
	if (n && !tw)
		barw -= (int)spacing;
	if (barw < (int)barh)
		barw = (int)barh;
	XMoveResizeWindow(dpy, barwin,
	                  sw - barw - 2 * (int)borderpx - (int)padding,
	                  atbottom ? sh - (int)barh - 2 * (int)borderpx
	                             - (int)padding
	                           : (int)padding,
	                  (unsigned int)barw, barh);
	x = (int)hpad + wsw();
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
	int x, w, ty = ((int)barh + font->ascent - font->descent) / 2;

	XClearWindow(dpy, barwin);
	if (desktxt[0]) {
		x = (int)hpad;
		XDrawString(dpy, barwin, gc, x, ty, desktxt,
		            (int)strlen(desktxt));
		x += textw(desktxt) + (int)spacing;
		XSetForeground(dpy, gc, borderpx_col);
		XDrawLine(dpy, barwin, gc, x, ((int)barh - (int)divh) / 2,
		          x, ((int)barh + (int)divh) / 2);
		XSetForeground(dpy, gc, fgpx);
	}
	x = barw - (int)hpad - statusw();
	if (cpupct >= 0) {
		drawcpu(x, ((int)barh - (int)cpuh) / 2);
		x += (int)(cpuw + icgap);
		w = slotw(cputxt, cputmpl);
		XDrawString(dpy, barwin, gc, x + w - textw(cputxt), ty,
		            cputxt, (int)strlen(cputxt));
		x += w + (int)spacing;
	}
	if (memtxt[0]) {
		drawmem(x, ((int)barh - (int)memh) / 2);
		x += (int)(memw + icgap);
		w = slotw(memtxt, memtmpl);
		XDrawString(dpy, barwin, gc, x + w - textw(memtxt), ty,
		            memtxt, (int)strlen(memtxt));
		x += w + (int)spacing;
	}
	if (volpct >= 0) {
		drawspeaker(x, ((int)barh - (int)spkh) / 2, volmute);
		x += (int)(spkw + icgap);
		w = slotw(voltxt, voltmpl);
		XDrawString(dpy, barwin, gc, x + w - textw(voltxt), ty,
		            voltxt, (int)strlen(voltxt));
		x += w + (int)spacing;
	}
	if (micpct >= 0) {
		drawmic(x, ((int)barh - (int)mich) / 2, micmute);
		x += (int)(micw + icgap);
		w = slotw(mictxt, mictmpl);
		XDrawString(dpy, barwin, gc, x + w - textw(mictxt), ty,
		            mictxt, (int)strlen(mictxt));
		x += w + (int)spacing;
	}
	if (batcap >= 0) {
		drawbattery(x, ((int)barh - (int)bath) / 2);
		x += (int)(batw + batnubw + icgap);
		XDrawString(dpy, barwin, gc, x, ty, battxt,
		            (int)strlen(battxt));
		x += textw(battxt) + (int)spacing;
	}
	if (clockstr[0])
		XDrawString(dpy, barwin, gc, x, ty, clockstr,
		            (int)strlen(clockstr));
}

static void
toggle(void)
{
	if (visible) {
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
	char selname[32];
	long orient = 0; /* horizontal */

	if (!(dpy = XOpenDisplay(NULL)))
		die("htray: cannot open display\n");
	XSetErrorHandler(xerror);
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	sw = DisplayWidth(dpy, screen);
	sh = DisplayHeight(dpy, screen);
	bgpx = getcolor(col_bg);
	fgpx = getcolor(col_fg);
	borderpx_col = getcolor(col_border);

	if (!(font = XLoadQueryFont(dpy, fontname))
	    && !(font = XLoadQueryFont(dpy, "fixed")))
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
	swa.event_mask = ExposureMask|SubstructureNotifyMask;
	barwin = XCreateWindow(dpy, root, 0, 0, barh, barh, borderpx,
	                       CopyFromParent, CopyFromParent, CopyFromParent,
	                       CWOverrideRedirect|CWBackPixel|CWBorderPixel
	                       |CWEventMask, &swa);
	gc = XCreateGC(dpy, barwin, 0, NULL);
	XSetFont(dpy, gc, font->fid);
	XSetForeground(dpy, gc, fgpx);

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
	XSelectInput(dpy, root, PropertyChangeMask);

	findbattery();
	updatedesktop();
	updatestatus();
	layout();
	if (!starthidden) {
		XMapRaised(dpy, barwin);
		visible = 1;
	}

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = sigusr1;
	sigemptyset(&sa.sa_mask); /* no SA_RESTART: select must wake up */
	sigaction(SIGUSR1, &sa, NULL);
	XSync(dpy, False);
}

static void
run(void)
{
	XEvent ev;
	fd_set fds;
	struct timeval tv;
	int xfd = ConnectionNumber(dpy);

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
		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		/* visible: poll every second so volume changes show up fast;
		 * hidden: wake just past the next minute boundary */
		tv.tv_sec = visible ? 1 : 60 - (long)(time(NULL) % 60);
		tv.tv_usec = 0;
		if (select(xfd + 1, &fds, NULL, NULL, &tv) < 0) {
			if (errno == EINTR)
				continue;
			die("htray: select failed\n");
		}
		if (!FD_ISSET(xfd, &fds) && updatestatus() && visible) {
			layout();
			drawbar();
			XSync(dpy, False);
		}
	}
}

int
main(void)
{
	setup();
	run();
	return 0;
}
