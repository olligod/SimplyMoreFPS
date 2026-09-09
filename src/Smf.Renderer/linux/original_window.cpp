#include "original_window.h"
#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/Xatom.h>
#include <atomic>
#include <cstring>
#include <sys/syscall.h>
#include <unistd.h>
#undef min
#undef max

namespace linux_session {
    namespace {

        constexpr unsigned max_nodes = 1024;
        constexpr unsigned max_depth = 8;
        constexpr unsigned max_clients = 256;
        constexpr unsigned max_screens = 16;

        std::atomic<uint32_t> owner{0};

        // Counts X errors on our private connection without a process-global handler.
        struct error_trap {
            Display* display;
            _XAsyncHandler handler{};
            _XAsyncErrorState errors{};

            explicit error_trap(Display* value) : display(value) {
                handler.handler = _XAsyncErrorHandler;
                handler.data = reinterpret_cast<XPointer>(&errors);

                XLockDisplay(display);
                handler.next = display->async_handlers;
                display->async_handlers = &handler;
                XUnlockDisplay(display);
            }

            ~error_trap() {
                XLockDisplay(display);
                _XDeqAsyncHandler(display, &handler);
                XUnlockDisplay(display);
            }
        };

        bool locking(Display* d) {
            return d && d->lock && d->lock_fns && d->lock_fns->lock_display && d->lock_fns->unlock_display;
        }

        uint32_t read_pid(Display* d, Window window, Atom property) {
            if (!property) return 0;

            Atom type = None;
            int format = 0;
            unsigned long count = 0;
            unsigned long left = 0;
            unsigned char* bytes = nullptr;

            int result = XGetWindowProperty(d, window, property, 0, 1, False, XA_CARDINAL, &type, &format, &count, &left, &bytes);
            uint32_t pid = 0;
            if (result == Success && type == XA_CARDINAL && format == 32 && count == 1 && left == 0 && bytes) {
                unsigned long value = *reinterpret_cast<unsigned long*>(bytes);
                if (value <= UINT32_MAX) pid = uint32_t(value);
            }

            if (bytes) XFree(bytes);
            return pid;
        }

        struct node {
            Window window = 0;
            Window parent = 0;
            Window root = 0;
            unsigned depth = 0;
            bool owned_ancestor = false;
        };

