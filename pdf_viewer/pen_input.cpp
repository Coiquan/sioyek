#include "pen_input.h"

#ifdef Q_OS_WIN

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QCursor>
#include <QDebug>
#include <QMouseEvent>
#include <QPointer>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
constexpr USAGE digitizer_page = 0x0d;
constexpr USAGE in_range = 0x32;
constexpr USAGE invert = 0x3c;
constexpr USAGE tip_switch = 0x42;
constexpr USAGE barrel_switch = 0x44;
constexpr USAGE eraser = 0x45;
constexpr USAGE tablet_pick = 0x46;
constexpr USAGE secondary_barrel_switch = 0x5a;
constexpr std::array<USAGE, 7> controls{
    in_range, invert, tip_switch, barrel_switch, eraser, tablet_pick, secondary_barrel_switch
};

struct ButtonField {
    USAGE usage;
    USHORT collection;
    UCHAR report_id;
    bool is_value;
    bool down = false;
};

struct PenDevice {
    std::vector<unsigned char> preparsed;
    std::vector<ButtonField> fields;
    bool usable = false;

    PHIDP_PREPARSED_DATA data() {
        return reinterpret_cast<PHIDP_PREPARSED_DATA>(preparsed.data());
    }

    explicit PenDevice(HANDLE device) {
        UINT size = 0;
        if (GetRawInputDeviceInfoW(device, RIDI_PREPARSEDDATA, nullptr, &size) == UINT(-1) || !size)
            return;
        preparsed.resize(size);
        if (GetRawInputDeviceInfoW(device, RIDI_PREPARSEDDATA, preparsed.data(), &size) == UINT(-1))
            return;
        HIDP_CAPS caps{};
        if (HidP_GetCaps(data(), &caps) != HIDP_STATUS_SUCCESS || caps.UsagePage != digitizer_page ||
            (caps.Usage != 0x02 && caps.Usage != 0x20))
            return; // Pen/Stylus collections only, never touchpads or touchscreens.

        auto add_fields = [this](const auto& cap, bool is_value) {
            if (cap.UsagePage != digitizer_page) return;
            const USAGE first = cap.IsRange ? cap.Range.UsageMin : cap.NotRange.Usage;
            const USAGE last = cap.IsRange ? cap.Range.UsageMax : cap.NotRange.Usage;
            for (USAGE usage : controls) {
                if (usage >= first && usage <= last)
                    fields.push_back({usage, cap.LinkCollection, cap.ReportID, is_value});
            }
        };
        USHORT count = caps.NumberInputButtonCaps;
        if (count) {
            std::vector<HIDP_BUTTON_CAPS> buttons(count);
            if (HidP_GetButtonCaps(HidP_Input, buttons.data(), &count, data()) == HIDP_STATUS_SUCCESS)
                for (USHORT i = 0; i < count; ++i) add_fields(buttons[i], false);
        }
        count = caps.NumberInputValueCaps;
        if (count) {
            std::vector<HIDP_VALUE_CAPS> values(count);
            if (HidP_GetValueCaps(HidP_Input, values.data(), &count, data()) == HIDP_STATUS_SUCCESS)
                for (USHORT i = 0; i < count; ++i) add_fields(values[i], true);
        }
        usable = std::any_of(fields.begin(), fields.end(), [](const ButtonField& field) {
            return field.usage == tip_switch;
        });
    }

