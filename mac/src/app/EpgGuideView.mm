// SPDX-License-Identifier: GPL-3.0-or-later
// See EpgGuideView.h.
#import "EpgGuideView.h"

#include <algorithm>
#include <cmath>

@implementation REGuideProgramme
@end

@implementation REGuideRow
@end

// Grid metrics (points; macOS is DPI-independent so no scaling needed).
static const CGFloat kChanColW  = 210;  // frozen channel column width
static const CGFloat kHeaderH   = 36;   // frozen hour-axis height
static const CGFloat kRowH      = 46;   // per-channel row height
static const CGFloat kPxPerHour = 160;  // time scale
static const CGFloat kBlockPad  = 6;    // text inset inside a programme block

@implementation EpgGuideView {
    NSArray<REGuideRow*>* _rows;
    long long _nowUtc;
    long long _timeStart;   // left edge of the timeline (epoch, floored to the hour)
    long long _timeEnd;     // right edge (epoch)
    CGFloat   _scrollX;     // px scrolled along time (>= 0)
    CGFloat   _scrollY;     // px scrolled down channels (>= 0)
    NSInteger _highlightRow;  // row tinted by -revealRowAtIndex:; -1 == none
    NSDateFormatter* _timeFmt;
    NSDateFormatter* _dayFmt;
}

- (instancetype)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) {
        _rows = @[];
        _highlightRow = -1;
        _timeFmt = [[NSDateFormatter alloc] init];
        _timeFmt.dateStyle = NSDateFormatterNoStyle;
        _timeFmt.timeStyle = NSDateFormatterShortStyle;
        _dayFmt = [[NSDateFormatter alloc] init];
        _dayFmt.dateFormat = [NSDateFormatter dateFormatFromTemplate:@"EEEMMMd"
                                                             options:0
                                                              locale:NSLocale.currentLocale];
        self.wantsLayer = YES;
    }
    return self;
}

- (NSArray<REGuideRow*>*)rows { return _rows; }
- (BOOL)isFlipped { return YES; }            // origin top-left, y grows downward
- (BOOL)acceptsFirstResponder { return YES; }
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; [self setNeedsDisplay:YES]; }

- (void)setRows:(NSArray<REGuideRow*>*)rows nowUtc:(long long)nowUtc {
    _rows = rows ?: @[];
    _nowUtc = nowUtc;
    _highlightRow = -1;  // the row indices these rows are addressed by are about to change

    // Timeline bounds from the data: floor the earliest start to the hour, and take the
    // latest stop (capped to 5 days so a stray far-future programme can't blow up the width).
    long long minStart = 0, maxStop = 0;
    for (REGuideRow* r in _rows)
        for (REGuideProgramme* p in r.programmes) {
            if (p.startUtc > 0 && (minStart == 0 || p.startUtc < minStart)) minStart = p.startUtc;
            if (p.stopUtc  > maxStop) maxStop = p.stopUtc;
        }
    if (minStart == 0) minStart = nowUtc - 3600;
    if (maxStop <= minStart) maxStop = minStart + 6 * 3600;
    _timeStart = (minStart / 3600) * 3600;                        // floor to the hour
    _timeEnd   = std::min<long long>(maxStop, _timeStart + 5LL * 24 * 3600);

    // Start with "now" ~100px in from the left of the content area.
    _scrollX = std::max<CGFloat>(0, (nowUtc - _timeStart) / 3600.0 * kPxPerHour - 100);
    _scrollY = 0;
    [self setNeedsDisplay:YES];
}

// ---- geometry helpers --------------------------------------------------------

- (CGFloat)contentW { return std::max<CGFloat>(0, self.bounds.size.width - kChanColW); }
- (CGFloat)contentH { return std::max<CGFloat>(0, self.bounds.size.height - kHeaderH); }
- (CGFloat)timelineW { return (_timeEnd - _timeStart) / 3600.0 * kPxPerHour; }
- (CGFloat)rowsH { return _rows.count * kRowH; }
- (CGFloat)maxScrollX { return std::max<CGFloat>(0, [self timelineW] - [self contentW]); }
- (CGFloat)maxScrollY { return std::max<CGFloat>(0, [self rowsH] - [self contentH]); }