        int discover(Display* d, error_trap& trap, original_window& out) {
            Atom pid_property = XInternAtom(d, "_NET_WM_PID", True);
            Atom clients_property = XInternAtom(d, "_NET_CLIENT_LIST", True);
            Window clients[max_clients]{};
            unsigned client_count = 0;
            node nodes[max_nodes]{};
            unsigned head = 0;
            unsigned tail = 0;
            bool complete = true;
            window_selection selection;

            int screens = ScreenCount(d);
            out.stage = 2;
            if (screens < 1 || screens > int(max_screens)) {
                out.limit = 1;
                return -5;
            }

            for (int screen = 0; screen < screens; screen++) {
                Window root = RootWindow(d, screen);
                nodes[tail++] = {root, None, root, 0, false};
                if (!clients_property) continue;

                Atom type = None;
                int format = 0;
                unsigned long count = 0;
                unsigned long left = 0;
                unsigned char* bytes = nullptr;

                int result = XGetWindowProperty(d, root, clients_property, 0, max_clients, False, XA_WINDOW, &type, &format, &count, &left, &bytes);
                if (result == Success && type == XA_WINDOW && format == 32 && bytes) {
                    if (left || count > max_clients - client_count) {
                        complete = false;
                        out.limit = 2;
                    } else {
                        for (unsigned long i = 0; i < count; i++) clients[client_count++] = reinterpret_cast<unsigned long*>(bytes)[i];
                    }
                }

                if (bytes) XFree(bytes);
            }

            out.stage = 3;
            while (complete && head < tail) {
                node current = nodes[head++];
                Window tree_root = None;
                Window parent = None;
                Window* children = nullptr;
                unsigned count = 0;

                // Every node is queried; the EWMH client list is evidence, not a shortcut.
                if (!XQueryTree(d, current.window, &tree_root, &parent, &children, &count)) {
                    if (children) XFree(children);
                    complete = false;
                    break;
                }

                uint32_t pid = current.depth ? read_pid(d, current.window, pid_property) : 0;
                bool owned = pid == out.pid && pid != 0;
                if (owned) {
                    XWindowAttributes a{};
                    if (XGetWindowAttributes(d, current.window, &a)) {
                        bool listed = false;
                        for (unsigned i = 0; i < client_count; i++) {
                            if (clients[i] == current.window) listed = true;
                        }
                        window_candidate candidate{current.window, tree_root, parent, pid, uint32_t(a.width), uint32_t(a.height), uint32_t(a.depth),
                                                   a.c_class == InputOutput, a.map_state == IsViewable, current.owned_ancestor, listed};
                        selection.observe(candidate, out.pid);
                    }
                }

                if (count && (current.depth >= max_depth || count > max_nodes - tail)) {
                    complete = false;
                    out.limit = current.depth >= max_depth ? 3 : 4;
                }
                if (complete) {
                    for (unsigned i = 0; i < count; i++) {
                        nodes[tail++] = {children[i], current.window, current.root, current.depth + 1, current.owned_ancestor || owned};
                    }
                }

                if (children) XFree(children);
            }

            out.scanned = head;
            out.candidates = selection.count;
            out.stage = 4;
            XSync(d, False);
            out.x_errors = trap.errors.error_count;
            out.last_x_error = trap.errors.last_error_received;

            int result = selection.result(complete && head == tail, out.x_errors == 0);
            if (result) return result;

            const auto& found = selection.selected;
            out.window = found.window;
            out.root = found.root;
            out.parent = found.parent;
            out.width = found.width;
            out.height = found.height;
            out.depth = found.depth;
            out.map_state = IsViewable;
            out.flags = 8 | (found.ewmh_listed ? 1 : 0);

            // WM_CLASS is copied as raw bytes only; no Unity or SDL spelling is assumed.
            Atom type = None;
            int format = 0;
            unsigned long count = 0;
            unsigned long left = 0;
            unsigned char* bytes = nullptr;

            int read = XGetWindowProperty(d, out.window, XA_WM_CLASS, 0, 32, False, XA_STRING, &type, &format, &count, &left, &bytes);
            if (read == Success && type == XA_STRING && format == 8 && bytes && count <= sizeof(out.wm_class)) {
                std::memcpy(out.wm_class, bytes, count);
                out.reserved[0] = count;
                out.flags |= 2;
                if (left) out.flags |= 4;
            }

            if (bytes) XFree(bytes);
            XSync(d, False);
            out.x_errors = trap.errors.error_count;
            out.last_x_error = trap.errors.last_error_received;

            if (out.x_errors) {
                out.window = 0;
                return -4;
            }

            out.stage = 5;
            return 0;
        }

    }
}

extern "C" __attribute__((visibility("default"))) int smf_linux_find_original_window(linux_session::original_window* packet, uint32_t bytes) {
    using namespace linux_session;
    if (!packet || bytes != sizeof(*packet)) return -1;

    original_window out;
    out.pid = uint32_t(getpid());
    out.thread = uint32_t(syscall(SYS_gettid));
    out.stage = 1;

    uint32_t expected = 0;
    owner.compare_exchange_strong(expected, out.thread);
    if (owner.load() != out.thread) {
        out.result = -2;
        *packet = out;
        return out.result;
    }

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        out.result = -3;
        *packet = out;
        return out.result;
    }

    // Unity must already have Xlib locking on: never call XInitThreads this late
    // and never install a process-global X error handler.
    if (!locking(display)) {
        XCloseDisplay(display);
        out.result = -6;
        *packet = out;
        return out.result;
    }

    {
        error_trap trap(display);
        out.result = discover(display, trap, out);
    }

    XCloseDisplay(display);
    *packet = out;
    return out.result;
}

extern "C" __attribute__((visibility("default"))) int smf_session_find_original_window(uint64_t* original) {
    if (!original) return -1;

    *original = 0;
    linux_session::original_window packet;
    int result = smf_linux_find_original_window(&packet, sizeof(packet));
    if (result == 0) *original = packet.window;
    return result;
}
