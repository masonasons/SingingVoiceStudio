#include "app/widgets.h"

#ifdef __WXMSW__
#include <windows.h>
#include <commctrl.h>
#endif

namespace svs {

wxString W(const std::string &utf8) { return wxString::FromUTF8(utf8.c_str()); }
std::string U(const wxString &s) { return std::string(s.ToUTF8().data()); }

ReportList::ReportList(wxWindow *parent, long style, const wxSize &size)
    : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, size, style) {}

std::vector<int> ReportList::SelectedRows() const {
    std::vector<int> rows;
    long i = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    while (i != -1) {
        rows.push_back(int(i));
        i = GetNextItem(i, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    }
    return rows;
}

int ReportList::FirstSelected() const {
    return int(GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED));
}

void ReportList::SelectRow(int index, bool on) {
    if (index < 0 || index >= GetItemCount()) return;
    SetItemState(index, on ? wxLIST_STATE_SELECTED : 0, wxLIST_STATE_SELECTED);
}

void ReportList::FocusRow(int index) {
    if (index < 0 || index >= GetItemCount()) return;
    SetItemState(index, wxLIST_STATE_FOCUSED, wxLIST_STATE_FOCUSED);
    EnsureVisible(index);
}

#if wxUSE_ACCESSIBILITY
// Only the control itself is named here. Its children -- the rows of a list
// -- are left to the native control, which reads them correctly.
wxAccStatus Named::GetName(int childId, wxString *out) {
    if (childId != wxACC_SELF) return wxACC_NOT_IMPLEMENTED;
    *out = name;
    return wxACC_OK;
}

wxAccStatus Named::GetDescription(int childId, wxString *out) {
    if (childId != wxACC_SELF || hint.empty()) return wxACC_NOT_IMPLEMENTED;
    *out = hint;
    return wxACC_OK;
}
#endif

static Named *set_accessible(wxWindow *ctrl, const wxString &name, const wxString &hint) {
#if wxUSE_ACCESSIBILITY
    Named *named = new Named(ctrl, name, hint);
    ctrl->SetAccessible(named);   // the window owns it from here
    return named;
#else
    return nullptr;
#endif
}

// A spin control on Windows is two windows: the arrows, and a separate edit
// box beside them that takes the focus and the typing. wx wraps only the
// arrows, so naming the control names the half nobody lands on. The edit box
// is moved to sit behind its own caption in Z-order, which is where Windows
// looks for a name.
static void name_spin_buddy(wxWindow *ctrl, wxStaticText *text) {
#ifdef __WXMSW__
    wxSpinCtrl *spin = wxDynamicCast(ctrl, wxSpinCtrl);
    if (!spin) return;
    HWND buddy = (HWND)SendMessageW((HWND)spin->GetHWND(), UDM_GETBUDDY, 0, 0);
    if (!buddy) {
        // wx's own spin control is the edit box with the arrows as its
        // buddy; either way round, the edit box goes after the caption
        buddy = (HWND)spin->GetHWND();
    }
    SetWindowPos(buddy, (HWND)text->GetHWND(), 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
    (void)ctrl;
    (void)text;
#endif
}

wxWindow *labelled(wxWindow *parent, wxSizer *sizer, const wxString &label, wxWindow *ctrl,
                   int proportion, const wxString &hint) {
    wxStaticText *text =
        new wxStaticText(parent, wxID_ANY, hint.empty() ? label : label + " (" + hint + ")");
    ctrl->MoveAfterInTabOrder(text);
    ctrl->SetName(label);
    set_accessible(ctrl, label, hint);
    name_spin_buddy(ctrl, text);
    wxBoxSizer *box = new wxBoxSizer(wxHORIZONTAL);
    box->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    box->Add(ctrl, proportion, wxALIGN_CENTER_VERTICAL);
    sizer->Add(box, 0, wxEXPAND | wxALL, 4);
    return ctrl;
}

ReportList *caption(wxWindow *parent, wxSizer *sizer, const wxString &label, ReportList *ctrl,
                    int proportion) {
    wxStaticText *text = new wxStaticText(parent, wxID_ANY, label);
    ctrl->MoveAfterInTabOrder(text);
    ctrl->SetName(label);
    ctrl->caption_named = set_accessible(ctrl, label, "");
    ctrl->caption_text = text;
    sizer->Add(text, 0, wxLEFT | wxTOP, 8);
    sizer->Add(ctrl, proportion, wxEXPAND | wxALL, 8);
    return ctrl;
}

void relabel(ReportList *ctrl, const wxString &label) {
    if (ctrl->caption_text) ctrl->caption_text->SetLabel(label);
#if wxUSE_ACCESSIBILITY
    if (ctrl->caption_named) ctrl->caption_named->name = label;
#endif
    ctrl->SetName(label);
}

// Changing an item's text tells a screen reader nothing, so the focus event
// is raised by hand. Both ways of making the control raise a real one damage
// it: clearing the focused state moves the selection, and clearing the
// selected state stops the arrow keys moving between rows.
void reannounce(wxListCtrl *list, int row) {
#ifdef __WXMSW__
    if (row < 0 || row >= list->GetItemCount()) return;
    NotifyWinEvent(EVENT_OBJECT_FOCUS, (HWND)list->GetHWND(), OBJID_CLIENT, row + 1);
#else
    (void)list;
    (void)row;
#endif
}

}  // namespace svs
