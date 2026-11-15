/*
 * Firebrand Monitor: live clocks, temperatures, power and the driver's state
 * for every Firebrand card, in a window and the menu bar. Read-only and for
 * any user: it talks to the driver's 'frbs' status client (frb_user.h), never
 * to the administrator's control client; every change stays in frbctl.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#import <Cocoa/Cocoa.h>
#import "FMController.h"

int main(int argc, char **argv)
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	FMController *c;

	[NSApplication sharedApplication];
	c = [[FMController alloc] init];
	[NSApp setDelegate:c];
	[c buildMainMenu];
	[NSApp run];
	[c release];
	[pool release];
	return 0;
}
