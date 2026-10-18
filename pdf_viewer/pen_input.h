#pragma once

#include <QEvent>
#include <QPoint>

// Logical HID controls, independent of report byte offsets and device handles.
struct PenInputState {
    bool in_range = true;
    bool tip_down = false;
    bool lower_down = false;
    bool upper_down = false;
};

class PenInputEvent : public QEvent {
public:
    static QEvent::Type event_type() {
        static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
        return type;
    }

    PenInputEvent(PenInputState state, QPoint global_pos)
        : QEvent(event_type()), state(state), global_pos(global_pos) {
        setAccepted(false);
    }

    PenInputState state;
    QPoint global_pos;
};

// One foreground Raw Input registration per application, shared by all windows.
// Other platforms continue to use Qt tablet events.
void initialize_windows_pen_input();
