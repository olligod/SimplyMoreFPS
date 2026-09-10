#pragma once
#include "camera_packets.h"
#include <cmath>

namespace camera_worker_protocol {

    constexpr uint32_t flag_eligible = 1;
    constexpr uint32_t flag_owned = 2;
    constexpr uint32_t flag_motion_blocked = 4;
    constexpr uint32_t flag_text_captured = 8;
    constexpr uint32_t flag_search_focused = 16;

    inline bool valid_pose(const smf_camera_pose& p) {
        return p.version == 2 && p.size == sizeof(p) && p.session && p.epoch && p.pose_sequence && p.map_id >= 0 &&
            !p.reserved && !(p.pan_flags & ~SMF_CAMERA_PAN_COMPLETED) &&
            p.active_pan_id <= INT64_MAX && p.finished_pan_id <= INT64_MAX &&
            (!p.pan_flags || p.finished_pan_id) &&
            std::isfinite(p.x) && std::isfinite(p.z) && std::isfinite(p.root_size) && p.root_size > 0 &&
            std::isfinite(p.projection_half_height) && p.projection_half_height > 0;
    }

    // Worker-owned values only. The platform owns synchronization, focus and kernel loading.
    struct worker {
        smf_bridge_main main{};
        smf_bridge_status status{};
        smf_camera_pose pose{};
        smf_bridge_desired prepared{};
        bool have_main = false;
        bool previous_middle = false;
        bool prepared_valid = false;
        bool prepared_publish = false;
        double previous_x = 0;
        double previous_y = 0;
        double seed_size = 0;
        uint64_t input_sequence = 0;
        uint64_t settings_revision = 0;

        worker() {
            status.size = sizeof(status);
            status.version = 2;
        }

        void begin_prepare() {
            prepared_valid = false;
            prepared_publish = false;
        }

        void accept_main(const smf_bridge_main& incoming) {
            if (!have_main || incoming.publication > main.publication) {
                main = incoming;
                have_main = true;
                status.main_revision = incoming.publication;
            }
        }

        bool matches_epoch(uint64_t epoch) const {
            return have_main && epoch && main.state.epoch == epoch;
        }

        void withdraw() {
            status.desired = {};
            previous_middle = false;
        }

        void fault(int32_t error) {
            status.state = bridge_fault;
            status.result = error;
            begin_prepare();
            withdraw();
        }

        bool prepare_main_pose(uint64_t epoch, smf_bridge_desired& target) {
            const auto& state = main.state;
            if (state.map_id < 0 || !std::isfinite(state.x) || !std::isfinite(state.z) ||
                !std::isfinite(state.root_size) || state.root_size <= 0 ||
                !std::isfinite(state.projection_half_height) || state.projection_half_height <= 0) return false;

            prepared = {2, sizeof(smf_bridge_desired), epoch, 0, state.map_id, 0, state.x, state.z,
                state.root_size, state.projection_half_height, 0, 0, 0, 0};
            prepared_valid = true;
            target = prepared;
            return true;
        }

        template <class Kernel>
        int32_t adopt_epoch(Kernel& kernel, uint64_t epoch, double seconds, int32_t invalid_pose) {
            if (status.kernel_session && status.worker_epoch == epoch) return SMF_CAMERA_OK;

            const auto& state = main.state;
            smf_camera_init init{2, sizeof(smf_camera_init), epoch, state.map_id, 0,
                state.x, state.z, state.root_size, seconds};
            const int32_t result = status.kernel_session
                ? kernel.adopt(status.kernel_session, &init, sizeof(init), &main.settings, sizeof(main.settings), &pose, sizeof(pose))
                : kernel.create(&init, sizeof(init), &main.settings, sizeof(main.settings), &pose, sizeof(pose));
            if (result != SMF_CAMERA_OK) return result;
            if (!valid_pose(pose)) return invalid_pose;

            status.kernel_session = pose.session;
            status.worker_epoch = epoch;
            status.seed_sequence = pose.pose_sequence;
            status.state = bridge_seed;
            status.result = SMF_CAMERA_OK;
            ++status.adopts;
            input_sequence = 0;
            settings_revision = pose.settings_revision;
            seed_size = pose.root_size;
            previous_middle = false;
            return SMF_CAMERA_OK;
        }

