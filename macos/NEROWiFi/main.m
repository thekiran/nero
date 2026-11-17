#import <Cocoa/Cocoa.h>
#import <CoreLocation/CoreLocation.h>
#import <CoreWLAN/CoreWLAN.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Reading the OS neighbor (ARP) cache needs no Location permission, but the
 * routing-table sysctl returns an empty dump to the sandboxed `nero` CLI
 * process on recent macOS. This signed, LaunchServices-launched helper runs as
 * its own responsible process, so it reads the real table. It only reports
 * local-link IPv4 neighbors the kernel already holds; it never probes, scans,
 * or sends traffic. */
#define NERO_RT_ROUNDUP(a) ((a) > 0 ? (1 + (((a) - 1) | (sizeof(long) - 1))) : sizeof(long))

static int neighbor_mac_is_unicast(const unsigned char *mac)
{
    static const unsigned char zero[6] = {0};
    if (memcmp(mac, zero, 6) == 0)
        return 0;
    return (mac[0] & 0x01) == 0; /* skip multicast and broadcast */
}

/* Returns an array of @{@"ip": NSString, @"mac": NSString} for the current
 * IPv4 ARP cache, optionally filtered to one interface. */
static NSArray *read_neighbors(NSString *ifaceName)
{
    NSMutableArray *rows = [NSMutableArray array];
    int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_LLINFO};
    size_t needed = 0;
    if (sysctl(mib, 6, NULL, &needed, NULL, 0) != 0 || needed == 0)
        return rows;
    char *buffer = malloc(needed);
    if (!buffer)
        return rows;
    size_t actual = needed;
    if (sysctl(mib, 6, buffer, &actual, NULL, 0) != 0) {
        free(buffer);
        return rows;
    }
    unsigned wanted_index = ifaceName.length ? if_nametoindex(ifaceName.UTF8String) : 0;
    char *cursor = buffer, *limit = buffer + actual;
    while (cursor + sizeof(struct rt_msghdr) <= limit && rows.count < 256) {
        struct rt_msghdr *message = (struct rt_msghdr *)cursor;
        if (message->rtm_msglen == 0 || cursor + message->rtm_msglen > limit)
            break;
        if ((message->rtm_flags & RTF_LLINFO) != 0 && message->rtm_addrs != 0) {
            struct sockaddr *address = (struct sockaddr *)(message + 1);
            struct sockaddr_in *destination = NULL;
            struct sockaddr_dl *link = NULL;
            for (int index = 0; index < RTAX_MAX; ++index) {
                if ((message->rtm_addrs & (1 << index)) == 0)
                    continue;
                if (address->sa_len == 0)
                    break;
                if (index == RTAX_DST && address->sa_family == AF_INET)
                    destination = (struct sockaddr_in *)address;
                else if (index == RTAX_GATEWAY && address->sa_family == AF_LINK)
                    link = (struct sockaddr_dl *)address;
                address = (struct sockaddr *)((char *)address + NERO_RT_ROUNDUP(address->sa_len));
            }
            if (destination && link && link->sdl_alen == 6) {
                const unsigned char *mac = (const unsigned char *)LLADDR(link);
                int matches = wanted_index == 0 || link->sdl_index == wanted_index;
                if (matches && neighbor_mac_is_unicast(mac)) {
                    char ip[INET_ADDRSTRLEN] = {0};
                    if (inet_ntop(AF_INET, &destination->sin_addr, ip, sizeof(ip))) {
                        [rows addObject:@{
                            @"ip": [NSString stringWithUTF8String:ip],
                            @"mac": [NSString stringWithFormat:@"%02x:%02x:%02x:%02x:%02x:%02x",
                                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]]
                        }];
                    }
                }
            }
        }
        cursor += message->rtm_msglen;
    }
    free(buffer);
    return rows;
}

/* No coordinates are requested. LaunchServices gives both UI and scan launches
 * the same signed bundle identity. Scan launches NEVER request authorization. */
