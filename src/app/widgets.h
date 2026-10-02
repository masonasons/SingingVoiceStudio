// widgets.h -- the pieces every window here is built from, and what makes
// them read properly with a screen reader.
//
// Everything is built to be used without seeing it. Every control states its
// own accessible name rather than leaving Windows to guess it from whatever
// static text happens to precede it, which is wrong more often than not, and
// a spin control's edit box -- a separate window wx does not wrap -- is moved
// to sit behind its own caption, which is where Windows looks.
//
// The lists are the one place the platforms part. wxListCtrl in report mode
// is the native control on Windows and a screen reader reads it as it is.
// On macOS it is not native: wxWidgets draws it itself, and what reaches
// VoiceOver is an empty box. wxDataViewListCtrl there is a real NSOutlineView
// and is read properly, so ReportList is that on macOS and wxListCtrl on
// Windows, with the small part of wxListCtrl's interface this program uses
// over whichever is right, so nothing above it has to care.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/spinctrl.h>
#if wxUSE_ACCESSIBILITY
#include <wx/access.h>
#endif

#if defined(__WXMSW__)
#define SVS_NATIVE_LISTCTRL 1
#else
#define SVS_NATIVE_LISTCTRL 0
#include <wx/dataview.h>
#endif

namespace svs {

wxString W(const std::string &utf8);
std::string U(const wxString &s);

//: A label with the keys named as this platform names them: Ctrl and Alt on
//: Windows, Cmd and Option on macOS. Only the words before any tab are
//: changed; wx reads the accelerator after the tab itself, and already
//: puts Ctrl on the Command key on a Mac.
wxString K(const char *label);
//: True on macOS, where some keys are the system's own (Cmd+H hides the
//: program) and have to be given up.
bool is_mac();

#if SVS_NATIVE_LISTCTRL
//: A report list. wx.ListCtrl is native, and accessible, on Windows.
class ReportList : public wxListCtrl {
public:
    ReportList(wxWindow *parent, long style = wxLC_REPORT, const wxSize &size = wxDefaultSize);
#else
//: The same interface over the control macOS exposes to VoiceOver.
class ReportList : public wxDataViewListCtrl {
public:
    ReportList(wxWindow *parent, long style = wxLC_REPORT, const wxSize &size = wxDefaultSize);
    long InsertColumn(long col, const wxString &heading, int format = wxLIST_FORMAT_LEFT,
                      int width = -1);
    long InsertItem(long index, const wxString &label);
    bool SetItem(long index, int column, const wxString &label);
    int GetItemCount() const { return int(wxDataViewListCtrl::GetItemCount()); }
#endif
    std::vector<int> SelectedRows() const;
    int FirstSelected() const;
    void SelectRow(int index, bool on = true);
    void FocusRow(int index);

    //: What the window does when a row is activated (Enter, or a double
    //: click), when the selection moves, and when a key is pressed with the
    //: list focused. These stand in for Bind, since the events differ
    //: between the two controls.
    void OnActivated(std::function<void()> fn);
    void OnSelected(std::function<void()> fn);
    void OnKey(std::function<void(wxKeyEvent &)> fn);

    //: the caption above it, and the name a screen reader reads, together
    wxStaticText *caption_text = nullptr;
    class Named *caption_named = nullptr;

#if !SVS_NATIVE_LISTCTRL
private:
    //: Keys the native table claims for itself as well: Option and Command
    //: with the arrows mean "go to the end of the list" to an NSTableView,
    //: and the editor uses those same combinations to transpose a note and
    //: to change its length. The window is given first refusal on them.
    static bool contested(const wxKeyEvent &evt);
    bool owns_focus() const;
    void on_char_hook(wxKeyEvent &evt);
    int cols_ = 0;
    std::function<void(wxKeyEvent &)> key_;
#endif
};

#if wxUSE_ACCESSIBILITY
//: Gives one control the name and description a screen reader reads. The
//: name is kept to a word or two; what the field wants goes in the
//: description, which is read after it and can be turned off.
class Named : public wxAccessible {
public:
    Named(wxWindow *win, const wxString &name, const wxString &hint = "")
        : wxAccessible(win), name(name), hint(hint) {}
    wxAccStatus GetName(int childId, wxString *out) override;
    wxAccStatus GetDescription(int childId, wxString *out) override;
    wxString name, hint;
};
#else
class Named {
public:
    wxString name;
};
#endif

//: A caption beside a control, and the same words to a screen reader.
wxWindow *labelled(wxWindow *parent, wxSizer *sizer, const wxString &label, wxWindow *ctrl,
                   int proportion = 0, const wxString &hint = "");
//: A caption above a control that fills the window, such as a list.
ReportList *caption(wxWindow *parent, wxSizer *sizer, const wxString &label, ReportList *ctrl,
                    int proportion = 1);
//: Change a captioned list's label and what is read out with it, together.
void relabel(ReportList *ctrl, const wxString &label);
//: Ask a screen reader to read a row again after its contents changed.
void reannounce(ReportList *list, int row);

}  // namespace svs
