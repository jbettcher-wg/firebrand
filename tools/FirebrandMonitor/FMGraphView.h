/*
 * Firebrand Monitor: one reading's last minute, a line over a filled area,
 * the value now in the corner. A second, dashed series is optional (the
 * hotspot under the edge temperature), and so is an overlay on its own
 * 0-100 % axis (the GPU's load over its clock), whose value is shown big
 * beside the main one.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#import <Cocoa/Cocoa.h>

#define FM_HISTORY	60

@interface FMGraphView : NSView
{
	NSString *title, *valueText, *detailText, *unavailableText, *scaleUnit;
	NSColor *color, *overlayColor;
	NSString *overlayText, *overlayCaption;
	double values[FM_HISTORY], second[FM_HISTORY], overlay[FM_HISTORY];
	BOOL have[FM_HISTORY], haveSecond[FM_HISTORY], haveOverlay[FM_HISTORY];
	int head, count;
	double scaleMax, scaleFloor;
	BOOL available;
}

- (id)initWithFrame:(NSRect)frame title:(NSString *)t color:(NSColor *)c unit:(NSString *)unit;
/* a new sample; second < 0: none. Shifts the minute along. */
- (void)push:(double)v second:(double)s;
/* the same with the overlay's value (0-100; < 0: none this second) */
- (void)push:(double)v second:(double)s overlay:(double)o;
/* the overlay's colour and its caption under the big number ("load, avg") */
- (void)setOverlayColor:(NSColor *)c caption:(NSString *)caption;
/* the overlay's big number ("12 %"); nil: none */
- (void)setOverlayText:(NSString *)t;
/* no reading this second (a gap in the line) */
- (void)pushGap;
- (void)setValueText:(NSString *)v detail:(NSString *)d;
/* nil: available; else why not, drawn instead of the plot */
- (void)setUnavailable:(NSString *)why;
/* the top of the scale: fixed (a DPM ladder's top, the power limit), else from what was seen */
- (void)setScaleMax:(double)m;
- (void)setScaleFloor:(double)f;
@end
