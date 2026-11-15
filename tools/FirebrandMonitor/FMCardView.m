/*
 * FMCardView.h
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#import "FMCardView.h"
#import "FMGraphView.h"

static NSTextField *label(NSRect r, NSFont *font, NSColor *color)
{
	NSTextField *t = [[NSTextField alloc] initWithFrame:r];

	[t setBezeled:NO];
	[t setDrawsBackground:NO];
	[t setEditable:NO];
	[t setSelectable:YES];
	[t setFont:font];
	[t setTextColor:color];
	[[t cell] setLineBreakMode:NSLineBreakByTruncatingTail];
	return [t autorelease];
}

static NSColor *rgb(int r, int g, int b)
{
	return [NSColor colorWithCalibratedRed:r / 255.0 green:g / 255.0 blue:b / 255.0 alpha:1];
}

/* text with non-ASCII characters is written as UTF-8 C strings (\xc2\xb0 degree, \xc2\xb7 middle dot) */
#define U(s)	[NSString stringWithUTF8String:(s)]
#define DOT	" \xc2\xb7 "
#define DEG	" \xc2\xb0" "C"

static NSString *utf8(const char *s)
{
	NSString *n = s ? [NSString stringWithUTF8String:s] : nil;

	return n ? n : @"";
}

@implementation FMCardView

#define PAD	14
#define GRAPH_H	128

