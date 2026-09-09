#include "linux_unity_swap_slot.h"
#include "unity_engine_signature.h"
#include <dlfcn.h>
#include <link.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace linux_session {
    namespace {

        struct range {
            uintptr_t begin;
            uintptr_t end;
            bool write;
            bool execute;
        };

        // Readable mappings of this process, taken from /proc/self/maps.
        struct address_map {
            std::vector<range> ranges;

            address_map() {
                FILE* f = fopen("/proc/self/maps", "r");
                if (!f) return;

                char line[512];
                while (fgets(line, sizeof(line), f)) {
                    unsigned long b = 0;
                    unsigned long e = 0;
                    char p[5]{};
                    if (sscanf(line, "%lx-%lx %4s", &b, &e, p) == 3 && p[0] == 'r') ranges.push_back({b, e, p[1] == 'w', p[2] == 'x'});
                }

                fclose(f);
            }

            bool has(uintptr_t at, size_t bytes, bool write = false, bool execute = false) const {
                if (!at || at + bytes < at) return false;
                for (const auto& r : ranges) {
                    if (at >= r.begin && at + bytes <= r.end && (!write || r.write) && (!execute || r.execute) && (!write || !r.execute)) return true;
                }
                return false;
            }

            bool read_pointer(uintptr_t at, uintptr_t& value) const {
                if (at % alignof(void*) || !has(at, sizeof(value))) return false;
                std::memcpy(&value, reinterpret_cast<void*>(at), sizeof(value));
                return value != 0;
            }
        };

        struct module_info {
            uintptr_t base = 0;
            unsigned count = 0;
            bool build_id_match = false;
        };

        int find_module(dl_phdr_info* m, size_t, void* opaque) {
            const char* name = strrchr(m->dlpi_name, '/');
            name = name ? name + 1 : m->dlpi_name;
            if (strcmp(name, "UnityPlayer.so")) return 0;

            auto& r = *static_cast<module_info*>(opaque);
            r.base = m->dlpi_addr;
            r.count++;
            for (unsigned i = 0; i < m->dlpi_phnum; i++) {
                const auto& p = m->dlpi_phdr[i];
                if (p.p_type != PT_NOTE) continue;

                auto at = reinterpret_cast<const unsigned char*>(m->dlpi_addr + p.p_vaddr);
                size_t left = p.p_memsz;

                while (left >= sizeof(ElfW(Nhdr))) {
                    ElfW(Nhdr) n;
                    std::memcpy(&n, at, sizeof(n));
                    size_t names = (size_t(n.n_namesz) + 3) & ~size_t(3);
                    size_t desc = (size_t(n.n_descsz) + 3) & ~size_t(3);
                    if (names > left - sizeof(n) || desc > left - sizeof(n) - names) break;

                    if (n.n_type == NT_GNU_BUILD_ID && n.n_namesz == 4 && n.n_descsz == sizeof(engine_signature::build_id) &&
                        !std::memcmp(at + sizeof(n), "GNU", 4) &&
                        !std::memcmp(at + sizeof(n) + names, engine_signature::build_id, sizeof(engine_signature::build_id))) r.build_id_match = true;

                    size_t bytes = sizeof(n) + names + desc;
                    at += bytes;
                    left -= bytes;
                }
            }

            return 0;
        }

        // True when value is libGL's own export of name rather than a wrapper.
        bool exact_export(void* value, const char* name) {
            if (!value) return false;
            void* gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
            if (!gl) return false; // the handle is kept on purpose

            void* exported = dlsym(gl, name);
            void* proc = reinterpret_cast<void*>(glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(name)));
            Dl_info info{};

            return (value == exported || value == proc) && dladdr(value, &info) && info.dli_sname && !strcmp(info.dli_sname, name);
        }

        int follow_chain(const address_map& m, uintptr_t base, Display* d, GLXDrawable x, GLXContext c, unity_swap_slot& out) {
            uintptr_t device = 0;
            uintptr_t table = 0;
            uintptr_t window = 0;
            uintptr_t callback = 0;
            uintptr_t window_data = 0;
            uintptr_t display_data = 0;
            uintptr_t engine_display = 0;
            uintptr_t engine_drawable = 0;
            uintptr_t engine_context = 0;

            if (!m.read_pointer(base + 0x2025818, device) || !m.has(device, 0x5d0)) return -12;
            if (!m.read_pointer(device + 0x5c8, table) || !m.has(table, 0xa0, true)) return -12;
            if (!m.read_pointer(device + 0x1b8, callback) || callback != base + 0x1946c86) return -13;
            if (!m.read_pointer(device + 0x480, window) || !m.has(window, 0xe8)) return -14;
            if (!m.read_pointer(window + 0xe0, window_data) || !m.has(window_data, 0xf0)) return -14;
            if (!m.read_pointer(window_data + 8, engine_drawable)) return -14;
            if (!m.read_pointer(window_data + 0xe8, display_data) || !m.read_pointer(display_data, engine_display)) return -14;
            if (!m.read_pointer(device + 0x488, engine_context)) return -14;
            if (engine_drawable != x || engine_display != reinterpret_cast<uintptr_t>(d) || engine_context != reinterpret_cast<uintptr_t>(c)) return -14;

            out = {base, device, table, window, reinterpret_cast<void**>(table + 0x70)};
            return 0;
        }

    }

    int find_unity_swap_slot(Display* d, GLXDrawable x, GLXContext c, unity_swap_slot& out) {
        module_info module;
        dl_iterate_phdr(find_module, &module);
        if (module.count != 1 || !module.build_id_match) return -10;

        address_map maps;
        for (const auto& p : engine_signature::patterns) {
            if (!maps.has(module.base + p.address, p.size, false, true)) return -11;
            if (std::memcmp(reinterpret_cast<void*>(module.base + p.address), p.bytes, p.size)) return -11;
        }

        int result = follow_chain(maps, module.base, d, x, c, out);
        if (result) return result;

        const uintptr_t offsets[] = {0x68, 0x70, 0x78};
        const char* names[] = {"glXMakeCurrent", "glXSwapBuffers", "glXQueryDrawable"};

        for (unsigned i = 0; i < 3; i++) {
            uintptr_t value = 0;
            if (!maps.read_pointer(out.table + offsets[i], value)) return -15;
            if (!exact_export(reinterpret_cast<void*>(value), names[i])) return -15;
        }

        return 0;
    }

    bool restore_unity_swap_slot(const unity_swap_slot& saved, Display* d, GLXDrawable x, GLXContext c, void* hook, void* original) {
        // Only called at a live source render boundary; quit never walks this heap chain without one.
        if (glXGetCurrentDisplay() != d || glXGetCurrentDrawable() != x || glXGetCurrentContext() != c) return false;

        address_map maps;
        unity_swap_slot current;
        if (follow_chain(maps, saved.module, d, x, c, current)) return false;
        if (current.device != saved.device || current.table != saved.table || current.window != saved.window || current.slot != saved.slot) return false;

        void* expected = hook;
        return __atomic_compare_exchange_n(current.slot, &expected, original, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE) || expected == original;
    }

}