// Epoch -> x within the content area (already scroll-adjusted; may be off-screen).
- (CGFloat)xForTime:(long long)t { return kChanColW + (t - _timeStart) / 3600.0 * kPxPerHour - _scrollX; }

// Scroll `index` into view, vertically centred, and tint it until the next scroll or click.
// Only Y moves: the caller wants "where is this channel", and the time axis is already parked
// on "now" from -setRows:nowUtc:.
- (void)revealRowAtIndex:(NSInteger)index {
    if (index < 0 || index >= (NSInteger)_rows.count) return;
    _scrollY = std::max<CGFloat>(0, index * kRowH - ([self contentH] - kRowH) / 2);
    _highlightRow = index;
    [self clampScroll];
    [self setNeedsDisplay:YES];
}

// The highlight is transient: any deliberate navigation clears it, so it never lingers as a
// stale "selection" the user can't dismiss.
- (void)clearHighlight {
    if (_highlightRow < 0) return;
    _highlightRow = -1;
    [self setNeedsDisplay:YES];
}

- (void)clampScroll {
    _scrollX = std::clamp<CGFloat>(_scrollX, 0, [self maxScrollX]);
    _scrollY = std::clamp<CGFloat>(_scrollY, 0, [self maxScrollY]);
}

// ---- drawing -----------------------------------------------------------------