    std::optional<PenInputState> read(char* report, ULONG size) {
        if (!usable || size == 0) return {};
        bool updated = false;
        // Update only fields carried by this report. Controls in other report
        // IDs retain their state instead of being spuriously released.
        auto next = fields;
        for (auto& field : next) {
            if (field.report_id != static_cast<unsigned char>(report[0])) continue;
            if (field.is_value) {
                ULONG value = 0;
                if (HidP_GetUsageValue(HidP_Input, digitizer_page, field.collection, field.usage,
                                      &value, data(), report, size) != HIDP_STATUS_SUCCESS)
                    return {};
                field.down = value != 0;
            }
            else {
                ULONG count = HidP_MaxUsageListLength(HidP_Input, digitizer_page, data());
                if (!count) return {};
                std::vector<USAGE> usages(count);
                if (HidP_GetUsages(HidP_Input, digitizer_page, field.collection, usages.data(),
                                  &count, data(), report, size) != HIDP_STATUS_SUCCESS)
                    return {};
                field.down = std::find(usages.begin(), usages.begin() + count, field.usage) != usages.begin() + count;
            }
            updated = true;
        }
        if (!updated) return {};
        fields = std::move(next);
        auto has = [this](USAGE usage) {
            return std::any_of(fields.begin(), fields.end(), [usage](const ButtonField& field) {
                return field.usage == usage;
            });
        };
        auto down = [this](USAGE usage) {
            return std::any_of(fields.begin(), fields.end(), [usage](const ButtonField& field) {
                return field.usage == usage && field.down;
            });
        };
        PenInputState state;
        state.in_range = !has(in_range) || down(in_range);
        state.tip_down = state.in_range && down(tip_switch);
        state.lower_down = state.in_range && down(barrel_switch);
        state.upper_down = state.in_range &&
            (down(secondary_barrel_switch) || down(tablet_pick) || down(invert) || down(eraser));
        return state;
    }
};

class WindowsPenInput : public QObject, public QAbstractNativeEventFilter {
    std::unordered_map<HANDLE, PenDevice> devices;
    QPointer<QWidget> receiver;
    HANDLE current_device = nullptr;
    bool suppress_pen_mouse = false;
    unsigned suppressed_buttons = 0;
    bool registered = false;

    void reset() {
        if (receiver) {
            PenInputState state;
            state.in_range = false;
            PenInputEvent event(state, QCursor::pos());
            QApplication::sendEvent(receiver, &event);
        }
        receiver.clear();
        current_device = nullptr;
        suppress_pen_mouse = false;
        suppressed_buttons = 0;
        for (auto& entry : devices)
            for (auto& field : entry.second.fields) field.down = false;
    }

    void dispatch(HANDLE device, const PenInputState& state) {
        if (!state.in_range && current_device != device) return;
        QWidget* target = QApplication::activeWindow();
        if (!target) return;
        if (receiver && (receiver != target || current_device != device)) reset();
        receiver = target;
        current_device = device;
        PenInputEvent event(state, QCursor::pos());
        QApplication::sendEvent(target, &event);
        // An upper-button press must not become a fake toolbar click. Lower
        // button mouse input is suppressed only while a drawing tool uses it.
        suppress_pen_mouse = state.upper_down || (event.isAccepted() && state.lower_down);
    }

