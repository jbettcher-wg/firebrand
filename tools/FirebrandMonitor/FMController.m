/*
 * FMController.h
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#import "FMController.h"
#import "FMCardView.h"
#include <CoreServices/CoreServices.h>	/* kAEOpenApplication, keyAELaunchedAsLogInItem */

#define FM_VERSION	"1.2"
#define FM_NAME		@"Firebrand Monitor"
#define U(s)		[NSString stringWithUTF8String:(s)]
#define DOT		" \xc2\xb7 "
#define DEG		"\xc2\xb0"

static NSString *const kShowStatusItem = @"ShowStatusItem";
static NSString *const kLoadInMenuBar = @"ShowLoadInMenuBar";

static NSString *utf8(const char *s)
{
	NSString *n = s ? [NSString stringWithUTF8String:s] : nil;

	return n ? n : @"";
}

static NSString *clockShort(unsigned mhz)
{
	return mhz >= 1000 ? [NSString stringWithFormat:@"%.1f GHz", mhz / 1000.0] : [NSString stringWithFormat:@"%u MHz", mhz];
}

@implementation FMController

- (id)init
{
	if ((self = [super init])) {
		NSDictionary *d = [NSDictionary dictionaryWithObjectsAndKeys:
				   [NSNumber numberWithBool:YES], kShowStatusItem,
				   [NSNumber numberWithBool:YES], kLoadInMenuBar, nil];

		[[NSUserDefaults standardUserDefaults] registerDefaults:d];
		readingItems = [[NSMutableArray alloc] init];
		cardViews = [[NSMutableArray alloc] init];
	}
	return self;
}

/* --- menus ------------------------------------------------------------------------ */

static NSMenuItem *add(NSMenu *m, NSString *title, SEL action, NSString *key, id target)
{
	NSMenuItem *i = [m addItemWithTitle:title action:action keyEquivalent:key];

	if (target)
		[i setTarget:target];
	return i;
}

- (void)buildMainMenu
{
	NSMenu *bar = [[[NSMenu alloc] initWithTitle:@"MainMenu"] autorelease];
	NSMenu *app = [[[NSMenu alloc] initWithTitle:FM_NAME] autorelease];
	NSMenu *file = [[[NSMenu alloc] initWithTitle:@"File"] autorelease];
	NSMenu *edit = [[[NSMenu alloc] initWithTitle:@"Edit"] autorelease];
	NSMenu *win = [[[NSMenu alloc] initWithTitle:@"Window"] autorelease];
	NSMenuItem *i;

	add(app, @"About " FM_NAME, @selector(orderFrontStandardAboutPanel:), @"", nil);
	[app addItem:[NSMenuItem separatorItem]];
	appMenuBarItem = add(app, @"Show in Menu Bar", @selector(toggleStatusItem:), @"", self);
	appMenuLoadItem = add(app, @"Show Load in Menu Bar", @selector(toggleLoadInMenuBar:), @"", self);
	[app addItem:[NSMenuItem separatorItem]];
	add(app, @"Hide " FM_NAME, @selector(hide:), @"h", nil);
	i = add(app, @"Hide Others", @selector(hideOtherApplications:), @"h", nil);
	[i setKeyEquivalentModifierMask:NSAlternateKeyMask | NSCommandKeyMask];
	add(app, @"Show All", @selector(unhideAllApplications:), @"", nil);
	[app addItem:[NSMenuItem separatorItem]];
	add(app, @"Quit " FM_NAME, @selector(terminate:), @"q", nil);
	[[bar addItemWithTitle:FM_NAME action:nil keyEquivalent:@""] setSubmenu:app];

	i = add(file, @"Copy Report", @selector(copyReport:), @"C", self);
	[file addItem:[NSMenuItem separatorItem]];
	add(file, @"Close Window", @selector(performClose:), @"w", nil);
	[[bar addItemWithTitle:@"File" action:nil keyEquivalent:@""] setSubmenu:file];

	add(edit, @"Copy", @selector(copy:), @"c", nil);
	add(edit, @"Select All", @selector(selectAll:), @"a", nil);
	[[bar addItemWithTitle:@"Edit" action:nil keyEquivalent:@""] setSubmenu:edit];

	add(win, @"Minimize", @selector(performMiniaturize:), @"m", nil);
	add(win, @"Zoom", @selector(performZoom:), @"", nil);
	[win addItem:[NSMenuItem separatorItem]];
	add(win, FM_NAME, @selector(openWindow:), @"1", self);
	[win addItem:[NSMenuItem separatorItem]];
	add(win, @"Bring All to Front", @selector(arrangeInFront:), @"", nil);
	[[bar addItemWithTitle:@"Window" action:nil keyEquivalent:@""] setSubmenu:win];

	[NSApp setMainMenu:bar];
	/* Leopard takes the application menu from here when there is no nib */
	if ([NSApp respondsToSelector:@selector(setAppleMenu:)])
		[NSApp performSelector:@selector(setAppleMenu:) withObject:app];
	[NSApp setWindowsMenu:win];
}