static NSString *permission(CLLocationManager *manager)
{
    if (![CLLocationManager locationServicesEnabled]) return @"disabled";
    switch (manager.authorizationStatus) {
        case kCLAuthorizationStatusNotDetermined: return @"not_determined";
        case kCLAuthorizationStatusRestricted: return @"restricted";
        case kCLAuthorizationStatusDenied: return @"denied";
        case kCLAuthorizationStatusAuthorizedAlways: return @"authorized";
    }
    return @"restricted";
}

static NSString *hex(NSData *data)
{
    if (!data) return @"";
    NSMutableString *result = [NSMutableString string];
    const unsigned char *bytes = data.bytes;
    for (NSUInteger i = 0; i < data.length && i < 32; ++i)
        [result appendFormat:@"%02x", bytes[i]];
    return result;
}

static int security(CWNetwork *n)
{
    if ([n supportsSecurity:kCWSecurityWPA3Transition]) return 5;
    if ([n supportsSecurity:kCWSecurityWPA3Personal] || [n supportsSecurity:kCWSecurityWPA3Enterprise]) return 6;
    if ([n supportsSecurity:kCWSecurityOWE] || [n supportsSecurity:kCWSecurityOWETransition]) return 7;
    if ([n supportsSecurity:kCWSecurityWPA2Personal] || [n supportsSecurity:kCWSecurityWPA2Enterprise] ||
        [n supportsSecurity:kCWSecurityWPAPersonalMixed] || [n supportsSecurity:kCWSecurityWPAEnterpriseMixed]) return 4;
    if ([n supportsSecurity:kCWSecurityWPAPersonal] || [n supportsSecurity:kCWSecurityWPAEnterprise]) return 3;
    if ([n supportsSecurity:kCWSecurityWEP] || [n supportsSecurity:kCWSecurityDynamicWEP]) return 2;
    if ([n supportsSecurity:kCWSecurityNone]) return 1;
    return 0;
}

/* Only write to the pre-created regular file in the caller's private directory.
 * No network identifiers are written to logs or supplied to a shell. */
