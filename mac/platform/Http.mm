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

// ⚠ THIS FILE IS MRC (it is absent from the -fobjc-arc list in mac/CMakeLists.txt).

// Delegate for httpProbe. It exists because the probe must see the response HEADERS and then stop:
// a live stream's body never ends, so a completion-handler task would download forever. Two jobs,
// both of which need a delegate rather than a block:
//   • cancel the transfer the moment the status line arrives;
//   • REFUSE redirects — a 3xx is itself a healthy answer (it is how a CDN hands off to an edge),
//     and following one turns a cheap probe into a chain of requests.
//
// It holds NO Objective-C objects, so there is nothing to retain/release in MRC and no dealloc.
// `sem_` is deliberately NOT owned: it lives on httpProbe's stack, which blocks on it until
// -didCompleteWithError: signals, and no further callback can arrive for a finished task
// (-URLSession:didBecomeInvalidWithError: is deliberately not implemented, so nothing touches this
// object after the wait returns).
@interface RE_ProbeDelegate : NSObject <NSURLSessionDataDelegate> {
@public
    long                  status_;       // HTTP status, once any response is seen
    BOOL                  gotResponse_;  // a server answered — outranks any later error
    long                  errCode_;      // NSURLError* code when it did not
    dispatch_semaphore_t  sem_;          // NOT owned; see above
}
@end

@implementation RE_ProbeDelegate

- (void)URLSession:(NSURLSession*)__unused session
          dataTask:(NSURLSessionDataTask*)__unused task
didReceiveResponse:(NSURLResponse*)response
 completionHandler:(void (^)(NSURLSessionResponseDisposition))handler {
    if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
        status_ = (long)[(NSHTTPURLResponse*)response statusCode];
        gotResponse_ = YES;
    }
    handler(NSURLSessionResponseCancel);   // headers are all we wanted
}

- (void)URLSession:(NSURLSession*)__unused session
                          task:(NSURLSessionTask*)__unused task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)__unused request
             completionHandler:(void (^)(NSURLRequest*))handler {
    // Record the 3xx and stop here — passing nil completes the task with this response instead of
    // following it. Recorded in BOTH callbacks because a refused redirect may never reach
    // -didReceiveResponse:.
    status_ = (long)response.statusCode;
    gotResponse_ = YES;
    handler(nil);
}

- (void)URLSession:(NSURLSession*)__unused session
                task:(NSURLSessionTask*)__unused task
didCompleteWithError:(NSError*)error {
    // NSURLErrorCancelled here is OUR cancel after the headers — success, not failure. It is
    // ignored because gotResponse_ already outranks it.
    if (error) errCode_ = (long)error.code;
    dispatch_semaphore_signal(sem_);
}

@end

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

// Ask whether `url` is still there WITHOUT downloading it — the peer of the WinHTTP httpProbe.
// See RE_ProbeDelegate above for why this needs a delegate rather than a completion handler.
//
// The classification this feeds (classifyProbe / sweepIsTrustworthy) treats "we could not connect"
// very differently from "the server refused", because an offline laptop must never be read as
// "every channel is gone". So the NSError mapping here matters as much as the HTTP status: a
// server that ANSWERED always wins (gotResponse_), and only when nothing answered do we decide
// between Timeout and NoConnect.
ProbeTransport httpProbe(const std::wstring& url, int& httpStatus, int timeoutMs) {
    @autoreleasepool {
        httpStatus = 0;
        NSString* urlStr = [NSString stringWithUTF8String:utf8FromWide(url).c_str()];
        NSURL* nsurl = urlStr ? [NSURL URLWithString:urlStr] : nil;
        if (!nsurl || !nsurl.host) return ProbeTransport::NoConnect;  // unparseable = not our network

        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:nsurl];
        [req setValue:@"VLC/3.0.23 LibVLC/3.0.23" forHTTPHeaderField:@"User-Agent"];
        if (timeoutMs > 0) req.timeoutInterval = timeoutMs / 1000.0;
        // Never serve a probe from cache: a cached 200 would report a channel alive long after the
        // provider dropped it, which is the exact opposite of what this feature is for.
        req.cachePolicy = NSURLRequestReloadIgnoringLocalAndRemoteCacheData;

        NSURLSessionConfiguration* cfg = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        if (timeoutMs > 0) {
            cfg.timeoutIntervalForRequest = timeoutMs / 1000.0;
            // Also bound the whole exchange. Without this a server that trickles bytes forever
            // holds the sweep's only worker indefinitely, and the sweep never reports.
            cfg.timeoutIntervalForResource = timeoutMs / 1000.0;
        }

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        RE_ProbeDelegate* d = [[RE_ProbeDelegate alloc] init];   // +1, ours
        d->sem_ = sem;                                           // borrowed, see the class comment

        // The session RETAINS its delegate until invalidated — hence finishTasksAndInvalidate
        // below. Skipping it leaks a session, a queue and the delegate on every probe, 250 per
        // sweep.
        NSURLSession* session = [NSURLSession sessionWithConfiguration:cfg
                                                             delegate:d
                                                        delegateQueue:nil];
        NSURLSessionDataTask* task = [session dataTaskWithRequest:req];
        [task resume];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

        const BOOL answered = d->gotResponse_;
        const long status = d->status_;
        const long err = d->errCode_;

        [session finishTasksAndInvalidate];   // releases the session's ref to `d`
        [d release];                          // ours
        dispatch_release(sem);

        if (answered) {
            httpStatus = (int)status;
            return ProbeTransport::Ok;
        }
        if (err == NSURLErrorTimedOut) return ProbeTransport::Timeout;
        // Everything else — DNS, connect, TLS, no route, airplane mode — is NoConnect, which
        // classifyProbe treats as Inconclusive. That is the conservative direction on purpose.
        return ProbeTransport::NoConnect;
    }
}

}  // namespace rabbitears