- (void)buildStatusMenu
{
	NSFont *bold = [[NSFontManager sharedFontManager] convertFont:[NSFont menuFontOfSize:0] toHaveTrait:NSBoldFontMask];
	int i, k;

	if (!statusMenu) {
		statusMenu = [[NSMenu alloc] initWithTitle:FM_NAME];
		[statusMenu setAutoenablesItems:NO];
	}
	while ([statusMenu numberOfItems])
		[statusMenu removeItemAtIndex:0];
	[readingItems removeAllObjects];
	for (i = 0; i < ncards; i++) {
		char name[96];
		NSMenuItem *t = [statusMenu addItemWithTitle:@"" action:nil keyEquivalent:@""];

		frb_card_short_name(&cards[i], name, sizeof(name));
		[t setAttributedTitle:[[[NSAttributedString alloc] initWithString:utf8(name)
			attributes:[NSDictionary dictionaryWithObject:bold forKey:NSFontAttributeName]] autorelease]];
		[t setEnabled:YES];
		/* four reading lines a card: clocks, load, temperatures, power */
		for (k = 0; k < 4; k++) {
			NSMenuItem *r = [statusMenu addItemWithTitle:@"" action:nil keyEquivalent:@""];

			[r setEnabled:YES];
			[r setIndentationLevel:1];
			[readingItems addObject:r];
		}
		[statusMenu addItem:[NSMenuItem separatorItem]];
	}
	if (!ncards) {
		[[statusMenu addItemWithTitle:@"No Firebrand card found" action:nil keyEquivalent:@""] setEnabled:NO];
		[statusMenu addItem:[NSMenuItem separatorItem]];
	}
	add(statusMenu, [@"Open " FM_NAME stringByAppendingString:U("\xe2\x80\xa6")], @selector(openWindow:), @"", self);
	[add(statusMenu, @"Show in Menu Bar", @selector(toggleStatusItem:), @"", self) setState:NSOnState];
	statusMenuLoadItem = add(statusMenu, @"Show Load in Menu Bar", @selector(toggleLoadInMenuBar:), @"", self);
	[statusMenuLoadItem setState:[[NSUserDefaults standardUserDefaults] boolForKey:kLoadInMenuBar] ? NSOnState : NSOffState];
	[statusMenu addItem:[NSMenuItem separatorItem]];
	add(statusMenu, @"Quit " FM_NAME, @selector(terminate:), @"", NSApp);
}

/* --- the status item ------------------------------------------------------------------ */

/* the first card with a temperature and a clock: "48° 1.2 GHz", and its load with the preference: "48° 1.2 GHz 97%" */
- (NSString *)statusTitle
{
	BOOL withLoad = [[NSUserDefaults standardUserDefaults] boolForKey:kLoadInMenuBar];
	int i;

	for (i = 0; i < ncards; i++) {
		const struct frb_card *c = &cards[i];
		const struct frb_user_status *st = &c->st;
		BOOL t = frb_card_has(c, FRB_ST_TEMP) || frb_card_has(c, FRB_ST_TEMP_HOTSPOT);
		int deg = frb_card_has(c, FRB_ST_TEMP) ? st->temp_mc / 1000 : st->temp_hotspot_mc / 1000;

		if (!t && !frb_card_has(c, FRB_ST_CLOCKS))
			continue;
		return [NSString stringWithFormat:@"%@ %@%@",
			t ? [NSString stringWithFormat:U("%d" DEG), deg] : U("\xe2\x80\x93" DEG),
			frb_card_has(c, FRB_ST_CLOCKS) ? clockShort(st->gfx_mhz) : U("\xe2\x80\x93"),
			!withLoad ? @"" : frb_card_has(c, FRB_ST_LOAD) ? [NSString stringWithFormat:@" %u%%", st->gpu_load] :
			U(" \xe2\x80\x93%")];
	}
	return ncards ? U("\xe2\x80\x93" DEG " \xe2\x80\x93") : @"Firebrand";
}

