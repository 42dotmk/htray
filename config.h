/* htray configuration. Every setting in the first block can also be
 * overridden at startup by the environment variable named in its
 * comment (numbers decimal, flags 0/1); see loadconfig(). Unset
 * variables leave these defaults in effect. */

static unsigned int barh = 30;     /* bar height in px, HTRAY_HEIGHT */
static unsigned int borderpx = 2;  /* border width, HTRAY_BORDERPX */
static unsigned int padding = 0;   /* gap from screen corner, HTRAY_PADDING */
static unsigned int hpad = 10;     /* inner horizontal padding, HTRAY_HPAD */
static unsigned int iconsize = 18; /* tray icon side in px, HTRAY_ICONSIZE */
static unsigned int spacing = 8;   /* gap between icons/text, HTRAY_SPACING */
static const char *col_bg = "#1a1b26"; /* background, HTRAY_BG */
static const char *col_fg = "#7aa2f7"; /* text and icons, HTRAY_FG */
static const char *col_border = "#3b4261"; /* border and dividers, HTRAY_BORDER */
static const char *fontname = "Iosevka NFM"; /* fontconfig name/pattern, HTRAY_FONT */
static unsigned int fontsize = 16; /* px size appended to the pattern; 0 = pattern's own, HTRAY_FONTSIZE */
static const char *timefmt = "%d(%a) %H:%M"; /* strftime clock format, HTRAY_TIMEFMT */
static unsigned int cmdtimeout =     500; /* ms before readcmd kills, HTRAY_CMDTIMEOUT */
static unsigned int inputw = 180; /* command input box width, HTRAY_INPUTW */
static int starthidden = 0; /* start with the bar unmapped, HTRAY_HIDDEN */
static int atbottom = 1;    /* anchor to bottom edge, HTRAY_BOTTOM */

/* hand-drawn status icon dimensions; the draw* functions hardcode their
 * strokes to these sizes, so they are compile-time only */
static const unsigned int batw = 18;   /* battery icon body width */
static const unsigned int bath = 10;   /* battery icon body height */

static const unsigned int batnubw = 2; /* battery nub width */
static const unsigned int batnubh = 4; /* battery nub height */

static const unsigned int spkw = 15;   /* speaker icon width */
static const unsigned int spkh = 10;   /* speaker icon height */

static const unsigned int micw = 9;    /* mic icon width */
static const unsigned int mich = 12;   /* mic icon height */

static const unsigned int memw = 14;   /* mem chip body width */
static const unsigned int memh = 13;   /* mem chip height incl pins */

static const unsigned int cpuw = 13;   /* cpu chip width incl pins */
static const unsigned int cpuh = 13;   /* cpu chip height incl pins */

static const unsigned int icgap = 5;   /* gap between icon and text */
static const unsigned int divh = 16;   /* divider height */


/* reserved readout widths so the bar doesn't resize as values change */
static const char cputmpl[] = "00%";
static const char memtmpl[] = "00.0G";
static const char voltmpl[] = "00%";
static const char mictmpl[] = "00%";
