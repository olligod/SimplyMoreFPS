#pragma once
#include "linux_core.h"
#include "../common/scene_packets.h"

namespace linux_session {

    enum storage_layer { base_storage, world_storage, hud_storage, cache_storage };

    struct texture_storage {
        uint32_t name = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 0;
        uint64_t bytes = 0;
    };

    struct slot_storage {
        uint64_t generation = 0;
        uint64_t content = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        texture_storage textures[4]{};
        texture_storage scene_textures[smf_scene::maximum_images]{};
        smf_scene::image scene_images[smf_scene::maximum_images]{};
        cache_packet copied_cache{};
        bool cache_valid = false;

        bool has_names() const {
            for (const auto& t : textures) {
                if (t.name) return true;
            }
            for (const auto& t : scene_textures) {
                if (t.name) return true;
            }
            return false;
        }

        uint64_t allocated_bytes() const {
            uint64_t result = 0;
            for (const auto& texture : textures) if (texture.name) result += texture.bytes;
            for (const auto& texture : scene_textures) if (texture.name) result += texture.bytes;
            return result;
        }

        bool has_scene() const {
            for (const auto& texture : scene_textures) if (texture.name) return true;
            return false;
        }

        bool compatible(uint64_t gen, uint64_t revision, uint32_t w, uint32_t h) const {
            return generation == gen && content == revision && width == w && height == h;
        }

        bool reusable(const lease_policy& lease, uint64_t gen, uint64_t revision, uint32_t w, uint32_t h) const {
            return lease.state == lease_free && compatible(gen, revision, w, h);
        }

        bool cache_hit(bool has_map, uint64_t gen, uint64_t revision, const cache_packet& descriptor) const {
            const auto& cache = textures[cache_storage];
            return has_map && cache_valid && generation == gen && content == revision && cache.name &&
                   cache.width == descriptor.width && cache.height == descriptor.height &&
                   descriptor.texture && descriptor.serial && !std::memcmp(&copied_cache, &descriptor, sizeof(descriptor));
        }
    };

    inline bool storage_needs_purge(const slot_storage& storage, const lease_policy& lease, bool generation_exists, bool retiring, bool stopping) {
        return lease.state == lease_free && storage.has_names() && (!generation_exists || retiring || stopping);
    }

    // Layers are frame-local: an allocated world or cache texture never draws for an absent-world frame.
    inline uint32_t logical_layer_mask(bool has_map, bool has_cache) {
        return 1u | 4u | (has_map ? 2u : 0u) | (has_map && has_cache ? 8u : 0u);
    }

}
