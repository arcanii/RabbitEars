// SPDX-License-Identifier: GPL-3.0-or-later
// See LogoLoader.h.
#import "LogoLoader.h"

#import <CommonCrypto/CommonDigest.h>
#import <ImageIO/ImageIO.h>

namespace {
// Logo URLs come from arbitrary (untrusted) playlists, so both the download and the decode are
// hardened against a hostile file:
//  - kMaxLogoBytes caps the DOWNLOAD (streamed, cancelled past the cap — so a chunked response
//    with no Content-Length can't balloon memory before a post-hoc check).
//  - the DECODE reads the pixel dimensions from the header only (CImageSource, no rasterization)
//    and rejects an absurd image, then produces a small THUMBNAIL — so a "decompression bomb"
//    (a few-KB PNG that rasterizes to gigabytes) is never fully decoded, and even a legit large
//    logo is downsampled instead of kept full-size in the cache.
constexpr long long   kMaxLogoBytes = 3LL * 1024 * 1024;  // 3 MB download cap
constexpr long        kMaxLogoPixels = 2048;              // header bound: reject (no decode) before thumbnailing;
                                                          // a real channel logo is far smaller, and <=2048px
                                                          // bounds the transient decode to a few MB, not GBs.
constexpr CGFloat     kThumbMaxPx = 96;                   // stored thumbnail size (28pt cell, retina headroom)
constexpr NSUInteger  kMemCacheCount = 600;              // in-memory thumbnails (NSCache also evicts under pressure)
constexpr NSInteger   kDiskCacheMaxFiles = 4000;         // pruned to this on launch (oldest by mtime dropped)

// SHA-256 hex of the URL — a filesystem-safe, collision-free cache filename.
NSString* cacheKey(NSString* url) {
    const char* utf8 = url.UTF8String ?: "";
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(utf8, (CC_LONG)strlen(utf8), d);
    NSMutableString* hex = [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) [hex appendFormat:@"%02x", d[i]];
    return hex;
}
}  // namespace

@interface LogoLoader () <NSURLSessionDataDelegate>
@end

@implementation LogoLoader {
    NSCache<NSString*, NSImage*>*        _mem;       // url -> thumbnail (touched on main only)
    NSMutableSet<NSString*>*             _inFlight;  // urls currently loading (MAIN-thread only)
    NSMutableSet<NSString*>*             _failed;    // urls that failed this session (negative cache; MAIN only)
    NSString*                            _dir;       // disk cache directory
    NSURLSession*                        _session;
    void (^_onReady)(void);
    // Per-task receive buffers, accessed only on the session's serial delegate queue.
    NSMutableDictionary<NSNumber*, NSMutableData*>* _rx;
}

- (instancetype)initWithCacheDir:(NSString*)cacheDir onReady:(void (^)(void))onReady {
    if ((self = [super init])) {
        _mem = [[NSCache alloc] init];
        _mem.countLimit = kMemCacheCount;
        _inFlight = [NSMutableSet set];
        _failed = [NSMutableSet set];
        _rx = [NSMutableDictionary dictionary];
        _dir = [cacheDir copy];
        _onReady = [onReady copy];

        NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest = 15;
        cfg.timeoutIntervalForResource = 30;
        cfg.HTTPMaximumConnectionsPerHost = 4;
        cfg.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;  // our disk cache is the cache
        NSOperationQueue* q = [[NSOperationQueue alloc] init];
        q.maxConcurrentOperationCount = 1;  // serial: _rx is only touched from delegate callbacks
        q.name = @"LogoLoader.delegate";
        // The session retains its delegate (self); self is app-lifetime, so the cycle is benign
        // (never invalidated — that would kill future loads).
        _session = [NSURLSession sessionWithConfiguration:cfg delegate:self delegateQueue:q];

        [[NSFileManager defaultManager] createDirectoryAtPath:_dir withIntermediateDirectories:YES
                                                   attributes:nil error:nil];
        [self pruneDiskCacheAsync];
    }
    return self;
}

- (nullable NSImage*)imageForURL:(NSString*)url {
    NSAssert(NSThread.isMainThread, @"LogoLoader is main-thread only");
    if (url.length == 0) return nil;
    if (NSImage* img = [_mem objectForKey:url]) return img;
    if ([_failed containsObject:url] || [_inFlight containsObject:url]) return nil;  // don't re-dispatch
    [_inFlight addObject:url];
    [self loadAsync:url];
    return nil;
}

// Off-main: disk cache first; on a miss, start a streamed download (the delegate finishes it).
- (void)loadAsync:(NSString*)url {
    NSString* file = [_dir stringByAppendingPathComponent:cacheKey(url)];
    __weak LogoLoader* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        LogoLoader* self1 = weakSelf;  // ARC: strong-assign before ivar access
        if (!self1) return;
        NSData* onDisk = [NSData dataWithContentsOfFile:file];
        if (onDisk.length) {
            if (NSImage* img = [self1 decodeThumbnail:onDisk]) { [self1 publish:img forURL:url]; return; }
            // A corrupt cache file: drop it and fall through to a fresh fetch.
            [[NSFileManager defaultManager] removeItemAtPath:file error:nil];
        }
        NSURL* u = [NSURL URLWithString:url];
        if (!u || !(u.scheme.length)) { [self1 failURL:url]; return; }
        NSURLSessionDataTask* task = [self1->_session dataTaskWithURL:u];
        task.taskDescription = url;  // carries the url through the delegate callbacks
        [task resume];
    });
}

