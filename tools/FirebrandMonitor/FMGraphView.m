/*
 * FMGraphView.h
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#import "FMGraphView.h"

@implementation FMGraphView

- (id)initWithFrame:(NSRect)frame title:(NSString *)t color:(NSColor *)c unit:(NSString *)unit
{
	if ((self = [super initWithFrame:frame])) {
		title = [t copy];
		color = [c retain];
		scaleUnit = [unit copy];
		valueText = [@"" retain];
		detailText = [@"" retain];
		available = YES;
	}
	return self;
}

- (void)dealloc
{
	[title release];
	[color release];
	[scaleUnit release];
	[valueText release];
	[detailText release];
	[unavailableText release];
	[overlayColor release];
	[overlayText release];
	[overlayCaption release];
	[super dealloc];
}

- (BOOL)isFlipped
{
	return NO;
}

- (BOOL)isOpaque
{
	return NO;
}

- (void)pushValue:(double)v second:(double)s overlay:(double)o ok:(BOOL)ok
{
	head = (head + 1) % FM_HISTORY;
	values[head] = v;
	second[head] = s;
	overlay[head] = o;
	have[head] = ok;
	haveSecond[head] = ok && s >= 0;
	haveOverlay[head] = ok && o >= 0;
	if (count < FM_HISTORY)
		count++;
	[self setNeedsDisplay:YES];
}

- (void)push:(double)v second:(double)s
{
	[self pushValue:v second:s overlay:-1 ok:YES];
}

- (void)push:(double)v second:(double)s overlay:(double)o
{
	[self pushValue:v second:s overlay:o ok:YES];
}

- (void)pushGap
{
	[self pushValue:0 second:-1 overlay:-1 ok:NO];
}

- (void)setOverlayColor:(NSColor *)c caption:(NSString *)caption
{
	[overlayColor release];
	overlayColor = [c retain];
	[overlayCaption release];
	overlayCaption = [caption copy];
	[self setNeedsDisplay:YES];
}

- (void)setOverlayText:(NSString *)t
{
	[overlayText release];
	overlayText = [t copy];
	[self setNeedsDisplay:YES];
}

- (void)setValueText:(NSString *)v detail:(NSString *)d
{
	[valueText release];
	valueText = [(v ? v : @"") copy];
	[detailText release];
	detailText = [(d ? d : @"") copy];
	[self setNeedsDisplay:YES];
}

- (void)setUnavailable:(NSString *)why
{
	[unavailableText release];
	unavailableText = [why copy];
	available = why == nil;
	[self setNeedsDisplay:YES];
}

- (void)setScaleMax:(double)m
{
	scaleMax = m;
}

- (void)setScaleFloor:(double)f
{
	scaleFloor = f;
}

/* the top of the plot: the fixed scale, else the largest value seen (and a margin), at least the floor */
- (double)top
{
	double m = scaleMax;
	int i;

	if (m <= 0) {
		for (i = 0; i < FM_HISTORY; i++) {
			if (have[i] && values[i] > m)
				m = values[i];
			if (haveSecond[i] && second[i] > m)
				m = second[i];
		}
		m *= 1.2;
	}
	if (m < scaleFloor)
		m = scaleFloor;
	return m > 0 ? m : 1;
}

static NSDictionary *attrs(NSFont *f, NSColor *c)
{
	return [NSDictionary dictionaryWithObjectsAndKeys:f, NSFontAttributeName, c, NSForegroundColorAttributeName, nil];
}

- (NSBezierPath *)lineFor:(double *)v have:(BOOL *)h in:(NSRect)plot top:(double)top fill:(NSBezierPath **)fill
{
	NSBezierPath *line = [NSBezierPath bezierPath], *area = fill ? [NSBezierPath bezierPath] : nil;
	BOOL drawing = NO;
	double dx = plot.size.width / (FM_HISTORY - 1), firstX = 0, lastX = 0;
	int k;

	/* oldest on the left, now on the right edge */
	for (k = 0; k < FM_HISTORY; k++) {
		int i = (head - (FM_HISTORY - 1) + k + FM_HISTORY) % FM_HISTORY;
		double x = plot.origin.x + k * dx, y;

		if (k < FM_HISTORY - count || !h[i]) {
			if (drawing && area) {
				[area lineToPoint:NSMakePoint(lastX, plot.origin.y)];
				[area lineToPoint:NSMakePoint(firstX, plot.origin.y)];
				[area closePath];
			}
			drawing = NO;
			continue;
		}
		y = v[i] / top;
		if (y > 1)
			y = 1;
		if (y < 0)
			y = 0;
		y = plot.origin.y + y * plot.size.height;
		if (!drawing) {
			[line moveToPoint:NSMakePoint(x, y)];
			if (area) {
				[area moveToPoint:NSMakePoint(x, plot.origin.y)];
				[area lineToPoint:NSMakePoint(x, y)];
			}
			firstX = x;
			drawing = YES;
		} else {
			[line lineToPoint:NSMakePoint(x, y)];
			[area lineToPoint:NSMakePoint(x, y)];
		}
		lastX = x;
	}
	if (drawing && area) {
		[area lineToPoint:NSMakePoint(lastX, plot.origin.y)];
		[area lineToPoint:NSMakePoint(firstX, plot.origin.y)];
		[area closePath];
	}
	if (fill)
		*fill = area;
	return line;
}