    void read_input(HRAWINPUT handle) {
        UINT size = 0;
        if (GetRawInputData(handle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) || !size)
            return;
        std::vector<std::max_align_t> buffer((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        const UINT bytes = GetRawInputData(handle, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER));
        constexpr size_t report_offset = offsetof(RAWINPUT, data) + offsetof(RAWHID, bRawData);
        if (bytes == UINT(-1) || bytes < report_offset) return;
        auto* input = reinterpret_cast<RAWINPUT*>(buffer.data());
        if (input->header.dwType != RIM_TYPEHID) return;
        auto& hid = input->data.hid;
        if (!hid.dwSizeHid || hid.dwCount > (bytes - report_offset) / hid.dwSizeHid) return;
        auto& device = devices.try_emplace(input->header.hDevice, input->header.hDevice).first->second;
        for (DWORD i = 0; i < hid.dwCount; ++i) {
            auto state = device.read(reinterpret_cast<char*>(hid.bRawData + size_t(i) * hid.dwSizeHid), hid.dwSizeHid);
            if (state) dispatch(input->header.hDevice, *state);
        }
    }

    bool filter_pen_mouse(const MSG& message) {
        // Windows marks compatibility mouse messages generated by pens/touch.
        // Require the pen tool's state too; do not intercept physical mice.
        if ((GetMessageExtraInfo() & 0xffffff00) != 0xff515700) return false;
        unsigned button = 0;
        bool down = false, up = false;
        switch (message.message) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: button = 1; down = true; break;
        case WM_LBUTTONUP: button = 1; up = true; break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: button = 2; down = true; break;
        case WM_RBUTTONUP: button = 2; up = true; break;
        case WM_MOUSEMOVE: return suppress_pen_mouse || suppressed_buttons != 0;
        default: return false;
        }
        const bool suppress = suppress_pen_mouse || (suppressed_buttons & button) != 0;
        if (down && suppress) suppressed_buttons |= button;
        if (up) suppressed_buttons &= ~button;
        return suppress;
    }

public:
    explicit WindowsPenInput(QObject* parent) : QObject(parent) {
        RAWINPUTDEVICE registrations[] = {
            {digitizer_page, 0x02, RIDEV_DEVNOTIFY, nullptr}, // Pen
            {digitizer_page, 0x20, RIDEV_DEVNOTIFY, nullptr}  // Stylus
        };
        if (!RegisterRawInputDevices(registrations, 2, sizeof(RAWINPUTDEVICE))) {
            qWarning() << "Could not register raw pen input:" << GetLastError();
            return;
        }
        registered = true;
        qApp->installNativeEventFilter(this);
        qApp->installEventFilter(this);
    }

    ~WindowsPenInput() override {
        if (!registered) return;
        if (qApp) qApp->removeNativeEventFilter(this);
        RAWINPUTDEVICE registrations[] = {
            {digitizer_page, 0x02, RIDEV_REMOVE, nullptr},
            {digitizer_page, 0x20, RIDEV_REMOVE, nullptr}
        };
        RegisterRawInputDevices(registrations, 2, sizeof(RAWINPUTDEVICE));
    }

    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::ApplicationDeactivate ||
            (event->type() == QEvent::ApplicationStateChange &&
             QGuiApplication::applicationState() != Qt::ApplicationActive) ||
            (object == receiver && event->type() == QEvent::WindowDeactivate))
            reset();
        // Qt can synthesize mouse events itself when a toolbar ignores a
        // tablet event. These do not pass through the native WM_MOUSE filter.
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick ||
            event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseMove) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->source() != Qt::MouseEventNotSynthesized) {
                const unsigned button = unsigned(mouse->button()) & 3;
                const bool suppress = suppress_pen_mouse ||
                    (event->type() == QEvent::MouseMove ? suppressed_buttons != 0 :
                     (suppressed_buttons & button) != 0);
                if (suppress && (event->type() == QEvent::MouseButtonPress ||
                                 event->type() == QEvent::MouseButtonDblClick))
                    suppressed_buttons |= button;
                if (event->type() == QEvent::MouseButtonRelease) suppressed_buttons &= ~button;
                if (suppress) return true;
            }
        }
        return false;
    }

    bool nativeEventFilter(const QByteArray&, void* raw_message,
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                           qintptr* result
#else
                           long* result
#endif
                           ) override {
        const auto& message = *static_cast<MSG*>(raw_message);
        if (message.message == WM_INPUT) {
            read_input(reinterpret_cast<HRAWINPUT>(message.lParam));
            return false; // Keep normal Raw Input cleanup in DefWindowProc.
        }
        if (message.message == WM_INPUT_DEVICE_CHANGE && message.wParam == GIDC_REMOVAL) {
            const auto device = reinterpret_cast<HANDLE>(message.lParam);
            if (current_device == device) reset();
            devices.erase(device);
        }
        if (filter_pen_mouse(message)) {
            *result = 0;
            return true;
        }
        return false;
    }
};
} // namespace

void initialize_windows_pen_input() {
    static auto* input = new WindowsPenInput(qApp);
    Q_UNUSED(input);
}

#else

void initialize_windows_pen_input() {}

#endif
