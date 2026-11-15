/*
 * Firebrand Monitor: one card's page. Its name and ids, four graphs (GPU
 * clock, memory clock, temperature, power) and the status list, whose lines
 * are frb_card_rows' (tools/frb_status.c), the same as frbctl report's.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#import <Cocoa/Cocoa.h>
#include "frb_status.h"

@class FMGraphView;

@interface FMCardView : NSView
{
	NSTextField *nameLabel, *idLabel;
	FMGraphView *gfx, *mem, *temp, *power;
	NSTableView *table;
	NSMutableArray *rows;		/* NSArray: key, value, header flag */
}

- (id)initWithFrame:(NSRect)frame;
/* a new reading (once a second): the graphs move on, the list is redrawn */
- (void)update:(const struct frb_card *)card;
@end