/* fixed: the widest title, so the menu bar does not shift as the numbers change */
- (float)statusLength
{
	NSDictionary *a = [NSDictionary dictionaryWithObject:[NSFont menuBarFontOfSize:0] forKey:NSFontAttributeName];
	BOOL withLoad = [[NSUserDefaults standardUserDefaults] boolForKey:kLoadInMenuBar];

	return [(withLoad ? U("188" DEG " 8.8 GHz 100%") : U("188" DEG " 8.8 GHz")) sizeWithAttributes:a].width + 14;
}

- (void)addStatusItem
{
	if (statusItem)
		return;
	statusItem = [[[NSStatusBar systemStatusBar] statusItemWithLength:[self statusLength]] retain];
	[statusItem setHighlightMode:YES];
	[self buildStatusMenu];
	[statusItem setMenu:statusMenu];
	[statusItem setTitle:[self statusTitle]];
}

- (void)removeStatusItem
{
	if (!statusItem)
		return;
	[[NSStatusBar systemStatusBar] removeStatusItem:statusItem];
	[statusItem release];
	statusItem = nil;
}

- (void)syncMenuBarControls
{
	BOOL on = statusItem != nil;

	BOOL load = [[NSUserDefaults standardUserDefaults] boolForKey:kLoadInMenuBar];

	[appMenuBarItem setState:on ? NSOnState : NSOffState];
	[menuBarBox setState:on ? NSOnState : NSOffState];
	[appMenuLoadItem setState:load ? NSOnState : NSOffState];
	[statusMenuLoadItem setState:load ? NSOnState : NSOffState];
	[menuLoadBox setState:load ? NSOnState : NSOffState];
	[menuLoadBox setEnabled:on];
}

- (IBAction)toggleLoadInMenuBar:(id)sender
{
	BOOL on = ![[NSUserDefaults standardUserDefaults] boolForKey:kLoadInMenuBar];

	if (sender == menuLoadBox)
		on = [menuLoadBox state] == NSOnState;
	[[NSUserDefaults standardUserDefaults] setBool:on forKey:kLoadInMenuBar];
	if (statusItem) {
		[statusItem setLength:[self statusLength]];
		[statusItem setTitle:[self statusTitle]];
	}
	[self syncMenuBarControls];
}

- (IBAction)toggleStatusItem:(id)sender
{
	BOOL on = statusItem == nil;

	if (sender == menuBarBox)
		on = [menuBarBox state] == NSOnState;
	[[NSUserDefaults standardUserDefaults] setBool:on forKey:kShowStatusItem];
	if (on) {
		[self addStatusItem];
	} else {
		[self removeStatusItem];
		/* without the menu-bar item the window is the way back */
		if (![window isVisible])
			[self openWindow:nil];
	}
	[self syncMenuBarControls];
}