- (id)initWithFrame:(NSRect)frame
{
	NSScrollView *scroll;
	NSTableColumn *col;
	float w = frame.size.width, h = frame.size.height, gw = (w - 3 * PAD) / 2, y;

	if (!(self = [super initWithFrame:frame]))
		return nil;
	rows = [[NSMutableArray alloc] init];

	nameLabel = label(NSMakeRect(PAD, h - PAD - 20, w - 2 * PAD, 20), [NSFont boldSystemFontOfSize:15],
			  [NSColor blackColor]);
	[nameLabel setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
	[self addSubview:nameLabel];
	idLabel = label(NSMakeRect(PAD, h - PAD - 38, w - 2 * PAD, 16), [NSFont systemFontOfSize:11],
			[NSColor colorWithCalibratedWhite:0.4 alpha:1]);
	[idLabel setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
	[self addSubview:idLabel];

	/* the graphs: two rows of two, under the name */
	y = h - PAD - 44 - GRAPH_H;
	gfx = [[[FMGraphView alloc] initWithFrame:NSMakeRect(PAD, y, gw, GRAPH_H) title:@"GPU clock and load"
					    color:rgb(0xe2, 0x55, 0x1f) unit:@"MHz"] autorelease];
	mem = [[[FMGraphView alloc] initWithFrame:NSMakeRect(2 * PAD + gw, y, gw, GRAPH_H) title:@"Memory clock and controller load"
					    color:rgb(0x2f, 0x74, 0xc0) unit:@"MHz"] autorelease];
	y -= GRAPH_H + 10;
	temp = [[[FMGraphView alloc] initWithFrame:NSMakeRect(PAD, y, gw, GRAPH_H) title:@"Temperature"
					     color:rgb(0xc8, 0x26, 0x1b) unit:U("\xc2\xb0" "C")] autorelease];
	power = [[[FMGraphView alloc] initWithFrame:NSMakeRect(2 * PAD + gw, y, gw, GRAPH_H) title:@"Power"
					      color:rgb(0xc9, 0x86, 0x0e) unit:@"W"] autorelease];
	[gfx setAutoresizingMask:NSViewWidthSizable | NSViewMaxXMargin | NSViewMinYMargin];
	[temp setAutoresizingMask:NSViewWidthSizable | NSViewMaxXMargin | NSViewMinYMargin];
	[mem setAutoresizingMask:NSViewWidthSizable | NSViewMinXMargin | NSViewMinYMargin];
	[power setAutoresizingMask:NSViewWidthSizable | NSViewMinXMargin | NSViewMinYMargin];
	/* the SMU's activity (load) over each clock, on its own 0-100 % axis, in green */
	[gfx setOverlayColor:rgb(0x1d, 0x8f, 0x4e) caption:@"load (avg)"];
	[mem setOverlayColor:rgb(0x1d, 0x8f, 0x4e) caption:@"load (avg)"];
	[temp setScaleFloor:60];
	[power setScaleFloor:20];
	[self addSubview:gfx];
	[self addSubview:mem];
	[self addSubview:temp];
	[self addSubview:power];

	/* the status list fills the rest */
	y -= 10;
	scroll = [[[NSScrollView alloc] initWithFrame:NSMakeRect(PAD, 0, w - 2 * PAD, y)] autorelease];
	[scroll setHasVerticalScroller:YES];
	[scroll setBorderType:NSBezelBorder];
	[scroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
	table = [[[NSTableView alloc] initWithFrame:[[scroll contentView] bounds]] autorelease];
	col = [[[NSTableColumn alloc] initWithIdentifier:@"key"] autorelease];
	[[col headerCell] setStringValue:@"Status"];
	[col setWidth:170];
	[col setMinWidth:120];
	[col setEditable:NO];
	[table addTableColumn:col];
	col = [[[NSTableColumn alloc] initWithIdentifier:@"value"] autorelease];
	[[col headerCell] setStringValue:@"Value"];
	[col setWidth:w - 2 * PAD - 190];
	[col setEditable:NO];
	[col setResizingMask:NSTableColumnAutoresizingMask];
	[[col dataCell] setLineBreakMode:NSLineBreakByTruncatingTail];
	[table addTableColumn:col];
	[table setColumnAutoresizingStyle:NSTableViewLastColumnOnlyAutoresizingStyle];
	[table setUsesAlternatingRowBackgroundColors:YES];
	[table setRowHeight:17];
	[table setDataSource:self];
	[table setDelegate:self];
	[scroll setDocumentView:table];
	[self addSubview:scroll];
	return self;
}

- (void)dealloc
{
	[table setDataSource:nil];
	[table setDelegate:nil];
	[rows release];
	[super dealloc];
}

/* --- the readings ------------------------------------------------------------ */

static NSString *clockText(unsigned mhz)
{
	return mhz >= 1000 ? [NSString stringWithFormat:@"%.2f GHz", mhz / 1000.0] : [NSString stringWithFormat:@"%u MHz", mhz];
}

- (void)update:(const struct frb_card *)c
{
	const struct frb_user_status *st = &c->st;
	struct frb_status_row r[FRB_MAX_ROWS];
	const char *section = 0;
	NSString *why = c->have_status ? @"not available on this card" : @"not available (driver without the status interface)";
	int n, i;

	[nameLabel setStringValue:utf8(frb_card_name(c))];
	[idLabel setStringValue:[NSString stringWithFormat:U("%@ " DOT " %04x:%04x rev %02x " DOT " stage %@"),
				 utf8(frb_family_label(c)), c->have_status ? st->vendor : c->vendor,
				 c->have_status ? st->device : c->device, c->have_status ? st->revision : c->revision,
				 c->have_status ? utf8(frb_stage_label(st->stage_done)) : utf8(c->stage)]];

	/* GPU and memory clocks: the scale is the DPM ladder's top */
	if (frb_card_has(c, FRB_ST_CLOCKS)) {
		BOOL lv = frb_card_has(c, FRB_ST_LEVELS);

		[gfx setUnavailable:nil];
		[mem setUnavailable:nil];
		[gfx setScaleMax:lv && st->gfx_level_count ? st->gfx_levels[st->gfx_level_count - 1] : 0];
		[mem setScaleMax:lv && st->mem_level_count ? st->mem_levels[st->mem_level_count - 1] : 0];
		BOOL load = frb_card_has(c, FRB_ST_LOAD);
		/* Vega 20's GPU clock is the SMU's average (AverageGfxclkFrequency), Polaris's the clock now */
		BOOL avgClock = st->features_kind == FRB_FEATURES_SMU11;
		NSString *ladder = lv && st->gfx_level_count ?
			[NSString stringWithFormat:@"DPM %u-%u MHz", st->gfx_levels[0], st->gfx_levels[st->gfx_level_count - 1]] : @"";

		[gfx push:st->gfx_mhz second:-1 overlay:load ? st->gpu_load : -1];
		[mem push:st->mem_mhz second:-1 overlay:load ? st->mem_load : -1];
		[gfx setOverlayText:load ? [NSString stringWithFormat:@"%u %%", st->gpu_load] : nil];
		[mem setOverlayText:load ? [NSString stringWithFormat:@"%u %%", st->mem_load] : nil];
		[gfx setValueText:clockText(st->gfx_mhz)
			   detail:avgClock ? [NSString stringWithFormat:U("clock (avg)" DOT "%@"), ladder] : ladder];
		[mem setValueText:clockText(st->mem_mhz)
			   detail:frb_card_has(c, FRB_ST_SOC_CLOCKS) ?
				  [NSString stringWithFormat:U("SoC %u" DOT "fabric %u MHz"), st->soc_mhz, st->fclk_mhz] : @""];
	} else {
		[gfx pushGap];
		[mem pushGap];
		[gfx setUnavailable:why];
		[mem setUnavailable:why];
		[gfx setValueText:@"" detail:@""];
		[mem setValueText:@"" detail:@""];
	}

	/* temperature: edge, with the hotspot dashed under it where there is one */
	if (frb_card_has(c, FRB_ST_TEMP) || frb_card_has(c, FRB_ST_TEMP_HOTSPOT)) {
		BOOL edge = frb_card_has(c, FRB_ST_TEMP), hot = frb_card_has(c, FRB_ST_TEMP_HOTSPOT);
		double now = edge ? st->temp_mc / 1000.0 : st->temp_hotspot_mc / 1000.0;
		NSMutableString *d = [NSMutableString string];

		if (edge && hot)
			[d appendFormat:U("hotspot %d" DEG " (dashed)"), (int)(st->temp_hotspot_mc / 1000)];
		else if (!edge)
			[d appendString:@"hotspot"];
		if (frb_card_has(c, FRB_ST_TEMP_MEM))
			[d appendFormat:U("%@memory %d" DEG), [d length] ? U(DOT) : @"", (int)(st->temp_mem_mc / 1000)];
		[temp setUnavailable:nil];
		[temp push:now second:edge && hot ? st->temp_hotspot_mc / 1000.0 : -1];
		[temp setValueText:[NSString stringWithFormat:U("%.0f" DEG), now] detail:d];
	} else {
		[temp pushGap];
		[temp setUnavailable:why];
		[temp setValueText:@"" detail:@""];
	}

	/* power: the scale is the power limit where the SMU gives one */
	if (frb_card_has(c, FRB_ST_POWER)) {
		NSString *d = frb_card_has(c, FRB_ST_POWER_LIMIT) ? [NSString stringWithFormat:@"limit %u W", st->power_limit_w] : @"";

		if (frb_card_has(c, FRB_ST_VDDGFX))
			d = [NSString stringWithFormat:@"%@%@VDDGFX %u mV", d, [d length] ? U(DOT) : @"", st->vddgfx_mv];
		[power setUnavailable:nil];
		[power setScaleMax:frb_card_has(c, FRB_ST_POWER_LIMIT) ? st->power_limit_w : 0];
		[power push:st->power_mw / 1000.0 second:-1];
		[power setValueText:[NSString stringWithFormat:@"%.1f W", st->power_mw / 1000.0] detail:d];
	} else {
		[power pushGap];
		[power setUnavailable:why];
		[power setValueText:@"" detail:@""];
	}

	/* the list: frb_card_rows, a bold line where a section starts */
	[rows removeAllObjects];
	n = frb_card_rows(c, r, FRB_MAX_ROWS);
	for (i = 0; i < n; i++) {
		if (!section || strcmp(section, r[i].section)) {
			section = r[i].section;
			[rows addObject:[NSArray arrayWithObjects:utf8(section), @"", [NSNumber numberWithBool:YES], nil]];
		}
		[rows addObject:[NSArray arrayWithObjects:utf8(r[i].key), utf8(r[i].value), [NSNumber numberWithBool:NO], nil]];
	}
	[table reloadData];
}

/* --- the list's data source and delegate ---------------------------------------- */

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv
{
	return (NSInteger)[rows count];
}

- (id)tableView:(NSTableView *)tv objectValueForTableColumn:(NSTableColumn *)col row:(NSInteger)row
{
	NSArray *r;

	if (row < 0 || row >= (int)[rows count])
		return @"";
	r = [rows objectAtIndex:row];
	if ([[col identifier] isEqual:@"key"])
		return [[r objectAtIndex:2] boolValue] ? [r objectAtIndex:0] : [@"    " stringByAppendingString:[r objectAtIndex:0]];
	return [r objectAtIndex:1];
}

- (void)tableView:(NSTableView *)tv willDisplayCell:(id)cell forTableColumn:(NSTableColumn *)col row:(NSInteger)row
{
	BOOL header = row >= 0 && row < (int)[rows count] && [[[rows objectAtIndex:row] objectAtIndex:2] boolValue];
	NSString *value = row >= 0 && row < (int)[rows count] ? [[rows objectAtIndex:row] objectAtIndex:1] : @"";
	BOOL missing = [value hasPrefix:@"not available"];

	[cell setFont:header ? [NSFont boldSystemFontOfSize:11] : [NSFont systemFontOfSize:11]];
	[cell setTextColor:missing && [[col identifier] isEqual:@"value"] ? [NSColor grayColor] : [NSColor blackColor]];
}

- (BOOL)tableView:(NSTableView *)tv shouldEditTableColumn:(NSTableColumn *)col row:(NSInteger)row
{
	return NO;
}

@end