        template <class Kernel>
        int32_t configure(Kernel& kernel, uint64_t epoch, int32_t invalid_pose) {
            if (main.state.map_id != pose.map_id || main.state.applied_sequence > pose.pose_sequence) return invalid_pose;
            if (main.settings.revision == settings_revision) return SMF_CAMERA_OK;

            const int32_t result = kernel.configure(status.kernel_session, epoch,
                &main.settings, sizeof(main.settings), &pose, sizeof(pose));
            if (result != SMF_CAMERA_OK) return result;
            if (!valid_pose(pose)) return invalid_pose;

            settings_revision = pose.settings_revision;
            ++status.configs;
            return SMF_CAMERA_OK;
        }

        bool acknowledged() const {
            return (main.state.flags & flag_owned) != 0 && main.state.applied_sequence >= status.seed_sequence;
        }

        smf_camera_input next_input(uint64_t epoch, double seconds) {
            smf_camera_input input{};
            input.version = 2;
            input.size = sizeof(input);
            input.epoch = epoch;
            input.sequence = ++input_sequence;
            input.settings_revision = settings_revision;
            input.monotonic_seconds = seconds;
            return input;
        }

        smf_camera_input motion_input(uint64_t epoch, double seconds, bool blocked, bool pointer_valid, double x, double y) {
            smf_camera_input input = next_input(epoch, seconds);
            input.trajectory = main.trajectory;
            input.pointer_x = pointer_valid ? x : 0;
            input.pointer_y = pointer_valid ? y : 0;
            if (blocked) input.flags |= SMF_CAMERA_MOTION_BLOCKED;
            return input;
        }

        void pointer_motion(bool blocked, double x, double y, bool middle, smf_camera_input& input) {
            if (!blocked) {
                if (middle && previous_middle) {
                    input.drag_x = x - previous_x;
                    input.drag_y = y - previous_y;
                }
                if (!middle && previous_middle) input.flags |= SMF_CAMERA_MIDDLE_RELEASED_PULSE;
            }

            previous_middle = !blocked && middle;
            previous_x = x;
            previous_y = y;
        }

        template <class Kernel>
        int32_t step(Kernel& kernel, const smf_camera_input& input, int64_t now, bool blocked,
            bool keyboard_blocked, bool middle, int32_t invalid_pose) {
            const int32_t result = kernel.step(status.kernel_session, &input, sizeof(input), &pose, sizeof(pose));
            if (result != SMF_CAMERA_OK) return result;
            if (!valid_pose(pose)) return invalid_pose;

            ++status.steps;
            status.step_qpc = now;
            status.state = blocked ? bridge_blocked : bridge_owned;
            status.flags = (keyboard_blocked ? 1u : 0u) | (blocked ? 2u : 0u) | (middle ? 4u : 0u);
            return SMF_CAMERA_OK;
        }

        template <class Kernel>
        int32_t wait_for_seed(Kernel& kernel, uint64_t epoch, double seconds, int32_t invalid_pose, int32_t moved_seed) {
            // Only the clock advances until main applies the seed: no input, trajectory or clamping.
            smf_camera_input idle = next_input(epoch, seconds);
            idle.flags = SMF_CAMERA_CLOCK_ONLY;
            const int32_t result = kernel.step(status.kernel_session, &idle, sizeof(idle), &pose, sizeof(pose));
            if (result != SMF_CAMERA_OK) return result;
            if (!valid_pose(pose)) return invalid_pose;
            if (pose.root_size != seed_size || pose.pose_sequence != status.seed_sequence) return moved_seed;

            status.state = bridge_seed;
            previous_middle = false;
            return SMF_CAMERA_OK;
        }

        void prepare_pose(smf_bridge_desired& target) {
            prepared = {2, sizeof(smf_bridge_desired), pose.epoch, pose.pose_sequence, pose.map_id, 0,
                pose.x, pose.z, pose.root_size, pose.projection_half_height,
                pose.active_pan_id, pose.finished_pan_id, pose.pan_flags, 0};
            prepared_valid = true;
            prepared_publish = true;
            target = prepared;
        }

        void publish_prepared() {
            status.desired = prepared_publish ? prepared : smf_bridge_desired{};
        }

        void committed(int64_t now) {
            ++status.commit_sequence;
            status.commit_qpc = now;
            publish_prepared();
        }

        template <class Kernel>
        void removed(Kernel& kernel) {
            if (status.kernel_session && kernel.release) {
                smf_camera_pose output{};
                const int32_t result = kernel.release(status.kernel_session, &output, sizeof(output));
                status.result = result;
                if (result == SMF_CAMERA_OK) status.kernel_session = 0;
            }

            withdraw();
            prepared_valid = false;
            status.state = bridge_dormant;
        }
    };

}