- (void)updateStatusMenu
{
	int i, k = 0;

	for (i = 0; i < ncards && k + 3 < (int)[readingItems count]; i++, k += 4) {
		const struct frb_card *c = &cards[i];
		const struct frb_user_status *st = &c->st;
		NSMutableString *t = [NSMutableString string], *p = [NSMutableString string];

		[[readingItems objectAtIndex:k] setTitle:frb_card_has(c, FRB_ST_CLOCKS) ?
			[NSString stringWithFormat:U("GPU %u MHz" DOT "memory %u MHz"), st->gfx_mhz, st->mem_mhz] :
			(c->have_status ? @"Clocks not available" : @"No live readings (driver without the status interface)")];
		if (frb_card_has(c, FRB_ST_TEMP))
			[t appendFormat:U("%d " DEG "C edge"), (int)(st->temp_mc / 1000)];
		if (frb_card_has(c, FRB_ST_TEMP_HOTSPOT))
			[t appendFormat:U("%@%d " DEG "C hotspot"), [t length] ? U(DOT) : @"", (int)(st->temp_hotspot_mc / 1000)];
		if (frb_card_has(c, FRB_ST_TEMP_MEM))
			[t appendFormat:U("%@%d " DEG "C memory"), [t length] ? U(DOT) : @"", (int)(st->temp_mem_mc / 1000)];
		[[readingItems objectAtIndex:k + 1] setTitle:frb_card_has(c, FRB_ST_LOAD) ?
			[NSString stringWithFormat:U("Load %u %%" DOT "memory controller %u %%" DOT "SMU average"), st->gpu_load,
			 st->mem_load] : @"Load not available"];
		[[readingItems objectAtIndex:k + 2] setTitle:[t length] ? t : @"Temperature not available"];
		if (frb_card_has(c, FRB_ST_POWER))
			[p appendFormat:@"%.1f W", st->power_mw / 1000.0];
		if (frb_card_has(c, FRB_ST_UVD) && st->uvd_state == FRB_UVD_STATE_RUNNING)
			[p appendFormat:U("%@UVD decoding"), [p length] ? U(DOT) : @""];
		[[readingItems objectAtIndex:k + 3] setTitle:[p length] ? p :
			[NSString stringWithFormat:@"Stage %@", c->have_status ? utf8(frb_stage_label(st->stage_done)) : utf8(c->stage)]];
	}
}

/* --- the window ------------------------------------------------------------------------- */

