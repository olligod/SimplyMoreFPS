// Loads the built camera kernel, replays the managed reference fixture against it and
// then runs the negative and pan scenarios that the fixture cannot express.
// Exit codes: 0 pass, 1 a check failed (failure.txt written), 2 could not run at all.
#include "../camera_kernel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__)
// AAPCS64 preserves the low 64 bits of v8-v15. Capture them before C++ can reuse them.
// Restore the caller's values only to contain a bad callee; the test still fails.
extern "C" int32_t smf_test_call_registers(uintptr_t target, const uint64_t* arguments, uint64_t* observed);

// Every export takes at most seven scalar arguments, all passed in x0-x6.
asm(R"(
.text
.p2align 2
.globl _smf_test_call_registers
_smf_test_call_registers:
.cfi_startproc
stp x29, x30, [sp, #-96]!
.cfi_def_cfa_offset 96
.cfi_offset 29, -96
.cfi_offset 30, -88
mov x29, sp
stp d8, d9, [sp, #16]
stp d10, d11, [sp, #32]
stp d12, d13, [sp, #48]
stp d14, d15, [sp, #64]
.cfi_offset 72, -80
.cfi_offset 73, -72
.cfi_offset 74, -64
.cfi_offset 75, -56
.cfi_offset 76, -48
.cfi_offset 77, -40
.cfi_offset 78, -32
.cfi_offset 79, -24
str x2, [sp, #80]
mov x16, x0
mov x9, x1
ldp x0, x1, [x9, #0]
ldp x2, x3, [x9, #16]
ldp x4, x5, [x9, #32]
ldr x6, [x9, #48]
movz x9, #0x3ff0, lsl #48
movk x9, #8
fmov d8, x9
movk x9, #9
fmov d9, x9
movk x9, #10
fmov d10, x9
movk x9, #11
fmov d11, x9
movk x9, #12
fmov d12, x9
movk x9, #13
fmov d13, x9
movk x9, #14
fmov d14, x9
movk x9, #15
fmov d15, x9
blr x16
ldr x9, [sp, #80]
stp d8, d9, [x9, #0]
stp d10, d11, [x9, #16]
stp d12, d13, [x9, #32]
stp d14, d15, [x9, #48]
ldp d8, d9, [sp, #16]
ldp d10, d11, [sp, #32]
ldp d12, d13, [sp, #48]
ldp d14, d15, [sp, #64]
ldp x29, x30, [sp], #96
.cfi_def_cfa_offset 0
ret
.cfi_endproc
)");
#endif

namespace {

    constexpr double nan_value = std::numeric_limits<double>::quiet_NaN();
    constexpr double infinity_value = std::numeric_limits<double>::infinity();
    constexpr double max_double = std::numeric_limits<double>::max();

    enum fixture_operation : uint32_t {
        op_create = 1,
        op_step = 2,
        op_configure = 3,
        op_adopt = 4,
        op_release = 5
    };

    // One record of reference.bin, written by Reference.cs.
    struct fixture_record {
        uint32_t operation;
        uint32_t scenario;
        smf_camera_init init;
        smf_camera_settings settings;
        smf_camera_input input;
        smf_camera_pose expected;
    };
    static_assert(sizeof(fixture_record) == 2312 && offsetof(fixture_record, expected) == 2176, "fixture record layout");
    static_assert(std::is_trivially_copyable<fixture_record>::value, "fixture record must be plain data");

    std::string current_thread_name() {
        std::ostringstream id;
        id << std::this_thread::get_id();
        return id.str();
    }

    std::string input_bytes(const smf_camera_input& input) {
        std::ostringstream text;
        text << std::hex << std::setfill('0');
        // Read storage so diagnostics do not reuse the compiler's known field values.
        const volatile auto* bytes = reinterpret_cast<const unsigned char*>(&input);
        for (size_t i = 0; i < sizeof(input); i++) {
            text << std::setw(2) << static_cast<unsigned>(bytes[i]);
        }
        return text.str();
    }

    smf_camera_init make_init(uint64_t epoch, int32_t map_id, double x, double z, double root_size, double seconds) {
        smf_camera_init init{};
        init.version = SMF_CAMERA_VERSION;
        init.size = sizeof(init);
        init.epoch = epoch;
        init.map_id = map_id;
        init.x = x;
        init.z = z;
        init.root_size = root_size;
        init.monotonic_seconds = seconds;

        return init;
    }

    smf_camera_settings make_settings(uint32_t flags, double map_width, double map_height,
        double pixel_width, double pixel_height, double ui_scale, double min_size, double max_size) {
        smf_camera_settings settings{};
        settings.version = SMF_CAMERA_VERSION;
        settings.size = sizeof(settings);
        settings.revision = 1;
        settings.flags = flags;
        settings.map_width = map_width;
        settings.map_height = map_height;
        settings.pixel_width = pixel_width;
        settings.pixel_height = pixel_height;
        settings.ui_scale = ui_scale;
        settings.min_size = min_size;
        settings.max_size = max_size;
        settings.dolly_rate_keys = 45;
        settings.dolly_rate_screen_edge = 36;
        settings.speed_decay = .85;
        settings.move_speed = 2;
        settings.zoom_speed = 2.6;
        settings.scroll_wheel_rate = .35;
        settings.zoom_preserve_factor = 0;
        settings.drag_sensitivity = 1.3;

        return settings;
    }

    smf_camera_trajectory make_pan(uint64_t id, double start_seconds, double duration_seconds,
        double source_x, double source_z, double source_root_size,
        double target_x, double target_z, double target_root_size) {
        smf_camera_trajectory pan{};
        pan.id = id;
        pan.kind = 1;
        pan.start_seconds = start_seconds;
        pan.duration_seconds = duration_seconds;
        pan.source_x = source_x;
        pan.source_z = source_z;
        pan.source_root_size = source_root_size;
        pan.target_x = target_x;
        pan.target_z = target_z;
        pan.target_root_size = target_root_size;

        return pan;
    }

    class kernel_library {
    public:
        decltype(&smf_camera_create) create = nullptr;
        decltype(&smf_camera_adopt) adopt = nullptr;
        decltype(&smf_camera_configure) configure = nullptr;
        decltype(&smf_camera_step) step = nullptr;
        decltype(&smf_camera_release) release = nullptr;

        explicit kernel_library(const std::filesystem::path& path) {
            if (!path.is_absolute() || !std::filesystem::is_regular_file(path)) {
                throw std::runtime_error("An existing absolute native module path is required.");
            }

#ifdef _WIN32
            module = LoadLibraryW(path.c_str());
#else
            module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif

            if (!module) throw std::runtime_error("Native module load failed.");

            resolve(create, "smf_camera_create");
            resolve(adopt, "smf_camera_adopt");
            resolve(configure, "smf_camera_configure");
            resolve(step, "smf_camera_step");
            resolve(release, "smf_camera_release");
        }

        // The module is never unloaded, not even on failure; process exit owns that.

    private:
        template <class T>
        void resolve(T& target, const char* name) {
#ifdef _WIN32
            auto address = GetProcAddress(module, name);
#else
            auto address = dlsym(module, name);
#endif

            if (!address) throw std::runtime_error(std::string("Missing export: ") + name);
            static_assert(sizeof(target) == sizeof(address), "function pointer size");
            std::memcpy(&target, &address, sizeof(target));
        }

#ifdef _WIN32
        HMODULE module = nullptr;
#else
        void* module = nullptr;
#endif
    };

    class test_driver {
    public:
        uint64_t assertions = 0;
        uint64_t reference_records = 0;
        uint64_t negative_cases = 0;
        double maximum_error = 0;
        std::string owner_thread;
        std::string other_thread;

        test_driver(kernel_library& library, std::ofstream& trace) : kernel(library), calls(trace) {}

        void run_all(const std::filesystem::path& fixtures) {
            owner_thread = current_thread_name();

            try {
                phase = "reference";
                replay_fixtures(fixtures);
                phase = "negative-cases";
                run_negative_cases();
                phase = "profiles-and-pans";
                run_profiles_and_pans();
                phase = "bounds-size-cap";
                run_bounds_size_cap();
                phase = "clock-only-seed";
                run_clock_only_seed();
            } catch (...) {
                if (session) {
                    smf_camera_pose pose{};
                    call_abi("release", kernel.release, session, &pose, sizeof(pose));
                    session = 0;
                }
                throw;
            }
        }

    private:
        kernel_library& kernel;
        std::ofstream& calls;
        const char* phase = "startup";
        std::string last_call;
        uint64_t session = 0;
        smf_camera_init init{};
        smf_camera_settings settings{};
        smf_camera_input last_input{};
        smf_camera_pose baseline{};

        template <class T>
        static uint64_t abi_argument(T value) {
            if constexpr (std::is_same<T, std::nullptr_t>::value) return 0;
            else if constexpr (std::is_pointer<T>::value) return reinterpret_cast<uintptr_t>(value);
            else return static_cast<uint64_t>(value);
        }

        template <class... Parameters, class... Args>
        int32_t call_abi(const char* name, int32_t (*target)(Parameters...), Args... args) {
            static_assert(sizeof...(Parameters) == sizeof...(Args), "Camera ABI argument count");
#if defined(__APPLE__) && defined(__aarch64__)
            static_assert(sizeof...(Args) <= 7, "Camera ABI arguments must fit x0-x6");
            static_assert(sizeof(target) == sizeof(uintptr_t), "Camera function pointer size");
            const uint64_t arguments[7] = { abi_argument(static_cast<Parameters>(args))... };
            uint64_t observed[8]{};
            int32_t result = smf_test_call_registers(reinterpret_cast<uintptr_t>(target), arguments, observed);
            for (unsigned i = 0; i < 8; ++i) {
                const uint64_t expected = UINT64_C(0x3ff0000000000000) | (i + 8);
                if (observed[i] != expected) {
                    std::ostringstream context;
                    context << "Callee-saved register changed case=" << name << " register=d" << (i + 8)
                        << " result=" << result << std::hex << " expected=0x" << expected
                        << " actual=0x" << observed[i];
                    require(false, context.str());
                }
            }
            return result;
#else
            (void)name;
            return target(static_cast<Parameters>(args)...);
#endif
        }

        void require(bool ok, const std::string& label) {
            ++assertions;
            if (!ok) throw std::runtime_error(label + " phase=" + phase + " " + last_call);
        }

        void log_call(const char* label, int32_t result, const smf_camera_pose& p, uint32_t scenario = 0) {
            std::ostringstream context;
            context << "call=" << label << " record=" << reference_records << " scenario=" << scenario
                << " result=" << result << " pose_result=" << p.result << " session=" << session
                << " pose_session=" << p.session << " epoch=" << p.epoch
                << " input_sequence=" << p.input_sequence << " settings_revision=" << p.settings_revision;
            last_call = context.str();
            calls << label << '\t' << reference_records << '\t' << scenario << '\t' << result << '\t'
                << p.session << '\t' << p.epoch << '\t' << p.pose_sequence << '\t' << p.input_sequence << '\t'
                << p.settings_revision << '\t' << p.map_id << '\t' << p.x << '\t' << p.z << '\t'
                << p.root_size << '\t' << p.desired_size << '\t' << p.velocity_x << '\t' << p.velocity_z << '\t'
                << p.projection_half_height << '\t' << p.active_pan_id << '\t' << p.finished_pan_id << '\t'
                << p.pan_flags << '\n';

            require(static_cast<bool>(calls), "Trace write failed");
        }

        void expect_near(double actual, double expected, const char* field) {
            double error = std::abs(actual - expected);
            if (std::isfinite(error)) maximum_error = std::max(maximum_error, error);
            bool close = std::isfinite(actual) && std::isfinite(expected) && error <= 1e-9 + 1e-11 * std::abs(expected);
            require(close, std::string("Reference comparison: ") + field);
        }

        void expect_pose(const smf_camera_pose& actual, const smf_camera_pose& expected, int32_t result) {
            require(actual.version == SMF_CAMERA_VERSION && actual.size == sizeof(actual) && actual.result == result,
                "Pose header/result");
            require(actual.session == session && actual.epoch == expected.epoch && actual.map_id == expected.map_id &&
                actual.pose_sequence == expected.pose_sequence && actual.input_sequence == expected.input_sequence &&
                actual.settings_revision == expected.settings_revision, "Pose metadata changed");

            expect_near(actual.x, expected.x, "x");
            expect_near(actual.z, expected.z, "z");
            expect_near(actual.root_size, expected.root_size, "size");
            expect_near(actual.desired_size, expected.desired_size, "target");
            expect_near(actual.velocity_x, expected.velocity_x, "velocity_x");
            expect_near(actual.velocity_z, expected.velocity_z, "velocity_z");
            expect_near(actual.projection_half_height, expected.projection_half_height, "projection_half_height");

            require(actual.active_pan_id == expected.active_pan_id && actual.finished_pan_id == expected.finished_pan_id &&
                actual.pan_flags == expected.pan_flags && actual.reserved == 0, "Pan metadata changed");
        }

        // Errors and release must leave nothing but the header in the output.
        void expect_header_only(const smf_camera_pose& p, int32_t result) {
            smf_camera_pose expected{};
            expected.version = SMF_CAMERA_VERSION;
            expected.size = sizeof(expected);
            expected.map_id = -1;
            expected.result = result;

            require(std::memcmp(&p, &expected, sizeof(p)) == 0, "Error/release exposed non-header state");
        }

        void expect_error(const char* name, int32_t result, int32_t expected, const smf_camera_pose& p) {
            log_call(name, result, p);
            require(result == expected, std::string(name) + " result");
            expect_header_only(p, expected);
            ++negative_cases;
        }

        // Re-sending the last accepted input must be STALE and must not move the pose.
        void probe_unchanged() {
            smf_camera_pose p{};
            int32_t result = call_abi("step", kernel.step, session, &last_input, sizeof(last_input), &p, sizeof(p));

            log_call("no-mutation-probe", result, p);
            require(result == SMF_CAMERA_STALE, "Rejected call changed accepted sequence");
            expect_pose(p, baseline, SMF_CAMERA_STALE);
        }

        void replay_fixtures(const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary);
            char magic[8]{};
            uint32_t version = 0;
            uint32_t count = 0;

            file.read(magic, sizeof(magic));
            file.read(reinterpret_cast<char*>(&version), sizeof(version));
            file.read(reinterpret_cast<char*>(&count), sizeof(count));

            require(file && std::memcmp(magic, "SMFCAM02", 8) == 0 && version == 2 && count > 4000 && count < 20000,
                "Reference fixture header/count");

            for (uint32_t index = 0; index < count; ++index) {
                fixture_record record{};
                file.read(reinterpret_cast<char*>(&record), sizeof(record));
                require(static_cast<bool>(file), "Truncated fixture");

                smf_camera_pose p{};
                int32_t result = SMF_CAMERA_INTERNAL_ERROR;

                switch (record.operation) {
                case op_create:
                    require(session == 0, "Fixture create while session active");
                    result = call_abi("create", kernel.create, &record.init, sizeof(record.init), &record.settings, sizeof(record.settings), &p, sizeof(p));
                    session = p.session;
                    require(session != 0, "Create did not return a session");
                    break;
                case op_step:
                    result = call_abi("step", kernel.step, session, &record.input, sizeof(record.input), &p, sizeof(p));
                    break;
                case op_configure:
                    result = call_abi("configure", kernel.configure, session, record.init.epoch, &record.settings, sizeof(record.settings), &p, sizeof(p));
                    break;
                case op_adopt:
                    result = call_abi("adopt", kernel.adopt, session, &record.init, sizeof(record.init), &record.settings, sizeof(record.settings), &p, sizeof(p));
                    break;
                case op_release:
                    result = call_abi("release", kernel.release, session, &p, sizeof(p));
                    break;
                default:
                    throw std::runtime_error("Unknown fixture operation");
                }

                ++reference_records;
                log_call("reference", result, p, record.scenario);
                require(result == record.expected.result, "Reference result mismatch");

                if (record.operation == op_release) {
                    expect_header_only(p, 0);
                    session = 0;
                } else {
                    expect_pose(p, record.expected, result);
                }
            }

            require(file.peek() == std::ifstream::traits_type::eof(), "Trailing fixture bytes");
        }

        smf_camera_input next_input() const {
            smf_camera_input input{};
            input.version = SMF_CAMERA_VERSION;
            input.size = sizeof(input);
            input.epoch = init.epoch;
            input.sequence = last_input.sequence + 1;
            input.settings_revision = settings.revision;
            input.monotonic_seconds = last_input.monotonic_seconds + .001;
            input.pointer_x = settings.pixel_width / 2;
            input.pointer_y = settings.pixel_height / 2;

            return input;
        }

        void reject_input(const char* name, const smf_camera_input& input, int32_t expected = SMF_CAMERA_INVALID_ARGUMENT) {
            smf_camera_pose p{};
            int32_t result = call_abi("step", kernel.step, session, &input, sizeof(input), &p, sizeof(p));
            expect_error(name, result, expected, p);
            probe_unchanged();
        }

        void reject_settings(const char* name, const smf_camera_settings& candidate) {
            smf_camera_pose p{};
            int32_t result = call_abi(name, kernel.configure, session, init.epoch,
                &candidate, sizeof(candidate), &p, sizeof(p));
            expect_error(name, result, SMF_CAMERA_INVALID_ARGUMENT, p);
            probe_unchanged();
        }

        void check_input_header(const smf_camera_input& input, const char* stage) {
            const volatile smf_camera_input& stored = input;
            const std::string bytes = input_bytes(input);
            require(stored.version == SMF_CAMERA_VERSION && stored.size == sizeof(input),
                std::string("Invalid input header stage=") + stage + " raw_header=" + bytes.substr(0, 16));
        }

        void accept(const smf_camera_input& input) {
            smf_camera_pose p{};
            const std::string before_bytes = input_bytes(input);
            int32_t result = call_abi("step", kernel.step, session, &input, sizeof(input), &p, sizeof(p));
            const std::string after_bytes = input_bytes(input);
            log_call("accept", result, p);
            require(before_bytes == after_bytes,
                "Input changed across kernel.step stage=accept input_before=" + before_bytes + " input_after=" + after_bytes);

            if (result != 0 || p.session != session) {
                std::ostringstream context;
                context << std::setprecision(17) << " input_epoch=" << input.epoch
                    << " sequence=" << input.sequence << " revision=" << input.settings_revision
                    << " clock=" << input.monotonic_seconds << " previous_clock=" << last_input.monotonic_seconds
                    << " flags=" << input.flags << " pan_x=" << input.pan_x << " pan_z=" << input.pan_z
                    << " drag_x=" << input.drag_x << " drag_y=" << input.drag_y << " wheel=" << input.wheel_delta
                    << " trajectory_id=" << input.trajectory.id << " trajectory_start=" << input.trajectory.start_seconds
                    << " trajectory_duration=" << input.trajectory.duration_seconds
                    << " dolly_rate_keys=" << settings.dolly_rate_keys
                    << " previous_pose=(" << baseline.x << ',' << baseline.z << ',' << baseline.root_size << ')';
                context << " input_before=" << before_bytes << " input_after=" << after_bytes;
                last_call += context.str();
            }

            require(result == 0 && p.session == session, "Valid step rejected");
            require(std::isfinite(p.x) && std::isfinite(p.z) && std::isfinite(p.root_size) && p.root_size > 0 &&
                std::isfinite(p.desired_size) && std::isfinite(p.velocity_x) && std::isfinite(p.velocity_z),
                "Invalid accepted pose");

            last_input = input;
            baseline = p;
        }

        void configure(const smf_camera_settings& candidate) {
            smf_camera_pose p{};
            int32_t result = call_abi("configure", kernel.configure, session, init.epoch, &candidate, sizeof(candidate), &p, sizeof(p));
            log_call("configure", result, p);
            require(result == 0, "Valid configure rejected");

            smf_camera_pose expected = baseline;
            expected.settings_revision = candidate.revision;
            expect_pose(p, expected, 0);

            settings = candidate;
            baseline = p;
            last_input.settings_revision = candidate.revision;
        }

        // Create a session and take one accepted step so there is a baseline to compare against.
        void start_session(double first_clock) {
            smf_camera_pose p{};
            int32_t result = call_abi("create", kernel.create, &init, sizeof(init), &settings, sizeof(settings), &p, sizeof(p));
            log_call("create", result, p);
            require(result == 0 && p.session != 0, "Create failed");

            session = p.session;
            last_input = {};
            last_input.monotonic_seconds = first_clock;
            accept(next_input());
        }

        void release_session(const char* label) {
            smf_camera_pose p{};
            int32_t result = call_abi("release", kernel.release, session, &p, sizeof(p));
            log_call("release", result, p);
            require(result == 0, label);

            expect_header_only(p, 0);
            session = 0;
        }

        void run_negative_cases() {
            init = make_init(10000, 7, 125, 112.5, 24, 100);
            settings = make_settings(7, 250, 225, 1920, 1080, 1.25, 11, 60);
            start_session(init.monotonic_seconds);

            smf_camera_pose p{};
            int32_t result = call_abi("create", kernel.create, &init, sizeof(init), &settings, sizeof(settings), &p, sizeof(p));
            expect_error("double-create", result, SMF_CAMERA_BUSY, p);
            probe_unchanged();

            // Every mutating export from a second OS thread must be refused while the owner waits.
            std::exception_ptr other_failure;
            std::thread other([&] {
                try {
                    other_thread = current_thread_name();
                    smf_camera_input input = next_input();
                    smf_camera_init newer = init;
                    newer.epoch++;
                    smf_camera_settings config = settings;
                    config.revision++;
                    smf_camera_pose output{};

                    expect_error("wrong-thread-step",
                        call_abi("step", kernel.step, session, &input, sizeof(input), &output, sizeof(output)), SMF_CAMERA_WRONG_THREAD, output);
                    expect_error("wrong-thread-configure",
                        call_abi("configure", kernel.configure, session, init.epoch, &config, sizeof(config), &output, sizeof(output)), SMF_CAMERA_WRONG_THREAD, output);
                    expect_error("wrong-thread-adopt",
                        call_abi("adopt", kernel.adopt, session, &newer, sizeof(newer), &config, sizeof(config), &output, sizeof(output)), SMF_CAMERA_WRONG_THREAD, output);
                    expect_error("wrong-thread-release",
                        call_abi("release", kernel.release, session, &output, sizeof(output)), SMF_CAMERA_WRONG_THREAD, output);
                } catch (...) {
                    other_failure = std::current_exception();
                }
            });

            other.join();
            if (other_failure) std::rethrow_exception(other_failure);
            probe_unchanged();
            require(owner_thread != other_thread, "Driver did not create distinct OS threads");

            smf_camera_input input = next_input();
            result = call_abi("step", kernel.step, session + 1, &input, sizeof(input), &p, sizeof(p));
            expect_error("bad-session", result, SMF_CAMERA_BAD_SESSION, p);
            probe_unchanged();

            result = call_abi("step", kernel.step, session, nullptr, sizeof(input), &p, sizeof(p));
            expect_error("null-input", result, SMF_CAMERA_INVALID_ARGUMENT, p);
            probe_unchanged();

            result = call_abi("step", kernel.step, session, &input, sizeof(input) - 1, &p, sizeof(p));
            expect_error("input-byte-size", result, SMF_CAMERA_INVALID_ARGUMENT, p);
            probe_unchanged();

            // A wrong output size must not write a single byte.
            smf_camera_pose sentinel{};
            std::memset(&sentinel, 0xA5, sizeof(sentinel));
            p = sentinel;

            result = call_abi("step", kernel.step, session, &input, sizeof(input), &p, sizeof(p) - 1);
            require(result == SMF_CAMERA_INVALID_ARGUMENT && std::memcmp(&p, &sentinel, sizeof(p)) == 0, "Invalid output size wrote memory");
            ++negative_cases;
            probe_unchanged();

            require(call_abi("step", kernel.step, session, &input, sizeof(input), nullptr, sizeof(p)) == SMF_CAMERA_INVALID_ARGUMENT, "Null output accepted");
            ++negative_cases;
            probe_unchanged();

            auto reject_input_field = [&](const char* name, auto mutate) {
                smf_camera_input bad = next_input();
                mutate(bad);
                reject_input(name, bad);
            };

            reject_input_field("input-version", [](smf_camera_input& i) { i.version = 1; });
            reject_input_field("embedded-input-size", [](smf_camera_input& i) { i.size = 1; });
            reject_input_field("input-reserved", [](smf_camera_input& i) { i.reserved = 1; });
            reject_input_field("input-flags", [](smf_camera_input& i) { i.flags = 512; });
            reject_input_field("zero-epoch", [](smf_camera_input& i) { i.epoch = 0; });
            reject_input_field("zero-sequence", [](smf_camera_input& i) { i.sequence = 0; });
            reject_input_field("settings-revision", [&](smf_camera_input& i) { i.settings_revision = settings.revision + 1; });
            reject_input_field("backwards-clock", [&](smf_camera_input& i) { i.monotonic_seconds = last_input.monotonic_seconds - 1; });
            reject_input_field("nan-clock", [](smf_camera_input& i) { i.monotonic_seconds = nan_value; });
            reject_input_field("pan-domain", [](smf_camera_input& i) { i.pan_x = 1.01; });
            reject_input_field("nan-drag", [](smf_camera_input& i) { i.drag_y = nan_value; });
            reject_input_field("infinite-wheel", [](smf_camera_input& i) { i.wheel_delta = infinity_value; });
            reject_input_field("nan-pointer", [](smf_camera_input& i) { i.pointer_x = nan_value; });
            reject_input_field("negative-inspect-height", [](smf_camera_input& i) { i.inspect_pane_height = -1; });

            input = next_input();
            input.epoch++;
            reject_input("wrong-epoch", input, SMF_CAMERA_WRONG_EPOCH);

            smf_camera_input duplicate = last_input;
            duplicate.wheel_delta = 50;
            duplicate.drag_x = 100;

            result = call_abi("step", kernel.step, session, &duplicate, sizeof(duplicate), &p, sizeof(p));
            log_call("duplicate-impulses", result, p);
            require(result == SMF_CAMERA_STALE, "Duplicate impulse accepted");
            expect_pose(p, baseline, SMF_CAMERA_STALE);
            ++negative_cases;

            auto reject_settings_field = [&](const char* name, auto mutate) {
                smf_camera_settings bad = settings;
                bad.revision++;
                mutate(bad);
                reject_settings(name, bad);
            };

            reject_settings_field("settings-version", [](smf_camera_settings& s) { s.version = 1; });
            reject_settings_field("settings-size", [](smf_camera_settings& s) { s.size = 1; });
            reject_settings_field("settings-reserved", [](smf_camera_settings& s) { s.reserved = 1; });
            reject_settings_field("settings-flags", [](smf_camera_settings& s) { s.flags = 16; });
            reject_settings_field("settings-zero-revision", [](smf_camera_settings& s) { s.revision = 0; });
            reject_settings_field("map-width", [](smf_camera_settings& s) { s.map_width = 3; });
            reject_settings_field("map-height", [](smf_camera_settings& s) { s.map_height = infinity_value; });
            reject_settings_field("pixel-width", [](smf_camera_settings& s) { s.pixel_width = 0; });
            reject_settings_field("pixel-height", [](smf_camera_settings& s) { s.pixel_height = -1; });
            reject_settings_field("ui-scale", [](smf_camera_settings& s) { s.ui_scale = 0; });
            reject_settings_field("min-size", [](smf_camera_settings& s) { s.min_size = 0; });
            reject_settings_field("max-size", [&](smf_camera_settings& s) { s.max_size = settings.min_size; });
            reject_settings_field("decay", [](smf_camera_settings& s) { s.speed_decay = 1.01; });
            reject_settings_field("preserve", [](smf_camera_settings& s) { s.zoom_preserve_factor = -1; });
            reject_settings_field("nan-rate", [](smf_camera_settings& s) { s.drag_sensitivity = nan_value; });

            result = call_abi("configure", kernel.configure, session, init.epoch, &settings, sizeof(settings), &p, sizeof(p));
            log_call("stale-configure", result, p);
            require(result == SMF_CAMERA_STALE, "Stale configuration applied");
            expect_pose(p, baseline, SMF_CAMERA_STALE);
            ++negative_cases;

            smf_camera_init newer = init;
            newer.epoch++;
            newer.root_size = 0;

            result = call_abi("adopt", kernel.adopt, session, &newer, sizeof(newer), &settings, sizeof(settings), &p, sizeof(p));
            expect_error("invalid-adopt", result, SMF_CAMERA_INVALID_ARGUMENT, p);
            probe_unchanged();

            result = call_abi("adopt", kernel.adopt, session, &init, sizeof(init), &settings, sizeof(settings), &p, sizeof(p));
            expect_error("stale-adopt", result, SMF_CAMERA_WRONG_EPOCH, p);
            probe_unchanged();

            // A rejected drag must not leave anything in the drag queue for the next accepted step.
            smf_camera_settings config = settings;
            config.revision++;
            config.pixel_height = 1;
            configure(config);

            input = next_input();
            input.drag_x = max_double;
            reject_input("finite-drag-overflow", input);

            input.drag_x = 0;
            input.flags = SMF_CAMERA_MIDDLE_RELEASED_PULSE;
            accept(input);
            expect_near(baseline.velocity_x, 0, "No poisoned drag queue after rejection");

            // Blocked drag is never queued, so a huge finite drag is fine while blocked.
            input = next_input();
            input.drag_x = max_double;
            input.flags = SMF_CAMERA_MOTION_BLOCKED;
            accept(input);

            input = last_input;
            input.sequence = 1;
            input.wheel_delta = 40;
            result = call_abi("step", kernel.step, session, &input, sizeof(input), &p, sizeof(p));
            log_call("reordered-impulse", result, p);
            require(result == SMF_CAMERA_STALE, "Reordered impulse applied");
            expect_pose(p, baseline, SMF_CAMERA_STALE);
            ++negative_cases;

            // An arithmetic overflow inside the step faults the session until a newer epoch is adopted.
            config = settings;
            config.revision++;
            config.dolly_rate_keys = max_double;
            configure(config);

            input = next_input();
            input.pan_x = 1;
            input.flags = SMF_CAMERA_FAST_PAN;
            result = call_abi("step", kernel.step, session, &input, sizeof(input), &p, sizeof(p));
            expect_error("math-fault", result, SMF_CAMERA_FAULTED, p);

            input = next_input();
            result = call_abi("step", kernel.step, session, &input, sizeof(input), &p, sizeof(p));
            expect_error("faulted-retry", result, SMF_CAMERA_FAULTED, p);

            config = settings;
            config.revision++;
            config.dolly_rate_keys = 45;
            result = call_abi("configure", kernel.configure, session, init.epoch, &config, sizeof(config), &p, sizeof(p));
            expect_error("faulted-configure", result, SMF_CAMERA_FAULTED, p);

            newer = init;
            newer.epoch++;
            newer.monotonic_seconds = 200;
            result = call_abi("adopt", kernel.adopt, session, &newer, sizeof(newer), &config, sizeof(config), &p, sizeof(p));
            log_call("recover-adopt", result, p);
            require(result == 0 && p.epoch == newer.epoch && p.input_sequence == 0, "New-epoch recovery failed");

            init = newer;
            settings = config;
            last_input = {};
            last_input.monotonic_seconds = init.monotonic_seconds;
            accept(next_input());

            // The highest sequence stays usable and a wrapped retry cannot replay an impulse.
            input = next_input();
            input.sequence = std::numeric_limits<uint64_t>::max();
            accept(input);
            probe_unchanged();
            input.sequence = 0;
            reject_input("sequence-wrap", input);

            const uint64_t old_session = session;
            release_session("Owner release failed");
            result = call_abi("release", kernel.release, old_session, &p, sizeof(p));
            expect_error("stale-release", result, SMF_CAMERA_BAD_SESSION, p);

            result = call_abi("create", kernel.create, &init, sizeof(init), &settings, sizeof(settings), &p, sizeof(p));
            log_call("recreate", result, p);
            require(result == 0 && p.session != 0 && p.session != old_session, "Recreated session reused stale token");
            session = p.session;
            input = {};
            result = call_abi("step", kernel.step, old_session, &input, sizeof(input), &p, sizeof(p));
            expect_error("old-session-after-recreate", result, SMF_CAMERA_BAD_SESSION, p);

            result = call_abi("release", kernel.release, session, &p, sizeof(p));
            require(result == 0, "Final owner release failed");
            expect_header_only(p, 0);
            session = 0;
        }

        void run_profiles_and_pans() {
            init = make_init(20000, 8, 125, 112.5, 24, 100);
            settings = make_settings(SMF_CAMERA_ZOOM_TO_MOUSE, 1000, 1000, 1920, 1080, 1, 5, 100);
            settings.profile.present_mask = 1;
            smf_camera_curve& projection = settings.profile.projection;
            projection.mode = SMF_CAMERA_CURVE_POWER_RANGE;
            projection.input_min = 5;
            projection.input_max = 100;
            projection.a = 5;
            projection.b = 600;
            projection.exponent = 1.4;

            start_session(100);
            expect_near(baseline.projection_half_height, 5 + 595 * std::pow((24.0 - 5) / 95, 1.4), "Known nonlinear projection");

            // Zoom-to-mouse keeps the world point under the pointer, measured through the projection.
            smf_camera_input input = next_input();
            input.pointer_x = 1500;
            input.pointer_y = 200;
            input.wheel_delta = -3;
            const double world_x = baseline.x + (input.pointer_x / 1920 * 2 - 1) * baseline.projection_half_height * 1920 / 1080;
            const double world_z = baseline.z + (1 - input.pointer_y / 1080 * 2) * baseline.projection_half_height;

            accept(input);
            expect_near(baseline.x + (input.pointer_x / 1920 * 2 - 1) * baseline.projection_half_height * 1920 / 1080, world_x, "Projected mouse anchor X");
            expect_near(baseline.z + (1 - input.pointer_y / 1080 * 2) * baseline.projection_half_height, world_z, "Projected mouse anchor Z");

            auto reject_settings_field = [&](const char* name, auto mutate) {
                smf_camera_settings bad = settings;
                bad.revision++;
                mutate(bad);
                reject_settings(name, bad);
            };

            reject_settings_field("old-settings-size", [](smf_camera_settings& s) { s.size = 1752; });
            reject_settings_field("old-bounds-settings-size", [](smf_camera_settings& s) { s.size = 1912; });
            reject_settings_field("bounds-negative-size-cap", [](smf_camera_settings& s) { s.bounds.maximum_size = -1; });
            reject_settings_field("bounds-infinite-size-cap", [](smf_camera_settings& s) { s.bounds.maximum_size = infinity_value; });
            reject_settings_field("bounds-nan-size-cap", [](smf_camera_settings& s) { s.bounds.maximum_size = nan_value; });
            reject_settings_field("absent-bounds-reserved", [](smf_camera_settings& s) { s.bounds.x.reserved = 1; });
            reject_settings_field("absent-bounds-data", [](smf_camera_settings& s) { s.bounds.x.minimum_b = 1; });
            reject_settings_field("bounds-enabled", [](smf_camera_settings& s) { s.bounds.x.enabled = 2; });
            reject_settings_field("bounds-infinite", [](smf_camera_settings& s) {
                s.bounds.x.enabled = 1;
                s.bounds.x.minimum_a = infinity_value;
            });
            reject_settings_field("bounds-evaluation-overflow", [](smf_camera_settings& s) {
                s.bounds.x.enabled = 1;
                s.bounds.x.minimum_a = max_double;
            });
            reject_settings_field("profile-mask", [](smf_camera_settings& s) { s.profile.present_mask = 32; });
            reject_settings_field("curve-mode", [](smf_camera_settings& s) { s.profile.projection.mode = 9; });
            reject_settings_field("curve-exponent", [](smf_camera_settings& s) { s.profile.projection.exponent = 0; });
            reject_settings_field("projection-domain", [](smf_camera_settings& s) { s.profile.projection.domain = 1; });
            reject_settings_field("unused-point", [](smf_camera_settings& s) { s.profile.projection.x[15] = 1; });
            reject_settings_field("absent-curve", [](smf_camera_settings& s) { s.profile.keyboard_rate.a = 4; });
            reject_settings_field("zero-projection", [](smf_camera_settings& s) {
                s.profile.projection.mode = 0;
                s.profile.projection.a = 0;
            });
            reject_settings_field("unordered-table", [](smf_camera_settings& s) {
                s.profile.present_mask |= 2;
                s.profile.keyboard_rate.mode = SMF_CAMERA_CURVE_STEP;
                s.profile.keyboard_rate.point_count = 2;
                s.profile.keyboard_rate.x[0] = 10;
                s.profile.keyboard_rate.x[1] = 10;
                s.profile.keyboard_rate.y[0] = 10;
                s.profile.keyboard_rate.y[1] = 20;
            });

            // The first delivery of a pan catches up from its absolute start, here across a five second gap.
            input = next_input();
            check_input_header(input, "first-pan-after-next_input");
            input.monotonic_seconds = 105;
            input.trajectory = make_pan(1, 100, 10, 100, 110, 20, 200, 210, 40);
            check_input_header(input, "first-pan-after-make_pan");
            input.pan_x = 1;
            input.drag_x = 20;
            input.wheel_delta = -5;
            input.flags = SMF_CAMERA_MOTION_BLOCKED; // blocked input cannot interrupt the pan
            accept(input);

            expect_near(baseline.x, 150, "Pan absolute midpoint");
            expect_near(baseline.z, 160, "Pan midpoint Z");
            expect_near(baseline.root_size, 30, "Pan midpoint root");
            require(baseline.active_pan_id == 1 && baseline.finished_pan_id == 0, "Pan active metadata");

            const smf_camera_trajectory command = input.trajectory;
            input = next_input();
            input.monotonic_seconds = 105;
            input.trajectory = command;
            accept(input);
            expect_near(baseline.x, 150, "Identical command not restarted");

            input = next_input();
            input.trajectory = command;
            input.trajectory.target_x++;
            reject_input("same-id-mutation", input);

            input = next_input();
            input.monotonic_seconds = 110;
            input.trajectory = command;
            accept(input);
            require(baseline.x == 200 && baseline.z == 210 && baseline.root_size == 40 && baseline.active_pan_id == 0 &&
                baseline.finished_pan_id == 1 && baseline.pan_flags == 1, "Pan exact completion");

            input = next_input();
            input.trajectory = command;
            accept(input);
            expect_near(baseline.x, 200, "Completed command does not replay");
            require(baseline.finished_pan_id == 1 && baseline.pan_flags == 1, "Completion persists");

            // A newer command replaces the active pan and starts from its own explicit source.
            input = next_input();
            input.trajectory = make_pan(2, 110, 10, 200, 210, 40, 300, 310, 50);
            accept(input);

            input = next_input();
            input.trajectory = make_pan(3, input.monotonic_seconds, 1, 50, 60, 20, 80, 90, 25);
            accept(input);
            require(baseline.active_pan_id == 3 && baseline.finished_pan_id == 2 && baseline.pan_flags == 0, "Replacement cancels old pan");
            expect_near(baseline.x, 50, "Replacement explicit source");

            const smf_camera_trajectory replacement = input.trajectory;
            input = next_input();
            input.trajectory = command;
            smf_camera_pose p{};
            int32_t result = call_abi("step", kernel.step, session, &input, sizeof(input), &p, sizeof(p));
            require(result == SMF_CAMERA_STALE, "Old trajectory accepted");
            expect_pose(p, baseline, SMF_CAMERA_STALE);
            probe_unchanged();

            input = next_input();
            accept(input);
            require(baseline.active_pan_id == 0 && baseline.finished_pan_id == 3 && baseline.pan_flags == 0, "Zero command cancels");

            input = next_input();
            input.trajectory = replacement;
            accept(input);
            require(baseline.active_pan_id == 0 && baseline.finished_pan_id == 3 && baseline.pan_flags == 0, "Cancelled command does not replay");

            input = next_input();
            input.trajectory = make_pan(4, input.monotonic_seconds, 0, 70, 80, 22, 900, 950, 150);
            accept(input);
            require(baseline.x == 900 && baseline.z == 950 && baseline.root_size == 150 && baseline.finished_pan_id == 4 && baseline.pan_flags == 1,
                "Zero-duration exact target bypasses normal zoom clamp");

            input = next_input();
            input.trajectory = make_pan(5, input.monotonic_seconds + 1, 1, 70, 80, 22, 90, 95, 30);
            reject_input("future-pan", input);

            input.trajectory.start_seconds = input.monotonic_seconds;
            input.trajectory.duration_seconds = -1;
            reject_input("negative-pan-duration", input);
            input.trajectory.duration_seconds = 1;
            input.trajectory.id = std::numeric_limits<uint64_t>::max();
            reject_input("pan-id-overflow", input);

            input.trajectory.id = 5;
            input.trajectory.source_x = -max_double;
            input.trajectory.target_x = max_double;
            reject_input("pan-subtraction-overflow", input);

            input = next_input();
            input.trajectory.source_x = 1;
            reject_input("nonzero-empty-pan", input);

            // A new epoch starts a fresh command history, so pan id 1 is valid again.
            init.epoch++;
            init.monotonic_seconds = 200;
            result = call_abi("adopt", kernel.adopt, session, &init, sizeof(init), &settings, sizeof(settings), &p, sizeof(p));
            require(result == 0 && p.active_pan_id == 0 && p.finished_pan_id == 0, "Epoch cleared pan state");

            last_input = {};
            last_input.monotonic_seconds = 200;
            input = next_input();
            input.trajectory = make_pan(1, 200, 0, 100, 110, 20, 130, 140, 30);
            accept(input);
            require(baseline.finished_pan_id == 1 && baseline.x == 130, "Epoch ID reuse failed");

            // Keyboard and middle drag finish the pan at its exact target; wheel and edge scrolling do not.
            input = next_input();
            input.trajectory = make_pan(2, input.monotonic_seconds, 10, 100, 110, 20, 300, 310, 40);
            input.pan_x = 1;
            accept(input);
            require(baseline.active_pan_id == 0 && baseline.finished_pan_id == 2 && baseline.pan_flags == 1 &&
                baseline.x == 300 && baseline.z == 310 && baseline.root_size == 40, "Keyboard finishes exact target on first delivery");

            const smf_camera_trajectory keyboard_pan = input.trajectory;
            input = next_input();
            input.trajectory = keyboard_pan;
            input.pan_x = 1;
            accept(input);
            require(baseline.x > 300 && baseline.finished_pan_id == 2 && baseline.pan_flags == 1, "Manual movement resumes without replay/completion change");

            input = next_input();
            input.trajectory = make_pan(3, input.monotonic_seconds, 10, 100, 110, 20, 500, 510, 50);
            accept(input);
            const smf_camera_trajectory drag_pan = input.trajectory;

            input = next_input();
            input.trajectory = drag_pan;
            input.wheel_delta = -5;
            input.flags = SMF_CAMERA_ALLOW_EDGE_SCROLL;
            input.pointer_x = 1;
            input.pointer_y = 500;
            accept(input);
            require(baseline.active_pan_id == 3 && baseline.finished_pan_id == 2 && baseline.x < 101, "Wheel and edge input preserve active trajectory");

            input = next_input();
            input.trajectory = drag_pan;
            input.drag_y = 10;
            accept(input);
            require(baseline.active_pan_id == 0 && baseline.finished_pan_id == 3 && baseline.pan_flags == 1 &&
                baseline.x == 500 && baseline.z == 510 && baseline.root_size == 50, "Middle drag finishes exact target on later delivery");

            // Pan bookkeeping publishes a new pose sequence exactly when something changed.
            const uint64_t before_zero_distance = baseline.pose_sequence;
            input = next_input();
            input.trajectory = make_pan(4, input.monotonic_seconds, 0, 500, 510, 50, 500, 510, 50);
            accept(input);
            require(baseline.finished_pan_id == 4 && baseline.pose_sequence == before_zero_distance + 1 && baseline.x == 500,
                "Zero-distance immediate completion publishes a new sequence");

            const smf_camera_trajectory zero_distance = input.trajectory;
            const uint64_t immediate_sequence = baseline.pose_sequence;
            input = next_input();
            input.trajectory = zero_distance;
            accept(input);
            require(baseline.pose_sequence == immediate_sequence, "Completed metadata does not republish on repeated command");

            input = next_input();
            accept(input);
            require(baseline.pose_sequence == immediate_sequence, "Idle zero trajectory does not bump sequence");

            input = next_input();
            input.trajectory = make_pan(5, input.monotonic_seconds, 1, 500, 510, 50, 500, 510, 50);
            accept(input);
            require(baseline.active_pan_id == 5 && baseline.pose_sequence == immediate_sequence + 1, "Zero-distance active pan publishes metadata");

            const smf_camera_trajectory stationary_pan = input.trajectory;
            const uint64_t active_sequence = baseline.pose_sequence;
            input = next_input();
            input.trajectory = stationary_pan;
            input.monotonic_seconds = stationary_pan.start_seconds + .5;
            accept(input);
            require(baseline.active_pan_id == 5 && baseline.pose_sequence == active_sequence, "Stationary active pan does not repeat metadata");

            input = next_input();
            input.trajectory = stationary_pan;
            input.monotonic_seconds = stationary_pan.start_seconds + 1;
            accept(input);
            require(baseline.finished_pan_id == 5 && baseline.pan_flags == 1 && baseline.pose_sequence == active_sequence + 1,
                "Stationary natural completion publishes a new sequence");

            // Manual completion of a near-infinite pan must not poison the elapsed clock.
            input = next_input();
            input.trajectory = make_pan(6, input.monotonic_seconds, max_double / 2, 100, 110, 20, 550, 560, 55);
            input.pan_z = 1;
            accept(input);
            require(baseline.finished_pan_id == 6 && baseline.x == 550, "Huge duration manual completion is finite");

            input = next_input();
            input.monotonic_seconds = max_double * .75;
            input.flags = SMF_CAMERA_MOTION_BLOCKED;
            accept(input);
            require(baseline.finished_pan_id == 6 && baseline.x == 550, "Manual completion did not poison elapsed clock");

            release_session("Profile/pan release");
        }

        void run_bounds_size_cap() {
            init = make_init(30000, 9, 40, 40, 15, 100);
            settings = make_settings(0, 250, 225, 1280, 720, 1, 5, 100);
            settings.bounds.maximum_size = 20;

            smf_camera_pose p{};
            int32_t result = call_abi("create", kernel.create, &init, sizeof(init), &settings, sizeof(settings), &p, sizeof(p));
            require(result == 0, "Cap-only bounds create");
            session = p.session;

            last_input = {};
            last_input.monotonic_seconds = init.monotonic_seconds;

            smf_camera_input input = next_input();
            input.trajectory = make_pan(1, input.monotonic_seconds, 2, 40, 40, 15, 100, 100, 80);
            accept(input);
            const smf_camera_trajectory pan = input.trajectory;

            input = next_input();
            input.trajectory = pan;
            input.monotonic_seconds = pan.start_seconds + 1;
            accept(input);
            expect_near(baseline.root_size, 20, "Pan midpoint applies geometry size cap");
            expect_near(baseline.x, 70, "Size cap preserves original positional interpolation");

            input = next_input();
            input.trajectory = pan;
            input.pan_x = 1;
            accept(input);
            expect_near(baseline.root_size, 20, "Manual pan completion applies geometry size cap");
            require(baseline.finished_pan_id == 1 && baseline.pan_flags == 1, "Capped completion metadata");

            result = call_abi("release", kernel.release, session, &p, sizeof(p));
            require(result == 0, "Cap-only release");
            session = 0;
        }

        void run_clock_only_seed() {
            init = make_init(40000, 9, 100, 100, 24, 100);
            settings = make_settings(0, 250, 225, 1280, 720, 1, 5, 100);
            settings.bounds.maximum_size = 40;

            smf_camera_pose p{};
            require(call_abi("create", kernel.create, &init, sizeof(init), &settings, sizeof(settings), &p, sizeof(p)) == 0, "Clock-only create");
            session = p.session;

            // The adopted seed sits outside the bounds; clock-only steps must leave it exactly as adopted.
            init.epoch++;
            init.root_size = 60;
            init.x = -10;
            require(call_abi("adopt", kernel.adopt, session, &init, sizeof(init), &settings, sizeof(settings), &p, sizeof(p)) == 0, "Clock-only adopt");

            baseline = p;
            last_input = {};
            last_input.monotonic_seconds = init.monotonic_seconds;

            for (int n = 0; n < 3; ++n) {
                smf_camera_input idle = next_input();
                idle.flags = SMF_CAMERA_CLOCK_ONLY | (n == 1 ? SMF_CAMERA_MOTION_BLOCKED : 0);
                idle.pointer_x = 0;
                idle.pointer_y = 0;
                idle.monotonic_seconds += 10;

                smf_camera_pose expected = baseline;
                expected.input_sequence = idle.sequence;
                accept(idle);
                expect_pose(baseline, expected, 0);
                probe_unchanged();
            }

            smf_camera_input malformed = last_input;
            malformed.sequence++;
            malformed.monotonic_seconds++;
            malformed.pan_x = 1;
            reject_input("clock-only-pan", malformed);

            malformed.pan_x = 0;
            malformed.pointer_y = 1;
            reject_input("clock-only-pointer", malformed);

            malformed.pointer_y = 0;
            malformed.flags |= SMF_CAMERA_ZOOM_IN_PULSE;
            reject_input("clock-only-pulse", malformed);

            malformed.flags = SMF_CAMERA_CLOCK_ONLY;
            malformed.trajectory = make_pan(1, malformed.monotonic_seconds, 1, 100, 100, 60, 110, 110, 40);
            reject_input("clock-only-trajectory", malformed);

            // The first ordinary step applies the bounds, and the clock-only wait does not count as motion time.
            smf_camera_input ordinary = next_input();
            ordinary.pan_x = 1;
            accept(ordinary);
            expect_near(baseline.root_size, 40, "Acknowledged ordinary motion applies cap");
            expect_near(baseline.x, 2, "Acknowledged ordinary motion applies position bounds");

            ordinary = next_input();
            ordinary.pan_x = 1;
            accept(ordinary);
            require(baseline.x > 2 && baseline.x < 2.1, "Clock-only wait excluded from next motion delta");

            smf_camera_input pan_input = next_input();
            pan_input.trajectory = make_pan(1, pan_input.monotonic_seconds, 2, 100, 100, 40, 150, 150, 30);
            accept(pan_input);
            const smf_camera_trajectory pan = pan_input.trajectory;

            smf_camera_input idle = next_input();
            idle.flags = SMF_CAMERA_CLOCK_ONLY;
            idle.pointer_x = 0;
            idle.pointer_y = 0;
            idle.monotonic_seconds += 10;

            smf_camera_pose expected = baseline;
            expected.input_sequence = idle.sequence;
            accept(idle);
            expect_pose(baseline, expected, 0);

            ordinary = next_input();
            ordinary.flags = SMF_CAMERA_MOTION_BLOCKED;
            ordinary.trajectory = pan;
            accept(ordinary);
            require(baseline.active_pan_id == 1 && baseline.x > 100 && baseline.x < 101,
                "Clock-only retains pan and blocked motion resumes with fresh delta");

            require(call_abi("release", kernel.release, session, &p, sizeof(p)) == 0, "Clock-only release");
            session = 0;
        }
    };

}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: native_driver ABSOLUTE_NATIVE_LIBRARY REFERENCE.bin NEW_OUTPUT_DIRECTORY\n";
        return 2;
    }
    try {
        const uint16_t endian = 1;
        if (*reinterpret_cast<const unsigned char*>(&endian) != 1) throw std::runtime_error("Fixture requires little endian");

        const std::filesystem::path directory = std::filesystem::absolute(argv[3]);
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Output directory must be new");

        std::ofstream calls(directory / "calls.tsv");
        calls << std::setprecision(17);
        calls << "label\trecord\tscenario_hz\tresult\tsession\tepoch\tpose_sequence\tinput_sequence\tsettings_revision"
            "\tmap_id\tx\tz\troot_size\tdesired_size\tvelocity_x\tvelocity_z\tprojection_half_height"
            "\tactive_pan_id\tfinished_pan_id\tpan_flags\n";

        kernel_library kernel{std::filesystem::path(argv[1])};
        test_driver driver{kernel, calls};

        // All kernel calls run on one worker thread, like the renderer does; main only waits.
        std::exception_ptr failure;
        std::thread owner([&] {
            try {
                driver.run_all(std::filesystem::path(argv[2]));
            } catch (...) {
                failure = std::current_exception();
            }
        });

        owner.join();
        calls.flush();

        std::string reason;

        if (failure) {
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& e) {
                reason = e.what();
            }
        }

        std::ofstream summary(directory / "result.json");
        summary << std::setprecision(17)
            << "{\"passed\":" << (failure ? "false" : "true")
            << ",\"referenceRecords\":" << driver.reference_records
            << ",\"assertions\":" << driver.assertions
            << ",\"negativeCases\":" << driver.negative_cases
            << ",\"maximumAbsoluteError\":" << driver.maximum_error
            << ",\"ownerNativeThread\":\"" << driver.owner_thread << "\""
            << ",\"otherNativeThread\":\"" << driver.other_thread << "\""
            << ",\"benchmark\":false,\"graphicsCalled\":false}\n";
        summary.flush();

        if (!summary || !calls) throw std::runtime_error("Result write failed");

        if (failure) {
            std::ofstream error(directory / "failure.txt");
            error << reason << '\n';
            std::cerr << reason << '\n';
            return 1;
        }

        std::cout << "PASS: " << driver.reference_records << " reference records; " << driver.negative_cases
            << " negative cases; max absolute error " << std::setprecision(17) << driver.maximum_error << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