- (void)drawRect:(NSRect)__unused dirty {
    [self clampScroll];
    const NSRect b = self.bounds;
    const CGFloat W = b.size.width, H = b.size.height;

    [NSColor.controlBackgroundColor setFill];
    NSRectFill(b);

    if (_rows.count == 0) {
        NSDictionary* a = @{ NSFontAttributeName: [NSFont systemFontOfSize:14],
                             NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
        NSString* msg = @"No guide loaded — run Refresh Guide first.";
        NSSize sz = [msg sizeWithAttributes:a];
        [msg drawAtPoint:NSMakePoint((W - sz.width) / 2, (H - sz.height) / 2) withAttributes:a];
        return;
    }

    const CGFloat contentH = [self contentH];
    const NSInteger first = std::max<NSInteger>(0, (NSInteger)(_scrollY / kRowH));
    const NSInteger last  = std::min<NSInteger>((NSInteger)_rows.count - 1,
                                                (NSInteger)((_scrollY + contentH) / kRowH));

    [self drawProgrammes:first through:last];
    [self drawChannelColumn:first through:last];
    [self drawHourAxis];
    [self drawCorner];
}

// Region 1: programme blocks + hour gridlines + the "now" line, clipped to the content area.
- (void)drawProgrammes:(NSInteger)first through:(NSInteger)last {
    const NSRect content = NSMakeRect(kChanColW, kHeaderH, [self contentW], [self contentH]);
    [NSGraphicsContext saveGraphicsState];
    NSRectClip(content);

    // Faint vertical gridline at each hour.
    [[NSColor.separatorColor colorWithAlphaComponent:0.5] setStroke];
    NSBezierPath* grid = [NSBezierPath bezierPath];
    grid.lineWidth = 1;
    for (long long t = _timeStart; t <= _timeEnd; t += 3600) {
        const CGFloat x = std::round([self xForTime:t]) + 0.5;
        if (x < kChanColW || x > NSMaxX(content)) continue;
        [grid moveToPoint:NSMakePoint(x, kHeaderH)];
        [grid lineToPoint:NSMakePoint(x, NSMaxY(content))];
    }
    [grid stroke];

    NSMutableParagraphStyle* clip = [[NSMutableParagraphStyle alloc] init];
    clip.lineBreakMode = NSLineBreakByTruncatingTail;
    NSDictionary* titleAttr = @{ NSFontAttributeName: [NSFont systemFontOfSize:12 weight:NSFontWeightMedium],
                                 NSForegroundColorAttributeName: NSColor.labelColor,
                                 NSParagraphStyleAttributeName: clip };
    NSDictionary* timeAttr  = @{ NSFontAttributeName: [NSFont systemFontOfSize:10],
                                 NSForegroundColorAttributeName: NSColor.secondaryLabelColor,
                                 NSParagraphStyleAttributeName: clip };

    for (NSInteger i = first; i <= last; ++i) {
        REGuideRow* row = _rows[(NSUInteger)i];
        const CGFloat top = kHeaderH + i * kRowH - _scrollY;
        if (i == _highlightRow) {  // "Show in TV Guide" tint across the programme area
            [[NSColor.controlAccentColor colorWithAlphaComponent:0.18] setFill];
            NSRectFill(NSMakeRect(kChanColW, top, [self contentW], kRowH));
        }
        for (REGuideProgramme* p in row.programmes) {
            CGFloat x0 = [self xForTime:p.startUtc];
            long long stop = p.stopUtc > p.startUtc ? p.stopUtc : p.startUtc + 1800;  // guard 0/short
            CGFloat x1 = [self xForTime:stop];
            if (x1 <= kChanColW || x0 >= NSMaxX(content)) continue;  // off-screen in X
            NSRect block = NSInsetRect(NSMakeRect(x0, top, x1 - x0, kRowH), 1, 1);

            const BOOL airing = (p.startUtc <= _nowUtc && _nowUtc < stop);
            // A subtle labelColor tint reads as a distinct "card" in BOTH light and dark mode.
            // (alternatingContentBackgroundColors came out near-white in dark mode → low-contrast
            // light text on a light block.)
            NSColor* fill = airing ? [NSColor.controlAccentColor colorWithAlphaComponent:0.28]
                                   : [NSColor.labelColor colorWithAlphaComponent:0.08];
            NSBezierPath* bp = [NSBezierPath bezierPathWithRoundedRect:block xRadius:4 yRadius:4];
            [fill setFill];
            [bp fill];
            [(airing ? NSColor.controlAccentColor : NSColor.separatorColor) setStroke];
            bp.lineWidth = airing ? 1.5 : 1;
            [bp stroke];

            // Text: title on top, local time range below, both inset + clipped to the block.
            const NSRect tr = NSInsetRect(block, kBlockPad, 4);
            if (tr.size.width < 8) continue;
            [p.title drawInRect:NSMakeRect(tr.origin.x, tr.origin.y, tr.size.width, 16)
                 withAttributes:titleAttr];
            NSString* range = [NSString stringWithFormat:@"%@ – %@",
                               [self shortTime:p.startUtc], [self shortTime:stop]];
            [range drawInRect:NSMakeRect(tr.origin.x, tr.origin.y + 17, tr.size.width, 14)
               withAttributes:timeAttr];
        }
    }

    // The "now" line across the content area.
    const CGFloat nowX = [self xForTime:_nowUtc];
    if (nowX >= kChanColW && nowX <= NSMaxX(content)) {
        [NSColor.systemRedColor setStroke];
        NSBezierPath* nl = [NSBezierPath bezierPath];
        nl.lineWidth = 2;
        [nl moveToPoint:NSMakePoint(std::round(nowX) + 0.5, kHeaderH)];
        [nl lineToPoint:NSMakePoint(std::round(nowX) + 0.5, NSMaxY(content))];
        [nl stroke];
    }
    [NSGraphicsContext restoreGraphicsState];
}

// Region 2: the frozen channel column (scrolls vertically with the rows).
- (void)drawChannelColumn:(NSInteger)first through:(NSInteger)last {
    const NSRect col = NSMakeRect(0, kHeaderH, kChanColW, [self contentH]);
    [NSGraphicsContext saveGraphicsState];
    NSRectClip(col);
    [[NSColor.windowBackgroundColor colorWithAlphaComponent:0.98] setFill];
    NSRectFill(col);

    NSMutableParagraphStyle* clip = [[NSMutableParagraphStyle alloc] init];
    clip.lineBreakMode = NSLineBreakByTruncatingTail;
    NSDictionary* nameAttr = @{ NSFontAttributeName: [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold],
                                NSForegroundColorAttributeName: NSColor.labelColor,
                                NSParagraphStyleAttributeName: clip };

    [NSColor.separatorColor setStroke];
    for (NSInteger i = first; i <= last; ++i) {
        REGuideRow* row = _rows[(NSUInteger)i];
        const CGFloat top = kHeaderH + i * kRowH - _scrollY;
        if (i == _highlightRow)  // "Show in TV Guide" tint, matched in drawProgrammes:
            [[NSColor.controlAccentColor colorWithAlphaComponent:0.18] setFill],
            NSRectFill(NSMakeRect(0, top, kChanColW, kRowH));
        [row.channelName drawInRect:NSMakeRect(10, top + (kRowH - 16) / 2, kChanColW - 18, 16)
                     withAttributes:nameAttr];
        NSBezierPath* sep = [NSBezierPath bezierPath];  // row separator
        [sep moveToPoint:NSMakePoint(0, std::round(top + kRowH) + 0.5)];
        [sep lineToPoint:NSMakePoint(kChanColW, std::round(top + kRowH) + 0.5)];
        [sep stroke];
    }
    // Right edge of the column.
    [NSColor.separatorColor setStroke];
    NSBezierPath* edge = [NSBezierPath bezierPath];
    [edge moveToPoint:NSMakePoint(kChanColW - 0.5, kHeaderH)];
    [edge lineToPoint:NSMakePoint(kChanColW - 0.5, NSMaxY(col))];
    [edge stroke];
    [NSGraphicsContext restoreGraphicsState];
}

// Region 3: the frozen hour axis (scrolls horizontally with time). Labels each hour, and
// puts a day label ("Wed Jul 9") at each midnight boundary.
- (void)drawHourAxis {
    const NSRect axis = NSMakeRect(kChanColW, 0, [self contentW], kHeaderH);
    [NSGraphicsContext saveGraphicsState];
    NSRectClip(axis);
    [[NSColor.windowBackgroundColor colorWithAlphaComponent:0.98] setFill];
    NSRectFill(axis);

    NSDictionary* hourAttr = @{ NSFontAttributeName: [NSFont systemFontOfSize:10],
                                NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    NSDictionary* dayAttr  = @{ NSFontAttributeName: [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold],
                                NSForegroundColorAttributeName: NSColor.labelColor };
    // Align ticks to LOCAL hour boundaries via NSCalendar, so hour labels read on-the-hour
    // and the day label lands on local midnight — correct even in fractional-hour zones
    // (UTC+5:30 etc.) and across DST, unlike stepping raw UTC hours from _timeStart.
    NSCalendar* cal = NSCalendar.currentCalendar;
    NSDateComponents* dc = [cal components:(NSCalendarUnitYear | NSCalendarUnitMonth |
                                            NSCalendarUnitDay | NSCalendarUnitHour)
                                  fromDate:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)_timeStart]];
    NSDate* tick = [cal dateFromComponents:dc];  // local hour boundary at/just before _timeStart
    while (tick && [tick timeIntervalSince1970] < _timeStart)
        tick = [cal dateByAddingUnit:NSCalendarUnitHour value:1 toDate:tick options:0];
    [NSColor.separatorColor setStroke];
    for (; tick && [tick timeIntervalSince1970] <= _timeEnd;
         tick = [cal dateByAddingUnit:NSCalendarUnitHour value:1 toDate:tick options:0]) {
        const long long te = (long long)[tick timeIntervalSince1970];
        const CGFloat x = [self xForTime:te];
        if (x < kChanColW - 1 || x > NSMaxX(axis)) continue;
        NSBezierPath* mark = [NSBezierPath bezierPath];  // hour tick
        [mark moveToPoint:NSMakePoint(std::round(x) + 0.5, kHeaderH - 6)];
        [mark lineToPoint:NSMakePoint(std::round(x) + 0.5, kHeaderH)];
        [mark stroke];
        [[self shortTime:te] drawAtPoint:NSMakePoint(x + 4, kHeaderH - 16) withAttributes:hourAttr];
        if ([cal component:NSCalendarUnitHour fromDate:tick] == 0)  // local midnight → day label
            [[_dayFmt stringFromDate:tick] drawAtPoint:NSMakePoint(x + 4, 3) withAttributes:dayAttr];
    }
    // Bottom edge of the axis.
    NSBezierPath* edge = [NSBezierPath bezierPath];
    [edge moveToPoint:NSMakePoint(kChanColW, kHeaderH - 0.5)];
    [edge lineToPoint:NSMakePoint(NSMaxX(axis), kHeaderH - 0.5)];
    [edge stroke];
    [NSGraphicsContext restoreGraphicsState];
}

