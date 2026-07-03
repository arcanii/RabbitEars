// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS implementation of core/Http.h (the WinHTTP-based Http.cpp is the
// Windows peer). Synchronous GET via NSURLSession — call from a worker thread
// (it blocks). NSURLSession follows redirects and transparently decompresses
// gzip, matching the WinHTTP implementation's behavior.
#import <Foundation/Foundation.h>

#include <string>

#include "core/Http.h"
#include "platform/Encoding.h"  // non-Windows branch of the shared header

namespace rabbitears {

bool httpGet(const std::wstring& url, std::string& out, std::wstring& error, int timeoutMs) {
    @autoreleasepool {
        NSString* urlStr = [NSString stringWithUTF8String:utf8FromWide(url).c_str()];
        NSURL* nsurl = urlStr ? [NSURL URLWithString:urlStr] : nil;
        if (!nsurl) {
            error = L"Invalid URL.";
            return false;
        }

        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:nsurl];
        // Match the WinHTTP UA so servers that vary on it behave identically.
        [req setValue:@"VLC/3.0.23 LibVLC/3.0.23" forHTTPHeaderField:@"User-Agent"];
        if (timeoutMs > 0) req.timeoutInterval = timeoutMs / 1000.0;

        __block std::string body;
        __block std::wstring errLocal;
        __block bool ok = false;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        NSURLSessionDataTask* task =
            [[NSURLSession sharedSession] dataTaskWithRequest:req
                                            completionHandler:^(NSData* data,
                                                                NSURLResponse* resp,
                                                                NSError* err) {
                if (err) {
                    errLocal = wideFromUtf8(err.localizedDescription.UTF8String);
                } else {
                    long code = [resp isKindOfClass:[NSHTTPURLResponse class]]
                                    ? (long)[(NSHTTPURLResponse*)resp statusCode]
                                    : 200;
                    if (code >= 200 && code < 300) {
                        if (data.length) body.assign((const char*)data.bytes, data.length);
                        ok = true;
                    } else {
                        errLocal = L"HTTP status " + std::to_wstring(code);
                    }
                }
                dispatch_semaphore_signal(sem);
            }];
        [task resume];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

        if (ok) {
            out = std::move(body);
        } else {
            error = errLocal.empty() ? L"Download failed." : errLocal;
        }
        return ok;
    }
}

}  // namespace rabbitears
