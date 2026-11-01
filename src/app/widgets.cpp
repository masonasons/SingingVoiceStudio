#include "app/widgets.h"

#include <algorithm>

#ifdef __WXMSW__
#include <windows.h>
#include <commctrl.h>
#endif

namespace svs {

wxString W(const std::string &utf8) { return wxString::FromUTF8(utf8.c_str()); }
std::string U(const wxString &s) { return std::string(s.ToUTF8().data()); }

bool is_mac() {
#ifdef __WXOSX__
    return true;
#else
    return false;
#endif
}

wxString K(const char *label) {
    std::string text = label;
    if (!is_mac()) return W(text);
    size_t tab = text.find('\t');
    std::string head = tab == std::string::npos ? text : text.substr(0, tab);
    std::string tail = tab == std::string::npos ? std::string() : text.substr(tab);
    auto swap = [&head](const char *from, const char *to) {
        size_t at = 0;
        while ((at = head.find(from, at)) != std::string::npos) {
            head.replace(at, std::string(from).size(), to);
            at += std::string(to).size();
        }
    };
    swap("Ctrl", "Cmd");
    swap("Alt", "Option");
    return W(head + tail);
}

#if SVS_NATIVE_LISTCTRL

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

int ReportList::FocusedRow() const {
    return int(GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_FOCUSED));
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

void ReportList::OnActivated(std::function<void()> fn) {
    Bind(wxEVT_LIST_ITEM_ACTIVATED, [fn](wxListEvent &) { fn(); });
}

void ReportList::OnSelected(std::function<void()> fn) {
    Bind(wxEVT_LIST_ITEM_SELECTED, [fn](wxListEvent &evt) {
        evt.Skip();
        fn();
    });
}

void ReportList::OnKey(std::function<void(wxKeyEvent &)> fn) {
    Bind(wxEVT_KEY_DOWN, [fn](wxKeyEvent &evt) { fn(evt); });
}

void ReportList::OnContextMenu(std::function<void(wxPoint at)> fn) {
    Bind(wxEVT_CONTEXT_MENU, [this, fn](wxContextMenuEvent &evt) {
        wxPoint at = wxDefaultPosition;
        wxRect row;
        // from the keyboard, which gives no position: below its row
        if (evt.GetPosition() == wxDefaultPosition && FocusedRow() >= 0 && GetItemRect(FocusedRow(), row))
            at = row.GetBottomLeft();
        fn(at);
    });
}

// The list view's own check boxes: Space ticks them, and a screen reader
// says "checked" and "not checked".
void ReportList::EnableChecks() {
    EnableCheckBoxes(true);
    auto changed = [this](wxListEvent &evt) {
        evt.Skip();
        if (!setting_ && checked_) checked_(int(evt.GetIndex()));
    };
    Bind(wxEVT_LIST_ITEM_CHECKED, changed);
    Bind(wxEVT_LIST_ITEM_UNCHECKED, changed);
}

bool ReportList::IsChecked(int index) const {
    return index >= 0 && index < GetItemCount() && IsItemChecked(index);
}

void ReportList::SetChecked(int index, bool on) {
    if (index < 0 || index >= GetItemCount()) return;
    setting_ = true;
    CheckItem(index, on);
    setting_ = false;
}

void ReportList::OnChecked(std::function<void(int)> fn) { checked_ = std::move(fn); }

#else

ReportList::ReportList(wxWindow *parent, long style, const wxSize &size)
    : wxDataViewListCtrl(parent, wxID_ANY, wxDefaultPosition, size,
                         wxDV_ROW_LINES | ((style & wxLC_SINGLE_SEL) ? wxDV_SINGLE : wxDV_MULTIPLE)) {}

long ReportList::InsertColumn(long col, const wxString &heading, int, int width) {
    AppendTextColumn(heading, wxDATAVIEW_CELL_INERT, width > 0 ? width : -1);
    ++cols_;
    return col;
}

long ReportList::InsertItem(long index, const wxString &label) {
    wxVector<wxVariant> row;
    if (checks_) row.push_back(wxVariant(false));
    row.push_back(wxVariant(label));
    for (int c = 1; c < cols_; ++c) row.push_back(wxVariant(wxString()));
    if (index >= long(GetItemCount()))
        AppendItem(row);
    else
        wxDataViewListCtrl::InsertItem(unsigned(std::max(0L, index)), row);
    return index;
}

bool ReportList::SetItem(long index, int column, const wxString &label) {
    if (index < 0 || index >= long(GetItemCount()) || column < 0 || column >= cols_) return false;
    SetTextValue(label, unsigned(index), unsigned(column + (checks_ ? 1 : 0)));
    return true;
}

// The check boxes are a column of their own, ahead of the others.
void ReportList::EnableChecks() {
    if (checks_) return;
    checks_ = true;
    AppendToggleColumn("", wxDATAVIEW_CELL_ACTIVATABLE);
    Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, [this](wxDataViewEvent &evt) {
        evt.Skip();
        int row = ItemToRow(evt.GetItem());
        if (!setting_ && checked_ && evt.GetColumn() == 0 && row != wxNOT_FOUND) checked_(row);
    });
    Bind(wxEVT_CHAR_HOOK, &ReportList::on_space, this);
}

bool ReportList::IsChecked(int index) const {
    return checks_ && index >= 0 && index < GetItemCount() && GetToggleValue(unsigned(index), 0);
}

void ReportList::SetChecked(int index, bool on) {
    if (!checks_ || index < 0 || index >= GetItemCount()) return;
    setting_ = true;
    SetToggleValue(on, unsigned(index), 0);
    setting_ = false;
}

void ReportList::OnChecked(std::function<void(int)> fn) { checked_ = std::move(fn); }

