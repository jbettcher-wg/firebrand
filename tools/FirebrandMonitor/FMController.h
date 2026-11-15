/*
 * Firebrand Monitor: the application. Once a second it asks every Firebrand
 * card for its status (tools/frb_status.c: the driver's read-only 'frbs'
 * client, no root needed), moves the window's graphs along, redraws the
 * status list and the menu-bar item ("48° 1.2 GHz") and its menu.
 * Started at login (a login item, hidden) it shows no window until the Dock
 * icon or the status menu asks for one; a double-click launch shows it.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#import <Cocoa/Cocoa.h>
#include "frb_status.h"

@interface FMController : NSObject
{
	struct frb_card cards[FRB_MAX_CARDS];
	int ncards;
	int ticks, lastRescan;
	NSTimer *timer;
	NSStatusItem *statusItem;
	NSMenu *statusMenu;
	NSMutableArray *readingItems;	/* the status menu's reading lines */
	NSWindow *window;
	NSView *cardHolder;		/* the window's area above the bottom bar */
	NSTabView *tabs;
	NSMutableArray *cardViews;
	NSTextField *noCardLabel, *footLabel;
	NSButton *copyButton, *menuBarBox, *menuLoadBox;
	NSMenuItem *appMenuBarItem;	/* Firebrand Monitor > Show in Menu Bar */
	NSMenuItem *appMenuLoadItem, *statusMenuLoadItem;	/* ... > Show Load in Menu Bar */
}

- (void)buildMainMenu;
- (IBAction)openWindow:(id)sender;
- (IBAction)copyReport:(id)sender;
- (IBAction)toggleStatusItem:(id)sender;
- (IBAction)toggleLoadInMenuBar:(id)sender;
@end
