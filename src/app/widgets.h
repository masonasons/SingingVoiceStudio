// widgets.h -- the pieces every window here is built from, and what makes
// them read properly with a screen reader.
//
// Everything is built to be used without seeing it. Every control states its
// own accessible name rather than leaving Windows to guess it from whatever
// static text happens to precede it, which is wrong more often than not, and
// a spin control's edit box -- a separate window wx does not wrap -- is moved
// to sit behind its own caption, which is where Windows looks.
#pragma once

#include <string>
#include <vector>

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/spinctrl.h>
#if wxUSE_ACCESSIBILITY
#include <wx/access.h>
#endif

namespace svs {

wxString W(const std::string &utf8);
std::string U(const wxString &s);

//: A report list. wx.ListCtrl is native, and accessible, on Windows.
class ReportList : public wxListCtrl {
public:
    ReportList(wxWindow *parent, long style = wxLC_REPORT, const wxSize &size = wxDefaultSize);
    std::vector<int> SelectedRows() const;
    int FirstSelected() const;
    void SelectRow(int index, bool on = true);
    void FocusRow(int index);
    //: the caption above it, and the name a screen reader reads, together
    wxStaticText *caption_text = nullptr;
    class Named *caption_named = nullptr;
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
void reannounce(wxListCtrl *list, int row);

}  // namespace svs