// The table does not tick a box from the keyboard, so Space does it here.
void ReportList::on_space(wxKeyEvent &evt) {
    int row = FocusedRow();
    if (evt.GetKeyCode() != WXK_SPACE || evt.HasAnyModifiers() || !owns_focus() || row < 0) {
        evt.Skip();
        return;
    }
    SetChecked(row, !IsChecked(row));
    if (checked_) checked_(row);
}

std::vector<int> ReportList::SelectedRows() const {
    std::vector<int> rows;
    wxDataViewItemArray items;
    GetSelections(items);
    for (const wxDataViewItem &item : items) {
        int r = ItemToRow(item);
        if (r != wxNOT_FOUND) rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

int ReportList::FirstSelected() const {
    std::vector<int> rows = SelectedRows();
    return rows.empty() ? -1 : rows[0];
}

int ReportList::FocusedRow() const {
    wxDataViewItem item = GetCurrentItem();
    return item.IsOk() ? ItemToRow(item) : -1;
}

// The editor drives everything from the keyboard, so where the selection is
// *and* where the focus sits both matter: selecting a row without making it
// current leaves the next arrow key starting from wherever the control was.
void ReportList::SelectRow(int index, bool on) {
    if (index < 0 || index >= GetItemCount()) return;
    if (on)
        wxDataViewListCtrl::SelectRow(unsigned(index));
    else
        UnselectRow(unsigned(index));
}

void ReportList::FocusRow(int index) {
    if (index < 0 || index >= GetItemCount()) return;
    wxDataViewItem item = RowToItem(index);
    EnsureVisible(item);
    SetCurrentItem(item);
}

void ReportList::OnActivated(std::function<void()> fn) {
    Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [fn](wxDataViewEvent &) { fn(); });
}

void ReportList::OnSelected(std::function<void()> fn) {
    Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [fn](wxDataViewEvent &evt) {
        evt.Skip();
        fn();
    });
}

void ReportList::OnContextMenu(std::function<void(wxPoint at)> fn) {
    Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, [fn](wxDataViewEvent &) { fn(wxDefaultPosition); });
}

// wxEVT_KEY_DOWN arrives too late for the contested keys: the table has
// already acted by then, and not skipping the event does not undo it.
// wxEVT_CHAR_HOOK is delivered first, so a handler that takes a key there
// stops it reaching the table at all. Anything the handler does not want is
// skipped and carries on as before.
void ReportList::OnKey(std::function<void(wxKeyEvent &)> fn) {
    key_ = std::move(fn);
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent &evt) { key_(evt); });
    Bind(wxEVT_CHAR_HOOK, &ReportList::on_char_hook, this);
}

bool ReportList::contested(const wxKeyEvent &evt) {
    int code = evt.GetKeyCode();
    bool arrow = code == WXK_UP || code == WXK_DOWN || code == WXK_LEFT || code == WXK_RIGHT;
    return arrow && (evt.AltDown() || evt.ControlDown() || evt.CmdDown() || evt.RawControlDown());
}

bool ReportList::owns_focus() const {
    for (wxWindow *w = wxWindow::FindFocus(); w; w = w->GetParent())
        if (w == this) return true;
    return false;
}

void ReportList::on_char_hook(wxKeyEvent &evt) {
    if (!key_ || !contested(evt) || !owns_focus()) {
        evt.Skip();
        return;
    }
    key_(evt);
}

#endif

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

// Windows only: on Cocoa and GTK the class exists but has no effect, and the
// screen reader reads the control's name, which SetName supplies on every
// platform.
static Named *set_accessible(wxWindow *ctrl, const wxString &name, const wxString &hint) {
#if wxUSE_ACCESSIBILITY
    Named *named = new Named(ctrl, name, hint);
    ctrl->SetAccessible(named);   // the window owns it from here
    return named;
#else
    (void)ctrl;
    (void)name;
    (void)hint;
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
    if (wxDynamicCast(ctrl, wxSpinCtrl)) select_on_focus(ctrl);
    wxBoxSizer *box = new wxBoxSizer(wxHORIZONTAL);
    box->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    box->Add(ctrl, proportion, wxALIGN_CENTER_VERTICAL);
    sizer->Add(box, 0, wxEXPAND | wxALL, 4);
    return ctrl;
}

// Tabbing into a text box selects its text already. A spin control's edit
// box, and a box the program puts the focus in, get only the caret at the
// start, so what was typed went in front of what was there.
void select_on_focus(wxWindow *field) {
    field->Bind(wxEVT_SET_FOCUS, [field](wxFocusEvent &evt) {
        evt.Skip();
        if (wxGetMouseState().LeftIsDown()) return;
        field->CallAfter([field] {
            if (wxSpinCtrl *s = wxDynamicCast(field, wxSpinCtrl)) s->SetSelection(-1, -1);
            else if (wxTextCtrl *t = wxDynamicCast(field, wxTextCtrl)) t->SelectAll();
        });
    });
}

void enable_labelled(wxWindow *ctrl, bool on) {
    ctrl->Enable(on);
    if (wxStaticText *text = wxDynamicCast(ctrl->GetPrevSibling(), wxStaticText)) text->Enable(on);
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
// selected state stops the arrow keys moving between rows. On macOS the
// announcement itself (announce.h) does this job, and nothing is raised here.
void reannounce(ReportList *list, int row) {
#ifdef __WXMSW__
    if (row < 0 || row >= list->GetItemCount()) return;
    NotifyWinEvent(EVENT_OBJECT_FOCUS, (HWND)list->GetHWND(), OBJID_CLIENT, row + 1);
#else
    (void)list;
    (void)row;
#endif
}

}  // namespace svs
