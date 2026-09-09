#pragma once
#include "present_model.h"

namespace present_observer {

    // A restore replaces the observer, never its faulted history in place. The
    // caller serializes this with the session gate and pumps it on main.
    class recovery {
    public:
        enum phase { original, removing, installing, observing, failed };

    private:
        phase current = original;
        uint64_t session = 0, restore = 0, content_floor = 0, after_frame = 0;
        int last_error = 0;

    public:
        phase state() const { return current; }
        int error() const { return last_error; }
        bool replacing() const { return current == removing || current == installing; }

        void begin(const session_command& command) {
            session = command.session;
            restore = command.serial;
            content_floor = command.content_revision;
            after_frame = command.after_frame;
            current = removing;
            last_error = 0;
        }

        // At most one removal and one install attempt per call. Busy is retried on
        // the next main update; neither the source nor the worker waits for it.
        template<class Remove, class Install> void pump(Remove remove, Install install) {
            if (current == removing) {
                int result = remove();
                if (result < 0) {
                    last_error = result;
                    current = failed;
                    return;
                }

                if (result) return;
                current = installing;
            }

            if (current == installing) {
                int result = install();
                if (result < 0) {
                    last_error = result;
                    current = failed;
                    return;
                }

                if (result) return;
                current = observing;
            }
        }

        void fail(int result) {
            if (result < 0 && current != original) {
                last_error = result;
                current = failed;
            }
        }

        // Restored GUI is uncaptured, so its generation is zero. Content may advance
        // after the restore ticket was issued, but only the currently published
        // content can provide restoration evidence.
        bool allows(const session_native_frame& marker, uint64_t current_content) const {
            if (current == original) return true;
            return current == observing && marker.session == session && marker.restore_serial == restore &&
                marker.generation == 0 && marker.content_revision == current_content && current_content >= content_floor &&
                marker.source_frame > after_frame && !marker.flags;
        }

        bool allows(const presented_frame& frame, uint64_t current_content) const {
            if (current == original) return true;
            return current == observing && frame.session == session && frame.restore == restore &&
                frame.generation == 0 && frame.content == current_content && current_content >= content_floor &&
                frame.frame > after_frame;
        }
    };

}
