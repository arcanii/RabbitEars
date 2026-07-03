// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#import <Cocoa/Cocoa.h>

@interface AppDelegate : NSObject <NSApplicationDelegate>
- (void)showAboutPanel:(id)sender;   // custom About (libVLC attribution + disclaimer)
@end
