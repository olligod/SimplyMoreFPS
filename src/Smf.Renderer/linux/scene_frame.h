#pragma once
#include "linux_core.h"
#include "../common/scene_snapshot.h"

namespace linux_session {

    enum class scene_admission { ready, malformed, unsupported };

    inline bool valid_pre_gui_flags(const pre_gui_packet& frame) {
        return !frame.reserved && !(frame.flags & ~(pre_gui_map | pre_gui_scene)) &&
            (!(frame.flags & pre_gui_scene) || (frame.flags & pre_gui_map));
    }

    inline scene_admission read_scene_frame(const frame_packet& frame, smf_scene::snapshot& output) {
        if (!(frame.flags & 1) || !frame.scene_description || (frame.scene_description & 7) ||
            !std::isfinite(frame.pose.pixel_width) || !std::isfinite(frame.pose.pixel_height) ||
            frame.pose.pixel_width < 1 || frame.pose.pixel_height < 1 ||
            frame.pose.pixel_width > 16384 || frame.pose.pixel_height > 16384 ||
            std::floor(frame.pose.pixel_width) != frame.pose.pixel_width ||
            std::floor(frame.pose.pixel_height) != frame.pose.pixel_height ||
            !smf_scene::read_snapshot(reinterpret_cast<const smf_scene::description*>(frame.scene_description),
                frame.source_frame, uint32_t(frame.pose.pixel_width), uint32_t(frame.pose.pixel_height), output)) return scene_admission::malformed;
        for (uint32_t i = 0; i < output.frame.image_count; ++i) {
            if (output.images[i].texture > UINT32_MAX) return scene_admission::malformed;
        }
        for (uint32_t i = 0; i < output.frame.effect_count; ++i) {
            if (output.effects[i].kind == smf_scene::image_filter) return scene_admission::unsupported;
        }
        return scene_admission::ready;
    }

    inline bool scene_projection(const pose_packet& pose, const camera_model& model, const affine& captured,
        double root_x, double root_z, double half_height, affine& desired, double& camera_x, double& camera_z) {
        const double shake_x = pose.x - pose.root_x;
        const double shake_z = pose.z - pose.root_z;
        camera_x = root_x + shake_x;
        camera_z = root_z + shake_z;
        desired = captured;
        if (root_x == pose.root_x && root_z == pose.root_z && half_height == pose.orthographic_size) {
            camera_x = pose.x;
            camera_z = pose.z;
            return true;
        }
        if (!model.root(root_x, root_z, half_height, desired)) return false;
        desired.c -= desired.a * shake_x + desired.b * shake_z;
        desired.f -= desired.d * shake_x + desired.e * shake_z;
        return true;
    }

}
