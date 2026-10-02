// announce_mac.mm -- a screen reader announcement on macOS, without moving
// the keyboard focus.
//
// The announcement is posted on the control's own view, and VoiceOver reads
// it in its chosen voice. announce.cpp is the Windows side of the same
// function; the two are never built together.
#include "app/announce.h"

#import <AppKit/AppKit.h>

namespace svs {

bool announce(wxWindow *control, const wxString &text) {
    if (!control) return false;
    NSView *view = (NSView *)control->GetHandle();
    if (!view) return false;
    NSString *message = [NSString stringWithUTF8String:text.utf8_str().data()];
    if (!message) return false;
    NSDictionary *info = @{
        NSAccessibilityAnnouncementKey : message,
        NSAccessibilityPriorityKey : @(NSAccessibilityPriorityHigh),
    };
    NSAccessibilityPostNotificationWithUserInfo(view, NSAccessibilityAnnouncementRequestedNotification,
                                                info);
    return true;
}

}  // namespace svs
