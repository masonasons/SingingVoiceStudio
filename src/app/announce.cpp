#include "app/announce.h"

#ifdef __WXMSW__
#include <windows.h>
#include <oleacc.h>
#include <oleauto.h>
#endif

namespace svs {

#ifdef __WXMSW__
namespace {

// UIAutomationCore is loaded by hand: it is there on every Windows this runs
// on, but linking it would make the program refuse to start on a machine
// where it is not, rather than merely being quieter.
typedef HRESULT(WINAPI *ProviderFromIAccessible)(IAccessible *, long, DWORD, void **);
typedef HRESULT(WINAPI *RaiseNotification)(void *, int, int, BSTR, BSTR);
typedef HRESULT(WINAPI *ObjectFromWindow)(HWND, DWORD, REFIID, void **);

struct Uia {
    ProviderFromIAccessible from_acc = nullptr;
    RaiseNotification raise = nullptr;
    ObjectFromWindow from_window = nullptr;
    bool ok = false;
    Uia() {
        HMODULE uia = LoadLibraryW(L"UIAutomationCore.dll");
        HMODULE oleacc = LoadLibraryW(L"oleacc.dll");
        if (!uia || !oleacc) return;
        from_acc = (ProviderFromIAccessible)(void *)GetProcAddress(uia, "UiaProviderFromIAccessible");
        raise = (RaiseNotification)(void *)GetProcAddress(uia, "UiaRaiseNotificationEvent");
        from_window = (ObjectFromWindow)(void *)GetProcAddress(oleacc, "AccessibleObjectFromWindow");
        ok = from_acc && raise && from_window;
    }
};

const Uia &uia() {
    static Uia u;
    return u;
}

}  // namespace
#endif

bool announce(wxWindow *control, const wxString &text) {
#ifdef __WXMSW__
    const Uia &u = uia();
    if (!u.ok || !control) return false;
    IAccessible *acc = nullptr;
    // wx has already initialised COM on its UI thread. The list's own MSAA
    // object is wrapped in a complete UIA provider, since a bare window
    // provider cannot raise events.
    HRESULT hr = u.from_window((HWND)control->GetHWND(), DWORD(OBJID_CLIENT), IID_IAccessible,
                               (void **)&acc);
    if (FAILED(hr) || !acc) return false;
    IUnknown *provider = nullptr;
    hr = u.from_acc(acc, CHILDID_SELF, 0, (void **)&provider);
    bool ok = false;
    if (SUCCEEDED(hr) && provider) {
        BSTR message = SysAllocString(text.wc_str());
        BSTR activity = SysAllocString(L"SingingVoiceStudio.NoteAdjustment");
        if (message && activity) {
            // NotificationKind_Other, NotificationProcessing_ImportantMostRecent
            ok = SUCCEEDED(u.raise(provider, 4, 1, message, activity));
        }
        if (message) SysFreeString(message);
        if (activity) SysFreeString(activity);
        provider->Release();
    }
    acc->Release();
    return ok;
#else
    (void)control;
    (void)text;
    return false;
#endif
}

}  // namespace svs