// Region 4: the top-left corner (over both frozen bands).
- (void)drawCorner {
    const NSRect corner = NSMakeRect(0, 0, kChanColW, kHeaderH);
    [NSColor.windowBackgroundColor setFill];
    NSRectFill(corner);
    NSDictionary* a = @{ NSFontAttributeName: [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold],
                         NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    [@"Channel" drawAtPoint:NSMakePoint(10, (kHeaderH - 14) / 2) withAttributes:a];
    [NSColor.separatorColor setStroke];
    NSBezierPath* edge = [NSBezierPath bezierPath];
    [edge moveToPoint:NSMakePoint(kChanColW - 0.5, 0)];
    [edge lineToPoint:NSMakePoint(kChanColW - 0.5, kHeaderH)];
    [edge moveToPoint:NSMakePoint(0, kHeaderH - 0.5)];
    [edge lineToPoint:NSMakePoint(kChanColW, kHeaderH - 0.5)];
    [edge stroke];
}

- (NSString*)shortTime:(long long)epoch {
    return [_timeFmt stringFromDate:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)epoch]];
}

// ---- interaction -------------------------------------------------------------

- (void)scrollWheel:(NSEvent*)e {
    CGFloat dx = e.scrollingDeltaX, dy = e.scrollingDeltaY;
    if (e.modifierFlags & NSEventModifierFlagShift) { dx = dy; dy = 0; }  // shift-wheel scrubs time
    _scrollX = std::clamp<CGFloat>(_scrollX - dx, 0, [self maxScrollX]);
    _scrollY = std::clamp<CGFloat>(_scrollY - dy, 0, [self maxScrollY]);
    _highlightRow = -1;  // any deliberate scroll dismisses the "Show in Guide" tint
    [self setNeedsDisplay:YES];
}

- (void)mouseDown:(NSEvent*)e {
    [self clearHighlight];  // a click is deliberate navigation — drop the "Show in Guide" tint
    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if (p.x < kChanColW || p.y < kHeaderH) return;  // clicks in the frozen bands do nothing
    const NSInteger i = (NSInteger)((p.y - kHeaderH + _scrollY) / kRowH);
    if (i < 0 || i >= (NSInteger)_rows.count) return;
    REGuideRow* row = _rows[(NSUInteger)i];
    const long long t = _timeStart +
        (long long)((p.x - kChanColW + _scrollX) / kPxPerHour * 3600.0);
    REGuideProgramme* hit = nil;
    for (REGuideProgramme* pr in row.programmes) {
        long long stop = pr.stopUtc > pr.startUtc ? pr.stopUtc : pr.startUtc + 1800;
        if (pr.startUtc <= t && t < stop) { hit = pr; break; }
    }
    if (hit) [self.delegate guideView:self didActivateProgramme:hit inRow:row];
}

@end