#pragma mark NSURLSessionDataDelegate (serial delegate queue)

- (void)URLSession:(NSURLSession*)__unused session
          dataTask:(NSURLSessionDataTask*)task
didReceiveResponse:(NSURLResponse*)resp
 completionHandler:(void (^)(NSURLSessionResponseDisposition))completion {
    const long long declared = resp ? resp.expectedContentLength : NSURLResponseUnknownLength;
    const BOOL httpError = [resp isKindOfClass:NSHTTPURLResponse.class] &&
                           ((NSHTTPURLResponse*)resp).statusCode >= 400;
    if (httpError || declared > kMaxLogoBytes) {  // declared==-1 (unknown) passes; streaming cap below catches it
        completion(NSURLSessionResponseCancel);   // -> didCompleteWithError(cancelled) -> failURL
        return;
    }
    _rx[@(task.taskIdentifier)] = [NSMutableData data];
    completion(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)__unused session
          dataTask:(NSURLSessionDataTask*)task
    didReceiveData:(NSData*)data {
    NSMutableData* buf = _rx[@(task.taskIdentifier)];
    if (!buf) return;  // already cancelled
    [buf appendData:data];
    if ((long long)buf.length > kMaxLogoBytes) {  // bound peak memory regardless of Content-Length
        [_rx removeObjectForKey:@(task.taskIdentifier)];
        [task cancel];  // -> didCompleteWithError(cancelled) -> failURL
    }
}

- (void)URLSession:(NSURLSession*)__unused session
              task:(NSURLSessionTask*)task
didCompleteWithError:(NSError*)error {
    NSString* url = task.taskDescription;
    NSMutableData* buf = _rx[@(task.taskIdentifier)];
    [_rx removeObjectForKey:@(task.taskIdentifier)];
    if (!url) return;
    if (error || buf.length == 0) { [self failURL:url]; return; }
    NSImage* img = [self decodeThumbnail:buf];
    if (!img) { [self failURL:url]; return; }
    NSString* file = [_dir stringByAppendingPathComponent:cacheKey(url)];
    [[NSFileManager defaultManager] createDirectoryAtPath:_dir withIntermediateDirectories:YES
                                               attributes:nil error:nil];
    [buf writeToFile:file atomically:YES];  // best-effort; a miss just re-fetches next launch
    [self publish:img forURL:url];
}

#pragma mark decode + publish

// Bomb-safe decode: read the PIXEL dimensions from the header (no rasterization), reject an
// absurd image, then produce a downsampled thumbnail (ImageIO decodes at the reduced size).
// Never trusts NSImage.size (which is DPI-scaled points and attacker-controllable).
- (nullable NSImage*)decodeThumbnail:(NSData*)data {
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
    if (!src) return nil;
    NSImage* result = nil;
    NSDictionary* props =
        (__bridge_transfer NSDictionary*)CGImageSourceCopyPropertiesAtIndex(src, 0, NULL);
    const long w = [props[(id)kCGImagePropertyPixelWidth] longValue];
    const long h = [props[(id)kCGImagePropertyPixelHeight] longValue];
    if (w >= 1 && h >= 1 && w <= kMaxLogoPixels && h <= kMaxLogoPixels) {
        NSDictionary* opts = @{
            (id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
            (id)kCGImageSourceThumbnailMaxPixelSize: @(kThumbMaxPx),
            (id)kCGImageSourceCreateThumbnailWithTransform: @YES,
        };
        CGImageRef thumb = CGImageSourceCreateThumbnailAtIndex(src, 0, (__bridge CFDictionaryRef)opts);
        if (thumb) {
            result = [[NSImage alloc] initWithCGImage:thumb size:NSZeroSize];
            CGImageRelease(thumb);
        }
    }
    CFRelease(src);
    return result;
}

- (void)publish:(NSImage*)img forURL:(NSString*)url {
    dispatch_async(dispatch_get_main_queue(), ^{
        [_mem setObject:img forKey:url];
        [_inFlight removeObject:url];
        if (_onReady) _onReady();
    });
}

- (void)failURL:(NSString*)url {
    dispatch_async(dispatch_get_main_queue(), ^{
        [_inFlight removeObject:url];
        [_failed addObject:url];  // don't hammer a broken/blocked URL on every reload this session
    });
}

// One-time launch prune: if the cache dir exceeds the file cap, delete the oldest by mtime.
- (void)pruneDiskCacheAsync {
    NSString* dir = _dir;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
        NSFileManager* fm = NSFileManager.defaultManager;
        NSArray<NSString*>* names = [fm contentsOfDirectoryAtPath:dir error:nil];
        if ((NSInteger)names.count <= kDiskCacheMaxFiles) return;
        NSMutableArray<NSArray*>* byAge = [NSMutableArray arrayWithCapacity:names.count];
        for (NSString* n in names) {
            NSString* p = [dir stringByAppendingPathComponent:n];
            NSDate* m = [fm attributesOfItemAtPath:p error:nil].fileModificationDate;
            if (m) [byAge addObject:@[m, p]];
        }
        [byAge sortUsingComparator:^NSComparisonResult(NSArray* a, NSArray* b) {
            return [(NSDate*)a[0] compare:(NSDate*)b[0]];  // oldest first
        }];
        for (NSInteger i = 0; i + kDiskCacheMaxFiles < (NSInteger)byAge.count; ++i)
            [fm removeItemAtPath:byAge[(NSUInteger)i][1] error:nil];
    });
}

@end
