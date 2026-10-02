// announce.h -- say something through the screen reader without moving
// keyboard focus.
//
// A note nudged up a semitone changes a row of the list and nothing else, and
// a screen reader speaks what is focused when the focus arrives, so the change
// would happen in silence. A UI Automation notification carries the new value
// to whatever screen reader is running, in its own voice; a held arrow key
// supersedes earlier values rather than queueing them.
#pragma once

#include <wx/window.h>

namespace svs {

//: True if the notification was raised. False is not an error: the caller
//: falls back to raising a focus event on the row instead.
bool announce(wxWindow *control, const wxString &text);

}  // namespace svs