- (void)drawRect:(NSRect)dirty
{
	NSRect b = NSInsetRect([self bounds], 0.5, 0.5), plot;
	NSBezierPath *frame = [NSBezierPath bezierPathWithRoundedRect:b xRadius:6 yRadius:6];
	NSGradient *bg = [[[NSGradient alloc] initWithStartingColor:[NSColor colorWithCalibratedWhite:0.995 alpha:1]
						    endingColor:[NSColor colorWithCalibratedWhite:0.95 alpha:1]] autorelease];
	NSColor *grey = [NSColor colorWithCalibratedWhite:0.45 alpha:1];
	NSDictionary *titleA = attrs([NSFont boldSystemFontOfSize:11], grey);
	NSDictionary *valueA = attrs([NSFont boldSystemFontOfSize:17], [NSColor colorWithCalibratedWhite:0.12 alpha:1]);
	NSDictionary *detailA = attrs([NSFont systemFontOfSize:10], grey);
	NSDictionary *scaleA = attrs([NSFont systemFontOfSize:9], [NSColor colorWithCalibratedWhite:0.55 alpha:1]);
	NSSize vs;
	double top;
	int g;

	[bg drawInBezierPath:frame angle:270];
	[[NSColor colorWithCalibratedWhite:0.78 alpha:1] setStroke];
	[frame setLineWidth:1];
	[frame stroke];

	[title drawAtPoint:NSMakePoint(b.origin.x + 10, NSMaxY(b) - 19) withAttributes:titleA];
	vs = [valueText sizeWithAttributes:valueA];
	[valueText drawAtPoint:NSMakePoint(NSMaxX(b) - 10 - vs.width, NSMaxY(b) - 25) withAttributes:valueA];
	if (available && overlayColor && [overlayText length]) {
		/* the overlay's number left of the main one, in its colour, with its caption under it */
		NSDictionary *oa = attrs([NSFont boldSystemFontOfSize:17], overlayColor);
		NSDictionary *ca = attrs([NSFont systemFontOfSize:9], overlayColor);
		NSSize os = [overlayText sizeWithAttributes:oa], cs = [overlayCaption sizeWithAttributes:ca];
		float right = NSMaxX(b) - 10 - vs.width - 16;

		[overlayText drawAtPoint:NSMakePoint(right - os.width, NSMaxY(b) - 25) withAttributes:oa];
		[overlayCaption drawAtPoint:NSMakePoint(right - cs.width, NSMaxY(b) - 37) withAttributes:ca];
	}
	[detailText drawAtPoint:NSMakePoint(b.origin.x + 10, NSMaxY(b) - 33) withAttributes:detailA];

	plot = NSMakeRect(b.origin.x + 10, b.origin.y + 16, b.size.width - 20, b.size.height - 16 - 42);
	if (plot.size.height < 10 || plot.size.width < 20)
		return;
	if (!available) {
		NSDictionary *na = attrs([NSFont systemFontOfSize:11], grey);
		NSSize s = [unavailableText sizeWithAttributes:na];

		[unavailableText drawAtPoint:NSMakePoint(NSMidX(plot) - s.width / 2, NSMidY(plot) - s.height / 2)
			      withAttributes:na];
		return;
	}
	/* the grid: quarters of the scale */
	[[NSColor colorWithCalibratedWhite:0.86 alpha:1] setStroke];
	for (g = 0; g <= 4; g++) {
		NSBezierPath *l = [NSBezierPath bezierPath];
		double y = floor(plot.origin.y + plot.size.height * g / 4) + 0.5;

		[l moveToPoint:NSMakePoint(plot.origin.x, y)];
		[l lineToPoint:NSMakePoint(NSMaxX(plot), y)];
		[l setLineWidth:g ? 0.5 : 1];
		[l stroke];
	}
	top = [self top];
	[[NSString stringWithFormat:@"%.0f %@", top, scaleUnit] drawAtPoint:NSMakePoint(plot.origin.x + 2, NSMaxY(plot) - 11)
								 withAttributes:scaleA];
	if (overlayColor) {
		NSDictionary *oa = attrs([NSFont systemFontOfSize:9], overlayColor);
		NSString *axis = @"100 %";
		NSSize s = [axis sizeWithAttributes:oa];

		[axis drawAtPoint:NSMakePoint(NSMaxX(plot) - s.width - 2, NSMaxY(plot) - 11) withAttributes:oa];
	}
	[@"60 s" drawAtPoint:NSMakePoint(plot.origin.x, b.origin.y + 3) withAttributes:scaleA];
	{
		NSString *now = @"now";
		NSSize s = [now sizeWithAttributes:scaleA];

		[now drawAtPoint:NSMakePoint(NSMaxX(plot) - s.width, b.origin.y + 3) withAttributes:scaleA];
	}
	if (count) {
		NSBezierPath *area = nil, *line = [self lineFor:values have:have in:plot top:top fill:&area];

		[[color colorWithAlphaComponent:0.18] setFill];
		[area fill];
		[color setStroke];
		[line setLineWidth:1.6];
		[line setLineJoinStyle:NSRoundLineJoinStyle];
		[line stroke];
		line = [self lineFor:second have:haveSecond in:plot top:top fill:0];
		if (![line isEmpty]) {
			CGFloat dash[2] = { 3, 2 };

			[[color blendedColorWithFraction:0.45 ofColor:[NSColor blackColor]] setStroke];
			[line setLineWidth:1.1];
			[line setLineDash:dash count:2 phase:0];
			[line stroke];
		}
		/* the overlay on its own 0-100 % axis */
		line = overlayColor ? [self lineFor:overlay have:haveOverlay in:plot top:100 fill:0] : nil;
		if (line && ![line isEmpty]) {
			[overlayColor setStroke];
			[line setLineWidth:1.8];
			[line setLineJoinStyle:NSRoundLineJoinStyle];
			[line stroke];
		}
	}
}

@end
