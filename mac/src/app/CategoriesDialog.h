// SPDX-License-Identifier: GPL-3.0-or-later
//
// CategoriesDialog — the macOS "Categories" filter (Settings ⚙ ▸ Channels ▸ Categories…).
// A checklist of every group-title in the library; the grid then shows only channels whose
// group is checked (blank-group channels are always shown, matching Win32). The mac peer of
// the Win32 categories include filter. Multi-select, persisted by the caller as "category_filter".
// An empty (or all-checked) result means "no filter — show everything". Compiled with -fobjc-arc.
#pragma once

#import <Cocoa/Cocoa.h>

@interface CategoriesDialog : NSObject
// `groups` = every group-title in the library; `selected` = the currently-included set (empty = all).
- (instancetype)initWithGroups:(NSArray<NSString*>*)groups selected:(NSSet<NSString*>*)selected;
// Present as a modal sheet on `parent`. On Done, `onApply` runs with the included set — EMPTY means
// "no filter" (the caller shows everything). Cancel does not call back.
- (void)presentForWindow:(NSWindow*)parent onApply:(void (^)(NSSet<NSString*>* selected))onApply;
@end