- (void)buildWindow
{
	NSRect r = NSMakeRect(0, 0, 780, 720);
	NSView *content;
	float w = r.size.width, h = r.size.height;

	window = [[NSWindow alloc] initWithContentRect:r
					     styleMask:NSTitledWindowMask | NSClosableWindowMask | NSMiniaturizableWindowMask |
						       NSResizableWindowMask
					       backing:NSBackingStoreBuffered defer:NO];
	[window setTitle:FM_NAME];
	[window setMinSize:NSMakeSize(640, 600)];
	[window setReleasedWhenClosed:NO];
	[window setDelegate:self];
	content = [window contentView];

	copyButton = [[[NSButton alloc] initWithFrame:NSMakeRect(w - 14 - 140, 10, 140, 30)] autorelease];
	[copyButton setTitle:@"Copy Report"];
	[copyButton setBezelStyle:NSRoundedBezelStyle];
	[copyButton setTarget:self];
	[copyButton setAction:@selector(copyReport:)];
	[copyButton setAutoresizingMask:NSViewMinXMargin | NSViewMaxYMargin];
	[content addSubview:copyButton];

	menuBarBox = [[[NSButton alloc] initWithFrame:NSMakeRect(14, 15, 130, 20)] autorelease];
	[menuBarBox setButtonType:NSSwitchButton];
	[menuBarBox setTitle:@"Show in menu bar"];
	[menuBarBox setTarget:self];
	[menuBarBox setAction:@selector(toggleStatusItem:)];
	[menuBarBox setAutoresizingMask:NSViewMaxXMargin | NSViewMaxYMargin];
	[content addSubview:menuBarBox];

	menuLoadBox = [[[NSButton alloc] initWithFrame:NSMakeRect(146, 15, 90, 20)] autorelease];
	[menuLoadBox setButtonType:NSSwitchButton];
	[menuLoadBox setTitle:@"with load"];
	[menuLoadBox setTarget:self];
	[menuLoadBox setAction:@selector(toggleLoadInMenuBar:)];
	[menuLoadBox setAutoresizingMask:NSViewMaxXMargin | NSViewMaxYMargin];
	[content addSubview:menuLoadBox];

	footLabel = [[[NSTextField alloc] initWithFrame:NSMakeRect(244, 17, w - 244 - 170, 16)] autorelease];
	[footLabel setBezeled:NO];
	[footLabel setDrawsBackground:NO];
	[footLabel setEditable:NO];
	[footLabel setSelectable:NO];
	[footLabel setFont:[NSFont systemFontOfSize:11]];
	[footLabel setTextColor:[NSColor colorWithCalibratedWhite:0.4 alpha:1]];
	[[footLabel cell] setLineBreakMode:NSLineBreakByTruncatingTail];
	[footLabel setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
	[content addSubview:footLabel];

	cardHolder = [[[NSView alloc] initWithFrame:NSMakeRect(0, 48, w, h - 48)] autorelease];
	[cardHolder setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
	[content addSubview:cardHolder];

	[window center];
	[window setFrameUsingName:@"FirebrandMonitor"];
	[window setFrameAutosaveName:@"FirebrandMonitor"];
}

- (void)rebuildCards
{
	NSRect hb = [cardHolder bounds];
	int i;

	[tabs removeFromSuperview];
	tabs = nil;
	[noCardLabel removeFromSuperview];
	noCardLabel = nil;
	[cardViews removeAllObjects];
	if (!ncards) {
		noCardLabel = [[[NSTextField alloc] initWithFrame:NSMakeRect(20, hb.size.height / 2 - 30, hb.size.width - 40, 40)]
			       autorelease];
		[noCardLabel setBezeled:NO];
		[noCardLabel setDrawsBackground:NO];
		[noCardLabel setEditable:NO];
		[noCardLabel setAlignment:NSCenterTextAlignment];
		[noCardLabel setFont:[NSFont systemFontOfSize:13]];
		[noCardLabel setStringValue:@"No Firebrand card found.\nFirebrand.kext is not loaded, or it drives no card in this Mac."];
		[noCardLabel setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin | NSViewMaxYMargin];
		[cardHolder addSubview:noCardLabel];
	} else {
		tabs = [[[NSTabView alloc] initWithFrame:ncards > 1 ? NSInsetRect(hb, 8, 0) : hb] autorelease];
		[tabs setTabViewType:ncards > 1 ? NSTopTabsBezelBorder : NSNoTabsNoBorder];
		[tabs setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
		for (i = 0; i < ncards; i++) {
			FMCardView *v = [[[FMCardView alloc] initWithFrame:[tabs contentRect]] autorelease];
			NSTabViewItem *ti = [[[NSTabViewItem alloc] initWithIdentifier:[NSNumber numberWithInt:i]] autorelease];
			char name[96];

			frb_card_short_name(&cards[i], name, sizeof(name));
			[ti setLabel:utf8(name)];
			[ti setView:v];
			[tabs addTabViewItem:ti];
			[cardViews addObject:v];
		}
		[cardHolder addSubview:tabs];
	}
	if (statusItem) {
		[self buildStatusMenu];
		[statusItem setMenu:statusMenu];
	}
}

- (void)updateFoot
{
	int i, live = 0, old = 0;
	unsigned age = 0;

	for (i = 0; i < ncards; i++) {
		if (cards[i].have_status) {
			live++;
			age = cards[i].st.sensor_age_ms;
		} else {
			old++;
		}
	}
	for (i = 0; i < ncards; i++)
		if (frb_card_support(&cards[i]) == FRB_SUPPORT_UNTESTED || frb_card_support(&cards[i]) == FRB_SUPPORT_NOT_PORTED)
			break;
	if (!ncards)
		[footLabel setStringValue:@"Looking for Firebrand cards every 5 seconds"];
	else if (i < ncards)
		/* an untested card: what to do about it, in the footer the user sees */
		[footLabel setStringValue:@"Untested card: please send Copy Report as a GitHub issue"];
	else if (old && !live)
		[footLabel setStringValue:@"This Firebrand has no status interface: names and stage only"];
	else
		[footLabel setStringValue:[NSString stringWithFormat:U("%d card%s" DOT "updated every second" DOT "sample %.1f s old"),
				    ncards, ncards == 1 ? "" : "s", age / 1000.0]];
}

- (IBAction)openWindow:(id)sender
{
	[NSApp activateIgnoringOtherApps:YES];
	[window makeKeyAndOrderFront:nil];
}

/* --- the report --------------------------------------------------------------------------- */

- (void)restoreCopyButton
{
	[copyButton setTitle:@"Copy Report"];
	[self updateFoot];
}

- (IBAction)copyReport:(id)sender
{
	size_t need = frb_report(cards, ncards, "Firebrand Monitor " FM_VERSION, 1, 0, 0);
	char *text = (char *)malloc(need + 1);
	NSPasteboard *pb = [NSPasteboard generalPasteboard];
	NSString *s;

	if (!text)
		return;
	frb_report(cards, ncards, "Firebrand Monitor " FM_VERSION, 1, text, need + 1);
	s = utf8(text);
	free(text);
	[pb declareTypes:[NSArray arrayWithObject:NSStringPboardType] owner:nil];
	[pb setString:s forType:NSStringPboardType];
	[copyButton setTitle:@"Copied"];
	[footLabel setStringValue:[NSString stringWithFormat:@"Report copied (%u lines): paste it into a GitHub issue",
			    (unsigned)[[s componentsSeparatedByString:@"\n"] count] - 1]];
	[NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(restoreCopyButton) object:nil];
	[self performSelector:@selector(restoreCopyButton) withObject:nil afterDelay:4.0];
}

/* --- once a second -------------------------------------------------------------------------- */

static BOOL sameCards(const struct frb_card *a, int na, const struct frb_card *b, int nb)
{
	int i;

	if (na != nb)
		return NO;
	for (i = 0; i < na; i++)
		if (a[i].bus != b[i].bus || a[i].vendor != b[i].vendor || a[i].device != b[i].device ||
		    a[i].have_status != b[i].have_status)
			return NO;
	return YES;
}

/* the cards again (a driver loaded, unloaded or replaced): the views only when the set changed */
- (void)rescan
{
	struct frb_card before[FRB_MAX_CARDS];
	int n = ncards;

	memcpy(before, cards, sizeof(before));
	frb_cards_close(cards, ncards);
	ncards = frb_cards_open(cards, FRB_MAX_CARDS);
	if (!sameCards(before, n, cards, ncards))
		[self rebuildCards];
}

- (void)tick:(NSTimer *)t
{
	BOOL lost = NO;
	int i;

	ticks++;
	for (i = 0; i < ncards; i++)
		if (frb_card_refresh(&cards[i]) != KERN_SUCCESS && cards[i].conn)
			lost = YES;
	if ((!ncards || lost) && ticks - lastRescan >= 5) {
		lastRescan = ticks;
		[self rescan];
	}
	for (i = 0; i < ncards && i < (int)[cardViews count]; i++)
		[[cardViews objectAtIndex:i] update:&cards[i]];
	if (statusItem) {
		[statusItem setTitle:[self statusTitle]];
		[self updateStatusMenu];
	}
	if (![[copyButton title] isEqual:@"Copied"])
		[self updateFoot];
}

/* --- the application ---------------------------------------------------------------------------- */

/*
 * Launched by loginwindow as a login item (its open-application event
 * carries keyAELaunchedAsLogInItem), or hidden (a login item with Hide, an
 * open -j): then no window, only the Dock icon and the menu-bar item. The
 * window comes with a click on the Dock icon (the reopen event) or the
 * status menu's "Open Firebrand Monitor...". A double-click shows it.
 */
- (BOOL)launchedQuietly
{
	NSAppleEventDescriptor *e = [[NSAppleEventManager sharedAppleEventManager] currentAppleEvent];

	if ([NSApp isHidden])
		return YES;
	return e && [e eventClass] == kCoreEventClass && [e eventID] == kAEOpenApplication &&
	       [[e paramDescriptorForKeyword:keyAEPropData] enumCodeValue] == keyAELaunchedAsLogInItem;
}

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
	NSUserDefaults *d = [NSUserDefaults standardUserDefaults];

	ncards = frb_cards_open(cards, FRB_MAX_CARDS);
	[self buildWindow];
	[self rebuildCards];
	if ([d boolForKey:kShowStatusItem])
		[self addStatusItem];
	[self syncMenuBarControls];
	[self tick:nil];
	timer = [[NSTimer timerWithTimeInterval:1.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES] retain];
	/* common modes: the readings move on while a menu is open too */
	[[NSRunLoop currentRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
	if (![self launchedQuietly])
		[self openWindow:nil];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)visible
{
	if (!visible)
		[self openWindow:nil];
	return YES;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
	return NO;
}

- (void)applicationWillTerminate:(NSNotification *)n
{
	[timer invalidate];
	[self removeStatusItem];
	frb_cards_close(cards, ncards);
	ncards = 0;
	[[NSUserDefaults standardUserDefaults] synchronize];
}

@end