static int write_result(NSString *path, NSDictionary *payload)
{
    int dir = open(path.stringByDeletingLastPathComponent.fileSystemRepresentation,
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    struct stat st;
    if (dir < 0) return 1;
    if (fstat(dir, &st) || st.st_uid != getuid() || (st.st_mode & 0777) != 0700) { close(dir); return 1; }
    int fd = openat(dir, path.lastPathComponent.fileSystemRepresentation, O_WRONLY | O_NOFOLLOW);
    close(dir);
    if (fd < 0) return 1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        st.st_nlink != 1 || (st.st_mode & 0777) != 0600 || st.st_size != 0) { close(fd); return 1; }
    NSData *data = [NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
    if (!data) { close(fd); return 1; }
    const char *bytes = data.bytes;
    NSUInteger left = data.length;
    while (left) {
        ssize_t written = write(fd, bytes, left);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { close(fd); return 1; }
        bytes += written; left -= (NSUInteger)written;
    }
    close(fd);
    return 0;
}

static int scan(NSString *path, NSString *name, CLLocationManager *manager)
{
    NSString *status = permission(manager);
    NSMutableArray *rows = [NSMutableArray array];
    if ([status isEqualToString:@"authorized"]) {
        CWWiFiClient *client = [CWWiFiClient sharedWiFiClient];
        CWInterface *iface = name.length ? [client interfaceWithName:name] : client.interface;
        if (!iface) status = @"no_interface";
        else if (!iface.powerOn) status = @"powered_off";
        else {
            NSError *error = nil;
            NSSet<CWNetwork *> *networks = [iface scanForNetworksWithName:nil includeHidden:YES error:&error];
            if (!networks || error) status = @"scan_failed";
            else for (CWNetwork *n in networks) {
                if (rows.count == 256) break;
                [rows addObject:@{@"bssid": n.bssid ?: [NSNull null],
                    @"ssid_hex": n.ssidData ? hex(n.ssidData) : [NSNull null],
                    @"channel": @(n.wlanChannel.channelNumber), @"band": @(n.wlanChannel.channelBand),
                    @"rssi": @(n.rssiValue), @"security": @(security(n))}];
            }
        }
        /* Do not release identifiers if authorization was revoked mid-scan. */
        NSString *after = permission(manager);
        if (![after isEqualToString:@"authorized"]) { status = after; [rows removeAllObjects]; }
    }
    return write_result(path, @{@"version": @1, @"status": status, @"aps": rows});
}

/* Read-only OS neighbor (ARP) cache; no Location permission required. */
static int neighbors(NSString *path, NSString *name)
{
    NSArray *rows = read_neighbors(name);
    return write_result(path, @{@"version": @1, @"status": @"ok", @"neighbors": rows});
}

@interface App : NSObject <NSApplicationDelegate, CLLocationManagerDelegate>
@property CLLocationManager *manager;
@property NSWindow *window;
@property NSTextField *label;
@property NSString *outputPath;
@property NSString *interfaceName;
@property BOOL scanScheduled;
@end
@implementation App
- (void)refresh
{
    self.label.stringValue = [NSString stringWithFormat:@"Location permission: %@\n\nAllow access to reveal Wi-Fi SSIDs and BSSIDs in NERO. No coordinates are requested.\n\nIf denied, restricted or disabled, check System Settings → Privacy & Security → Location Services. Then retry the CLI scan.", permission(self.manager)];
}
- (void)allow:(id)sender
{
    (void)sender;
    [self.manager requestWhenInUseAuthorization];
    [self refresh];
}
- (void)locationManagerDidChangeAuthorization:(CLLocationManager *)manager
{
    (void)manager;
    [self refresh];
    /* A newly created manager initially reports notDetermined even for an
     * authorized app. Wait for its initial authorization callback before scan. */
    if (self.outputPath && !self.scanScheduled) {
        self.scanScheduled = YES;
        dispatch_async(dispatch_get_main_queue(), ^{
            exit(scan(self.outputPath, self.interfaceName, self.manager));
        });
    }
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    (void)notification;
    NSArray<NSString *> *args = NSProcessInfo.processInfo.arguments;
    if (args.count >= 2 && [args[1] isEqualToString:@"--neighbors"]) {
        /* Neighbor-cache read needs no Location permission and no async
         * callback: read the kernel table and write the result immediately. */
        if (args.count != 4 && args.count != 5) exit(1);
        if (![args[2] isEqualToString:@"--output"]) exit(1);
        exit(neighbors(args[3], args.count == 5 ? args[4] : @""));
    }
    if (args.count >= 2 && [args[1] isEqualToString:@"--scan"]) {
        if (args.count != 4 && args.count != 5) exit(1);
        if (![args[2] isEqualToString:@"--output"]) exit(1);
        alarm(25); /* Bound a stalled CoreWLAN call even if the CLI exits. */
        self.outputPath = args[3];
        self.interfaceName = args.count == 5 ? args[4] : @"";
        self.manager = [CLLocationManager new];
        self.manager.delegate = self;
        return;
    }
    self.manager = [CLLocationManager new];
    self.manager.delegate = self;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 490, 250)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"NERO Wi-Fi Access";
    self.label = [NSTextField wrappingLabelWithString:@""];
    self.label.frame = NSMakeRect(24, 72, 442, 155);
    [self.window.contentView addSubview:self.label];
    NSButton *button = [NSButton buttonWithTitle:@"Allow Wi-Fi Identifier Access" target:self action:@selector(allow:)];
    button.frame = NSMakeRect(24, 24, 280, 32);
    [self.window.contentView addSubview:button];
    [self refresh];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { (void)sender; return YES; }
@end
int main(void)
{
    @autoreleasepool {
        if (geteuid() == 0) return 1;
        NSApplication *app = [NSApplication sharedApplication];
        if ([NSProcessInfo.processInfo.arguments containsObject:@"--scan"] ||
            [NSProcessInfo.processInfo.arguments containsObject:@"--neighbors"])
            [app setActivationPolicy:NSApplicationActivationPolicyProhibited];
        App *delegate = [App new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
