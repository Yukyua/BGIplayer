#pragma once
#include "bgiplayer/resource_integrity.hpp"
#include "bgiplayer/media_source.hpp"
#include "bgiplayer/pixel_buffer.hpp"

#include "bgiplayer/window_policy.hpp"

#include "bgiplayer/common.hpp"
#include "bgiplayer/validation.hpp"
#include "bgiplayer/decode_warnings.hpp"
#include "bgiplayer/display_classes.hpp"
#include "bgiplayer/native.hpp"
#include "bgiplayer/bitmap.hpp"
#include "bgiplayer/compressed_bg_v2.hpp"
#include "bgiplayer/compressed_bg_mt.hpp"
#include "bgiplayer/icon_bitmap_selection.hpp"
#include "bgiplayer/control_axis.hpp"
#include "bgiplayer/d0.hpp"
#include "bgiplayer/host_text_edit.hpp"
#include "bgiplayer/host_child_windows.hpp"
#include "bgiplayer/host_dialogs.hpp"
#include "bgiplayer/sound.hpp"
#include "bgiplayer/text.hpp"
#include "bgiplayer/text_layout.hpp"
#include "bgiplayer/glyph_opacity.hpp"
#include "bgiplayer/text_procedure.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <numbers>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace bgiplayer {

class BpFault : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class OperandStack {
    friend struct SnapshotCodec;
public:
    static constexpr std::size_t kCapacity = 0x1000;

    explicit OperandStack(std::size_t capacity = kCapacity)
        : capacity_(capacity) {}
    void push(uint32_t value);
    uint32_t pop();
    [[nodiscard]] std::size_t size() const { return values_.size(); }
    [[nodiscard]] std::size_t capacity() const { return capacity_; }
    [[nodiscard]] bool empty() const { return values_.empty(); }
    [[nodiscard]] const std::deque<uint32_t>& values() const { return values_; }

private:
    std::size_t capacity_ = kCapacity;
    std::deque<uint32_t> values_;
};

class Memory {
    friend struct SnapshotCodec;
public:
    static constexpr uint32_t kCodeTag = 0x04000000U;
    static constexpr uint32_t kFrameTag = 0x08000000U;
    struct CodeRegion {
        std::span<const uint8_t> bytes;
        uint32_t base = 0;
    };

    explicit Memory(std::size_t frame_size = 0x400000U,
                    std::size_t heap_size = 0x400000U);

    void set_code(std::vector<uint8_t> code);
    void swap_code(std::vector<uint8_t>& code) { code_.swap(code); }
    void set_active_code(std::span<const uint8_t> code,
                         uint32_t base = 0) {
        active_code_ = code;
        active_code_base_ = base;
    }
    void set_code_fallbacks(std::vector<CodeRegion> code_fallbacks) {
        code_fallbacks_ = std::move(code_fallbacks);
    }
    [[nodiscard]] std::span<const uint8_t> code() const { return code_; }
    [[nodiscard]] std::span<const uint8_t> active_code() const {
        return active_code_;
    }
    [[nodiscard]] std::span<uint8_t> frame() { return frame_; }
    [[nodiscard]] std::span<const uint8_t> frame() const { return frame_; }
    void replace_global_frame(std::vector<uint8_t> data) {
        frame_ = std::move(data);
    }
    void clear_resize_global_frame(std::size_t size) {
        frame_.assign(size, 0);
    }
    void swap_thread_frame(std::vector<uint8_t>& frame) {
        local_frame_.swap(frame);
    }

    uint8_t read_u8(uint32_t address, uint32_t base) const;
    uint16_t read_u16(uint32_t address, uint32_t base) const;
    uint32_t read_u32(uint32_t address, uint32_t base) const;
    void write_u8(uint32_t address, uint32_t base, uint8_t value);
    void write_u16(uint32_t address, uint32_t base, uint16_t value);
    void write_u32(uint32_t address, uint32_t base, uint32_t value);
    std::vector<uint8_t> read_bytes(uint32_t address, std::size_t size,
                                    uint32_t base) const;
    void write_bytes(uint32_t address, std::span<const uint8_t> data,
                     uint32_t base);
    std::string read_cstring(uint32_t address, uint32_t base) const;
    void write_cstring(uint32_t address, std::string_view value, uint32_t base);
    uint32_t allocate(std::size_t size);
    bool release(uint32_t address);
    void set_frame_byte_watch(
        std::optional<std::size_t> offset,
        std::function<void(uint32_t, std::size_t, uint8_t)> callback = {}) {
        frame_byte_watches_.clear();
        if (offset && callback) {
            frame_byte_watches_[*offset] = std::move(callback);
        }
    }
    void add_frame_byte_watch(
        std::size_t offset,
        std::function<void(uint32_t, std::size_t, uint8_t)> callback) {
        frame_byte_watches_[offset] = std::move(callback);
    }

private:
    friend class VM;
    enum class Space {
        Code,
        ActiveCode,
        FallbackCode,
        GlobalFrame,
        ThreadFrame,
        Heap
    };
    struct Location {
        Space space;
        std::size_t offset;
        std::size_t fallback_index = 0;
        uint32_t allocation = 0;
    };
    struct HeapGroup {
        uint32_t capacity = 0;
        uint32_t slots = 0;
        uint32_t shift = 0;
        uint32_t base = 0;
        uint32_t mask = 0;
    };
    static constexpr std::array<HeapGroup, 5> heap_groups_{{
        {0x00001000U, 0x00010000U, 0x0CU, 0x10U, 0x00000FFFU},
        {0x00010000U, 0x00001000U, 0x10U, 0x14U, 0x0000FFFFU},
        {0x00100000U, 0x00000100U, 0x14U, 0x18U, 0x000FFFFFU},
        {0x01000000U, 0x00000010U, 0x18U, 0x1CU, 0x00FFFFFFU},
        {0x04000000U, 0x00000020U, 0x1AU, 0x20U, 0x03FFFFFFU},
    }};
    static uint32_t heap_handle(std::size_t group,
                                uint32_t slot) {
        const HeapGroup& info = heap_groups_[group];
        const uint32_t slot_bits = 26U - info.shift;
        const uint32_t low_mask =
            slot_bits == 0U ? 0U : (1U << slot_bits) - 1U;
        const uint32_t group_code =
            info.base + (slot >> slot_bits);
        return (group_code << 26U) |
               ((slot & low_mask) << info.shift);
    }
    static std::optional<std::tuple<std::size_t, uint32_t,
                                    uint32_t>>
    decode_heap_handle(uint32_t address) {
        const uint32_t group_code = address >> 26U;
        if (group_code < heap_groups_.front().base) {
            return std::nullopt;
        }
        for (std::size_t group = 0; group < heap_groups_.size();
             ++group) {
            const HeapGroup& info = heap_groups_[group];
            const uint32_t slot_bits = 26U - info.shift;
            const uint32_t group_count =
                (info.slots - 1U) >> slot_bits;
            if (group_code < info.base ||
                group_code > info.base + group_count) {
                continue;
            }
            const uint32_t slot =
                ((group_code - info.base) << slot_bits) |
                ((address & 0x03FFFFFFU) >> info.shift);
            if (slot >= info.slots) {
                return std::nullopt;
            }
            return std::tuple{
                group, slot, address & info.mask};
        }
        return std::nullopt;
    }

    [[nodiscard]] Location resolve(uint32_t address, uint32_t base,
                                   std::size_t width) const;

    std::vector<uint8_t> code_;
    std::span<const uint8_t> active_code_;
    uint32_t active_code_base_ = 0;
    std::vector<CodeRegion> code_fallbacks_;
    std::vector<uint8_t> frame_;
    std::vector<uint8_t> local_frame_;
    std::map<uint32_t, std::vector<uint8_t>> heap_allocations_;
    std::map<std::size_t,
             std::function<void(uint32_t, std::size_t, uint8_t)>>
        frame_byte_watches_;
};

enum class ThreadState { Running, Waiting, Done, Faulted };

struct CallFrame {
    uint32_t ip = 0;
    bool has_code_resource = false;
    uint32_t code_resource_base = 0;
    uint32_t return_slot = 0;
};

struct WaitProcedureResult {
    int status = 0;
    std::vector<uint32_t> outputs;
};

using WaitProcedure = std::function<WaitProcedureResult()>;
// Host closure identity for debug checkpoints. Normal scheduling still invokes
// the same wait procedure; addresses of C++ closures never enter snapshot files.
enum class SnapshotWaitKind : uint8_t { None, DisplayControl, ScreenShake, RichText, SoundLoad, Codec, MfMovie, ResourceIntegrity };

struct ThreadRange {
    uint32_t offset = 0;
    uint32_t size = 0;
    bool operator==(const ThreadRange&) const = default;
};

struct Thread {
    std::string name;
    uint32_t id = 0;
    uint32_t parent_id = 0;
    std::vector<uint32_t> children;
    uint32_t ip = 0;
    uint32_t base = 0;
    uint32_t data_base = 0;
    uint32_t code_capacity = 0;
    uint32_t data_capacity = 0;
    std::vector<ThreadRange> code_allocations;
    std::vector<ThreadRange> data_allocations;
    uint64_t steps = 0;
    uint64_t deadline_ms = 0;
    ThreadState state = ThreadState::Running;
    uint32_t status_flags = 0;
    uint32_t scheduler_marker = 0;
    NativeSignal scheduler_signal = NativeSignal::Continue;
    bool wait_for_timing = false;
    bool wait_for_input = false;
    uint32_t wait_scope = 0;
    WaitProcedure wait_procedure;
    SnapshotWaitKind snapshot_wait_kind = SnapshotWaitKind::None;
    uint64_t snapshot_wait_index = 0;
    OperandStack stack;
    std::vector<CallFrame> frame_stack;
    std::deque<uint32_t> command_queue;
    std::deque<std::array<uint32_t, 3>> triple_command_queue;
};

struct ModuleRequest {
    uint32_t index = 0;
    std::string prefix;
    std::string path;
    std::size_t byte_count = 0;
    bool loaded = false;
};

struct LoadedModule {
    std::string path;
    std::vector<uint8_t> bytes;
};

struct ProgramThreadState {
    Thread thread;
    std::string source;
    std::vector<uint8_t> code;
    std::vector<uint8_t> frame;
    uint32_t operand_slots = 0;
    uint32_t code_bytes = 0;
    uint32_t data_bytes = 0;
    uint32_t code_offset = 0;
    uint32_t data_offset = 0;
    uint32_t reserved_code_bytes = 0;
    uint32_t reserved_data_bytes = 0;
    uint32_t allocation_owner_id = 0;
    bool has_code_resource = false;
    uint32_t code_resource_base = 0;
};

struct SharedProgramThreadResult {
    uint32_t status = 0;
    uint32_t handle = 0;
};

struct ResourceSegment {
    std::vector<uint8_t> bytes;
    uint32_t code_offset = 0;
    uint32_t code_size = 0;
    std::string path;
};

class VM {
    friend struct SnapshotCodec;
public:
    enum class StepResult { Continue, Yield, Done, Fault };
    void set_parallel_worker_count(uint32_t count){parallel_worker_count_=std::min(count,64U);}
    [[nodiscard]] uint32_t parallel_worker_count()const{return parallel_worker_count_;}
    void dispatch_parallel_script(uint32_t operand_slots,uint32_t code_bytes,
                                  uint32_t data_bytes,uint32_t entry_ip);
    using ResourceIntegrityProvider = std::function<std::optional<ResourceIntegrityData>(
        std::string_view,std::string_view)>;
    void set_resource_integrity_provider(ResourceIntegrityProvider provider) {
        resource_integrity_provider_=std::move(provider);
    }
    std::optional<ResourceIntegrityData> read_resource_integrity_data(
        std::string_view archive,std::string_view resource) const {
        if(!resource_integrity_provider_)throw BpFault("resource integrity host provider is unavailable");
        return resource_integrity_provider_(archive,resource);
    }
    void install_resource_integrity_wait(Thread& thread,ResourceIntegrityData data);
    WaitProcedureResult advance_resource_integrity_procedure(uint32_t index);
    using MediaSourceProvider=std::function<std::optional<std::string>(const MediaSourceRequest&)>;
    void set_media_source_provider(MediaSourceProvider provider) {
        media_source_provider_=std::move(provider);
    }
    uint32_t select_media_source(const MediaSourceRequest& request) {
        if(!media_source_provider_)throw BpFault("removable-media host provider is unavailable");
        const auto selected=media_source_provider_(request);
        if(!selected)return 0;
        secondary_directory_=*selected;
        media_source_message_=request.message;
        media_source_caption_=request.caption;
        return 1;
    }
    const std::string& media_source_message() const {return media_source_message_;}
    const std::string& media_source_caption() const {return media_source_caption_;}
    using UiLanguageProvider = std::function<uint32_t()>;
    void set_ui_language_provider(UiLanguageProvider provider) { ui_language_provider_ = std::move(provider); }
    uint32_t set_ui_language(uint32_t value) {
        if (value == 0) {
            if (!ui_language_provider_) throw BpFault("host UI language provider is unavailable");
            value = ui_language_provider_() & 0xFFFFU;
        }
        ui_language_ = value;
        return value;
    }
    [[nodiscard]] uint32_t ui_language() const { return ui_language_; }
    [[nodiscard]] uint32_t ui_language_is_japanese() const { return (ui_language_ & 0x3FFU) == 0x11U ? 1U : 0U; }
    using ResourceLoader =
        std::function<std::vector<uint8_t>(std::string_view,
                                           std::string_view)>;
    using FileEnumerator =
        std::function<std::vector<std::string>(std::string_view, bool,
                                               std::size_t)>;
    using ArchiveEntryEnumerator = std::function<
        std::optional<std::vector<std::string>>(std::string_view)>;
    using DirectoryEnumerator =
        std::function<std::vector<std::string>(std::string_view,
                                               std::size_t)>;
    using DriveTypeEnumerator =
        std::function<std::array<uint32_t, 26>()>;
    using SpecialFolderProvider =
        std::function<std::optional<std::string>(uint32_t)>;
    struct HostFontFace {
        std::string name;
        uint32_t charset = 0;
    };
    using FontEnumerator =
        std::function<std::vector<HostFontFace>()>;
    struct HostSystemMetadata {
        std::string cpu_brand;
        std::string version;
        std::array<uint16_t, 4> version_words{};
        std::string os_version_string;
        std::array<uint32_t, 4> os_version_words{};
    };
    using HostSystemMetadataProvider =
        std::function<HostSystemMetadata()>;
    using VolumeLabelProvider =
        std::function<std::optional<std::string>(std::string_view)>;
    struct WindowWorkArea {
        int32_t left = 0;
        int32_t top = 0;
        int32_t right = 0;
        int32_t bottom = 0;
    };
    using WindowWorkAreaEnumerator =
        std::function<std::vector<WindowWorkArea>()>;
    using CompositeArchiveRegistrar =
        std::function<void(std::string_view, const std::vector<std::string>&)>;
    using FileReader =
        std::function<std::optional<std::vector<uint8_t>>(std::string_view)>;
    using FileSizeResolver =
        std::function<uint32_t(std::string_view, std::string_view)>;
    using FileExistsResolver =
        std::function<bool(std::string_view, std::string_view)>;
    using ExternalPathOpener =
        std::function<uint32_t(std::string_view)>;
    using ClipboardSetter =
        std::function<bool(std::string_view)>;
    using MessageBoxHandler =
        std::function<int(std::string_view, uint32_t)>;
    struct Ext0DialogRequest {
        bool unicode_dialog = false;
        uint32_t resource = 0;
        std::vector<std::string> arguments;
        std::vector<std::variant<uint32_t, std::string>>
            ordered_arguments;
    };
    using Ext0DialogHandler =
        std::function<uint32_t(const Ext0DialogRequest&)>;
    using FolderPicker =
        std::function<std::optional<std::string>(
            std::string_view, uint32_t)>;
    struct FileDialogFilter {
        std::string pattern;
        std::string description;
    };
    struct FileDialogRequest {
        bool save = false;
        std::string title;
        std::string initial_directory;
        std::vector<FileDialogFilter> filters;
    };
    using FileDialogHandler =
        std::function<std::optional<std::string>(
            const FileDialogRequest&)>;
    using SavedRecordHandler =
        std::function<bool(uint32_t, std::string_view,
                           std::span<const uint8_t>)>;
    using RegistryValueProvider =
        std::function<std::optional<std::string>(
            std::string_view, std::string_view)>;
    void set_game_identifier(std::string identifier) { game_identifier_ = std::move(identifier); }
    const std::string& game_identifier() const { return game_identifier_; }
    // The 64-bit engine (BURIKO GDB 3.00 targets such as has_t) defines this
    // compact instruction block. The older targets leave every one of these
    // dispatch slots empty, so the profile is a per-game compatibility choice,
    // not a semantic alternative.
    void set_compact_64_opcodes(bool enabled) { compact_64_opcodes_ = enabled; }
    bool compact_64_opcodes() const { return compact_64_opcodes_; }
    // Host metadata follows the original configure core's checked selector
    // bound. A bootstrap opcode is not evidence of renderer capabilities.
    void set_effector_mode_limit(uint32_t limit) {
        if(limit!=1U && limit!=5U)throw BpFault("unsupported effector mode capability");
        effector_mode_limit_=limit;
    }
    uint32_t effector_mode_limit() const { return compact_64_opcodes_ ? 5U : effector_mode_limit_; }
    bool extended_effector_modes() const { return effector_mode_limit()>=5U; }
    static constexpr bool is_compact_64_opcode(uint8_t opcode) {
        switch (opcode) {
        case 0x03: case 0x0D: case 0x0E: case 0x13:
        case 0x36: case 0x37: case 0x3E: case 0x3F:
        case 0xD4: case 0xD5: case 0xD6: case 0xD7: case 0xD8: case 0xD9:
        case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        case 0xE8: case 0xE9: case 0xEA: case 0xEC: case 0xED: case 0xEE:
        case 0xEF: case 0xF0: case 0xF1: case 0xF2: case 0xF4: case 0xF5:
        case 0xF7: case 0xF8: case 0xF9: case 0xFA: case 0xFB:
            return true;
        default:
            return false;
        }
    }
    using RegistryDeleteHandler =
        std::function<bool(std::string_view)>;
    struct DirectInputControllerState {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
        int32_t rx = 0;
        uint32_t pov = 0xFFFFFFFFU;
        uint32_t buttons = 0;
    };
    using DirectInputStateProvider =
        std::function<std::vector<DirectInputControllerState>(uint32_t)>;
    using KeyboardStateProvider = std::function<std::array<uint8_t, 256>()>;
    struct ShellLinkRequest {
        std::string directory;
        std::string file;
        std::string target;
        std::string description;
    };
    using ShellLinkHandler =
        std::function<bool(const ShellLinkRequest&)>;
    struct ResourceListRequest {
        std::string path;
        std::string title;
        std::string prompt;
        std::vector<std::string> items;
    };
    using ResourceListHandler =
        std::function<std::optional<std::string>(
            const ResourceListRequest&)>;
    using LegacyDialogHandler =
        std::function<intptr_t(const std::array<uint32_t, 6>&)>;
    struct LegacyConfigDialogRequest {
        uint32_t text_buffer = 0;
        uint32_t output_first = 0;
        uint32_t output_second = 0;
        std::string primary_text;
        uint32_t field_1 = 0;
        uint32_t field_2 = 0;
        std::string secondary_text;
        std::string tertiary_text;
    };
    struct LegacyConfigDialogResult {
        intptr_t dialog_result = 0;
        std::optional<std::string> text;
        std::optional<uint32_t> first;
        std::optional<uint32_t> second;
    };
    using LegacyConfigDialogHandler =
        std::function<LegacyConfigDialogResult(
            const LegacyConfigDialogRequest&)>;
    using PathCleanupHandler =
        std::function<void(std::string_view, std::string_view,
                           std::string_view, uint32_t)>;
    struct RequiredResourceRequest {
        std::string file;
        std::string context;
        std::string message;
    };
    enum class RequiredResourceAction {
        Retry,
        Quit,
    };
    using RequiredResourceHandler =
        std::function<RequiredResourceAction(
            const RequiredResourceRequest&)>;
    using MinimizeHandler = std::function<void()>;
    using CloseRequestHandler = std::function<void()>;
    using DragAcceptHandler = std::function<void(bool)>;
    using WindowRepositionHandler =
        std::function<void(int32_t, int32_t)>;
    using TouchWindowHandler = std::function<bool(bool)>;
    struct NamedMutexHooks {
        std::function<std::optional<uint64_t>(std::string_view)> open;
        std::function<void(uint64_t)> close;
    };
    struct NamedMutexState {
        std::string name;
        uint64_t handle = 0;
    };
    struct LockRecord {
        uint32_t id = 0;
        uint32_t blocking_refs = 0;
        uint32_t entries = 0;
        uint32_t owner = 0;
    };
    struct WindowModeRequest {
        uint32_t fullscreen = 0;
        uint32_t flag = 0;
        uint32_t index = 2;
        uint32_t mode_width = 0;
        uint32_t mode_height = 0;
        uint32_t window_width = 0;
        uint32_t window_height = 0;
        bool use_mode_size = false;
    };
    using WindowModeApplier =
        std::function<bool(const WindowModeRequest&)>;

    explicit VM(std::vector<uint8_t> code);

    Thread& main_thread() { return thread_; }
    Memory& memory() { return memory_; }
    void set_file_enumerator(FileEnumerator enumerator) {
        file_enumerator_ = std::move(enumerator);
    }
    [[nodiscard]] bool has_file_enumerator() const {
        return static_cast<bool>(file_enumerator_);
    }
    [[nodiscard]] std::vector<std::string> enumerate_files(
        std::string_view pattern, bool recursive, std::size_t limit) const {
        if (!file_enumerator_) {
            return {};
        }
        return file_enumerator_(pattern, recursive, limit);
    }
    void set_archive_entry_enumerator(ArchiveEntryEnumerator enumerator) {
        archive_entry_enumerator_ = std::move(enumerator);
    }
    [[nodiscard]] std::optional<std::vector<std::string>>
    enumerate_archive_entries(std::string_view archive) const {
        return archive_entry_enumerator_ ? archive_entry_enumerator_(archive)
                                         : std::nullopt;
    }
    void set_directory_enumerator(DirectoryEnumerator enumerator) {
        directory_enumerator_ = std::move(enumerator);
    }
    [[nodiscard]] std::vector<std::string> enumerate_directories(
        std::string_view pattern, std::size_t limit) const {
        if (!directory_enumerator_) {
            return {};
        }
        return directory_enumerator_(pattern, limit);
    }
    void set_drive_type_enumerator(DriveTypeEnumerator enumerator) {
        drive_type_enumerator_ = std::move(enumerator);
    }
    void set_special_folder_provider(SpecialFolderProvider provider) {
        special_folder_provider_ = std::move(provider);
    }
    [[nodiscard]] std::optional<std::string> special_folder(
        uint32_t mode) const {
        return special_folder_provider_
                   ? special_folder_provider_(mode)
                   : std::nullopt;
    }
    void set_font_enumerator(FontEnumerator enumerator) {
        font_enumerator_ = std::move(enumerator);
    }
    void set_host_system_metadata_provider(
        HostSystemMetadataProvider provider) {
        host_system_metadata_provider_ = std::move(provider);
    }
    void set_volume_label_provider(VolumeLabelProvider provider) {
        volume_label_provider_ = std::move(provider);
    }
    [[nodiscard]] std::array<uint32_t, 26> enumerate_drive_types() const {
        if (!drive_type_enumerator_) {
            std::array<uint32_t, 26> unavailable{};
            unavailable.fill(1);
            return unavailable;
        }
        return drive_type_enumerator_();
    }
    [[nodiscard]] std::vector<HostFontFace> enumerate_host_fonts(
        uint32_t charset) const {
        std::vector<HostFontFace> faces;
        if (font_enumerator_) {
            faces = font_enumerator_();
        } else {
            for (const auto& [name, id] : font_ids_) {
                (void)id;
                const auto found = font_charsets_.find(name);
                if (found != font_charsets_.end()) {
                    faces.push_back({name, found->second});
                }
            }
        }
        std::erase_if(faces, [charset](const HostFontFace& face) {
            return face.charset != charset;
        });
        return faces;
    }
    uint32_t enumerate_fonts(uint32_t destination, uint32_t charset,
                             uint32_t base) {
        const std::vector<HostFontFace> faces =
            enumerate_host_fonts(charset);
        if (destination == 0) {
            uint64_t bytes = 0;
            for (const HostFontFace& face : faces) {
                bytes += face.name.size() + 1U;
            }
            return static_cast<uint32_t>(bytes);
        }
        uint32_t cursor = destination;
        for (const HostFontFace& face : faces) {
            memory_.write_cstring(cursor, face.name, base);
            cursor += static_cast<uint32_t>(face.name.size() + 1U);
        }
        return static_cast<uint32_t>(faces.size());
    }
    void set_window_work_area_enumerator(
        WindowWorkAreaEnumerator enumerator) {
        window_work_area_enumerator_ = std::move(enumerator);
    }
    void set_composite_archive_registrar(CompositeArchiveRegistrar registrar) {
        composite_archive_registrar_ = std::move(registrar);
    }
    void set_file_reader(FileReader reader) {
        file_reader_ = std::move(reader);
    }
    void set_external_path_opener(ExternalPathOpener opener) {
        external_path_opener_ = std::move(opener);
    }
    uint32_t open_external_path(std::string_view path) const {
        return external_path_opener_ ? external_path_opener_(path) : 2U;
    }
    using ProcessLauncher =
        std::function<bool(std::string_view, std::string_view)>;
    struct ExtendedProcessLaunchRequest {
        std::string executable;
        std::string arguments;
        std::string working_directory;
        std::string quit_message;
        int32_t show_state = 0;
        bool mutex_mode = false;
    };
    struct ExtendedProcessLaunchResult {
        bool launched = false;
        uint32_t exit_code = 0;
    };
    using ExtendedProcessLauncher =
        std::function<ExtendedProcessLaunchResult(
            const ExtendedProcessLaunchRequest&)>;
    void set_process_launcher(ProcessLauncher launcher) {
        process_launcher_ = std::move(launcher);
    }
    bool launch_process(std::string_view executable,
                        std::string_view arguments) const {
        if (!process_launcher_) {
            throw BpFault("host process launcher is not configured");
        }
        return process_launcher_(executable, arguments);
    }
    void set_extended_process_launcher(ExtendedProcessLauncher launcher) {
        extended_process_launcher_ = std::move(launcher);
    }
    ExtendedProcessLaunchResult launch_process_extended(
        const ExtendedProcessLaunchRequest& request) const {
        if (!extended_process_launcher_) {
            throw BpFault("extended host process launcher is not configured");
        }
        return extended_process_launcher_(request);
    }
    using PluginLoadHandler =
        std::function<uint32_t(std::string_view)>;
    using PluginUnloadHandler =
        std::function<void(uint32_t)>;
    using PluginInvokeHandler =
        std::function<uint32_t(uint32_t, uint32_t, uint32_t, uint32_t)>;
    void set_plugin_handlers(PluginLoadHandler load,
                             PluginUnloadHandler unload = {},
                             PluginInvokeHandler invoke = {}) {
        plugin_load_handler_ = std::move(load);
        plugin_unload_handler_ = std::move(unload);
        plugin_invoke_handler_ = std::move(invoke);
    }
    uint32_t load_plugin(std::string_view path, uint32_t& id) {
        if (!plugin_load_handler_) {
            throw BpFault("host plugin loader is not configured");
        }
        const uint32_t status = plugin_load_handler_(path);
        if (status != 0) {
            return status;
        }
        id = ++next_plugin_id_;
        plugins_.emplace(id, std::string(path));
        return 0;
    }
    uint32_t unload_plugin(uint32_t id) {
        const auto plugin = plugins_.find(id);
        if (plugin == plugins_.end()) {
            return 3;
        }
        if (plugin_unload_handler_) {
            plugin_unload_handler_(id);
        }
        plugins_.erase(plugin);
        return 0;
    }
    struct PluginCallResult {
        uint32_t status = 0;
        uint32_t value = 0;
    };
    PluginCallResult invoke_plugin(
        uint32_t id, uint32_t first, uint32_t second, uint32_t third) {
        if (plugins_.count(id) == 0) {
            return {3, 0};
        }
        if (!plugin_invoke_handler_) {
            throw BpFault("host plugin invocation is not configured");
        }
        const uint32_t value =
            plugin_invoke_handler_(id, first, second, third);
        return {value == 0 ? 0U : 4U, value};
    }
    [[nodiscard]] const std::map<uint32_t, std::string>& plugins() const {
        return plugins_;
    }
    void set_file_size_resolver(FileSizeResolver resolver) {
        file_size_resolver_ = std::move(resolver);
    }
    void set_file_exists_resolver(FileExistsResolver resolver) {
        file_exists_resolver_ = std::move(resolver);
    }
    void set_clipboard_setter(ClipboardSetter setter) {
        clipboard_setter_ = std::move(setter);
    }
    bool set_clipboard_text(std::string_view text) const {
        return clipboard_setter_ && clipboard_setter_(text);
    }
    void set_message_box_handler(MessageBoxHandler handler) {
        message_box_handler_ = std::move(handler);
    }
    void set_ext0_dialog_handler(Ext0DialogHandler handler) {
        ext0_dialog_handler_ = std::move(handler);
    }
    void set_folder_picker(FolderPicker picker) {
        folder_picker_ = std::move(picker);
    }
    void set_file_dialog_handler(FileDialogHandler handler) {
        file_dialog_handler_ = std::move(handler);
    }
    void set_saved_record_handler(SavedRecordHandler handler) {
        saved_record_handler_ = std::move(handler);
    }
    void set_registry_handlers(
        RegistryValueProvider value_provider,
        RegistryDeleteHandler delete_handler = {}) {
        registry_value_provider_ = std::move(value_provider);
        registry_delete_handler_ = std::move(delete_handler);
    }
    void set_direct_input_state_provider(
        DirectInputStateProvider provider) {
        direct_input_state_provider_ = std::move(provider);
    }
    void set_shell_link_handler(ShellLinkHandler handler) {
        shell_link_handler_ = std::move(handler);
    }
    bool create_shell_link(const ShellLinkRequest& request) const {
        return shell_link_handler_ &&
               shell_link_handler_(request);
    }
    void set_resource_list_handler(ResourceListHandler handler) {
        resource_list_handler_ = std::move(handler);
    }
    std::optional<std::string> show_resource_list(
        const ResourceListRequest& request) const {
        return resource_list_handler_
                   ? resource_list_handler_(request)
                   : std::nullopt;
    }
    void set_legacy_dialog_handler(LegacyDialogHandler handler) {
        legacy_dialog_handler_ = std::move(handler);
    }
    void set_legacy_config_dialog_handler(
        LegacyConfigDialogHandler handler) {
        legacy_config_dialog_handler_ = std::move(handler);
    }
    void set_path_cleanup_handler(PathCleanupHandler handler) {
        path_cleanup_handler_ = std::move(handler);
    }
    void cleanup_paths(std::string_view first, std::string_view second,
                       std::string_view third, uint32_t flags) const {
        if (path_cleanup_handler_) {
            path_cleanup_handler_(first, second, third, flags);
        }
    }
    intptr_t show_legacy_dialog(
        const std::array<uint32_t, 6>& arguments) const {
        return legacy_dialog_handler_
                   ? legacy_dialog_handler_(arguments)
                   : 0;
    }
    LegacyConfigDialogResult show_legacy_config_dialog(
        const LegacyConfigDialogRequest& request) const {
        return legacy_config_dialog_handler_
                   ? legacy_config_dialog_handler_(request)
                   : LegacyConfigDialogResult{};
    }
    bool query_direct_input_state(
        uint32_t controller_id, uint32_t output, uint32_t base) {
        if (!direct_input_state_provider_ || output == 0) {
            return false;
        }
        static constexpr std::array<uint32_t, 64> hat_combine = {
            0, 1, 1, 2, 0xFFFFFFFFU, 6, 7, 7,
            1, 1, 1, 2, 2, 0xFFFFFFFFU, 0, 0,
            1, 1, 2, 3, 3, 4, 0xFFFFFFFFU, 0,
            2, 2, 3, 3, 3, 4, 4, 0xFFFFFFFFU,
            0xFFFFFFFFU, 2, 3, 3, 4, 5, 5, 6,
            6, 0xFFFFFFFFU, 4, 4, 5, 5, 5, 6,
            7, 0, 0xFFFFFFFFU, 4, 5, 5, 6, 7,
            7, 0, 0, 0xFFFFFFFFU, 6, 6, 7, 7,
        };
        const std::vector<DirectInputControllerState> states =
            direct_input_state_provider_(controller_id);
        if (states.empty()) {
            return false;
        }
        int64_t x = 0;
        int64_t y = 0;
        int64_t z = 0;
        int64_t rx = 0;
        uint32_t pov = 0xFFFFFFFFU;
        uint32_t buttons = 0;
        for (const DirectInputControllerState& state : states) {
            x += state.x;
            y += state.y;
            z += state.z;
            rx += state.rx;
            buttons |= state.buttons;
            if (state.pov < 36000U) {
                uint32_t index = state.pov / 4500U;
                if (pov != 0xFFFFFFFFU) {
                    index = hat_combine[
                        index + static_cast<std::size_t>(pov) * 8U];
                }
                pov = index;
            }
        }
        const auto clamp = [](int64_t value) -> uint32_t {
            return static_cast<uint32_t>(
                std::clamp<int64_t>(value, -1024, 1024));
        };
        memory_.write_u32(output, base, clamp(x));
        memory_.write_u32(output + 4U, base, clamp(y));
        memory_.write_u32(output + 8U, base, clamp(z));
        memory_.write_u32(output + 12U, base, clamp(rx));
        memory_.write_u32(output + 16U, base, pov);
        memory_.write_u32(output + 20U, base, buttons);
        return true;
    }
    bool query_registry_value(
        std::string_view key, std::string_view value,
        uint32_t destination, uint32_t base) {
        if (!registry_value_provider_) {
            return false;
        }
        const std::optional<std::string> result =
            registry_value_provider_(key, value);
        if (!result) {
            return false;
        }
        if (destination != 0) {
            memory_.write_cstring(destination, *result, base);
        }
        return true;
    }
    bool read_windows_file_strip_slash(
        uint32_t output, std::string_view name, uint32_t base) {
        if (output == 0 || !special_folder_provider_) {
            return false;
        }
        const std::optional<std::string> windows =
            special_folder_provider_(0);
        if (!windows) {
            return false;
        }
        const std::string separator =
            windows->ends_with('\\') || windows->ends_with('/')
                ? ""
                : "\\";
        const std::optional<std::vector<uint8_t>> data =
            read_virtual_file(*windows + separator + std::string(name));
        if (!data) {
            return false;
        }
        memory_.write_bytes(output, *data, base);
        if (data->size() >= 2U &&
            (*data)[data->size() - 2U] == '\\' &&
            (*data)[data->size() - 1U] == 0) {
            memory_.write_u8(
                output + static_cast<uint32_t>(data->size() - 2U),
                base, 0);
            return true;
        }
        return false;
    }
    bool delete_registry_key(std::string_view key) const {
        return registry_delete_handler_ &&
               registry_delete_handler_(key);
    }
    void set_required_resource_handler(
        RequiredResourceHandler handler) {
        required_resource_handler_ = std::move(handler);
    }
    bool save_record(uint32_t mode, std::string_view name,
                     std::span<const uint8_t> data) const {
        return saved_record_handler_ &&
               saved_record_handler_(mode, name, data);
    }
    bool has_binary_source(std::string_view resource) const {
        return read_binary_source({}, std::string(resource))
            .has_value();
    }
    uint32_t require_resource(
        const RequiredResourceRequest& request) const {
        while (true) {
            if (read_binary_source({}, request.file)) {
                return 1;
            }
            if (!required_resource_handler_) {
                return 0;
            }
            if (required_resource_handler_(request) ==
                RequiredResourceAction::Quit) {
                return 6;
            }
        }
    }
    bool show_file_dialog(const FileDialogRequest& request,
                          uint32_t destination, uint32_t base) {
        if (!file_dialog_handler_) {
            return false;
        }
        const std::optional<std::string> selected =
            file_dialog_handler_(request);
        if (!selected || selected->size() >= 0x30cU) {
            return false;
        }
        memory_.write_cstring(destination, *selected, base);
        return true;
    }
    std::optional<std::vector<uint8_t>> read_binary_source(
        const std::string& directory,
        const std::string& resource) const {
        std::vector<std::string> candidates;
        const auto append = [&candidates](
                                const std::string& base,
                                const std::string& name) {
            if (base.empty()) {
                candidates.push_back(name);
            } else {
                candidates.push_back(
                    base + (base.ends_with('/') ? "" : "/") + name);
            }
        };
        append(primary_directory_, resource);
        for (const std::string& search_path : search_paths_) {
            append(search_path, resource);
        }
        append({}, resource);
        if (!directory.empty()) {
            const auto mount = external_mounts_.find(directory);
            if (mount != external_mounts_.end()) {
                append(mount->second, resource);
            }
            append(directory, resource);
        }
        if (!secondary_directory_.empty()) {
            append(secondary_directory_, resource);
            if (!directory.empty()) {
                append(
                    secondary_directory_,
                    directory +
                        (directory.ends_with('/') ? "" : "/") +
                        resource);
            }
        }
        for (const std::string& candidate : candidates) {
            if (std::optional<std::vector<uint8_t>> data =
                    read_virtual_file(candidate)) {
                return data;
            }
        }
        if (resource_loader_ && !resource.empty()) {
            try {
                return resource_loader_(directory, resource);
            } catch (const std::exception&) {
                if (directory.empty()) {
                    return std::nullopt;
                }
            }
            try {
                return resource_loader_({}, resource);
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }
    int32_t load_binary_span(
        uint32_t destination, const std::string& directory,
        const std::string& resource, uint32_t offset, uint32_t size,
        uint32_t base) {
        const std::optional<std::vector<uint8_t>> data =
            read_binary_source(directory, resource);
        if (!data) {
            return 1;
        }
        if (destination == 0) {
            return 0;
        }
        if (offset >= data->size()) {
            return 2;
        }
        const std::size_t available =
            data->size() - static_cast<std::size_t>(offset);
        const std::size_t copied =
            std::min<std::size_t>(size, available);
        memory_.write_bytes(
            destination,
            std::span<const uint8_t>(*data).subspan(offset, copied),
            base);
        return copied == size ? 0 : 3;
    }
    int32_t load_sys0_resource(
        uint32_t destination, const std::string& directory,
        const std::string& resource, uint32_t base) {
        const std::optional<std::vector<uint8_t>> data =
            read_binary_source(directory, resource);
        if (!data) {
            if (resource_load_trace_) {
                resource_load_trace_(
                    directory, resource, destination, 1, 0);
            }
            return static_cast<int32_t>(0x80000020U);
        }
        if (data->size() > 0x04000000U) {
            if (resource_load_trace_) {
                resource_load_trace_(
                    directory, resource, destination, 6,
                    static_cast<uint32_t>(data->size()));
            }
            return static_cast<int32_t>(0x80000060U);
        }
        if (data->empty()) {
            if (resource_load_trace_) {
                resource_load_trace_(
                    directory, resource, destination,
                    static_cast<int32_t>(0x80000040U), 0);
            }
            return static_cast<int32_t>(0x80000040U);
        }
        if (destination != 0 && !data->empty()) {
            memory_.write_bytes(destination, *data, base);
        }
        if (resource_load_trace_) {
            resource_load_trace_(
                directory, resource, destination, 0,
                static_cast<uint32_t>(data->size()));
        }
        return static_cast<int32_t>(data->size());
    }
    int32_t load_sys0_resource_span(
        uint32_t destination, const std::string& directory,
        const std::string& resource, uint32_t offset, uint32_t size,
        uint32_t base) {
        const std::optional<std::vector<uint8_t>> data =
            read_binary_source(directory, resource);
        if (!data) {
            return static_cast<int32_t>(0x80000020U);
        }
        if (data->size() > 0x04000000U) {
            return static_cast<int32_t>(0x80000060U);
        }
        if (size == 0 || data->size() < size) {
            return static_cast<int32_t>(0x80000040U);
        }
        if (static_cast<uint64_t>(offset) + size > data->size()) {
            return static_cast<int32_t>(0x80000030U);
        }
        if (destination != 0) {
            memory_.write_bytes(
                destination,
                std::span<const uint8_t>(*data).subspan(offset, size),
                base);
        }
        return static_cast<int32_t>(size);
    }
    int32_t probe_binary_source(
        const std::string& archive, const std::string& resource) const {
        if (read_binary_source({}, resource)) {
            return 0;
        }
        if (!archive.empty() &&
            read_binary_source(archive, resource)) {
            return 0;
        }
        return 1;
    }
    uint32_t filter_indexed_values(
        uint32_t output, uint32_t input, uint32_t count,
        uint32_t stride, uint32_t compare_value,
        uint32_t width_selector, uint32_t packed_operator,
        uint32_t base) {
        const uint32_t element_width =
            1U << (width_selector & 3U);
        if (output == 0 || element_width > stride ||
            element_width > 4U) {
            return 0;
        }
        const uint32_t operator_code = packed_operator & 0xFFFFU;
        const uint32_t flags = (packed_operator >> 16U) & 0xFFU;
        uint32_t matches = 0;
        for (uint32_t index = 0; index < count; ++index) {
            uint32_t loaded = 0;
            const uint32_t address = input + index * stride;
            for (uint32_t byte = 0; byte < element_width; ++byte) {
                loaded |= static_cast<uint32_t>(
                              memory_.read_u8(address + byte, base))
                          << (byte * 8U);
            }
            if ((flags & 1U) != 0 && loaded == 0) {
                continue;
            }
            const bool signed_comparison = width_selector > 3U;
            int32_t signed_loaded = 0;
            int32_t signed_compare = 0;
            if (element_width == 1) {
                signed_loaded = static_cast<int8_t>(loaded);
                signed_compare =
                    static_cast<int8_t>(compare_value);
            } else if (element_width == 2) {
                signed_loaded = static_cast<int16_t>(loaded);
                signed_compare =
                    static_cast<int16_t>(compare_value);
            } else {
                signed_loaded = static_cast<int32_t>(loaded);
                signed_compare =
                    static_cast<int32_t>(compare_value);
            }
            bool matched = false;
            switch (operator_code) {
                case 0:
                    matched = loaded == compare_value;
                    break;
                case 1:
                    matched = loaded != 0;
                    break;
                case 2:
                    matched = (compare_value & loaded) == 0;
                    break;
                case 3:
                    matched = (compare_value & loaded) != 0;
                    break;
                case 4:
                    matched = signed_comparison
                                  ? signed_loaded < signed_compare
                                  : loaded < compare_value;
                    break;
                case 5:
                    matched = signed_comparison
                                  ? signed_loaded <= signed_compare
                                  : loaded <= compare_value;
                    break;
                case 6:
                    matched = signed_comparison
                                  ? signed_compare < signed_loaded
                                  : compare_value < loaded;
                    break;
                case 7:
                    matched = signed_comparison
                                  ? signed_compare <= signed_loaded
                                  : compare_value <= loaded;
                    break;
                default:
                    break;
            }
            if (matched) {
                memory_.write_u32(
                    output + matches * 4U, base,
                    element_width + index);
                ++matches;
            }
        }
        return matches;
    }
    bool pick_folder(uint32_t destination, std::string_view title,
                     uint32_t extra, uint32_t base) {
        if (!folder_picker_) {
            return false;
        }
        const std::optional<std::string> path =
            folder_picker_(title, extra);
        if (!path) {
            return false;
        }
        memory_.write_cstring(destination, *path, base);
        return true;
    }
    uint32_t show_ext0_dialog(Ext0DialogRequest request) {
        return ext0_dialog_handler_
                   ? ext0_dialog_handler_(request)
                   : 0U;
    }
    int show_message_box(std::string_view text, uint32_t flags) const {
        if (!message_box_handler_) {
            throw BpFault("host message box handler is not configured");
        }
        return message_box_handler_(text, flags);
    }
    void set_minimize_handler(MinimizeHandler handler) {
        minimize_handler_ = std::move(handler);
    }
    void set_close_request_handler(CloseRequestHandler handler) {
        close_request_handler_ = std::move(handler);
    }
    void set_drag_accept_handler(DragAcceptHandler handler) {
        drag_accept_handler_ = std::move(handler);
    }
    void set_window_reposition_handler(
        WindowRepositionHandler handler) {
        window_reposition_handler_ = std::move(handler);
    }
    void set_window_mode_applier(WindowModeApplier applier) {
        window_mode_applier_ = std::move(applier);
    }
    void set_touch_window_handler(TouchWindowHandler handler) {
        touch_window_handler_ = std::move(handler);
    }
    uint32_t set_touch_window_enabled(bool enabled) {
        if (!touch_window_handler_) {
            return 0;
        }
        return touch_window_handler_(enabled) ? 1U : 0U;
    }
    void set_named_mutex_hooks(NamedMutexHooks hooks) {
        named_mutex_hooks_ = std::move(hooks);
    }
    uint32_t create_named_mutex(std::string name) {
        if (!named_mutex_hooks_.open || !named_mutex_hooks_.close) {
            return 0;
        }
        const std::optional<uint64_t> handle =
            named_mutex_hooks_.open(name);
        if (!handle) {
            return 0;
        }
        const uint32_t id = ++named_mutex_next_id_;
        named_mutexes_[id] =
            NamedMutexState{std::move(name), *handle};
        return id;
    }
    bool release_named_mutex(uint32_t id) {
        const auto found = named_mutexes_.find(id);
        if (found == named_mutexes_.end()) {
            return false;
        }
        named_mutex_hooks_.close(found->second.handle);
        named_mutexes_.erase(found);
        return true;
    }
    [[nodiscard]] std::size_t named_mutex_count() const {
        return named_mutexes_.size();
    }
    uint32_t create_lock_record() {
        std::lock_guard lock(lock_registry_mutex_);
        const uint32_t id = next_lock_record_id_++;
        lock_records_[id] = LockRecord{id, 0, 0, 0};
        return id;
    }
    uint32_t remove_lock_record(uint32_t id, bool force = false) {
        std::lock_guard lock(lock_registry_mutex_);
        const auto found = lock_records_.find(id);
        if (found == lock_records_.end()) {
            return 1;
        }
        if (found->second.blocking_refs != 0 && !force) {
            return 4;
        }
        lock_records_.erase(found);
        lock_registry_event_.notify_all();
        return 0;
    }
    uint32_t try_acquire_lock_record(uint32_t id, uint32_t thread_id) {
        std::lock_guard lock(lock_registry_mutex_);
        const auto found = lock_records_.find(id);
        if (found == lock_records_.end()) {
            return 1;
        }
        LockRecord& record = found->second;
        if (record.owner != 0 && record.owner != thread_id) {
            return 2;
        }
        ++record.blocking_refs;
        ++record.entries;
        record.owner = thread_id;
        return 0;
    }
    uint32_t acquire_lock_record(uint32_t id, uint32_t thread_id) {
        std::unique_lock lock(lock_registry_mutex_);
        const auto found = lock_records_.find(id);
        if (found == lock_records_.end()) {
            return 1;
        }
        LockRecord& record = found->second;
        ++record.blocking_refs;
        lock_registry_event_.wait(lock, [&record, thread_id] {
            return record.owner == 0 || record.owner == thread_id;
        });
        ++record.entries;
        record.owner = thread_id;
        return 0;
    }
    uint32_t release_lock_record(uint32_t id, uint32_t thread_id) {
        std::lock_guard lock(lock_registry_mutex_);
        const auto found = lock_records_.find(id);
        if (found == lock_records_.end()) {
            return 1;
        }
        LockRecord& record = found->second;
        if (record.entries == 0) {
            return 3;
        }
        if (record.owner != thread_id) {
            return 2;
        }
        --record.blocking_refs;
        --record.entries;
        if (record.entries == 0) {
            record.owner = 0;
            lock_registry_event_.notify_all();
        }
        return 0;
    }
    [[nodiscard]] uint32_t file_size(std::string_view archive,
                                     std::string_view path) const {
        return file_size_resolver_ ? file_size_resolver_(archive, path) : 0U;
    }
    [[nodiscard]] bool file_exists(std::string_view archive,
                                   std::string_view path) const {
        if (file_exists_resolver_) {
            return file_exists_resolver_(archive, path);
        }
        return file_size_resolver_ &&
               file_size_resolver_(archive, path) != 0U;
    }
    std::optional<std::vector<uint8_t>> read_virtual_file(
        std::string_view path) const {
        const auto virtual_file =
            virtual_files_.find(stream_path_key(std::string(path)));
        if (virtual_file != virtual_files_.end()) {
            return virtual_file->second;
        }
        return file_reader_
                   ? file_reader_(std::string(path))
                   : std::optional<std::vector<uint8_t>>{};
    }
    uint32_t query_carchive_path(std::string_view path) const {
        if (read_virtual_file(path)) {
            return 0;
        }
        if (resource_loader_) {
            try {
                (void)resource_loader_("", path);
                return 0;
            }
            catch (const std::exception&) {
                // The target returns its archive-missing status below.
            }
        }
        return 0x80000010U;
    }
    uint32_t register_composite_archive(
        const std::string& name, const std::vector<std::string>& components) {
        if (name.empty() || components.empty()) {
            return 0;
        }
        if (composite_archive_registrar_) {
            composite_archive_registrar_(name, components);
        }
        composite_archives_[name] = components;
        return static_cast<uint32_t>(composite_archive_ids_.size() + 1);
    }
    [[nodiscard]] const std::map<std::string, std::vector<std::string>>&
    composite_archives() const {
        return composite_archives_;
    }
    struct StreamState {
        std::string path;
        uint32_t mode = 0;
        std::vector<uint8_t> data;
        std::size_t position = 0;
    };
    using FileWriter =
        std::function<bool(std::string_view, std::span<const uint8_t>)>;
    using FileStreamTrace = std::function<void(
        std::string_view, std::string_view, uint32_t, int64_t)>;
    void set_file_writer(FileWriter writer) {
        file_writer_ = std::move(writer);
    }
    void set_file_stream_trace_handler(FileStreamTrace handler) {
        file_stream_trace_ = std::move(handler);
    }
    uint32_t open_file_stream(uint32_t mode, std::string path,
                              uint32_t output_address, uint32_t base) {
        const auto trace = [this, mode, &path](uint32_t result) {
            if (file_stream_trace_) {
                file_stream_trace_("open", path, mode, result);
            }
            return result;
        };
        if (mode > 2) {
            return trace(1);
        }
        const std::string key = stream_path_key(path);
        for (const auto& [id, stream] : streams_) {
            (void)id;
            if (stream_path_key(stream.path) == key) {
                return trace(2);
            }
        }
        std::optional<std::vector<uint8_t>> source;
        {
            const auto existing = virtual_files_.find(key);
            if (existing != virtual_files_.end()) {
                source = existing->second;
            } else if (file_reader_) {
                source = file_reader_(path);
            }
        }
        if (mode == 0 && !source) {
            return trace(3);
        }
        std::vector<uint8_t> data =
            mode == 1 ? std::vector<uint8_t>{}
                      : (source ? std::move(*source)
                                : std::vector<uint8_t>{});
        const std::size_t position = mode == 2 ? data.size() : 0;
        const uint32_t id = next_stream_id_++;
        streams_[id] =
            StreamState{path, mode, std::move(data), position};
        if (output_address != 0) {
            memory_.write_u32(output_address, base, id);
        }
        virtual_files_[key] = streams_[id].data;
        return trace(0);
    }
    uint32_t replace_string_namespace(
        uint32_t table_id, int32_t count, uint32_t source, uint32_t base) {
        if (count == 0) {
            string_namespaces_.erase(table_id);
            return 1;
        }
        if (count < 0 || source == 0) {
            return 0;
        }
        std::vector<std::string> strings;
        strings.reserve(static_cast<std::size_t>(count));
        uint32_t cursor = source;
        for (int32_t index = 0; index < count; ++index) {
            const std::string value =
                memory_.read_cstring(cursor, base);
            strings.push_back(value);
            cursor += static_cast<uint32_t>(value.size() + 1);
        }
        string_namespaces_[table_id] = std::move(strings);
        return 1;
    }
    [[nodiscard]] const std::map<uint32_t, std::vector<std::string>>&
    string_namespaces() const {
        return string_namespaces_;
    }
    void set_string_namespace(
        uint32_t table_id, std::vector<std::string> strings) {
        string_namespaces_[table_id] = std::move(strings);
    }
    void clear_string_namespaces_except(uint32_t preserved_id) {
        std::erase_if(
            string_namespaces_,
            [preserved_id](const auto& entry) {
                return entry.first != preserved_id;
            });
    }
    std::size_t append_string_namespace(uint32_t table_id,
                                        std::string value) {
        std::vector<std::string>& strings =
            string_namespaces_[table_id];
        const auto found =
            std::find(strings.begin(), strings.end(), value);
        if (found != strings.end()) {
            return static_cast<std::size_t>(
                std::distance(strings.begin(), found));
        }
        strings.push_back(std::move(value));
        return strings.size() - 1U;
    }
    void intern_global_string(std::string value) {
        auto& strings = string_namespaces_[0x80000000U];
        if (std::find(strings.begin(), strings.end(), value) ==
            strings.end()) {
            strings.push_back(std::move(value));
        }
    }
    [[nodiscard]] bool has_global_string(std::string_view value) const {
        const auto table = string_namespaces_.find(0x80000000U);
        return table != string_namespaces_.end() &&
               std::find(table->second.begin(), table->second.end(),
                         std::string(value)) != table->second.end();
    }
    struct FlagTable {
        uint32_t length = 0;
        std::vector<uint8_t> bytes;
    };
    bool define_flag_table(const std::string& name, uint32_t length) {
        if (length == 0) {
            return false;
        }
        const std::size_t byte_count =
            (static_cast<std::size_t>(length) + 7U) / 8U;
        FlagTable& table = flag_tables_[name];
        std::vector<uint8_t> replacement(byte_count, 0);
        const std::size_t copied =
            std::min(replacement.size(), table.bytes.size());
        std::copy_n(table.bytes.begin(), copied, replacement.begin());
        table.bytes = std::move(replacement);
        table.length = length;
        return true;
    }
    uint32_t set_flag(const std::string& name, uint32_t index,
                      bool enabled) {
        const auto found = flag_tables_.find(name);
        if (found == flag_tables_.end()) {
            return 1;
        }
        FlagTable& table = found->second;
        if (index >= table.length) {
            return 2;
        }
        const uint8_t mask =
            static_cast<uint8_t>(0x80U >> (index & 7U));
        uint8_t& value = table.bytes[index >> 3U];
        value = enabled ? static_cast<uint8_t>(value | mask)
                        : static_cast<uint8_t>(value & ~mask);
        return 0;
    }
    uint32_t set_flag_range(const std::string& name, uint32_t start,
                            bool enabled, uint32_t count) {
        const auto found = flag_tables_.find(name);
        if (found == flag_tables_.end()) {
            return 1;
        }
        FlagTable& table = found->second;
        if (start >= table.length) {
            return 2;
        }
        if (count == 0 || count > 0x10000U ||
            static_cast<uint64_t>(start) + count > table.length) {
            return 3;
        }
        for (uint32_t bit = start; bit < start + count; ++bit) {
            const uint8_t mask =
                static_cast<uint8_t>(0x80U >> (bit & 7U));
            uint8_t& value = table.bytes[bit >> 3U];
            value = enabled ? static_cast<uint8_t>(value | mask)
                            : static_cast<uint8_t>(value & ~mask);
        }
        return 0;
    }
    [[nodiscard]] const std::map<std::string, FlagTable>& flag_tables()
        const {
        return flag_tables_;
    }
    struct ProcessRecord {
        std::array<uint32_t, 9> fields{};
        std::array<std::string, 5> strings;
    };
    void reset_process_records(uint32_t limit) {
        process_records_.clear();
        process_record_limit_ = limit;
    }
    bool add_process_record(ProcessRecord record) {
        static constexpr std::array<std::size_t, 5> limits{
            31, 31, 31, 255, 511};
        for (std::size_t index = 0; index < record.strings.size();
             ++index) {
            if (record.strings[index].size() > limits[index]) {
                return false;
            }
        }
        process_records_.push_back(std::move(record));
        while (process_records_.size() > process_record_limit_) {
            process_records_.pop_front();
        }
        return true;
    }
    bool process_record_index(uint32_t index,
                              ProcessRecord& record) const {
        if (index >= process_records_.size()) {
            return false;
        }
        record = process_records_[index];
        return true;
    }
    [[nodiscard]] std::size_t process_record_count() const {
        return process_records_.size();
    }
    void format_uninstaller_message(uint32_t address, uint32_t base) {
        const std::string name = memory_.read_cstring(address, base);
        memory_.write_cstring(
            address, "Uninstaller for " + name + " is executing.", base);
    }
    bool convert_cp932_to_utf8(uint32_t destination, uint32_t source,
                               uint32_t base) {
        const std::string input = memory_.read_cstring(source, base);
        try {
            const std::vector<uint32_t> code_points =
                decode_text_code_points(input, TextEncoding::ShiftJis);
            std::string output;
            for (const uint32_t code_point : code_points) {
                output += encode_code_point(
                    code_point, TextEncoding::Utf8);
            }
            if (destination != 0) {
                memory_.write_cstring(destination, output, base);
            }
            return true;
        } catch (const Error&) {
            return false;
        }
    }
    uint32_t create_program_thread(std::string name, std::vector<uint8_t> code,
                                   uint32_t operand_slots, uint32_t code_bytes,
                                   uint32_t data_bytes) {
        const uint32_t handle = ++next_thread_handle_;
        ProgramThreadState child;
        child.thread.name = std::move(name);
        child.thread.id = handle;
        const uint32_t tail_id = root_children_.empty()
            ? 1U : root_children_.back();
        const Thread* tail = find_thread_object(tail_id);
        child.thread.parent_id = tail != nullptr && tail->parent_id != 0
            ? tail->parent_id : tail_id;
        child.thread.state = ThreadState::Running;
        child.thread.stack = OperandStack(operand_slots);
        child.thread.code_capacity = code_bytes;
        child.thread.data_capacity = data_bytes;
        child.code = std::move(code);
        child.frame.resize(
            static_cast<std::size_t>(
                std::max<uint32_t>(data_bytes, 0x4000U)),
            0);
        child.operand_slots = operand_slots;
        child.code_bytes = code_bytes;
        child.data_bytes = data_bytes;
        // 0044BCB0 follows +0x0C to the tail and appends there. Ownership
        // is owner_or_self(tail), independently of the requesting thread.
        Thread* owner = find_thread_object(child.thread.parent_id);
        if (owner == nullptr) {
            throw BpFault("program thread owner is missing");
        }
        owner->children.push_back(handle);
        root_children_.push_back(handle);
        program_threads_[handle] = std::move(child);
        return handle;
    }
    static std::optional<uint32_t> allocate_thread_range(
        const std::vector<ThreadRange>& allocations,
        uint32_t capacity, uint32_t size) {
        if (size == 0) {
            return capacity;
        }
        if (size > capacity) {
            return std::nullopt;
        }
        std::vector<ThreadRange> ordered = allocations;
        std::sort(
            ordered.begin(), ordered.end(),
            [](const ThreadRange& left, const ThreadRange& right) {
                return left.offset > right.offset;
            });
        uint32_t cursor = capacity;
        for (const ThreadRange& range : ordered) {
            const uint64_t end =
                static_cast<uint64_t>(range.offset) + range.size;
            if (end > capacity || range.offset > cursor) {
                return std::nullopt;
            }
            if (cursor > end &&
                cursor - static_cast<uint32_t>(end) >= size) {
                return cursor - size;
            }
            cursor = range.offset;
        }
        return cursor >= size
                   ? std::optional<uint32_t>(cursor - size)
                   : std::nullopt;
    }
    static void release_thread_range(
        std::vector<ThreadRange>& allocations,
        uint32_t offset, uint32_t size) {
        const auto allocation = std::find_if(
            allocations.begin(), allocations.end(),
            [offset, size](const ThreadRange& range) {
                return range.offset == offset && range.size == size;
            });
        if (allocation != allocations.end()) allocations.erase(allocation);
    }
    SharedProgramThreadResult create_shared_program_thread(
        uint32_t operand_slots, uint32_t code_bytes, uint32_t data_bytes,
        uint32_t entry_ip) {
        return create_shared_program_thread_from(
            thread_id_, operand_slots, code_bytes, data_bytes, entry_ip);
    }
    SharedProgramThreadResult create_shared_program_thread_from(
        uint32_t parent_id, uint32_t operand_slots, uint32_t code_bytes,
        uint32_t data_bytes, uint32_t entry_ip) {
        Thread* caller = find_thread_object(parent_id);
        if (caller == nullptr) {
            return {2, 0};
        }
        uint32_t allocator_id = parent_id;
        for (;;) {
            const auto borrower = program_threads_.find(allocator_id);
            if (borrower == program_threads_.end() ||
                borrower->second.allocation_owner_id == 0) break;
            // DCTChildThread vtable+24 (0045B500) delegates to +0x90's
            // source allocator; nested reservations use that original arena.
            allocator_id = borrower->second.allocation_owner_id;
        }
        Thread* parent = find_thread_object(allocator_id);
        if (parent == nullptr) return {2, 0};
        const std::span<const uint8_t> current_code =
            memory_.active_code().empty()
                ? memory_.code()
                : memory_.active_code();
        const std::optional<uint32_t> code_offset =
            allocate_thread_range(
                parent->code_allocations, parent->code_capacity, code_bytes);
        const auto allocator_record = program_threads_.find(allocator_id);
        const uint32_t loaded_code_bytes = allocator_id == 2U
            ? static_cast<uint32_t>(main_code_.size())
            : (allocator_record == program_threads_.end() ? 0U
                : static_cast<uint32_t>(allocator_record->second.code.size()));
        if (!code_offset || *code_offset < loaded_code_bytes) {
            return {2, 0};
        }
        // DCTChildThread::vtable+4 (0045B460) delegates to its source
        // CThread's capacity bound (0044BEA0), not the active module length.
        const uint32_t source_base = allocator_record == program_threads_.end()
            ? 0U : allocator_record->second.code_offset;
        if (static_cast<uint64_t>(source_base) + parent->code_capacity <= entry_ip) {
            return {0xFFFFFFFFU, 0};
        }
        const std::optional<uint32_t> data_offset =
            allocate_thread_range(
                parent->data_allocations, parent->data_capacity, data_bytes);
        if (!data_offset) {
            return {3, 0};
        }

        const uint32_t handle = ++next_thread_handle_;
        ProgramThreadState child;
        child.thread.name = "child";
        child.thread.id = handle;
        child.thread.parent_id = caller->parent_id != 0
            ? caller->parent_id : parent_id;
        child.thread.state = ThreadState::Running;
        child.thread.ip = entry_ip;
        child.thread.stack = OperandStack(operand_slots);
        child.thread.code_capacity = code_bytes;
        child.thread.data_capacity = data_bytes;
        child.code.assign(current_code.begin(), current_code.end());
        child.frame.resize(
            static_cast<std::size_t>(
                std::max<uint32_t>(data_bytes, 0x4000U)),
            0);
        child.operand_slots = operand_slots;
        child.code_bytes = code_bytes;
        child.data_bytes = data_bytes;
        child.code_offset = *code_offset;
        child.data_offset = *data_offset;
        child.thread.data_base = *data_offset;
        child.thread.base = *data_offset;
        child.reserved_code_bytes = code_bytes;
        child.reserved_data_bytes = data_bytes;
        child.allocation_owner_id = allocator_id;
        child.has_code_resource = has_active_code_resource_;
        child.code_resource_base = active_code_resource_base_;
        parent->code_allocations.push_back({*code_offset, code_bytes});
        parent->data_allocations.push_back({*data_offset, data_bytes});
        ++parent->scheduler_marker; // CThread+0x68 shared-allocation count.
        Thread* owner = find_thread_object(child.thread.parent_id);
        if (owner == nullptr) {
            throw BpFault("shared program thread owner is missing");
        }
        owner->children.push_back(handle);
        root_children_.push_back(handle);
        program_threads_[handle] = std::move(child);
        return {0, handle};
    }
    [[nodiscard]] const std::map<uint32_t, ProgramThreadState>&
    program_threads() const {
        return program_threads_;
    }
    [[nodiscard]] std::map<uint32_t, ProgramThreadState>&
    program_threads() {
        return program_threads_;
    }
    void enqueue_system_record(uint32_t second, uint32_t first) {
        // Both PE32 4A2F40 and PE64 FBEA0 append through the tail pointer.
        system_records_.push_back({0, second, first});
        ++system_records_enqueued_;
    }
    void enqueue_system_event_record(uint32_t type, uint32_t value,
                                     uint32_t owner = 0) {
        system_records_.push_back({type, value, owner});
        ++system_records_enqueued_;
    }
    void enqueue_system_pointer_record(int32_t x, int32_t y,
                                       bool button, uint32_t owner = 0) {
        const uint32_t packed =
            (static_cast<uint32_t>(y) & 0xFFFU) << 12U |
            (static_cast<uint32_t>(x) & 0xFFFU) |
            (button ? 0x01000000U : 0U);
        enqueue_system_event_record(0x101U, packed, owner);
    }
    bool dequeue_system_record(uint32_t address, uint32_t base) {
        if (system_records_.empty() || address == 0) {
            return false;
        }
        const auto record = system_records_.front();
        system_records_.pop_front();
        ++system_records_dequeued_;
        memory_.write_u32(address, base, record[0]);
        memory_.write_u32(address + 4, base, record[1]);
        memory_.write_u32(address + 8, base, record[2]);
        return true;
    }
    [[nodiscard]] std::size_t system_record_count() const {
        return system_records_.size();
    }
    [[nodiscard]] uint64_t system_records_enqueued() const {
        return system_records_enqueued_;
    }
    [[nodiscard]] uint64_t system_records_dequeued() const {
        return system_records_dequeued_;
    }
    [[nodiscard]] const std::map<uint32_t, StreamState>& streams() const {
        return streams_;
    }
    uint32_t close_file_stream(uint32_t id, uint32_t status_address,
                               uint32_t base) {
        const auto found = streams_.find(id);
        if (found == streams_.end()) {
            if (status_address != 0) {
                memory_.write_u32(status_address, base, 4);
            }
            if (file_stream_trace_) {
                file_stream_trace_("close", {}, id, 4);
            }
            return 4;
        }
        const std::string closed_path = found->second.path;
        if (found->second.mode != 0 && file_writer_ &&
            !file_writer_(closed_path, found->second.data)) {
            if (status_address != 0) {
                memory_.write_u32(status_address, base, 0);
            }
            if (file_stream_trace_) {
                file_stream_trace_("close", closed_path, id, 1);
            }
            return 1;
        }
        virtual_files_[stream_path_key(closed_path)] =
            found->second.data;
        streams_.erase(found);
        if (status_address != 0) {
            memory_.write_u32(status_address, base, 1);
        }
        if (file_stream_trace_) {
            file_stream_trace_("close", closed_path, id, 0);
        }
        return 0;
    }
    uint32_t process_file_stream(uint32_t id, uint32_t length,
                                 uint32_t destination,
                                 uint32_t status_address, uint32_t base) {
        const auto found = streams_.find(id);
        if (found == streams_.end()) {
            if (status_address != 0) {
                memory_.write_u32(status_address, base, 4);
            }
            if (file_stream_trace_) {
                file_stream_trace_("io", {}, id, 4);
            }
            return 4;
        }
        StreamState& stream = found->second;
        uint32_t actual = 0;
        if (stream.mode == 0) {
            const std::size_t remaining =
                stream.position < stream.data.size()
                    ? stream.data.size() - stream.position
                    : 0;
            actual = static_cast<uint32_t>(
                remaining < length ? remaining : length);
            for (uint32_t index = 0; index < actual; ++index) {
                memory_.write_u8(
                    destination + index, base,
                    stream.data[stream.position + index]);
            }
            stream.position += actual;
        } else {
            actual = length;
            const std::size_t end =
                stream.position + static_cast<std::size_t>(actual);
            if (stream.data.size() < end) {
                stream.data.resize(end, 0);
            }
            for (uint32_t index = 0; index < actual; ++index) {
                stream.data[stream.position + index] =
                    memory_.read_u8(destination + index, base);
            }
            stream.position = end;
            virtual_files_[stream_path_key(stream.path)] = stream.data;
        }
        if (status_address != 0) {
            memory_.write_u32(
                status_address, base,
                actual == 0 ? 0xFFFFFFFFU : actual);
        }
        if (file_stream_trace_) {
            file_stream_trace_("io", stream.path, id, actual);
        }
        return 0;
    }
    uint32_t seek_file_stream(uint32_t id, int32_t offset,
                              uint32_t status_address, uint32_t base) {
        const auto found = streams_.find(id);
        if (found == streams_.end()) {
            if (status_address != 0) {
                memory_.write_u32(status_address, base, 4);
            }
            if (file_stream_trace_) {
                file_stream_trace_("seek", {}, id, 4);
            }
            return 4;
        }
        StreamState& stream = found->second;
        const int64_t requested =
            offset < 0
                ? static_cast<int64_t>(stream.data.size()) + offset + 1
                : offset;
        stream.position = static_cast<std::size_t>(
            requested < 0
                ? 0
                : (static_cast<uint64_t>(requested) > stream.data.size()
                       ? stream.data.size()
                       : static_cast<std::size_t>(requested)));
        if (status_address != 0) {
            memory_.write_u32(status_address, base, 1);
        }
        if (file_stream_trace_) {
            file_stream_trace_(
                "seek", stream.path, id,
                static_cast<int64_t>(stream.position));
        }
        return 0;
    }

    StepResult step();
    StepResult run(std::size_t instruction_limit, std::size_t* executed = nullptr);
    bool service_scheduler_pass() {
        if (engine_exit_requested_) return false;
        return run_scheduled_children();
    }
    [[nodiscard]] bool has_live_scheduled_threads() const {
        for (const uint32_t id : root_children_) {
            const Thread* scheduled = find_thread_object(id);
            if (scheduled != nullptr &&
                scheduled->state != ThreadState::Done &&
                scheduled->state != ThreadState::Faulted) {
                return true;
            }
        }
        return false;
    }

    // Original global 0x0052A2F0. Zero enables the system Close command;
    // nonzero disables it. The host must mirror this in its window menu.
    void set_system_close_value(uint32_t value) {
        system_close_value_ = value;
    }
    [[nodiscard]] uint32_t system_close_value() const {
        return system_close_value_;
    }
    void set_shutdown_strings(
        std::optional<std::string> path_542cb0,
        std::string path_542c38,
        std::optional<std::string> path_542cb4) {
        shutdown_path_542cb0_ = std::move(path_542cb0);
        shutdown_path_542c38_ = std::move(path_542c38);
        shutdown_path_542cb4_ = std::move(path_542cb4);
    }
    [[nodiscard]] const std::optional<std::string>&
    shutdown_path_542cb0() const { return shutdown_path_542cb0_; }
    [[nodiscard]] const std::string&
    shutdown_path_542c38() const { return shutdown_path_542c38_; }
    [[nodiscard]] const std::optional<std::string>&
    shutdown_path_542cb4() const { return shutdown_path_542cb4_; }
    void set_launcher_mode(uint32_t value) { launcher_mode_ = value; }
    [[nodiscard]] uint32_t launcher_mode() const { return launcher_mode_; }

    // Original global 0x0052A65C: Sys0:0x52 stores one raw value here and
    // the original main loop reads it back unchanged.
    void set_sys0_52_value(uint32_t value) { sys0_52_value_ = value; }
    [[nodiscard]] uint32_t sys0_52_value() const { return sys0_52_value_; }
    void set_sys0_state_542c54(uint32_t value) {
        sys0_state_542c54_ = value;
    }
    [[nodiscard]] uint32_t sys0_state_542c54() const {
        return sys0_state_542c54_;
    }
    void set_sys0_state_542c58(uint32_t value) {
        sys0_state_542c58_ = value;
    }
    [[nodiscard]] uint32_t sys0_state_542c58() const {
        return sys0_state_542c58_;
    }
    void set_sys0_state_54092c(uint32_t value) {
        sys0_state_54092c_ = value;
    }
    [[nodiscard]] uint32_t sys0_state_54092c() const {
        return sys0_state_54092c_;
    }
    void set_sys0_counter_542e90(uint32_t value) {
        sys0_counter_542e90_ = value;
    }
    [[nodiscard]] uint32_t sys0_counter_542e90() const {
        return sys0_counter_542e90_;
    }
    void set_sys0_state_block_542c60(std::size_t index, uint32_t value) {
        if (index < sys0_state_block_542c60_.size()) {
            sys0_state_block_542c60_[index] = value;
        }
    }
    [[nodiscard]] const std::array<uint32_t, 16>&
    sys0_state_block_542c60() const {
        return sys0_state_block_542c60_;
    }
    void set_sys0_input_global_52a5f8(uint32_t value) {
        sys0_input_global_52a5f8_ = value;
    }
    [[nodiscard]] uint32_t sys0_input_global_52a5f8() const {
        return sys0_input_global_52a5f8_;
    }
    void set_sys0_input_global_52a5ac(uint32_t value) {
        sys0_input_global_52a5ac_ = value;
    }
    [[nodiscard]] uint32_t sys0_input_global_52a5ac() const {
        return sys0_input_global_52a5ac_;
    }
    void set_sys0_input_global_5446a4(uint32_t value) {
        sys0_input_global_5446a4_ = value;
    }
    [[nodiscard]] uint32_t sys0_input_global_5446a4() const {
        return sys0_input_global_5446a4_;
    }
    void set_sys0_ring_head_545550(uint32_t value) {
        sys0_ring_head_545550_ = value < sys0_ring_544bf0_.size()
                                     ? value
                                     : static_cast<uint32_t>(
                                           sys0_ring_544bf0_.size() - 1U);
    }
    [[nodiscard]] uint32_t sys0_ring_head_545550() const {
        return sys0_ring_head_545550_;
    }
    void set_sys0_ring_value_544bf0(std::size_t index, uint32_t value) {
        if (index < sys0_ring_544bf0_.size()) {
            sys0_ring_544bf0_[index] = value;
        }
    }
    [[nodiscard]] uint32_t sys0_ring_value_544bf0(std::size_t index) const {
        return index < sys0_ring_544bf0_.size() ? sys0_ring_544bf0_[index]
                                                : 0U;
    }
    void set_sys0_state_540948(uint32_t value) {
        sys0_state_540948_ = value;
    }
    [[nodiscard]] uint32_t sys0_state_540948() const {
        return sys0_state_540948_;
    }
    void set_sys0_control_544be8(uint32_t value) {
        sys0_control_544be8_ = value;
    }
    [[nodiscard]] uint32_t sys0_control_544be8() const {
        return sys0_control_544be8_;
    }
    void set_sys0_control_529bb0(uint32_t value) {
        sys0_control_529bb0_ = value;
    }
    [[nodiscard]] uint32_t sys0_control_529bb0() const {
        return sys0_control_529bb0_;
    }
    void set_sys0_table_542bd0(std::size_t index, uint32_t value) {
        if (index < sys0_table_542bd0_.size()) {
            sys0_table_542bd0_[index] = value;
        }
    }
    [[nodiscard]] uint32_t sys0_table_542bd0(std::size_t index) const {
        return index < sys0_table_542bd0_.size() ? sys0_table_542bd0_[index]
                                                 : 0U;
    }
    void clear_sys0_table_542bd0() {
        sys0_table_542bd0_.fill(0);
    }
    void set_sys0_process_count_542e00(uint32_t value) {
        sys0_process_count_542e00_ = value;
    }
    [[nodiscard]] uint32_t sys0_process_count_542e00() const {
        return sys0_process_count_542e00_;
    }
    bool set_sys0_input_state_589a30(uint32_t value) {
        if (value > 1U) {
            return false;
        }
        sys0_input_state_589a30_ = value;
        return true;
    }
    [[nodiscard]] uint32_t sys0_input_state_589a30() const {
        return sys0_input_state_589a30_;
    }
    [[nodiscard]] uint32_t current_thread_id() const { return thread_id_; }
    [[nodiscard]] uint32_t current_opcode_ip() const {
        return current_opcode_ip_;
    }
    [[nodiscard]] uint8_t current_native_sub() const {
        return current_native_sub_;
    }
    bool has_thread_object(uint32_t id) const {
        return id == 1U || std::find(root_children_.begin(), root_children_.end(), id)
            != root_children_.end();
    }
    bool enqueue_thread_command(uint32_t id, uint32_t value) {
        if (!has_thread_object(id)) return false;
        Thread* target = find_thread_object(id);
        if (target == nullptr) {
            return false;
        }
        target->command_queue.push_back(value);
        return true;
    }
    bool enqueue_thread_triple(
        uint32_t id, uint32_t first, uint32_t second,
        uint32_t third) {
        if (!has_thread_object(id)) return false;
        Thread* target = find_thread_object(id);
        if (target == nullptr) {
            return false;
        }
        // CThread+5C (PE32) / +A0 (PE64) owns this message FIFO.
        // A live thread without a current wait object cannot receive it.
        if (!target->wait_procedure && !target->wait_for_timing &&
            !target->wait_for_input) return false;
        target->triple_command_queue.push_back(
            {first, second, third});
        return true;
    }
    bool dequeue_thread_command(uint32_t address, uint32_t base) {
        Thread& current = current_execution_thread();
        if (current.command_queue.empty()) {
            return false;
        }
        if (address != 0) {
            memory_.write_u32(address, base,
                              current.command_queue.front());
        }
        current.command_queue.pop_front();
        return true;
    }
    void set_exclusive_thread(uint32_t id) { exclusive_thread_id_ = id; }
    [[nodiscard]] D0Subsystem& d0() { return d0_; }
    [[nodiscard]] HostTextEdit& host_text_edit() { return host_text_edit_; }
    [[nodiscard]] HostChildWindows& host_child_windows() {
        return host_child_windows_;
    }
    [[nodiscard]] const HostChildWindows& host_child_windows() const {
        return host_child_windows_;
    }
    struct ChildWindowBitmapCommand {
        uint32_t window = 0;
        uint32_t bitmap = 0;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mode = 0;
        uint32_t opacity = 0;
    };
    uint32_t draw_child_window_bitmap(uint32_t window, int32_t x,
                                      int32_t y, uint32_t bitmap,
                                      uint32_t mode,
                                      uint32_t opacity) {
        if (!host_child_windows_.position(window).has_value()) {
            return 0xFF;
        }
        const auto surface = graphics_surfaces_.find(bitmap);
        if (surface == graphics_surfaces_.end()) {
            return 3;
        }
        child_window_bitmap_commands_.push_back({
            window,
            bitmap,
            x,
            y,
            surface->second.width,
            surface->second.height,
            mode,
            opacity,
        });
        display_dirty_ = true;
        return 0;
    }
    void clear_child_window_bitmap_commands(uint32_t window) {
        std::erase_if(
            child_window_bitmap_commands_,
            [window](const ChildWindowBitmapCommand& command) {
                return command.window == window;
            });
    }
    [[nodiscard]] const std::vector<ChildWindowBitmapCommand>&
    child_window_bitmap_commands() const {
        return child_window_bitmap_commands_;
    }
    struct ChildWindowTextCommand {
        uint32_t window = 0;
        int32_t x = 0;
        int32_t y = 0;
        std::string text;
        uint32_t font_id = 0;
        std::string font_name;
        uint32_t font_size = 0;
        uint32_t font_width = 0;
        uint32_t font_height = 0;
        uint32_t draw_mode = 0;
        uint32_t wrap = 0;
        uint32_t surface_width = 0;
        uint32_t surface_height = 0;
        uint32_t result = 0;
        bool result_measured = false;
    };
    using ChildWindowTextProvider = std::function<
        std::optional<uint32_t>(const ChildWindowTextCommand&)>;
    void set_child_window_text_provider(
        ChildWindowTextProvider provider) {
        child_window_text_provider_ = std::move(provider);
    }
    uint32_t draw_child_window_text(
        uint32_t window, int32_t x, int32_t y, std::string text,
        uint32_t font_id, uint32_t font_size, uint32_t font_width,
        uint32_t font_height, uint32_t draw_mode, uint32_t wrap,
        uint32_t& result) {
        const auto size = host_child_windows_.size(window);
        if (!size) {
            return 0xFFFFFFFFU;
        }
        const std::string name = font_name(font_id);
        if (name.empty()) {
            return 0x8000000BU;
        }
        ChildWindowTextCommand command;
        command.window = window;
        command.x = x;
        command.y = y;
        command.text = std::move(text);
        command.font_id = font_id;
        command.font_name = name;
        command.font_size = font_size;
        command.font_width = font_width;
        command.font_height = font_height;
        command.draw_mode = draw_mode;
        command.wrap = wrap;
        command.surface_width = size->first;
        command.surface_height = size->second;
        if (child_window_text_provider_) {
            const std::optional<uint32_t> measured =
                child_window_text_provider_(command);
            if (measured) {
                command.result = *measured;
                command.result_measured = true;
            }
        }
        result = command.result;
        child_window_text_commands_.push_back(std::move(command));
        display_dirty_ = true;
        return 0;
    }
    void clear_child_window_text_commands(uint32_t window) {
        std::erase_if(
            child_window_text_commands_,
            [window](const ChildWindowTextCommand& command) {
                return command.window == window;
            });
    }
    [[nodiscard]] const std::vector<ChildWindowTextCommand>&
    child_window_text_commands() const {
        return child_window_text_commands_;
    }
    [[nodiscard]] HostDialogs& host_dialogs() { return host_dialogs_; }
    void set_switch_target(uint32_t id) { switch_target_id_ = id; }

    // Two original 16-entry arrays indexed by display mode. The window
    // creation path reads index 2 as its default client width and height.
    void set_window_mode_size(std::size_t index, uint32_t width,
                              uint32_t height) {
        if (window_mode_applier_ && !host_window_changes_supported() &&
            index == window_mode_index_) return;
        window_widths_.at(index) = width;
        window_heights_.at(index) = height;
        if (index == window_mode_index_) {
            refresh_main_background_geometry();
        }
    }
    [[nodiscard]] uint32_t window_width(std::size_t index) const {
        return window_widths_.at(index);
    }
    [[nodiscard]] uint32_t window_height(std::size_t index) const {
        return window_heights_.at(index);
    }
    bool apply_window_mode(uint32_t fullscreen, uint32_t flag,
                           uint32_t index) {
        if (!host_window_mode_change_allowed(host_window_platform(),
                bool(window_mode_applier_), fullscreen, flag, index,
                window_fullscreen_, window_mode_flag_, window_mode_index_)) return false;
        if (window_mode_in_progress_ || index >= window_widths_.size()) {
            return false;
        }
        window_mode_in_progress_ = true;
        struct ResetProgress {
            bool& flag;
            ~ResetProgress() { flag = false; }
        } reset{window_mode_in_progress_};
        WindowModeRequest request = make_window_mode_request(fullscreen, flag, index);
        bool device_available = !window_mode_applier_ || window_mode_applier_(request);
        if (!device_available && fullscreen != 0) {
            fullscreen = 0;
            request.fullscreen = 0;
            device_available = !window_mode_applier_ || window_mode_applier_(request);
        }
        if (!device_available) {
            return false;
        }
        window_mode_flag_ = flag;
        window_mode_index_ = index;
        refresh_main_background_geometry();
        window_fullscreen_ = fullscreen;
        set_window_sizing_limit(0,0);
        window_display_dirty_ = true;
        ++window_mode_revision_;
        return true;
    }
    bool select_window_mode(uint32_t fullscreen, uint32_t flag,
                            uint32_t index) {
        return apply_window_mode(fullscreen, flag, index);
    }
    void reapply_current_window_mode() {
        apply_window_mode(
            window_fullscreen_, window_mode_flag_, window_mode_index_);
    }
    [[nodiscard]] uint32_t window_fullscreen() const {
        return window_fullscreen_;
    }
    [[nodiscard]] uint32_t window_mode_flag() const {
        return window_mode_flag_;
    }
    [[nodiscard]] uint32_t window_mode_index() const {
        return window_mode_index_;
    }
    void set_client_size(uint32_t width, uint32_t height) {
        if (!host_client_resize_allowed(host_window_platform(), bool(window_mode_applier_))) return;
        if (width == 0 || height == 0) {
            width = window_width(window_mode_index_);
            height = window_height(window_mode_index_);
            window_use_mode_size_ = true;
        } else {
            window_use_mode_size_ = false;
        }
        client_width_ = width;
        client_height_ = height;
        if (window_fullscreen_ == 0) {
            apply_window_mode(
                0, window_mode_flag_, window_mode_index_);
        }
    }
    [[nodiscard]] uint32_t client_width() const { return client_width_; }
    [[nodiscard]] uint32_t client_height() const { return client_height_; }
    [[nodiscard]] bool window_use_mode_size() const {
        return window_use_mode_size_;
    }
    [[nodiscard]] std::pair<uint32_t, uint32_t>
    effective_client_dimensions() const {
        if (window_use_mode_size_ ||
            client_width_ == 0 || client_height_ == 0) {
            return {
                window_width(window_mode_index_),
                window_height(window_mode_index_),
            };
        }
        return {client_width_, client_height_};
    }
    [[nodiscard]] std::pair<uint32_t, uint32_t>
    current_mode_dimensions() const {
        return {
            window_width(window_mode_index_),
            window_height(window_mode_index_),
        };
    }
    void set_raster_wait_enabled(uint32_t enabled) {
        raster_wait_enabled_ = enabled;
    }
    [[nodiscard]] uint32_t raster_wait_enabled() const {
        return raster_wait_enabled_;
    }
    [[nodiscard]] uint64_t window_mode_revision() const {
        return window_mode_revision_;
    }
    [[nodiscard]] bool window_display_dirty() const {
        return window_display_dirty_;
    }
    void clear_window_display_dirty() { window_display_dirty_ = false; }
    bool set_window_sizing_limit(int32_t width,int32_t height);
    void set_window_sizing_limit_handler(std::function<void(uint32_t,uint32_t)> handler) {
        window_sizing_limit_handler_=std::move(handler);
        if(window_sizing_limit_handler_ && sizing_limit_width_ && sizing_limit_height_)
            window_sizing_limit_handler_(sizing_limit_width_,sizing_limit_height_);
    }
    [[nodiscard]] std::pair<uint32_t,uint32_t> window_sizing_limit() const {
        return {sizing_limit_width_,sizing_limit_height_};
    }
    void set_window_resizable_handler(std::function<void(bool)> handler) {
        window_resizable_handler_ = std::move(handler);
    }
    void set_window_resizable(uint32_t value) {
        window_resizable_ = value;
        if (!window_fullscreen_ && window_resizable_handler_)
            window_resizable_handler_(value != 0);
    }
    [[nodiscard]] uint32_t window_resizable() const { return window_resizable_; }
    void set_window_title(std::string title) {
        window_title_ = std::move(title);
    }
    [[nodiscard]] const std::string& window_title() const {
        return window_title_;
    }
    void wake_for_timing(uint64_t now_ms) {
        if (thread_.state == ThreadState::Waiting &&
            thread_.wait_for_timing && thread_.deadline_ms <= now_ms) {
            thread_.state = ThreadState::Running;
            thread_.wait_for_timing = false;
            thread_.wait_for_input = false;
            thread_.status_flags &= ~1U;
        }
        for (auto& [handle, child] : program_threads_) {
            (void)handle;
            if (child.thread.state == ThreadState::Waiting &&
                child.thread.wait_for_timing &&
                child.thread.deadline_ms <= now_ms) {
                child.thread.state = ThreadState::Running;
                child.thread.wait_for_timing = false;
                child.thread.wait_for_input = false;
                child.thread.status_flags &= ~1U;
            }
        }
    }
    void wake_for_input() {
        if (thread_.state == ThreadState::Waiting &&
            thread_.wait_for_input) {
            thread_.state = ThreadState::Running;
            thread_.wait_for_input = false;
            thread_.wait_for_timing = false;
            thread_.status_flags &= ~1U;
        }
        for (auto& [handle, child] : program_threads_) {
            (void)handle;
            if (child.thread.state == ThreadState::Waiting &&
                child.thread.wait_for_input) {
                child.thread.state = ThreadState::Running;
                child.thread.wait_for_input = false;
                child.thread.wait_for_timing = false;
                child.thread.status_flags &= ~1U;
            }
        }
    }
    void set_window_position(int32_t x, int32_t y) {
        window_x_ = x;
        window_y_ = y;
    }
    void request_window_reposition() {
        if (window_reposition_handler_) {
            window_reposition_handler_(window_x_, window_y_);
        }
    }
    [[nodiscard]] bool window_position_fits_work_areas(
        int32_t x, int32_t y) const {
        uint32_t width = client_width_;
        uint32_t height = client_height_;
        if (width == 0) {
            width = window_widths_[window_mode_index_];
        }
        if (height == 0) {
            height = window_heights_[window_mode_index_];
        }
        const int32_t margin_x = static_cast<int32_t>(width >> 3U);
        const int32_t margin_y = static_cast<int32_t>(height >> 3U);
        const auto areas = window_work_area_enumerator_
                               ? window_work_area_enumerator_()
                               : std::vector<WindowWorkArea>{};
        if (areas.empty()) {
            return true;
        }
        for (const WindowWorkArea& area : areas) {
            const int64_t min_x =
                static_cast<int64_t>(area.left) - width + margin_x;
            const int64_t max_x =
                static_cast<int64_t>(area.right) - margin_x;
            const int64_t min_y =
                static_cast<int64_t>(area.top) - height + margin_y;
            const int64_t max_y =
                static_cast<int64_t>(area.bottom) - margin_y;
            if (x >= min_x && x <= max_x &&
                y >= min_y && y <= max_y) {
                return true;
            }
        }
        return false;
    }
    bool try_set_window_position(int32_t x, int32_t y) {
        if (window_minimized_ ||
            !window_position_fits_work_areas(x, y)) {
            return false;
        }
        set_window_position(x, y);
        return true;
    }
    [[nodiscard]] int32_t window_x() const { return window_x_; }
    [[nodiscard]] int32_t window_y() const { return window_y_; }

    // Original global 0x00529C1C. Sys1:0x62 accepts only 0 or 1.
    void set_sys1_62_value(uint32_t value) { sys1_62_value_ = value; }
    [[nodiscard]] uint32_t sys1_62_value() const { return sys1_62_value_; }

    // Original global 0x00540444. The display path accepts modes 0, 1 and 2.
    void set_display_mode(uint32_t value) { display_mode_ = value; }
    [[nodiscard]] uint32_t display_mode() const { return display_mode_; }
    void set_display_available_size(uint32_t width, uint32_t height) {
        display_available_width_ = width;
        display_available_height_ = height;
    }
    [[nodiscard]] std::pair<uint32_t, uint32_t>
    normalized_display_size() const {
        if (display_available_height_ == 0 ||
            window_mode_index_ >= window_widths_.size()) {
            throw BpFault("invalid display capability dimensions");
        }
        uint32_t available_width = display_available_width_;
        if (9U < (available_width * 4U) / display_available_height_) {
            available_width >>= 1;
        }
        if (available_width == 0) {
            throw BpFault("invalid normalized display width");
        }
        uint32_t available_height = display_available_height_;
        if (available_height / available_width != 0 &&
            (available_height * 100U) / available_width < 125U) {
            available_height >>= 1;
        }
        return {available_width, available_height};
    }
    [[nodiscard]] uint32_t effective_display_mode() const {
        if (display_mode_ != 2U) {
            return display_mode_;
        }
        const auto [available_width, available_height] =
            normalized_display_size();
        if (available_width < window_widths_[window_mode_index_] ||
            available_height < window_heights_[window_mode_index_]) {
            return 0;
        }
        return 2;
    }
    [[nodiscard]] bool display_aspect_changed() const {
        const auto [available_width, available_height] =
            normalized_display_size();
        const double available_aspect =
            (static_cast<double>(available_width) /
             static_cast<double>(available_height)) *
            8192.0;
        const double mode_aspect =
            (static_cast<double>(window_widths_[window_mode_index_]) /
             static_cast<double>(window_heights_[window_mode_index_])) *
            8192.0;
        return static_cast<int32_t>(available_aspect) !=
               static_cast<int32_t>(mode_aspect);
    }
    void set_shader_capability(bool available) {
        shader_capability_ = available;
    }
    [[nodiscard]] bool shader_capability() const {
        return shader_capability_;
    }
    [[nodiscard]] uint32_t upscale_mode() const { return upscale_mode_; }
    bool set_upscale_mode(uint32_t mode) {
        if (mode > 4U ||
            ((mode == 1U || mode == 4U) && !shader_capability_)) {
            return false;
        }
        upscale_mode_ = mode;
        return true;
    }

    // Main-background fields +0x40/+0x44 are the script-visible pointer
    // coordinates. 0x00449F10 stores both and dirties the display manager;
    // 0x00449F30 returns them only when they lie inside the main surface.
    void set_display_pair(uint32_t first, uint32_t second) {
        display_pair_first_ = first;
        display_pair_second_ = second;
        const auto [width, height] = current_mode_dimensions();
        const bool inside =
            static_cast<int32_t>(first) >= 0 &&
            static_cast<int32_t>(second) >= 0 &&
            first < width && second < height;
        set_pointer_position(
            static_cast<int32_t>(first),
            static_cast<int32_t>(second), inside);
        display_dirty_ = true;
    }
    // Grp1:06 writes 530E88/530E8C (PE64 1D8BA0/1D8BA4).
    // Base virtual-position getter contributes this pair only with raw+4C set.
    void set_display_transform_origin(int32_t x, int32_t y) {
        display_transform_origin_x_ = x;
        display_transform_origin_y_ = y;
        for (const auto& [handle, sprite] : sprites_) {
            (void)sprite;
            refresh_sprite_display_geometry(handle);
        }
        for (const auto& [handle, window] : window_objects_) {
            (void)window;
            refresh_window_display_geometry(handle);
        }
        display_dirty_ = true;
    }
    [[nodiscard]] uint32_t display_pair_first() const {
        return display_pair_first_;
    }
    [[nodiscard]] uint32_t display_pair_second() const {
        return display_pair_second_;
    }
    [[nodiscard]] bool display_dirty() const { return display_dirty_; }
    void set_display_dirty() { display_dirty_ = true; }
    void clear_display_dirty() { display_dirty_ = false; }
    void set_grp0_render_global_529c38(uint32_t value) {
        grp0_render_global_529c38_ = value;
    }
    [[nodiscard]] uint32_t grp0_render_global_529c38() const {
        return grp0_render_global_529c38_;
    }
    void set_grp0_render_pair_529ba8_530ff4(uint32_t first,
                                            uint32_t second) {
        grp0_render_global_529ba8_ = first;
        grp0_render_global_530ff4_ = second;
    }
    [[nodiscard]] uint32_t grp0_render_global_529ba8() const {
        return grp0_render_global_529ba8_;
    }
    [[nodiscard]] uint32_t grp0_render_global_530ff4() const {
        return grp0_render_global_530ff4_;
    }
    void set_grp0_render_global_530648(uint32_t value) {
        grp0_render_global_530648_ = value;
    }
    [[nodiscard]] uint32_t grp0_render_global_530648() const {
        return grp0_render_global_530648_;
    }
    void set_grp1_render_global_540a68(uint32_t value) {
        grp1_render_global_540a68_ = value;
    }
    [[nodiscard]] uint32_t grp1_render_global_540a68() const {
        return grp1_render_global_540a68_;
    }
    void set_grp1_render_global_52a658(uint32_t value) {
        grp1_render_global_52a658_ = value;
    }
    [[nodiscard]] uint32_t grp1_render_global_52a658() const {
        return grp1_render_global_52a658_;
    }
    bool set_grp1_bf_record(uint32_t index,
                            std::span<const uint32_t> values) {
        if (index < 4U || index > 7U || values.size() != 24U) {
            return false;
        }
        std::copy(values.begin(), values.end(),
                  grp1_bf_records_[index].begin());
        return true;
    }
    [[nodiscard]] const std::map<uint32_t, std::array<uint32_t, 24>>&
    grp1_bf_records() const {
        return grp1_bf_records_;
    }
    bool set_grp1_text_mode_530ea0(uint32_t value) {
        if (value > 1U) {
            return false;
        }
        grp1_text_mode_530ea0_ = value;
        return true;
    }
    [[nodiscard]] uint32_t grp1_text_mode_530ea0() const {
        return grp1_text_mode_530ea0_;
    }
    void set_grp0_render_global_53282c(uint32_t value) {
        grp0_render_global_53282c_ = value;
    }
    [[nodiscard]] uint32_t grp0_render_global_53282c() const {
        return grp0_render_global_53282c_;
    }
    void set_grp0_render_tuple_531004(uint32_t first, uint32_t second) {
        grp0_render_531004_ = first;
        grp0_render_531008_ = second;
    }
    [[nodiscard]] uint32_t grp0_render_tuple_531004() const {
        return grp0_render_531004_;
    }
    [[nodiscard]] uint32_t grp0_render_tuple_531008() const {
        return grp0_render_531008_;
    }
    void set_grp0_render_tuple_529c08(uint32_t first, uint32_t second) {
        grp0_render_529c08_ = first;
        grp0_render_529c0c_ = second;
    }
    [[nodiscard]] uint32_t grp0_render_tuple_529c08() const {
        return grp0_render_529c08_;
    }
    [[nodiscard]] uint32_t grp0_render_tuple_529c0c() const {
        return grp0_render_529c0c_;
    }
    void set_grp0_render_triplet_53100c(
        uint32_t first, uint32_t second, uint32_t third) {
        grp0_render_53100c_ = first;
        grp0_render_532820_ = second;
        grp0_render_532824_ = third;
    }
    [[nodiscard]] uint32_t grp0_render_triplet_53100c() const {
        return grp0_render_53100c_;
    }
    [[nodiscard]] uint32_t grp0_render_triplet_532820() const {
        return grp0_render_532820_;
    }
    [[nodiscard]] uint32_t grp0_render_triplet_532824() const {
        return grp0_render_532824_;
    }
    void set_grp0_render_quad_533228(
        uint32_t first, uint32_t second, uint32_t third, uint32_t fourth) {
        grp0_render_53322c_ = first;
        grp0_render_533230_ = second;
        grp0_render_533228_ = third;
        grp0_render_533238_ = fourth;
    }
    [[nodiscard]] uint32_t grp0_render_quad_53322c() const {
        return grp0_render_53322c_;
    }
    [[nodiscard]] uint32_t grp0_render_quad_533230() const {
        return grp0_render_533230_;
    }
    [[nodiscard]] uint32_t grp0_render_quad_533228() const {
        return grp0_render_533228_;
    }
    [[nodiscard]] uint32_t grp0_render_quad_533238() const {
        return grp0_render_533238_;
    }
    void set_grp0_render_global_529be0(uint32_t value) {
        grp0_render_global_529be0_ = value;
    }
    [[nodiscard]] uint32_t grp0_render_global_529be0() const {
        return grp0_render_global_529be0_;
    }
    void set_grp2_text_render_globals(
        uint32_t first, uint32_t second, uint32_t third) {
        grp2_text_render_global_533208_ = first;
        grp2_text_render_global_53320c_ = second;
        grp2_text_render_global_533214_ = third;
    }
    [[nodiscard]] uint32_t grp2_text_render_global_533208() const {
        return grp2_text_render_global_533208_;
    }
    [[nodiscard]] uint32_t grp2_text_render_global_53320c() const {
        return grp2_text_render_global_53320c_;
    }
    [[nodiscard]] uint32_t grp2_text_render_global_533214() const {
        return grp2_text_render_global_533214_;
    }
    void set_grp0_render_global_529bbc(uint32_t value) {
        grp0_render_global_529bbc_ = value;
    }
    [[nodiscard]] uint32_t grp0_render_global_529bbc() const {
        return grp0_render_global_529bbc_;
    }
    void set_grp0_render_global_529c10(uint32_t value) {
        grp0_render_global_529c10_ = value;
    }
    [[nodiscard]] uint32_t grp0_render_global_529c10() const {
        return grp0_render_global_529c10_;
    }
    void set_grp2_render_global_529be4(uint32_t value) {
        grp2_render_global_529be4_ = value;
    }
    [[nodiscard]] uint32_t grp2_render_global_529be4() const {
        return grp2_render_global_529be4_;
    }
    uint32_t exchange_grp0_render_global_529c3c(uint32_t value) {
        const uint32_t previous = grp0_render_global_529c3c_;
        grp0_render_global_529c3c_ = value;
        return previous;
    }
    [[nodiscard]] uint32_t grp0_render_global_529c3c() const {
        return grp0_render_global_529c3c_;
    }
    uint32_t set_grp0_render_config_532828(uint32_t mode,
                                            uint32_t value) {
        if (mode == 1U) {
            if (value >= 0x10000U) {
                return 0x80000005U;
            }
            grp0_render_global_532828_ = 1;
            grp0_render_global_53281c_ = value;
            return 0;
        }
        if (mode != 0U && mode != 2U) {
            return 0x80000004U;
        }
        grp0_render_global_532828_ = mode;
        return 0;
    }
    [[nodiscard]] uint32_t grp0_render_global_532828() const {
        return grp0_render_global_532828_;
    }
    [[nodiscard]] uint32_t grp0_render_global_53281c() const {
        return grp0_render_global_53281c_;
    }
    bool set_grp0_render_config_529bc0(uint32_t first,
                                        uint32_t second,
                                        uint32_t third) {
        if (first >= 0x65U || second >= 0x65U ||
            third >= 0x101U) {
            return false;
        }
        grp0_render_global_529bc4_ = second;
        grp0_render_global_529bc0_ = first;
        grp0_render_global_529bc8_ = 0;
        grp0_render_global_529bcc_ = third;
        return true;
    }
    [[nodiscard]] uint32_t grp0_render_global_529bc0() const {
        return grp0_render_global_529bc0_;
    }
    [[nodiscard]] uint32_t grp0_render_global_529bc4() const {
        return grp0_render_global_529bc4_;
    }
    [[nodiscard]] uint32_t grp0_render_global_529bc8() const {
        return grp0_render_global_529bc8_;
    }
    [[nodiscard]] uint32_t grp0_render_global_529bcc() const {
        return grp0_render_global_529bcc_;
    }
    void request_redraw(uint32_t full) {
        redraw_pending_ = true;
        redraw_full_ |= full;
    }
    [[nodiscard]] bool redraw_pending() const { return redraw_pending_; }
    [[nodiscard]] uint32_t redraw_full() const { return redraw_full_; }
    void clear_redraw_request() {
        redraw_pending_ = false;
        redraw_full_ = 0;
    }
    void set_current_render_controls(uint32_t first, uint32_t second) {
        render_control_first_ = first;
        render_control_second_ = second;
        background_render_enabled_ = second;
        refresh_main_background_geometry();
        display_dirty_ = true;
    }
    [[nodiscard]] uint32_t render_control_first() const {
        return render_control_first_;
    }
    [[nodiscard]] uint32_t render_control_second() const {
        return render_control_second_;
    }

    // Original globals 0x00542BCC/0x00542BD0. Sys0:0x70 allocates and
    // clears a 0x1000 << exponent scratch buffer used by save-data packing.
    void resize_runtime_buffer(uint32_t exponent) {
        memory_.clear_resize_global_frame(std::size_t{0x1000} << exponent);
    }
    [[nodiscard]] std::span<uint8_t> mutable_runtime_buffer() {
        return memory_.frame();
    }
    void set_runtime_buffer(std::vector<uint8_t> data) {
        memory_.replace_global_frame(std::move(data));
    }
    [[nodiscard]] std::size_t runtime_buffer_size() const {
        return memory_.frame().size();
    }
    [[nodiscard]] std::span<const uint8_t> runtime_buffer() const {
        return memory_.frame();
    }
    // Original Sys0:82/83 and GDB load/save share DAT_00542DBC.
    // Selection history must use that same persistent 1 MiB allocation.
    bool write_storage_buffer(
        uint32_t offset, std::span<const uint8_t> data) {
        if (data.empty() || offset > 0xFFFFFU ||
            data.size() > 0x100000U ||
            static_cast<uint64_t>(offset) + data.size() > 0x100000U) {
            return false;
        }
        std::copy(data.begin(), data.end(),
                  gdb_character_data_.begin() +
                      static_cast<std::ptrdiff_t>(offset));
        return true;
    }
    bool read_storage_buffer(
        uint32_t offset, std::span<uint8_t> data) const {
        if (data.empty() || offset > 0xFFFFFU ||
            data.size() > 0x100000U ||
            static_cast<uint64_t>(offset) + data.size() > 0x100000U) {
            return false;
        }
        std::copy_n(
            gdb_character_data_.begin() +
                static_cast<std::ptrdiff_t>(offset),
            data.size(), data.begin());
        return true;
    }

    // Original CRT per-thread holdrand field. Sys0:0x00 is srand(), and
    // the CRT rand() at 0x004D8C4E advances this exact 32-bit state.
    void set_random_seed(uint32_t seed) { random_state_ = seed; }
    [[nodiscard]] uint32_t random_state() const { return random_state_; }
    uint32_t random_next() {
        random_state_ = random_state_ * 0x343FDU + 0x269EC3U;
        return (random_state_ >> 16U) & 0x7FFFU;
    }

    // Original global 0x00529C20. Grp0:0x02 stores 1000 / requested FPS,
    // or zero when requested FPS is zero.
    void set_frame_delay_for_fps(uint32_t fps) {
        frame_delay_ms_ = fps == 0 ? 0U : 1000U / fps;
        display_fps_ = fps;
    }
    [[nodiscard]] uint32_t frame_delay_ms() const {
        return frame_delay_ms_;
    }
    struct FrameProfilerState {
        uint32_t enabled = 0;
        uint64_t budget_ticks = 0;
        uint32_t frame_count = 0;
        uint32_t overrun_count = 0;
        uint64_t accumulated_ticks = 0;
        uint64_t budget_accumulator = 0;
        uint64_t start_tick = 0;
        uint32_t start_sequence = 0xFFFFFFFFU;
    };
    void set_frame_profiler(uint32_t enabled) {
        frame_profiler_.enabled = enabled;
        if (enabled == 0) {
            return;
        }
        const uint32_t fps = display_fps_ == 0 ? 60U : display_fps_;
        const uint64_t ticks_per_frame = 1000000000ULL / fps;
        frame_profiler_.budget_ticks =
            ticks_per_frame * 96U / 100U;
        frame_profiler_.frame_count = 0;
        frame_profiler_.overrun_count = 0;
        frame_profiler_.accumulated_ticks = 0;
        frame_profiler_.budget_accumulator = 0;
        frame_profiler_.start_tick = 0;
        frame_profiler_.start_sequence = 0xFFFFFFFFU;
    }
    void frame_profiler_begin(uint64_t tick, uint32_t sequence) {
        if (frame_profiler_.enabled == 0) {
            return;
        }
        frame_profiler_.start_tick = tick;
        frame_profiler_.start_sequence = sequence;
    }
    void frame_profiler_end(uint64_t tick, uint32_t sequence) {
        if (frame_profiler_.enabled == 0) {
            return;
        }
        uint64_t elapsed =
            tick - frame_profiler_.start_tick +
            frame_profiler_.accumulated_ticks;
        if (sequence == 0) {
            frame_profiler_.accumulated_ticks = elapsed;
            return;
        }
        frame_profiler_.budget_accumulator +=
            frame_profiler_.budget_ticks;
        if ((frame_profiler_.start_sequence == 0xFFFFFFFFU ||
             sequence == 0xFFFFFFFFU ||
             frame_profiler_.start_sequence <= sequence) &&
            elapsed < frame_profiler_.budget_ticks) {
            ++frame_profiler_.overrun_count;
        }
        ++frame_profiler_.frame_count;
        frame_profiler_.accumulated_ticks = 0;
    }
    [[nodiscard]] uint32_t query_frame_profiler(
        uint32_t selector) const {
        if (frame_profiler_.enabled == 0) {
            return 0;
        }
        switch (selector) {
            case 0:
                return frame_profiler_.frame_count;
            case 1:
                if (frame_profiler_.frame_count == 0) {
                    return 0;
                }
                return static_cast<uint32_t>(
                    static_cast<double>(frame_profiler_.budget_ticks) *
                    1000000.0 /
                    (static_cast<double>(frame_profiler_.frame_count) *
                     1000000000.0));
            case 2:
                return frame_profiler_.budget_accumulator == 0
                           ? 0U
                           : frame_profiler_.frame_count * 5000U;
            case 3:
                if (frame_profiler_.frame_count == 0) {
                    return 0;
                } else {
                    const double ratio =
                        static_cast<double>(
                            frame_profiler_.overrun_count) /
                        static_cast<double>(frame_profiler_.frame_count);
                    return static_cast<uint32_t>(
                        10000.0 * ratio * ratio);
                }
            default:
                return 0;
        }
    }
    [[nodiscard]] const FrameProfilerState& frame_profiler() const {
        return frame_profiler_;
    }
    void set_default_procedure_duration(int32_t duration) {
        default_procedure_duration_ = duration;
        procedure_elapsed_ = 0;
    }
    [[nodiscard]] int32_t default_procedure_duration() const {
        return default_procedure_duration_;
    }
    [[nodiscard]] int32_t procedure_elapsed() const {
        return procedure_elapsed_;
    }

    // Original DCCache object at 0x00533220. Grp0:0x03 destroys the old
    // cache and creates one with this byte capacity and half as threshold.
    void set_image_cache_capacity(uint32_t capacity) {
        image_cache_capacity_ = capacity;
        image_cache_threshold_ = capacity / 2U;
        image_cache_used_ = 0;
        image_cache_records_.clear();
    }
    [[nodiscard]] uint32_t image_cache_capacity() const {
        return image_cache_capacity_;
    }
    [[nodiscard]] uint32_t image_cache_threshold() const {
        return image_cache_threshold_;
    }
    using ImageCacheClock = std::function<uint32_t()>;
    void set_image_cache_clock(ImageCacheClock clock) {
        image_cache_clock_ = std::move(clock);
    }
    struct ImageCacheRecord {
        std::string first;
        std::string second;
        std::vector<uint8_t> data;
        uint32_t timestamp = 0;
    };
    bool put_image_cache_record(
        std::string first, std::string second,
        std::span<const uint8_t> data) {
        const std::size_t size = data.size();
        if (image_cache_capacity_ == 0U ||
            size == 0U ||
            size > image_cache_threshold_) {
            return false;
        }
        while (static_cast<uint64_t>(image_cache_used_) + size >
               image_cache_capacity_) {
            if (image_cache_records_.empty()) {
                return false;
            }
            const auto oldest = std::min_element(
                image_cache_records_.begin(),
                image_cache_records_.end(),
                [](const ImageCacheRecord& left,
                   const ImageCacheRecord& right) {
                    return left.timestamp < right.timestamp;
                });
            image_cache_used_ -= static_cast<uint32_t>(
                oldest->data.size());
            image_cache_records_.erase(oldest);
        }
        ImageCacheRecord record;
        record.first = std::move(first);
        record.second = std::move(second);
        record.data.assign(data.begin(), data.end());
        record.timestamp =
            image_cache_clock_ ? image_cache_clock_()
                               : static_cast<uint32_t>(
                                     std::chrono::duration_cast<
                                         std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now()
                                             .time_since_epoch())
                                         .count());
        image_cache_used_ += static_cast<uint32_t>(size);
        image_cache_records_.insert(
            image_cache_records_.begin(), std::move(record));
        return true;
    }
    [[nodiscard]] const std::vector<ImageCacheRecord>&
    image_cache_records() const {
        return image_cache_records_;
    }
    [[nodiscard]] uint32_t image_cache_used() const {
        return image_cache_used_;
    }
    struct SourceResourceState {
        uint32_t source_id = 0;
        uint32_t field_1c = 0;
        uint32_t field_20 = 0;
        uint32_t field_24 = 0;
        uint32_t registered_id = 0;
    };
    struct GlobalResourceRecord {
        uint32_t id = 0;
        uint32_t source_id = 0;
        uint32_t field_1c = 0;
        uint32_t field_20 = 0;
        uint32_t field_24 = 0;
    };
    bool seed_source_resource(
        uint32_t source_id, uint32_t field_1c,
        uint32_t field_20, uint32_t field_24) {
        if (source_id == 0) {
            return false;
        }
        source_resources_[source_id] = {
            source_id, field_1c, field_20, field_24, 0};
        return true;
    }
    uint32_t register_global_resource(
        uint32_t source_id, uint32_t& record_id) {
        const auto source = source_resources_.find(source_id);
        if (source == source_resources_.end()) {
            return 3;
        }
        if (source->second.registered_id != 0) {
            return 7;
        }
        const uint32_t id = ++next_global_resource_id_;
        global_resource_records_[id] = {
            id, source_id, source->second.field_1c,
            source->second.field_20, source->second.field_24};
        source->second.registered_id = id;
        record_id = id;
        return 0;
    }
    uint32_t remove_global_resource(uint32_t record_id) {
        const auto record = global_resource_records_.find(record_id);
        if (record == global_resource_records_.end()) {
            return 3;
        }
        const auto source =
            source_resources_.find(record->second.source_id);
        if (source != source_resources_.end()) {
            source->second.registered_id = 0;
        }
        global_resource_records_.erase(record);
        return 0;
    }
    [[nodiscard]] const std::map<uint32_t, GlobalResourceRecord>&
    global_resource_records() const {
        return global_resource_records_;
    }
    struct BmvFrameProcedure {
        uint32_t procedure_id = 0;
        uint32_t slot = 0;
        uint32_t record_id = 0;
        uint32_t frame = 0;
        int32_t status = 6;
        bool complete = false;
    };
    uint32_t create_bmv_frame_procedure(
        uint32_t slot, uint32_t record_id, uint32_t frame) {
        const uint32_t id = next_procedure_id_++;
        const uint32_t token = 0xE6000000U | id;
        bmv_frame_procedures_[token] = {
            id, slot, record_id, frame, 6, false};
        return token;
    }
    [[nodiscard]] const std::map<uint32_t, BmvFrameProcedure>&
    bmv_frame_procedures() const {
        return bmv_frame_procedures_;
    }
    uint32_t request_bmv_frame(
        uint32_t slot, uint32_t record_id, uint32_t frame,
        bool asynchronous, uint32_t base, uint32_t& token) {
        if (slot >= 0x4000U) {
            return 5;
        }
        const auto record = global_resource_records_.find(record_id);
        if (record == global_resource_records_.end() ||
            record->second.field_20 == 0) {
            return 3;
        }
        const uint32_t header = record->second.field_20;
        if (memory_.read_cstring(header, base) != "BF_Movie_______") {
            return 3;
        }
        const uint32_t version = memory_.read_u32(header + 0x10, base);
        if (version != 0U && version != 0x10000U &&
            version != 0x10001U) {
            return 3;
        }
        const uint32_t frame_count =
            memory_.read_u32(header + 0x28, base);
        if (frame >= frame_count) {
            return 4;
        }
        const auto surface = graphics_surfaces_.find(slot);
        if (surface == graphics_surfaces_.end() ||
            surface->second.width !=
                memory_.read_u32(header + 0x14, base) ||
            surface->second.height !=
                memory_.read_u32(header + 0x18, base) ||
            surface->second.format !=
                memory_.read_u32(header + 0x20, base)) {
            return 5;
        }
        if (asynchronous) {
            token = create_bmv_frame_procedure(slot, record_id, frame);
            return 2;
        }
        // The proprietary BMV frame decompressor is a separate backend
        // frontier. A validated frame must not report false success.
        return 6;
    }

    // Original global 0x00530EA4. The font renderer consults this raw
    // value when deciding whether outline metrics may be used.
    void set_text_flag_530ea4(uint32_t value) { text_flag_530ea4_ = value; }
    [[nodiscard]] uint32_t text_flag_530ea4() const {
        return text_flag_530ea4_;
    }

    // Original font-registry list at 0x00529BA4 and default string at
    // 0x00530EA8. Ext0:0xC7 sets or replaces one substitution entry.
    void set_font_substitution(std::string key, std::string value) {
        if (key.empty()) {
            default_font_name_ = std::move(value);
        } else {
            font_substitutions_[std::move(key)] = std::move(value);
        }
    }
    [[nodiscard]] const std::string& default_font_name() const {
        return default_font_name_;
    }
    [[nodiscard]] const std::map<std::string, std::string>&
    font_substitutions() const {
        return font_substitutions_;
    }
    void set_font_alias(std::string key, std::string value) {
        font_aliases_[std::move(key)] = std::move(value);
    }
    [[nodiscard]] const std::map<std::string, std::string>&
    font_aliases() const {
        return font_aliases_;
    }

    // PE64 14006C740 owns the five-dword per-name glyph transform tuple.
    void set_scaled_font_tuple(const std::string& name, std::array<uint32_t,5> tuple) {
        scaled_font_tuples_[name] = tuple;
    }
    uint32_t set_scaled_font_spacing(const std::string& name, uint32_t spacing) {
        if (spacing + 0x10000U > 0x20000U) return 0x80000007U;
        auto found=scaled_font_tuples_.find(name);
        if(found==scaled_font_tuples_.end())return 0x80000008U;
        found->second[4]=spacing;
        return 0;
    }
    const std::map<std::string,std::array<uint32_t,5>>& scaled_font_tuples() const {
        return scaled_font_tuples_;
    }

    // Original font-name list/counter at 0x00542554/0x0054254C.
    uint32_t register_font_name(const std::string& name) {
        const auto found = font_ids_.find(name);
        if (found != font_ids_.end()) {
            return found->second;
        }
        const uint32_t id = next_font_id_++;
        font_ids_.emplace(name, id);
        return id;
    }
    void set_font_charset(const std::string& name, uint32_t charset) {
        font_charsets_[name] = charset;
    }
    [[nodiscard]] const std::map<std::string, uint32_t>&
    font_charsets() const {
        return font_charsets_;
    }
    [[nodiscard]] std::size_t font_id_count() const { return font_ids_.size(); }
    struct FontCacheRegistration {
        uint32_t font_id = 0;
        std::string face_name;
        uint32_t parameter_2 = 0;
        uint32_t parameter_3 = 0;
        uint32_t parameter_4 = 0;
        uint32_t first_init = 0;
        uint32_t second_init = 0;
    };
    uint32_t register_font_cache_entry(
        uint32_t font_id, uint32_t parameter_2, uint32_t parameter_3,
        uint32_t parameter_4, uint32_t first_init,
        uint32_t second_init) {
        const std::string face = font_name(font_id);
        if (face.empty()) {
            return 0x80000004U;
        }
        const auto found = std::find_if(
            font_cache_registrations_.begin(),
            font_cache_registrations_.end(),
            [&face, parameter_2, parameter_3, parameter_4](
                const FontCacheRegistration& entry) {
                return entry.face_name == face &&
                       entry.parameter_2 == parameter_2 &&
                       entry.parameter_3 == parameter_3 &&
                       entry.parameter_4 == parameter_4;
            });
        if (found != font_cache_registrations_.end()) {
            return 0;
        }
        font_cache_registrations_.push_back({
            font_id, face, parameter_2, parameter_3, parameter_4,
            first_init, second_init});
        return 0;
    }
    [[nodiscard]] const std::vector<FontCacheRegistration>&
    font_cache_registrations() const {
        return font_cache_registrations_;
    }

    bool query_cpu_brand(uint32_t destination, uint32_t base) {
        if (!host_system_metadata_provider_) {
            return false;
        }
        const HostSystemMetadata metadata =
            host_system_metadata_provider_();
        if (metadata.cpu_brand.empty()) {
            return false;
        }
        memory_.write_cstring(
            destination, clean_host_metadata_string(metadata.cpu_brand), base);
        return true;
    }
    void query_host_version(uint32_t destination,
                            uint32_t words_destination, uint32_t base) {
        if (!host_system_metadata_provider_) {
            return;
        }
        const HostSystemMetadata metadata =
            host_system_metadata_provider_();
        if (destination != 0) {
            memory_.write_cstring(
                destination,
                clean_host_metadata_string(metadata.version), base);
        }
        if (words_destination != 0) {
            for (std::size_t index = 0;
                 index < metadata.version_words.size(); ++index) {
                memory_.write_u32(
                    words_destination + static_cast<uint32_t>(index * 4U),
                    base, metadata.version_words[index]);
            }
        }
    }
    void query_os_version_info(uint32_t string_destination,
                               uint32_t words_destination, uint32_t base) {
        if (!host_system_metadata_provider_) {
            return;
        }
        const HostSystemMetadata metadata =
            host_system_metadata_provider_();
        if (words_destination != 0) {
            for (std::size_t index = 0;
                 index < metadata.os_version_words.size(); ++index) {
                memory_.write_u32(
                    words_destination + static_cast<uint32_t>(index * 4U),
                    base, metadata.os_version_words[index]);
            }
        }
        if (string_destination != 0) {
            memory_.write_cstring(
                string_destination,
                clean_host_metadata_string(metadata.os_version_string), base);
        }
    }
    bool query_volume_label(uint32_t buffer, uint32_t base) {
        if (buffer == 0 || !volume_label_provider_) {
            return false;
        }
        const std::string path = memory_.read_cstring(buffer, base);
        const std::optional<std::string> label =
            volume_label_provider_(path);
        if (!label) {
            return false;
        }
        memory_.write_cstring(buffer, *label, base);
        return true;
    }
    int32_t utf16_edit_distance(uint32_t first, uint32_t second,
                                uint32_t base) {
        if (first == 0 || second == 0) {
            return -1;
        }
        const auto read_units = [this, base](uint32_t address) {
            std::vector<uint16_t> units;
            while (true) {
                const uint16_t value = memory_.read_u16(address, base);
                if (value == 0) {
                    break;
                }
                units.push_back(value);
                address += 2;
            }
            return units;
        };
        const std::vector<uint16_t> left = read_units(first);
        const std::vector<uint16_t> right = read_units(second);
        if (left.empty()) {
            return 0;
        }
        std::vector<int32_t> previous(right.size() + 1);
        std::vector<int32_t> current(right.size() + 1);
        for (std::size_t index = 0; index <= right.size(); ++index) {
            previous[index] = static_cast<int32_t>(index);
        }
        for (std::size_t row = 1; row <= left.size(); ++row) {
            current[0] = static_cast<int32_t>(row);
            for (std::size_t column = 1; column <= right.size(); ++column) {
                const int32_t substitution =
                    previous[column - 1] +
                    (left[row - 1] == right[column - 1] ? 0 : 1);
                const int32_t deletion = previous[column] + 1;
                const int32_t insertion = current[column - 1] + 1;
                current[column] =
                    std::min({substitution, deletion, insertion});
            }
            std::swap(previous, current);
        }
        return previous.back();
    }

    [[nodiscard]] std::string font_name(uint32_t id) const {
        for (const auto& [name, value] : font_ids_) {
            if (value == id) {
                return name;
            }
        }
        return {};
    }
    void set_font_cache_entry(const std::string& name, uint32_t width,
                              uint32_t height, uint32_t style,
                              uint32_t value) {
        font_cache_entries_[{name, width, height, style}] = value;
    }
    [[nodiscard]] const
        std::map<std::tuple<std::string, uint32_t, uint32_t, uint32_t>,
                 uint32_t>&
    font_cache_entries() const {
        return font_cache_entries_;
    }

    // Original font-object list at display +0x16C. Each unique face stores
    // four 32-bit sizing parameters at object offsets +0x60..+0x6C.
    void set_font_parameters(const std::string& name,
                             const std::array<uint32_t, 4>& parameters) {
        font_parameters_[name] = parameters;
    }
    [[nodiscard]] const std::map<std::string, std::array<uint32_t, 4>>&
    font_parameters() const {
        return font_parameters_;
    }

    // Original text-quality globals 0x00529B8C/0x00529B80/0x00529B90.
    void set_text_quality(uint32_t level) {
        static constexpr std::array<std::array<uint32_t, 3>, 4> presets = {{
            {{2, 2, 1}},
            {{4, 4, 2}},
            {{6, 8, 3}},
            {{8, 16, 4}},
        }};
        text_quality_level_ = level;
        text_quality_scale_ = presets.at(level)[0];
        text_quality_glyph_scale_ = presets.at(level)[1];
        text_quality_shift_ = presets.at(level)[2];
    }
    [[nodiscard]] uint32_t text_quality_level() const {
        return text_quality_level_;
    }
    [[nodiscard]] uint32_t text_quality_scale() const {
        return text_quality_scale_;
    }
    [[nodiscard]] uint32_t text_quality_glyph_scale() const {
        return text_quality_glyph_scale_;
    }
    [[nodiscard]] uint32_t text_quality_shift() const {
        return text_quality_shift_;
    }
    void set_text_metrics(const std::array<uint32_t, 6>& values) {
        text_metrics_ = values;
    }
    [[nodiscard]] const std::array<uint32_t, 6>& text_metrics() const {
        return text_metrics_;
    }

    // Original global 0x0053341C. Grp1:0x09 accepts only 0 or 1.
    void set_text_flag_53341c(uint32_t value) { text_flag_53341c_ = value; }
    [[nodiscard]] uint32_t text_flag_53341c() const {
        return text_flag_53341c_;
    }

    // Original cursor index 0x00542C34, indexed into the cursor array.
    void set_cursor_index(uint32_t value) { cursor_index_ = value; }
    [[nodiscard]] uint32_t cursor_index() const { return cursor_index_; }

    // Original save-pack flag 0x0052A26C. A nonzero value enables the
    // checksum pass in FUN_00471140 while serializing save data.
    void set_save_checksum_enabled(uint32_t value) {
        save_checksum_enabled_ = value;
    }
    [[nodiscard]] uint32_t save_checksum_enabled() const {
        return save_checksum_enabled_;
    }

    // Original linked list 0x00541BBC. Sys0:0x37 prepends a resource/font
    // search path; lookup walks it from the newest entry to the oldest.
    void add_search_path(std::string path) {
        search_paths_.insert(search_paths_.begin(), std::move(path));
    }
    [[nodiscard]] const std::vector<std::string>& search_paths() const {
        return search_paths_;
    }

    // Original global 0x00529F5C. Nonzero enables fallback lookup through
    // the Sys0:0x37 search-path list.
    void set_search_paths_enabled(uint32_t value) {
        search_paths_enabled_ = value;
    }
    [[nodiscard]] uint32_t search_paths_enabled() const {
        return search_paths_enabled_;
    }

    // Original graphics global 0x00532814. Grp0:0x9F stores it verbatim.
    void set_graphics_flag_532814(uint32_t value) {
        graphics_flag_532814_ = value;
    }
    [[nodiscard]] uint32_t graphics_flag_532814() const {
        return graphics_flag_532814_;
    }
    void set_graphics_flag_529bd8(uint32_t value) {
        graphics_flag_529bd8_ = value;
    }
    [[nodiscard]] uint32_t graphics_flag_529bd8() const {
        return graphics_flag_529bd8_;
    }
    void set_graphics_pair_96(uint32_t value, uint32_t count) {
        graphics_flag_529bd0_ = value;
        graphics_count_529bdc_ = count;
    }
    [[nodiscard]] uint32_t graphics_flag_529bd0() const {
        return graphics_flag_529bd0_;
    }
    [[nodiscard]] uint32_t graphics_count_529bdc() const {
        return graphics_count_529bdc_;
    }
    void set_graphics_pair_95(uint32_t value, uint32_t count) {
        graphics_count_529bb4_ = count;
        graphics_flag_529bb8_ = value;
    }
    [[nodiscard]] uint32_t graphics_count_529bb4() const {
        return graphics_count_529bb4_;
    }
    [[nodiscard]] uint32_t graphics_flag_529bb8() const {
        return graphics_flag_529bb8_;
    }
    uint32_t swap_global_589ec4(uint32_t value) {
        const uint32_t old = global_589ec4_;
        global_589ec4_ = value;
        return old;
    }
    void register_display_object(uint32_t id) {
        display_objects_.insert(id);
        object_absolute_positions_.try_emplace(
            id, std::array<uint32_t, 3>{0, 0, 0});
    }
    bool remove_display_object(uint32_t id) {
        if (id == 0) {
            return display_objects_.count(0) != 0;
        }
        const bool removed = display_objects_.erase(id) != 0;
        if (removed) {
            object_display_classes_.erase(id);
            object_absolute_positions_.erase(id);
            object_absolute_position_dispatch_.erase(id);
            unregister_display_list_object(id);
            sprites_.erase(id);
            sprite_mode_configurations_.erase(id);
            sprite_mask_configs_.erase(id);
        }
        return removed;
    }
    uint32_t create_display_object();
    [[nodiscard]] const std::set<uint32_t>& display_objects() const {
        return display_objects_;
    }
    struct SpriteState {
        uint32_t bitmap = 0;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t blend_mode = 0;
        uint32_t alpha = 0;
        uint32_t priority = 0;
        uint32_t draw_enabled = 0;
        bool configured = false;
    };
    struct SpriteModeConfiguration {
        uint32_t mode = 0;
        std::vector<uint32_t> operands;
    };
    uint32_t set_sprite_mode_configuration(
        uint32_t handle, uint32_t mode,
        std::span<const uint32_t> operands) {
        static constexpr std::array<std::size_t, 6> widths{
            9, 13, 10, 10, 17, 20};
        if (mode >= widths.size() ||
            operands.size() != widths[mode] ||
            operands.front() != handle ||
            !object_exists(handle)) {
            return 0xFFU;
        }
        if (mode == 0) {
            const uint32_t first_bitmap = operands[3];
            const uint32_t second_bitmap = operands[4];
            const auto first = graphics_surfaces_.find(first_bitmap);
            if (first == graphics_surfaces_.end()) {
                return 1;
            }
            const auto second = graphics_surfaces_.find(second_bitmap);
            if (second == graphics_surfaces_.end()) {
                return 2;
            }
            if (first->second.width != second->second.width ||
                first->second.height != second->second.height) {
                return 3;
            }
            // 0042BFD0 -> 0042C4D0: two bitmap identities/tokens and mix.
            object_raw_fields_[{handle,0x160U}] = first_bitmap;
            object_raw_fields_[{handle,0x164U}] = second_bitmap;
            object_raw_fields_[{handle,0x168U}] = graphics_bitmap_token(first_bitmap);
            object_raw_fields_[{handle,0x16CU}] = graphics_bitmap_token(second_bitmap);
            object_raw_fields_[{handle,0x250U}] = operands[5];
            object_raw_fields_[{handle,0x254U}] = operands[8];
            object_raw_fields_[{handle,0xA0U}] = first->second.width;
            object_raw_fields_[{handle,0xA4U}] = first->second.height;
            set_object_position(handle,static_cast<int32_t>(operands[1]),static_cast<int32_t>(operands[2]));
            object_raw_fields_[{handle,0xB0U}] = 1U;
            set_raw_field_recursive(handle,0xB4U,operands[6]);
            set_object_priority_bias(handle,operands[7]);
            auto& sprite = sprites_[handle];
            sprite.bitmap = first_bitmap;
            sprite.x = static_cast<int32_t>(operands[1]);
            sprite.y = static_cast<int32_t>(operands[2]);
            sprite.blend_mode = 1U;
            sprite.alpha = operands[6];
        }
        if (mode == 4U) {
            initialize_affine_sprite_mode5(handle, operands);
        }
        sprite_mode_configurations_[handle] = {
            mode,
            std::vector<uint32_t>(
                operands.begin(), operands.end())};
        // Grp0:58..5D map to raw CDspObjSprite+144 modes1..6.
        object_raw_fields_[{handle, 0x144U}] = mode + 1U;
        SpriteState& sprite = sprites_[handle];
        sprite.configured = true;
        sprite.draw_enabled = 1;
        if (mode == 0U) refresh_sprite_display_geometry(handle);
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::map<uint32_t, SpriteModeConfiguration>&
    sprite_mode_configurations() const {
        return sprite_mode_configurations_;
    }
    struct FilterState {
        uint32_t field_148 = 0;
        uint32_t field_14c = 0;
        uint32_t query_field = 0;
        uint32_t bitmap = 0;
        uint32_t field_154 = 0;
        uint32_t bitmap_token = 0xFFFFFFFFU; // original filter +158 allocation identity
        bool active = false;
    };
    struct MapRasterState {
        struct Tile {
            uint32_t source_offset = 0;
            uint32_t source_stride = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t format = 0;
            uint32_t bytes_per_pixel = 0;
        };
        bool ready = false;
        uint32_t columns = 0;                  // +0x148
        uint32_t rows = 0;                     // +0x14C
        uint32_t sample_width = 0;             // +0x150
        uint32_t sample_height = 0;            // +0x154
        uint32_t tile_width = 0;               // +0x158
        uint32_t tile_height = 0;              // +0x15C
        uint32_t internal_format = 0;
        uint32_t blend_mode = 0;                // +0xB0
        uint32_t effect_level = 0;              // +0xB4
        uint32_t priority_bias = 0;             // +0x1C
        bool image_ready = false;               // +0x1B8
        uint32_t image_bitmap = 0;               // +0x1BC
        uint32_t image_width = 0;                // +0x1C0
        std::vector<Tile> tiles;                 // +0x1C8
        uint32_t output_x = 0;                 // +0x19C
        uint32_t output_y = 0;                 // +0x1A0
        uint32_t right = 0;                    // +0x1AC
        uint32_t bottom = 0;                   // +0x1B0
        bool dirty = false;                    // +0x1B4
        bool upload_ready = false;             // +0x180
        uint32_t upload_width = 0;              // +0x184
        uint32_t upload_height = 0;             // +0x188
        uint32_t cursor_x = 0;                  // +0x194
        uint32_t cursor_y = 0;                  // +0x198
        std::vector<uint16_t> samples_a;        // +0x178
        std::vector<uint16_t> samples_b;        // +0x17C
        std::vector<uint16_t> uploaded;         // +0x18C
    };
    [[nodiscard]] bool has_map_raster(uint32_t handle) const {
        if ((handle & 0xFF000000U) != 0xA0000000U) {
            return false;
        }
        const auto& registry =
            factory_objects_[static_cast<std::size_t>(
                FactoryObjectKind::Grp0Secondary)];
        const uint32_t slot = handle & 0x00FFFFFFU;
        return slot < registry.slot_count &&
               registry.instances[slot].has_value();
    }
    uint32_t configure_map_raster(uint32_t handle, uint32_t columns,
                                  uint32_t rows, uint32_t tile_width,
                                  uint32_t tile_height) {
        if (!has_map_raster(handle)) {
            return 0xFF;
        }
        if (tile_width == 0 || tile_width > 0x100U ||
            tile_height == 0 || tile_height > 0x100U) {
            return 3;
        }
        if (columns == 0 || columns > 0x400U / tile_width ||
            rows == 0 || rows > 0x300U / tile_height) {
            return 2;
        }
        MapRasterState state;
        state.ready = true;
        state.columns = columns;
        state.rows = rows;
        state.sample_width = columns + 1U;
        state.sample_height = rows + 1U;
        state.tile_width = tile_width;
        state.tile_height = tile_height;
        state.internal_format = current_capture_format_;
        const std::size_t sample_count =
            static_cast<std::size_t>(state.sample_width) *
            state.sample_height;
        state.samples_a.assign(sample_count, 0xFFFFU);
        state.samples_b.assign(sample_count, 0);
        map_rasters_[handle] = std::move(state);
        object_raw_fields_[{handle, 0xA0U}] = columns * tile_width;
        object_raw_fields_[{handle, 0xA4U}] = rows * tile_height;
        if (const auto display = display_object_states_.find(handle);
            display != display_object_states_.end()) {
            display->second.width = columns * tile_width;
            display->second.height = rows * tile_height;
        }
        display_dirty_ = true;
        return 0;
    }
    uint32_t configure_map_image(uint32_t handle, int32_t x, int32_t y,
                                 int32_t bitmap, uint32_t blend_mode,
                                 uint32_t effect_level,
                                 uint32_t priority_bias) {
        if (!has_map_raster(handle)) {
            return 0xFF;
        }
        const auto map = map_rasters_.find(handle);
        if (map == map_rasters_.end() || !map->second.ready ||
            bitmap < 0) {
            return 1;
        }
        const auto surface = graphics_surfaces_.find(
            static_cast<uint32_t>(bitmap));
        if (surface == graphics_surfaces_.end()) {
            return 1;
        }
        MapRasterState& state = map->second;
        const uint32_t columns = surface->second.width / state.tile_width;
        const uint32_t rows = surface->second.height / state.tile_height;
        if (columns == 0 || rows == 0 ||
            surface->second.format != state.internal_format) {
            return 1;
        }

        std::vector<MapRasterState::Tile> tiles;
        tiles.reserve(static_cast<std::size_t>(columns) * rows);
        for (uint32_t row = 0; row < rows; ++row) {
            for (uint32_t column = 0; column < columns; ++column) {
                MapRasterState::Tile tile;
                tile.source_offset =
                    row * state.tile_height * surface->second.stride +
                    column * state.tile_width *
                        surface->second.bytes_per_pixel;
                tile.source_stride = surface->second.stride;
                tile.width = state.tile_width;
                tile.height = state.tile_height;
                tile.format = surface->second.format;
                tile.bytes_per_pixel =
                    surface->second.bytes_per_pixel;
                tiles.push_back(tile);
            }
        }
        state.image_ready = true;
        state.image_bitmap = static_cast<uint32_t>(bitmap);
        state.image_width = surface->second.width;
        state.tiles = std::move(tiles);
        state.blend_mode = blend_mode;
        state.effect_level = effect_level;
        state.priority_bias = priority_bias;
        if (!set_object_position(handle, x, y)) {
            return 1;
        }
        object_raw_fields_[{handle, 0x1CU}] = priority_bias;
        display_dirty_ = true;
        return 0;
    }
    uint32_t upload_map_pixels(uint32_t handle, uint32_t width,
                               uint32_t height, uint32_t source,
                               uint32_t base) {
        if (!has_map_raster(handle)) {
            return 0xFF;
        }
        const auto found = map_rasters_.find(handle);
        if (found == map_rasters_.end() || width == 0 || height == 0) {
            return 4;
        }
        const std::size_t count =
            static_cast<std::size_t>(width) * height;
        if (count > std::numeric_limits<std::size_t>::max() / 2U ||
            width > std::numeric_limits<uint32_t>::max() / height) {
            return 4;
        }
        const auto bytes = memory_.read_bytes(
            source, count * 2U, base);
        if (bytes.size() < count * 2U) {
            return 4;
        }
        std::vector<uint16_t> pixels(count);
        for (std::size_t index = 0; index < count; ++index) {
            pixels[index] = static_cast<uint16_t>(
                bytes[index * 2U] |
                (static_cast<uint16_t>(bytes[index * 2U + 1U])
                 << 8U));
        }
        found->second.upload_ready = true;
        found->second.upload_width = width;
        found->second.upload_height = height;
        found->second.uploaded = std::move(pixels);
        return 0;
    }
    uint32_t render_map_block(uint32_t handle, uint32_t source_x,
                              uint32_t source_y, uint32_t output_x,
                              uint32_t output_y, uint32_t wrap) {
        if (!has_map_raster(handle)) {
            return 0xFF;
        }
        const auto found = map_rasters_.find(handle);
        if (found == map_rasters_.end() || !found->second.ready ||
            !found->second.upload_ready ||
            found->second.upload_width <= source_x ||
            found->second.upload_height <= source_y ||
            found->second.tile_width <= output_x ||
            found->second.tile_height <= output_y) {
            return 5;
        }
        MapRasterState& state = found->second;
        if (state.cursor_x != source_x || state.cursor_y != source_y) {
            state.cursor_x = source_x;
            state.cursor_y = source_y;
            if (wrap == 0) {
                std::fill(
                    state.samples_b.begin(), state.samples_b.end(),
                    0xFFFFU);
            }
            std::size_t destination = 0;
            uint32_t y = source_y;
            for (uint32_t row = 0; row < state.sample_height; ++row) {
                if (y >= state.upload_height) {
                    if (wrap == 0) {
                        break;
                    }
                    y = 0;
                }
                uint32_t x = source_x;
                for (uint32_t column = 0;
                     column < state.sample_width; ++column) {
                    if (x >= state.upload_width) {
                        if (wrap == 0) {
                            break;
                        }
                        x = 0;
                    }
                    state.samples_b[destination++] =
                        state.uploaded[
                            static_cast<std::size_t>(y) *
                                state.upload_width +
                            x];
                    ++x;
                }
                ++y;
            }
            state.dirty = true;
        }
        state.output_x = output_x;
        state.output_y = output_y;
        state.right = static_cast<uint32_t>(
            static_cast<int32_t>(state.columns) *
                static_cast<int32_t>(state.tile_width) -
            1 + static_cast<int32_t>(output_x));
        state.bottom = static_cast<uint32_t>(
            static_cast<int32_t>(state.rows) *
                static_cast<int32_t>(state.tile_height) -
            1 + static_cast<int32_t>(output_y));
        display_dirty_ = true;
        return 0;
    }
    uint32_t invert_map_samples(uint32_t handle, uint32_t key) {
        if (!has_map_raster(handle)) {
            return 0xFF;
        }
        const auto found = map_rasters_.find(handle);
        if (found == map_rasters_.end() || !found->second.ready) {
            return 6;
        }
        MapRasterState& state = found->second;
        for (std::size_t index = 0;
             index < state.samples_a.size(); ++index) {
            if (state.samples_a[index] ==
                static_cast<uint16_t>(key)) {
                state.samples_a[index] =
                    static_cast<uint16_t>(~state.samples_b[index]);
            }
        }
        state.dirty = true;
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::map<uint32_t, MapRasterState>&
    map_rasters() const {
        return map_rasters_;
    }
    uint32_t configure_single_bitmap_sprite(uint32_t handle, int32_t x, int32_t y,
                                        uint32_t bitmap, uint32_t blend_mode,
                                        uint32_t alpha, uint32_t priority) {
        if ((handle & 0xFF000000U) != 0x80000000U ||
            display_objects_.count(handle) == 0) {
            return 0xFFU;
        }
        const auto surface = graphics_surfaces_.find(bitmap);
        if (surface == graphics_surfaces_.end()) {
            return 1;
        }
        const uint32_t enabled = sprites_[handle].draw_enabled;
        set_object_position(handle, x, y);
        object_raw_fields_[{handle, 0xB0U}] = blend_mode;
        set_raw_field_recursive(handle, 0xB4U, alpha);
        set_object_priority_bias(handle, priority);
        object_raw_fields_[{handle, 0xA0U}] = surface->second.width;
        object_raw_fields_[{handle, 0xA4U}] = surface->second.height;
        sprites_[handle] = SpriteState{
            bitmap, x, y, blend_mode, alpha,
            object_raw_field(handle, 0x1CU), enabled, true};
        object_raw_fields_[{handle, 0x144U}] = 0;
        object_raw_fields_[{handle, 0x160U}] = bitmap;
        sprite_mode_configurations_.erase(handle);
        refresh_sprite_display_geometry(handle);
        display_dirty_ = true;
        return 0;
    }
    uint32_t add_sprite_input_rectangle(
        uint32_t handle, int32_t x, int32_t y,
        uint32_t width, uint32_t height) {
        const auto sprite = sprites_.find(handle);
        if (sprite == sprites_.end()) {
            return 0xFFU;
        }
        int32_t origin_x = sprite->second.x;
        int32_t origin_y = sprite->second.y;
        if (const auto offset = object_pair_3c_40_.find(handle);
            offset != object_pair_3c_40_.end()) {
            origin_x = static_cast<int32_t>(
                static_cast<uint32_t>(origin_x) +
                static_cast<uint32_t>(offset->second.first));
            origin_y = static_cast<int32_t>(
                static_cast<uint32_t>(origin_y) +
                static_cast<uint32_t>(offset->second.second));
        }
        if (const auto offset = object_pair_44_48_.find(handle);
            offset != object_pair_44_48_.end()) {
            origin_x = static_cast<int32_t>(
                static_cast<uint32_t>(origin_x) +
                static_cast<uint32_t>(offset->second.first));
            origin_y = static_cast<int32_t>(
                static_cast<uint32_t>(origin_y) +
                static_cast<uint32_t>(offset->second.second));
        }
        if (object_raw_field(handle, 0x4CU) != 0) {
            origin_x = static_cast<int32_t>(
                static_cast<uint32_t>(origin_x) +
                static_cast<uint32_t>(display_transform_origin_x_));
            origin_y = static_cast<int32_t>(
                static_cast<uint32_t>(origin_y) +
                static_cast<uint32_t>(display_transform_origin_y_));
        }
        const uint32_t origin_x_bits =
            static_cast<uint32_t>(origin_x);
        const uint32_t origin_y_bits =
            static_cast<uint32_t>(origin_y);
        const int32_t left = static_cast<int32_t>(
            origin_x_bits + static_cast<uint32_t>(x));
        const int32_t top = static_cast<int32_t>(
            origin_y_bits + static_cast<uint32_t>(y));
        const int32_t right = static_cast<int32_t>(
            origin_x_bits +
            (static_cast<uint32_t>(x) + width - 1U));
        const int32_t bottom = static_cast<int32_t>(
            origin_y_bits +
            (static_cast<uint32_t>(y) + height - 1U));
        if (right < left || bottom < top) {
            return 10;
        }
        // 0042DD90 -> 0044A350 submits to the current display manager
        // (00530E80)+14, not the mouse capture list (00542E54).
        // 0044A160 -> 004361F0 renders and clears that dirty-rectangle
        // queue. The host recomposes the complete canvas when dirty.
        display_dirty_ = true;
        return 0;
    }
    void set_sprite_draw_enabled(uint32_t handle, uint32_t enabled) {
        sprites_[handle].draw_enabled = enabled;
        set_base_draw_enabled(handle, enabled);
    }
    bool compose_single_bitmap_sprite(uint32_t destination, uint32_t handle);
    uint32_t set_sprite_bitmap(uint32_t handle, uint32_t bitmap) {
        const auto sprite = sprites_.find(handle);
        if (sprite == sprites_.end()) {
            return 0xFFU;
        }
        if (bitmap == 0xFFFFFFFFU) {
            object_raw_fields_[{handle, 0x148U}] = 0;
            object_raw_fields_[{handle, 0x14CU}] = 0xFFFFFFFFU;
            display_dirty_ = true;
            return 0;
        }
        const auto surface = graphics_surfaces_.find(bitmap);
        if (surface == graphics_surfaces_.end()) {
            return 1;
        }
        if (surface->second.format != 2U &&
            surface->second.format != 3U) {
            return 0;
        }
        // 0042D010 / PE64 1400646D0 changes the auxiliary image only.
        // Primary raster160 (and its host mirror) must survive -1 and binds.
        object_raw_fields_[{handle, 0x148U}] = 1;
        object_raw_fields_[{handle, 0x14CU}] = bitmap;
        object_raw_fields_[{handle, 0x150U}] = surface->second.allocation_token;
        display_dirty_ = true;
        return 0;
    }
    uint32_t rebind_sprite_bitmap(uint32_t handle, uint32_t bitmap) {
        const auto sprite = sprites_.find(handle);
        if (sprite == sprites_.end()) {
            return 0xFFU;
        }
        if (graphics_surfaces_.count(bitmap) == 0) {
            return 1;
        }
        sprite->second.bitmap = bitmap;
        object_raw_fields_[{handle, 0x160U}] = bitmap;
        refresh_sprite_display_geometry(handle);
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::map<uint32_t, SpriteState>& sprites() const {
        return sprites_;
    }
    struct GraphicsGroupRecord {
        std::string name;
        std::vector<uint8_t> bytes;
    };
    struct GraphicsGroupState {
        uint32_t record_size = 0;
        std::vector<GraphicsGroupRecord> records;
    };
    uint32_t create_graphics_group(uint32_t record_size) {
        ++graphics_group_counter_;
        graphics_groups_.emplace(
            graphics_group_counter_,
            GraphicsGroupState{record_size, {}});
        return graphics_group_counter_;
    }
    bool destroy_graphics_group(uint32_t id) {
        return graphics_groups_.erase(id) != 0;
    }
    bool write_graphics_group_record(
        uint32_t id, std::string name, std::span<const uint8_t> bytes) {
        const auto group = graphics_groups_.find(id);
        if (group == graphics_groups_.end() ||
            bytes.size() < group->second.record_size) {
            return false;
        }
        std::vector<uint8_t> copy(
            bytes.begin(),
            bytes.begin() +
                static_cast<std::ptrdiff_t>(group->second.record_size));
        const auto found = std::find_if(
            group->second.records.begin(), group->second.records.end(),
            [&name](const GraphicsGroupRecord& record) {
                return record.name == name;
            });
        if (found == group->second.records.end()) {
            group->second.records.push_back(
                {std::move(name), std::move(copy)});
        } else {
            found->bytes = std::move(copy);
        }
        return true;
    }
    bool delete_graphics_group_record(uint32_t id,
                                      const std::string& name) {
        const auto group = graphics_groups_.find(id);
        if (group == graphics_groups_.end()) {
            return false;
        }
        const auto previous = group->second.records.size();
        std::erase_if(
            group->second.records,
            [&name](const GraphicsGroupRecord& record) {
                return record.name == name;
            });
        return group->second.records.size() != previous;
    }
    bool read_graphics_group_record(
        uint32_t id, const std::optional<std::string>& name,
        uint32_t index, std::span<uint8_t> output) const {
        const auto group = graphics_groups_.find(id);
        if (group == graphics_groups_.end() ||
            output.size() < group->second.record_size) {
            return false;
        }
        const GraphicsGroupRecord* selected = nullptr;
        if (name.has_value()) {
            const auto found = std::find_if(
                group->second.records.begin(),
                group->second.records.end(),
                [&name](const GraphicsGroupRecord& record) {
                    return record.name == *name;
                });
            if (found == group->second.records.end()) {
                return false;
            }
            selected = &*found;
        } else {
            if (index >= group->second.records.size()) {
                return false;
            }
            selected = &group->second.records[index];
        }
        std::copy_n(
            selected->bytes.begin(), group->second.record_size,
            output.begin());
        return true;
    }
    [[nodiscard]] const std::map<uint32_t, GraphicsGroupState>&
    graphics_groups() const {
        return graphics_groups_;
    }
    void set_graphics_group_property(uint32_t key, uint32_t value) {
        // 0x0044C320/0x0044C340 consume the two VM values, then
        // 0x004477A0 broadcasts through the live CDspObjWindow slots.
        graphics_group_property_key_ = key;
        graphics_group_property_value_ = value;
        display_dirty_ = true;
    }
    [[nodiscard]] uint32_t graphics_group_property_key() const {
        return graphics_group_property_key_;
    }
    [[nodiscard]] uint32_t graphics_group_property_value() const {
        return graphics_group_property_value_;
    }
    [[nodiscard]] uint32_t window_effect_alpha(
        uint32_t window) const {
        const auto found = window_objects_.find(window);
        if (found == window_objects_.end()) {
            return 0;
        }
        const uint32_t mode = object_raw_field(window, 0xB0U);
        const uint32_t brightness = object_visibility_threshold(window);
        const uint32_t effect = object_effect_level(window);
        const uint32_t gate = object_raw_field(window, 0xBCU);
        switch (mode) {
            case 1:
            case 0x20:
            case 0x21:
            case 0x22:
            case 0x23:
            case 0x24:
                return 0x100U -
                       (((0x100U - brightness) *
                         (0x100U - effect) * gate) >> 16U);
            case 2:
            case 3:
            case 4:
            case 0xC0:
            case 0xC1:
                return ((0x100U - brightness) * effect * gate) >> 16U;
            default:
                return effect;
        }
    }
    [[nodiscard]] uint32_t window_composite_alpha(
        uint32_t window) const {
        if (graphics_group_property_key_ == 0) {
            return 0x100U;
        }
        const uint32_t effect = window_effect_alpha(window);
        return 0x100U -
               (((0x100U - effect) *
                 (0x100U - graphics_group_property_value_)) >> 8U);
    }
    struct GraphicsSurface {
        uint32_t height = 0;
        uint32_t width = 0;
        uint32_t format = 0;
        uint32_t bytes_per_pixel = 0;
        uint32_t stride = 0;
        uint32_t allocation_token = 0xFFFFFFFFU; // bitmap manager entry +1C
        PixelBuffer encoded;
        std::string archive;
        std::string resource;
        PixelBuffer rgba;
        PixelBuffer pixels;
        bool defer_rgba_mirror=false; // host-only compositor scratch
    };
    struct MovieSlotState {
        std::string directory;
        std::string resource;
        std::string resolved_path;
        uint32_t field_48 = 0;
        uint32_t field_4c = 0;
        uint32_t field_50 = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t duration_ms = 0;
        uint32_t position_ms = 0;
        uint32_t volume = 0;
        bool media_open = false;
        bool started = false;
        bool paused = false;
        bool playing = false;
    };
    std::optional<std::string> resolve_movie_source(
        const std::string& directory,
        const std::string& resource) const {
        std::vector<std::string> candidates;
        if (!directory.empty()) {
            const auto mounted = external_mounts_.find(directory);
            if (mounted != external_mounts_.end()) {
                candidates.push_back(
                    mounted->second +
                    (mounted->second.ends_with('/') ? "" : "/") +
                    resource);
            }
            candidates.push_back(directory + "/" + resource);
        }
        if (!resource.empty()) {
            candidates.push_back(resource);
        }
        for (const std::string& candidate : candidates) {
            if (read_virtual_file(candidate).has_value()) {
                return candidate;
            }
        }
        if (resource_loader_ && !resource.empty()) {
            try {
                const std::vector<uint8_t> bytes =
                    resource_loader_(directory, resource);
                if (!bytes.empty()) {
                    return resource;
                }
            } catch (const std::exception&) {
                // A missing archive/resource follows the target's status 1.
            }
        }
        if (file_size_resolver_ &&
            file_size_resolver_(directory, resource) != 0) {
            return resource;
        }
        return std::nullopt;
    }
    uint32_t open_movie_slot(
        uint32_t slot, std::string directory, std::string resource,
        uint32_t field_48 = 0, uint32_t field_4c = 0,
        uint32_t field_50 = 0,
        uint32_t width = 0, uint32_t height = 0) {
        if (slot >= 0x4000U) {
            return 4;
        }
        const std::optional<std::string> resolved =
            resolve_movie_source(directory, resource);
        if (!resolved) {
            movie_slots_.erase(slot);
            return 1;
        }
        MovieSlotState state;
        state.directory = std::move(directory);
        state.resource = std::move(resource);
        state.resolved_path = *resolved;
        state.field_48 = field_48;
        state.field_4c = field_4c;
        state.field_50 = field_50;
        state.width = width;
        state.height = height;
        state.media_open = true;
        movie_slots_[slot] = std::move(state);
        graphics_surfaces_.erase(slot);
        return 0;
    }
    uint32_t start_movie_slot(uint32_t slot, uint32_t duration_output,
                              uint32_t base) {
        if (slot >= 0x4000U) {
            return 4;
        }
        const auto found = movie_slots_.find(slot);
        if (found == movie_slots_.end() ||
            !found->second.media_open) {
            return 1;
        }
        found->second.started = true;
        if (duration_output != 0) {
            memory_.write_u32(
                duration_output, base, found->second.duration_ms);
        }
        return 0;
    }
    uint32_t set_movie_slot_paused(uint32_t slot, bool paused) {
        if (slot >= 0x4000U) {
            return 4;
        }
        const auto found = movie_slots_.find(slot);
        if (found == movie_slots_.end() ||
            !found->second.media_open) {
            return 1;
        }
        if (!found->second.started) {
            return 4;
        }
        found->second.paused = paused;
        found->second.playing = !paused;
        return 0;
    }
    uint32_t play_movie_slot(uint32_t slot, int32_t position_ms) {
        if (slot >= 0x4000U) {
            return 4;
        }
        const auto found = movie_slots_.find(slot);
        if (found == movie_slots_.end() ||
            !found->second.media_open) {
            return 1;
        }
        if (position_ms >= 0) {
            found->second.position_ms =
                static_cast<uint32_t>(position_ms);
        }
        found->second.paused = false;
        found->second.playing = true;
        return 0;
    }
    uint32_t query_movie_slot_position(
        uint32_t slot, uint32_t position_output, uint32_t base) {
        if (slot >= 0x4000U) {
            return 4;
        }
        const auto found = movie_slots_.find(slot);
        if (found == movie_slots_.end() ||
            !found->second.media_open) {
            return 1;
        }
        if (position_output != 0) {
            memory_.write_u32(
                position_output, base, found->second.position_ms);
        }
        return 0;
    }
    uint32_t set_movie_slot_volume(uint32_t slot, uint32_t volume) {
        if (volume > 0x80U) {
            return 3;
        }
        if (slot >= 0x4000U) {
            return 4;
        }
        const auto found = movie_slots_.find(slot);
        if (found == movie_slots_.end() ||
            !found->second.media_open) {
            return 1;
        }
        found->second.volume = volume;
        return 0;
    }
    void update_movie_slot_clock(uint32_t slot, uint32_t position_ms,
                                 uint32_t duration_ms, bool playing) {
        const auto found = movie_slots_.find(slot);
        if (found == movie_slots_.end()) {
            return;
        }
        found->second.position_ms = position_ms;
        found->second.duration_ms = duration_ms;
        found->second.playing = playing;
    }
    void install_movie_frame(uint32_t slot, uint32_t width,
                             uint32_t height,
                             std::span<const uint8_t> rgba) {
        if (slot >= 0x4000U ||
            rgba.size() !=
                static_cast<std::size_t>(width) * height * 4U) {
            return;
        }
        GraphicsSurface surface;
        surface.width = width;
        surface.height = height;
        surface.format = 2;
        surface.bytes_per_pixel = 4;
        surface.stride = width * 4U;
        surface.rgba.assign(rgba.begin(), rgba.end());
        surface.pixels.resize(rgba.size());
        for (std::size_t pixel = 0;
             pixel < static_cast<std::size_t>(width) * height;
             ++pixel) {
            surface.pixels[pixel * 4U] = rgba[pixel * 4U + 2U];
            surface.pixels[pixel * 4U + 1U] = rgba[pixel * 4U + 1U];
            surface.pixels[pixel * 4U + 2U] = rgba[pixel * 4U];
            surface.pixels[pixel * 4U + 3U] = rgba[pixel * 4U + 3U];
        }
        graphics_surfaces_[slot] = std::move(surface);
        display_dirty_ = true;
    }
    [[nodiscard]] const std::map<uint32_t, MovieSlotState>&
    movie_slots() const {
        return movie_slots_;
    }
    uint32_t create_global_movie(
        std::string directory, std::string resource,
        uint32_t field_48, uint32_t field_4c,
        uint32_t width, uint32_t height) {
        const std::optional<std::string> resolved =
            resolve_movie_source(directory, resource);
        if (!resolved) {
            global_movie_.reset();
            return 0;
        }
        global_movie_ = MovieSlotState{};
        global_movie_->directory = std::move(directory);
        global_movie_->resource = std::move(resource);
        global_movie_->resolved_path = *resolved;
        global_movie_->field_48 = field_48;
        global_movie_->field_4c = field_4c;
        global_movie_->width = width;
        global_movie_->height = height;
        global_movie_->media_open = true;
        global_movie_->started = true;
        global_movie_->playing = true;
        const uint32_t handle = 0x50000000U | next_global_movie_id_++;
        global_movie_handle_ = handle;
        return handle;
    }
    void reset_global_movie() {
        global_movie_.reset();
        global_movie_handle_ = 0;
    }
    [[nodiscard]] bool global_movie_playing() const {
        return global_movie_ &&
               global_movie_->media_open &&
               global_movie_->started &&
               global_movie_->playing &&
               global_movie_->position_ms <
                   global_movie_->duration_ms;
    }
    void update_global_movie_clock(
        uint32_t position_ms, uint32_t duration_ms, bool playing) {
        if (!global_movie_) {
            return;
        }
        global_movie_->position_ms = position_ms;
        global_movie_->duration_ms = duration_ms;
        global_movie_->playing = playing;
    }
    void set_global_movie_volume(uint32_t volume) {
        if (global_movie_) {
            global_movie_->volume = volume;
        }
    }
    [[nodiscard]] const std::optional<MovieSlotState>&
    global_movie() const {
        return global_movie_;
    }
    [[nodiscard]] uint32_t global_movie_handle() const {
        return global_movie_handle_;
    }
    struct WaveTableState {
        bool active = false;
        uint32_t field_1c = 0;
        uint32_t field_20 = 0;
        uint32_t row_dwords = 0;
        uint32_t total_dwords = 0;
        std::vector<uint32_t> samples;
    };
    static uint32_t truncate_wave_sample(double value) {
        if (!std::isfinite(value) ||
            value >= 2147483648.0 ||
            value < -2147483648.0) {
            return 0x80000000U;
        }
        return static_cast<uint32_t>(static_cast<int32_t>(value));
    }
    static uint32_t packed_wave_sample(
        uint32_t index, uint32_t width, int32_t amplitude,
        uint32_t divisor = 1U) {
        const double angle =
            static_cast<double>(index) *
            (std::numbers::pi_v<double> * 2.0) /
            static_cast<double>(width);
        const uint16_t value = static_cast<uint16_t>(
            truncate_wave_sample(
                std::sin(angle) * static_cast<double>(amplitude) /
                static_cast<double>(divisor)));
        return static_cast<uint32_t>(value) |
               (static_cast<uint32_t>(value) << 16U);
    }
    bool configure_compact_wave_table(
        uint32_t slot, uint32_t cycles, int32_t amplitude,
        uint32_t zero_rows_after_cycle,
        uint32_t block_count) {
        if (slot >= 8U) {
            return false;
        }
        cycles = cycles == 0 ? 1U : cycles;
        zero_rows_after_cycle =
            zero_rows_after_cycle == 0 ? 1U : zero_rows_after_cycle;
        block_count = block_count == 0 ? 1U : block_count;
        const uint64_t width =
            static_cast<uint64_t>(cycles) * 4U;
        const uint64_t row_dwords =
            width * zero_rows_after_cycle;
        const uint64_t total_dwords = row_dwords * block_count;
        if (width == 0 ||
            width > std::numeric_limits<uint32_t>::max() ||
            row_dwords > std::numeric_limits<uint32_t>::max() ||
            total_dwords > std::numeric_limits<uint32_t>::max() ||
            total_dwords >
                static_cast<uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
            throw std::runtime_error(
                "compact wave table dimensions overflow");
        }
        WaveTableState table;
        table.active = true;
        table.row_dwords = static_cast<uint32_t>(row_dwords);
        table.total_dwords = static_cast<uint32_t>(total_dwords);
        table.samples.reserve(static_cast<std::size_t>(total_dwords));
        const uint32_t sample_count = static_cast<uint32_t>(width);
        for (uint32_t block = 0; block < block_count; ++block) {
            for (uint32_t index = 0; index < sample_count; ++index) {
                table.samples.push_back(
                    packed_wave_sample(index, sample_count, amplitude));
            }
            for (uint32_t row = 1;
                 row < zero_rows_after_cycle; ++row) {
                table.samples.insert(
                    table.samples.end(), sample_count, 0);
            }
        }
        wave_tables_[slot] = std::move(table);
        return true;
    }
    bool configure_staged_wave_table(
        uint32_t slot, uint32_t cycles, int32_t amplitude,
        uint32_t lead_rows, uint32_t trail_rows,
        uint32_t outer_count, uint32_t spacing) {
        if (slot >= 8U) {
            return false;
        }
        cycles = cycles == 0 ? 1U : cycles;
        outer_count = outer_count == 0 ? 1U : outer_count;
        spacing = spacing == 0 ? 1U : spacing;
        const uint64_t width =
            static_cast<uint64_t>(cycles) * 4U;
        const uint64_t base_rows =
            static_cast<uint64_t>(lead_rows) + trail_rows + 1U;
        const uint64_t base_sequence_dwords = width * base_rows;
        const uint64_t row_dwords =
            base_sequence_dwords * spacing;
        const uint64_t total_dwords = row_dwords * outer_count;
        if (width == 0 ||
            width > std::numeric_limits<uint32_t>::max() ||
            base_sequence_dwords >
                std::numeric_limits<uint32_t>::max() ||
            row_dwords > std::numeric_limits<uint32_t>::max() ||
            total_dwords > std::numeric_limits<uint32_t>::max() ||
            total_dwords >
                static_cast<uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
            throw std::runtime_error(
                "staged wave table dimensions overflow");
        }
        WaveTableState table;
        table.active = true;
        table.row_dwords = static_cast<uint32_t>(row_dwords);
        table.total_dwords = static_cast<uint32_t>(total_dwords);
        table.samples.reserve(static_cast<std::size_t>(total_dwords));
        const uint32_t sample_count = static_cast<uint32_t>(width);
        for (uint32_t outer = 0; outer < outer_count; ++outer) {
            const std::size_t zero_count =
                static_cast<std::size_t>(spacing - 1U) *
                static_cast<std::size_t>(base_sequence_dwords);
            table.samples.insert(
                table.samples.end(), zero_count, 0);
            uint32_t level = lead_rows;
            for (uint32_t row = 0; row < lead_rows;
                 ++row, --level) {
                const uint32_t divisor = 1U << (level & 31U);
                for (uint32_t index = 0; index < sample_count; ++index) {
                    table.samples.push_back(packed_wave_sample(
                        index, sample_count, amplitude, divisor));
                }
            }
            for (uint32_t index = 0; index < sample_count; ++index) {
                table.samples.push_back(packed_wave_sample(
                    index, sample_count, amplitude));
            }
            uint32_t divisor = 1U;
            for (uint32_t row = 0; row < trail_rows; ++row) {
                divisor =
                    (divisor << 1U) | (divisor >> 31U);
                for (uint32_t index = 0; index < sample_count; ++index) {
                    table.samples.push_back(packed_wave_sample(
                        index, sample_count, amplitude, divisor));
                }
            }
        }
        wave_tables_[slot] = std::move(table);
        return true;
    }
    [[nodiscard]] const std::array<WaveTableState, 8>&
    wave_tables() const {
        return wave_tables_;
    }
    static uint32_t bitmap_bytes_per_pixel(uint32_t format) {
        static constexpr std::array<uint32_t, 8> sizes{
            2, 4, 4, 1, 4, 4, 6, 4,
        };
        return format < sizes.size() ? sizes[format] : 0;
    }
    static std::optional<uint32_t> bitmap_format_from_header(
        std::span<const uint8_t> data) {
        if (data.size() < 16) {
            return std::nullopt;
        }
        const uint16_t bits = static_cast<uint16_t>(
            static_cast<uint16_t>(data[4]) |
            static_cast<uint16_t>(
                static_cast<uint16_t>(data[5]) << 8U));
        const uint16_t selector = static_cast<uint16_t>(
            static_cast<uint16_t>(data[8]) |
            static_cast<uint16_t>(
                static_cast<uint16_t>(data[9]) << 8U));
        switch (bits) {
            case 8: return 3;
            case 0x10: return 0;
            case 0x18: return 1;
            case 0x30: return 6;
            case 0x20:
                if (selector == 4 || selector == 5 ||
                    selector == 7) {
                    return selector;
                }
                return 2;
            default: return std::nullopt;
        }
    }
    static std::vector<uint8_t> bitmap_internal_pixels(
        uint32_t format, uint32_t width, uint32_t height,
        std::span<const uint8_t> source) {
        const uint32_t destination_size =
            bitmap_bytes_per_pixel(format);
        const std::size_t pixel_count =
            static_cast<std::size_t>(width) * height;
        if (format == 1U) {
            std::vector<uint8_t> pixels(pixel_count * 4U);
            for (std::size_t index = 0; index < pixel_count; ++index) {
                pixels[index * 4U] = source[index * 3U];
                pixels[index * 4U + 1U] = source[index * 3U + 1U];
                pixels[index * 4U + 2U] = source[index * 3U + 2U];
                pixels[index * 4U + 3U] = 0;
            }
            return pixels;
        }
        const std::size_t size =
            pixel_count * destination_size;
        return std::vector<uint8_t>(source.begin(), source.begin() +
            static_cast<std::ptrdiff_t>(size));
    }
    // 00408430 initializes manager+10/+14 to zero. 004089B0 (Grp0:0B)
    // controls whether 00408BE0 retains an occupied slot's allocation token.
    void set_bitmap_token_reuse(uint32_t value) { bitmap_token_reuse_ = value; }
    [[nodiscard]] uint32_t bitmap_token_reuse() const { return bitmap_token_reuse_; }
    [[nodiscard]] uint32_t graphics_bitmap_token(uint32_t bitmap) const {
        const auto found = graphics_surfaces_.find(bitmap);
        return found == graphics_surfaces_.end()
            ? 0xFFFFFFFFU : found->second.allocation_token;
    }
    uint32_t allocate_bitmap_token(uint32_t bitmap) {
        const uint32_t previous = graphics_bitmap_token(bitmap);
        return previous == 0xFFFFFFFFU || bitmap_token_reuse_ == 0U
            ? next_bitmap_token_++ : previous;
    }
    void install_allocated_graphics_surface(uint32_t bitmap, GraphicsSurface surface) {
        surface.allocation_token = allocate_bitmap_token(bitmap);
        graphics_surfaces_[bitmap] = std::move(surface);
    }
    void create_graphics_surface(uint32_t type, uint32_t height,
                                 uint32_t width, uint32_t format) {
        GraphicsSurface surface;
        surface.height = height;
        surface.width = width;
        surface.format = format;
        surface.bytes_per_pixel = bitmap_bytes_per_pixel(format);
        surface.stride = surface.bytes_per_pixel * width;
        install_allocated_graphics_surface(type, std::move(surface));
    }
    uint32_t split_palette_bitmap(uint32_t count, int32_t bitmap) {
        if (count == 0) {
            for (uint32_t index = 0; index < 0x100U; ++index) {
                graphics_surfaces_.erase(0xFF01U + index);
            }
            return 0;
        }
        if (count > 0xFFU) {
            return 1;
        }
        if (bitmap < 0) {
            return 2;
        }
        const auto source = graphics_surfaces_.find(
            static_cast<uint32_t>(bitmap));
        if (source == graphics_surfaces_.end()) {
            return 2;
        }
        if (source->second.width % count != 0) {
            return 3;
        }
        const uint32_t segment_width = source->second.width / count;
        if (segment_width == 0 || source->second.height == 0) {
            return 3;
        }
        for (uint32_t index = 0; index < count; ++index) {
            GraphicsSurface piece;
            piece.width = segment_width;
            piece.height = source->second.height;
            piece.format = source->second.format;
            piece.bytes_per_pixel = source->second.bytes_per_pixel;
            piece.stride = piece.width * piece.bytes_per_pixel;
            piece.pixels.resize(
                static_cast<std::size_t>(piece.stride) * piece.height);
            piece.rgba.resize(
                static_cast<std::size_t>(piece.width) *
                piece.height * 4U);
            for (uint32_t y = 0; y < piece.height; ++y) {
                const std::size_t source_row =
                    static_cast<std::size_t>(y) *
                    source->second.stride;
                const std::size_t source_x =
                    static_cast<std::size_t>(index) * piece.stride;
                const std::size_t destination_row =
                    static_cast<std::size_t>(y) * piece.stride;
                std::copy_n(
                    source->second.pixels.begin() +
                        static_cast<std::ptrdiff_t>(
                            source_row + source_x),
                    piece.stride,
                    piece.pixels.begin() +
                        static_cast<std::ptrdiff_t>(destination_row));
                if (source->second.rgba.size() >=
                    static_cast<std::size_t>(
                        source->second.width) *
                        source->second.height * 4U) {
                    const std::size_t source_rgba =
                        static_cast<std::size_t>(y) *
                            source->second.width * 4U +
                        static_cast<std::size_t>(index) *
                            piece.width * 4U;
                    const std::size_t destination_rgba =
                        static_cast<std::size_t>(y) *
                        piece.width * 4U;
                    std::copy_n(
                        source->second.rgba.begin() +
                            static_cast<std::ptrdiff_t>(source_rgba),
                        static_cast<std::size_t>(piece.width) * 4U,
                        piece.rgba.begin() +
                            static_cast<std::ptrdiff_t>(destination_rgba));
                }
            }
            graphics_surfaces_[0xFF01U + index] =
                std::move(piece);
        }
        display_dirty_ = true;
        return 0;
    }
    uint32_t transform_bitmap(uint32_t destination, uint32_t source,
                              uint32_t mode) {
        const auto input = graphics_surfaces_.find(source);
        if (input == graphics_surfaces_.end()) {
            return 10;
        }
        if (destination == source || destination >= 0x4000U ||
            input->second.format > 7U) {
            return 9;
        }
        GraphicsSurface output;
        output.width = input->second.width;
        output.height = input->second.height;
        output.format = input->second.format;
        output.bytes_per_pixel = input->second.bytes_per_pixel;
        output.stride = input->second.stride;
        output.pixels.resize(input->second.pixels.size());
        output.rgba.resize(input->second.rgba.size());

        const uint32_t source_stride = input->second.stride;
        const uint32_t destination_stride = output.stride;
        const uint32_t pixel_bytes =
            input->second.bytes_per_pixel;
        if (mode > 1U) {
            output.pixels.assign(output.pixels.size(), 0);
            output.rgba.assign(output.rgba.size(), 0);
            install_allocated_graphics_surface(destination, std::move(output));
            return 22;
        }
        for (uint32_t row = 0; row < output.height; ++row) {
            const uint32_t source_row = output.height - 1U - row;
            for (uint32_t column = 0; column < output.width; ++column) {
                const uint32_t source_column =
                    mode == 1 ? output.width - 1U - column : column;
                const std::size_t source_offset =
                    static_cast<std::size_t>(source_row) *
                        source_stride +
                    static_cast<std::size_t>(source_column) *
                        pixel_bytes;
                const std::size_t destination_offset =
                    static_cast<std::size_t>(row) *
                        destination_stride +
                    static_cast<std::size_t>(column) * pixel_bytes;
                std::copy_n(
                    input->second.pixels.begin() +
                        static_cast<std::ptrdiff_t>(source_offset),
                    pixel_bytes,
                    output.pixels.begin() +
                        static_cast<std::ptrdiff_t>(destination_offset));
                if (input->second.rgba.size() >=
                    static_cast<std::size_t>(output.width) *
                        output.height * 4U) {
                    const std::size_t source_rgba =
                        (static_cast<std::size_t>(source_row) *
                             output.width +
                         source_column) * 4U;
                    const std::size_t destination_rgba =
                        (static_cast<std::size_t>(row) *
                             output.width +
                         column) * 4U;
                    std::copy_n(
                        input->second.rgba.begin() +
                            static_cast<std::ptrdiff_t>(source_rgba),
                        4U,
                        output.rgba.begin() +
                            static_cast<std::ptrdiff_t>(destination_rgba));
                }
            }
        }
        install_allocated_graphics_surface(destination, std::move(output));
        display_dirty_ = true;
        return 0;
    }
    uint32_t crop_bitmap_to_slot(uint32_t type, uint32_t source,
                                 int32_t x, int32_t y, uint32_t width,
                                 uint32_t height) {
        // All three verified PE32 workers (004381D0/00438E10/00438BE0)
        // use a character-slot validator, as does PE64 1400734A0. The old
        // type<4000 implementation was not the original Grp2:98 behavior.
        // PE32 manager shims tag the incoming character according to the
        // current byte/Unicode mode before validation.
        const uint32_t character = type | ((!compact_64_opcodes_ &&
            text_character_mode_589d54_ == 1U) ? 0x80000000U : 0U);
        const bool positive = character >= 0xFF01U && character <= 0xFFFFU;
        const bool tagged = character >= 0x8000F001U && character <= 0x8000F7FFU;
        if (!positive && !tagged) return 6;
        const uint32_t slot = 0x8000F000U | (character & 0xFFU);
        // Source -1 releases the low-byte slot even when dimensions are zero.
        if (source == 0xFFFFFFFFU) {
            graphics_surfaces_.erase(slot);
            display_dirty_ = true;
            return 0;
        }
        const auto input = graphics_surfaces_.find(source);
        if (input == graphics_surfaces_.end()) {
            return 2;
        }
        if (width == 0 || height == 0) {
            return 7;
        }
        GraphicsSurface output;
        output.width = width;
        output.height = height;
        output.format = input->second.format;
        output.bytes_per_pixel = input->second.bytes_per_pixel;
        output.stride = output.bytes_per_pixel * width;
        output.pixels.assign(
            static_cast<std::size_t>(output.stride) * height, 0);
        output.rgba.assign(
            static_cast<std::size_t>(width) * height * 4U, 0);
        for (uint32_t row = 0; row < height; ++row) {
            const int64_t source_y =
                static_cast<int64_t>(y) + row;
            if (source_y < 0 ||
                source_y >= static_cast<int64_t>(input->second.height)) {
                continue;
            }
            for (uint32_t column = 0; column < width; ++column) {
                const int64_t source_x =
                    static_cast<int64_t>(x) + column;
                if (source_x < 0 ||
                    source_x >=
                        static_cast<int64_t>(input->second.width)) {
                    continue;
                }
                const std::size_t source_offset =
                    static_cast<std::size_t>(source_y) *
                        input->second.stride +
                    static_cast<std::size_t>(source_x) *
                        output.bytes_per_pixel;
                const std::size_t destination_offset =
                    static_cast<std::size_t>(row) * output.stride +
                    static_cast<std::size_t>(column) *
                        output.bytes_per_pixel;
                std::copy_n(
                    input->second.pixels.begin() +
                        static_cast<std::ptrdiff_t>(source_offset),
                    output.bytes_per_pixel,
                    output.pixels.begin() +
                        static_cast<std::ptrdiff_t>(destination_offset));
                if (input->second.rgba.size() >=
                    static_cast<std::size_t>(input->second.width) *
                        input->second.height * 4U) {
                    const std::size_t source_rgba =
                        (static_cast<std::size_t>(source_y) *
                             input->second.width +
                         static_cast<std::size_t>(source_x)) * 4U;
                    const std::size_t destination_rgba =
                        (static_cast<std::size_t>(row) * width +
                         column) * 4U;
                    std::copy_n(
                        input->second.rgba.begin() +
                            static_cast<std::ptrdiff_t>(source_rgba),
                        4U,
                        output.rgba.begin() +
                            static_cast<std::ptrdiff_t>(destination_rgba));
                }
            }
        }
        install_allocated_graphics_surface(slot, std::move(output));
        display_dirty_ = true;
        return 0;
    }
    void set_text_character_mode_589d54(uint32_t mode) {
        text_character_mode_589d54_ = mode == 1U ? 1U : 0U;
    }
    [[nodiscard]] uint32_t text_character_mode_589d54() const {
        return text_character_mode_589d54_;
    }
    bool create_graphics_surface_from_pixels(
        uint32_t type, uint32_t width, uint32_t height, uint32_t format,
        uint32_t source, uint32_t base) {
        if (type >= 0x4000U) {
            return false;
        }
        const uint32_t token = allocate_bitmap_token(type);
        graphics_surfaces_.erase(type);
        if (format > 7U) {
            return false;
        }
        if (format == 7U) {
            format = 1U;
        }
        const uint32_t bytes_per_pixel =
            bitmap_bytes_per_pixel(format);
        const uint32_t source_stride =
            (format == 1U ? 3U : bytes_per_pixel) * width;
        const uint64_t source_size =
            static_cast<uint64_t>(source_stride) * height;
        if (source_size > std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        const std::vector<uint8_t> source_pixels = memory_.read_bytes(
            source, static_cast<std::size_t>(source_size), base);

        GraphicsSurface surface;
        surface.width = width;
        surface.height = height;
        surface.format = format;
        surface.bytes_per_pixel = bytes_per_pixel;
        surface.stride = bytes_per_pixel * width;
        if (format == 1U) {
            const std::size_t pixel_count =
                static_cast<std::size_t>(width) * height;
            surface.pixels.resize(pixel_count * 4U);
            surface.rgba.resize(pixel_count * 4U);
            for (std::size_t index = 0; index < pixel_count; ++index) {
                const uint8_t blue = source_pixels[index * 3U];
                const uint8_t green = source_pixels[index * 3U + 1U];
                const uint8_t red = source_pixels[index * 3U + 2U];
                surface.pixels[index * 4U] = blue;
                surface.pixels[index * 4U + 1U] = green;
                surface.pixels[index * 4U + 2U] = red;
                surface.pixels[index * 4U + 3U] = 0;
                surface.rgba[index * 4U] = red;
                surface.rgba[index * 4U + 1U] = green;
                surface.rgba[index * 4U + 2U] = blue;
                surface.rgba[index * 4U + 3U] = 0xFF;
            }
        } else {
            surface.pixels = source_pixels;
            if (format == 2U &&
                source_pixels.size() % 4U == 0) {
                const std::size_t pixel_count =
                    source_pixels.size() / 4U;
                surface.rgba.resize(pixel_count * 4U);
                for (std::size_t index = 0; index < pixel_count; ++index) {
                    const uint8_t blue = source_pixels[index * 4U];
                    const uint8_t green = source_pixels[index * 4U + 1U];
                    const uint8_t red = source_pixels[index * 4U + 2U];
                    surface.rgba[index * 4U] = red;
                    surface.rgba[index * 4U + 1U] = green;
                    surface.rgba[index * 4U + 2U] = blue;
                    surface.rgba[index * 4U + 3U] =
                        source_pixels[index * 4U + 3U];
                }
            }
        }
        surface.allocation_token = token;
        graphics_surfaces_[type] = std::move(surface);
        return true;
    }
    static bool materialize_graphics_pixels(GraphicsSurface& surface) {
        if (!surface.pixels.empty()) {
            return true;
        }
        if (surface.encoded.empty() &&
            surface.bytes_per_pixel != 0) {
            surface.pixels.assign(
                static_cast<std::size_t>(surface.stride) *
                    surface.height,
                0);
            return true;
        }
        if (surface.encoded.size() < 16) {
            return false;
        }
        const std::size_t source_size =
            static_cast<std::size_t>(surface.width) * surface.height *
            (surface.format == 1U
                 ? 3U
                 : bitmap_bytes_per_pixel(surface.format));
        if (surface.encoded.size() - 16U < source_size) {
            return false;
        }
        surface.pixels = bitmap_internal_pixels(
            surface.format, surface.width, surface.height,
            std::span<const uint8_t>(
                surface.encoded).subspan(16, source_size));
        return true;
    }
    using CompressedBgJpegEncoder = std::function<
        std::optional<std::vector<uint8_t>>(
            const GraphicsSurface&, uint32_t)>;
    void set_compressed_bg_jpeg_encoder(
        CompressedBgJpegEncoder encoder) {
        compressed_bg_jpeg_encoder_ = std::move(encoder);
    }
    static uint8_t compressed_bg_prng_next(uint32_t& state) {
        const uint32_t original = state;
        const uint16_t low =
            static_cast<uint16_t>(original & 0xFFFFU);
        const uint32_t high = original >> 16U;
        const uint32_t low_product =
            static_cast<uint32_t>(low) * 0x4E35U;
        state = (low_product & 0xFFFFU) + 1U +
                original * 0x015A0000U +
                high * 0x4E350000U +
                (low_product & 0xFFFF0000U);
        const uint32_t result =
            (low_product >> 16U) - high * 0x31CBU +
            original * 0x015AU;
        return static_cast<uint8_t>((result & 0x7FFFU) & 0xFFU);
    }
    static std::vector<uint8_t> compressed_bg_rle(
        std::span<const uint8_t> input) {
        std::vector<uint8_t> output;
        std::size_t position = 0;
        bool literal = true;
        const auto append_length = [&output](std::size_t length) {
            do {
                uint8_t byte =
                    static_cast<uint8_t>(length & 0x7FU);
                length >>= 7U;
                if (length != 0) {
                    byte |= 0x80U;
                }
                output.push_back(byte);
            } while (length != 0);
        };
        while (position < input.size()) {
            const std::size_t start = position;
            if (literal) {
                while (position < input.size()) {
                    if (input[position] == 0 &&
                        position + 1U < input.size() &&
                        input[position + 1U] == 0) {
                        break;
                    }
                    ++position;
                }
                const std::size_t length = position - start;
                append_length(length);
                output.insert(
                    output.end(), input.begin() +
                        static_cast<std::ptrdiff_t>(start),
                    input.begin() +
                        static_cast<std::ptrdiff_t>(position));
                literal = false;
            } else {
                while (position < input.size() &&
                       input[position] == 0) {
                    ++position;
                }
                append_length(position - start);
                literal = true;
            }
        }
        return output;
    }
    struct CompressedBgHuffmanNode {
        bool active = false;
        uint32_t weight = 0;
        int32_t parent = -1;
        int32_t left = -1;
        int32_t right = -1;
    };
    static int32_t compressed_bg_huffman_root(
        const std::array<uint32_t, 256>& frequencies) {
        std::array<CompressedBgHuffmanNode, 511> nodes{};
        uint64_t total = 0;
        for (std::size_t symbol = 0; symbol < frequencies.size();
             ++symbol) {
            nodes[symbol].active = frequencies[symbol] != 0;
            nodes[symbol].weight = frequencies[symbol];
            nodes[symbol].left = static_cast<int32_t>(symbol);
            nodes[symbol].right = static_cast<int32_t>(symbol);
            total += frequencies[symbol];
        }
        if (total == 0) {
            return -1;
        }
        uint32_t count = 256;
        while (count < nodes.size()) {
            std::array<int32_t, 2> selected{-1, -1};
            for (int32_t& choice : selected) {
                int32_t best = -1;
                uint32_t weight = 0xFFFFFFFFU;
                for (uint32_t index = 0; index < count; ++index) {
                    if (nodes[index].active &&
                        nodes[index].weight < weight) {
                        best = static_cast<int32_t>(index);
                        weight = nodes[index].weight;
                    }
                }
                if (best < 0) {
                    return -1;
                }
                nodes[static_cast<std::size_t>(best)].active = false;
                choice = best;
            }
            const uint32_t weight =
                nodes[static_cast<std::size_t>(selected[0])].weight +
                nodes[static_cast<std::size_t>(selected[1])].weight;
            nodes[count].active = true;
            nodes[count].weight = weight;
            nodes[count].left = selected[0];
            nodes[count].right = selected[1];
            nodes[count].parent = -1;
            nodes[static_cast<std::size_t>(selected[0])].parent =
                static_cast<int32_t>(count);
            nodes[static_cast<std::size_t>(selected[1])].parent =
                static_cast<int32_t>(count);
            if (weight == total) {
                return static_cast<int32_t>(count);
            }
            ++count;
        }
        return -1;
    }
    static std::array<std::vector<uint8_t>, 256>
    compressed_bg_huffman_paths(
        const std::array<uint32_t, 256>& frequencies) {
        std::array<std::vector<uint8_t>, 256> paths;
        std::array<CompressedBgHuffmanNode, 511> nodes{};
        uint64_t total = 0;
        for (std::size_t symbol = 0; symbol < frequencies.size();
             ++symbol) {
            nodes[symbol].active = frequencies[symbol] != 0;
            nodes[symbol].weight = frequencies[symbol];
            nodes[symbol].left = static_cast<int32_t>(symbol);
            nodes[symbol].right = static_cast<int32_t>(symbol);
            total += frequencies[symbol];
        }
        if (total == 0) {
            return paths;
        }
        uint32_t count = 256;
        while (count < nodes.size()) {
            std::array<int32_t, 2> selected{-1, -1};
            for (int32_t& choice : selected) {
                int32_t best = -1;
                uint32_t weight = 0xFFFFFFFFU;
                for (uint32_t index = 0; index < count; ++index) {
                    if (nodes[index].active &&
                        nodes[index].weight < weight) {
                        best = static_cast<int32_t>(index);
                        weight = nodes[index].weight;
                    }
                }
                if (best < 0) {
                    return paths;
                }
                nodes[static_cast<std::size_t>(best)].active = false;
                choice = best;
            }
            const uint32_t weight =
                nodes[static_cast<std::size_t>(selected[0])].weight +
                nodes[static_cast<std::size_t>(selected[1])].weight;
            nodes[count].active = true;
            nodes[count].weight = weight;
            nodes[count].left = selected[0];
            nodes[count].right = selected[1];
            nodes[count].parent = -1;
            nodes[static_cast<std::size_t>(selected[0])].parent =
                static_cast<int32_t>(count);
            nodes[static_cast<std::size_t>(selected[1])].parent =
                static_cast<int32_t>(count);
            if (weight == total) {
                break;
            }
            ++count;
        }
        for (std::size_t symbol = 0; symbol < paths.size(); ++symbol) {
            if (frequencies[symbol] == 0) {
                continue;
            }
            int32_t node = static_cast<int32_t>(symbol);
            while (nodes[static_cast<std::size_t>(node)].parent >= 0) {
                const int32_t parent =
                    nodes[static_cast<std::size_t>(node)].parent;
                paths[symbol].push_back(
                    node == nodes[static_cast<std::size_t>(parent)].right
                        ? 1U
                        : 0U);
                node = parent;
            }
            std::reverse(paths[symbol].begin(), paths[symbol].end());
        }
        return paths;
    }
    struct CompressedBgDecoded {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t bits_per_pixel = 0;
        uint32_t format = 0;
        std::vector<uint8_t> pixels;
    };
    static std::optional<CompressedBgDecoded> decode_compressed_bg(
        std::span<const uint8_t> encoded,
        std::string* error = nullptr, std::string_view resource = {}) {
        std::string local_error;
        if (error == nullptr) error = &local_error;
        const auto fail =
            [error, encoded, resource](std::string_view reason)
            -> std::optional<CompressedBgDecoded> {
            const std::string message(reason);
            *error = message;
            // Unknown headers may be normal format probes. Warn only after
            // recognizing a compressed bitmap container.
            if (encoded.size() >= 13U && std::string_view(
                    reinterpret_cast<const char*>(encoded.data()),13U) == "CompressedBG_")
                warn_decode_failure("bitmap", message, encoded, resource);
            return std::nullopt;
        };
        if (encoded.size() >= 15U && std::string_view(
                reinterpret_cast<const char*>(encoded.data()),15U) ==
                std::string_view("CompressedBG_MT\0",15U)) {
            auto decoded=decode_compressed_bg_mt(encoded,error);
            if(!decoded)return fail(*error);
            return CompressedBgDecoded{decoded->width,decoded->height,
                decoded->bits_per_pixel,decoded->format,std::move(decoded->pixels)};
        }
        static constexpr std::string_view magic =
            "CompressedBG___";
        if (encoded.size() < 0x30U ||
            std::string_view(
                reinterpret_cast<const char*>(encoded.data()), 15) !=
                magic ||
            encoded[15] != 0) {
            return fail("header");
        }
        const auto word = [encoded](std::size_t offset) {
            return static_cast<uint16_t>(
                static_cast<uint16_t>(encoded[offset]) |
                (static_cast<uint16_t>(encoded[offset + 1U]) << 8U));
        };
        const auto dword = [encoded](std::size_t offset) {
            uint32_t value = 0;
            for (uint32_t byte = 0; byte < 4U; ++byte) {
                value |=
                    static_cast<uint32_t>(encoded[offset + byte])
                    << (byte * 8U);
            }
            return value;
        };
        const uint32_t width = word(0x10);
        const uint32_t height = word(0x12);
        const uint32_t bits = word(0x14);
        const uint32_t format = word(0x18);
        if (word(0x2E) == 2U) {
            auto decoded = decode_compressed_bg_v2(encoded, error);
            if (!decoded) return fail(*error);
            return CompressedBgDecoded{decoded->width, decoded->height,
                decoded->bits_per_pixel, decoded->format, std::move(decoded->pixels)};
        }
        const uint32_t rle_size = dword(0x20);
        const uint32_t frequency_size = dword(0x28);
        const uint32_t timestamp = dword(0x24);
        if (width == 0 || height == 0 ||
            (bits != 8U && bits != 24U && bits != 32U) ||
            word(0x2E) != 1U ||
            0x30U + static_cast<uint64_t>(frequency_size) >
                encoded.size()) {
            return fail("descriptor");
        }
        std::vector<uint8_t> frequency_bytes(
            encoded.begin() + 0x30,
            encoded.begin() + 0x30 +
                static_cast<std::ptrdiff_t>(frequency_size));
        uint32_t seed = timestamp;
        for (uint8_t& byte : frequency_bytes) {
            byte = static_cast<uint8_t>(
                byte - compressed_bg_prng_next(seed));
        }
        uint8_t frequency_sum = 0;
        uint8_t frequency_parity = 0;
        for (const uint8_t byte : frequency_bytes) {
            frequency_sum =
                static_cast<uint8_t>(frequency_sum + byte);
            frequency_parity ^= byte;
        }
        if ((frequency_sum != encoded[0x2C] ||
             frequency_parity != encoded[0x2D]) &&
            !allow_content_check_failure("CompressedBG frequency checksum mismatch")) {
            return fail("frequency checksum");
        }
        std::array<uint32_t, 256> frequencies{};
        std::size_t frequency_position = 0;
        for (uint32_t& frequency : frequencies) {
            uint32_t shift = 0;
            bool complete = false;
            while (frequency_position < frequency_bytes.size() &&
                   shift < 35U) {
                const uint8_t byte =
                    frequency_bytes[frequency_position++];
                frequency |=
                    static_cast<uint32_t>(byte & 0x7FU) << shift;
                shift += 7U;
                if ((byte & 0x80U) == 0) {
                    complete = true;
                    break;
                }
            }
            if (!complete) {
                return fail("frequency varint");
            }
        }
        if (frequency_position != frequency_bytes.size()) {
            return fail("frequency length");
        }
        uint64_t frequency_total = 0;
        for (const uint32_t frequency : frequencies) {
            frequency_total += frequency;
        }
        if (frequency_total != rle_size) {
            return fail(
                "frequency total expected=" +
                std::to_string(rle_size) + " decoded=" +
                std::to_string(frequency_total));
        }
        struct DecodeNode {
            bool active = false;
            bool has_children = false;
            uint32_t weight = 0;
            int32_t child[2]{-1, -1};
            int32_t symbol = -1;
        };
        std::array<DecodeNode, 511> nodes{};
        uint64_t total_frequency = 0;
        uint32_t used_symbols = 0;
        uint8_t sole_symbol = 0;
        for (uint32_t symbol = 0;
             symbol < frequencies.size(); ++symbol) {
            DecodeNode& node = nodes[symbol];
            node.weight = frequencies[symbol];
            node.active = frequencies[symbol] != 0;
            node.symbol = static_cast<int32_t>(symbol);
            total_frequency += frequencies[symbol];
            if (node.active) {
                ++used_symbols;
                sole_symbol = static_cast<uint8_t>(symbol);
            }
        }
        if (used_symbols == 0 || total_frequency == 0) {
            return fail("empty tree");
        }
        uint32_t node_count = 256;
        int32_t root = -1;
        while (node_count < nodes.size()) {
            std::array<int32_t, 2> selected{-1, -1};
            for (int32_t& choice : selected) {
                uint32_t best_weight = 0xFFFFFFFFU;
                for (uint32_t index = 0; index < node_count; ++index) {
                    if (nodes[index].active &&
                        nodes[index].weight < best_weight) {
                        choice = static_cast<int32_t>(index);
                        best_weight = nodes[index].weight;
                    }
                }
                if (choice < 0) {
                    return fail("huffman nodes");
                }
                nodes[static_cast<std::size_t>(choice)].active = false;
            }
            DecodeNode& node = nodes[node_count];
            node.active = true;
            node.has_children = true;
            node.weight =
                nodes[static_cast<std::size_t>(selected[0])].weight +
                nodes[static_cast<std::size_t>(selected[1])].weight;
            node.child[0] = selected[0];
            node.child[1] = selected[1];
            root = static_cast<int32_t>(node_count);
            if (node.weight == total_frequency) {
                break;
            }
            ++node_count;
        }
        if (root < 0) {
            return fail("huffman root");
        }
        const std::size_t bitstream_start =
            0x30U + frequency_size;
        std::vector<uint8_t> rle;
        rle.reserve(rle_size);
        std::array<uint32_t, 256> decoded_frequencies{};
        std::size_t bit_position = 0;
        while (rle.size() < rle_size) {
            if (used_symbols == 1U) {
                rle.push_back(sole_symbol);
                ++decoded_frequencies[sole_symbol];
                continue;
            }
            int32_t node = root;
            while (nodes[static_cast<std::size_t>(node)].has_children) {
                const std::size_t byte_index =
                    bitstream_start + bit_position / 8U;
                if (byte_index >= encoded.size()) {
                    if (error != nullptr) {
                        *error =
                            "bitstream end target=" +
                            std::to_string(rle_size) + " decoded=" +
                            std::to_string(rle.size()) + " bits=" +
                            std::to_string(bit_position) + " available=" +
                            std::to_string(
                                encoded.size() - bitstream_start);
                    }
                    return std::nullopt;
                }
                const uint8_t bit = static_cast<uint8_t>(
                    (encoded[byte_index] >>
                     (7U - (bit_position & 7U))) &
                    1U);
                ++bit_position;
                node =
                    nodes[static_cast<std::size_t>(node)].child[bit];
                if (node < 0) {
                    return fail("bit path");
                }
            }
            const uint8_t symbol = static_cast<uint8_t>(
                nodes[static_cast<std::size_t>(node)].symbol);
            rle.push_back(symbol);
            ++decoded_frequencies[symbol];
        }
        for (std::size_t symbol = 0;
             symbol < frequencies.size(); ++symbol) {
            if (decoded_frequencies[symbol] != frequencies[symbol]) {
                return fail(
                    "huffman frequency symbol=" +
                    std::to_string(symbol) + " expected=" +
                    std::to_string(frequencies[symbol]) +
                    " decoded=" +
                    std::to_string(decoded_frequencies[symbol]));
            }
        }
        const auto encoded_paths =
            compressed_bg_huffman_paths(frequencies);
        std::vector<uint8_t> rounded_bitstream(
            (bit_position + 7U) / 8U, 0);
        std::size_t verify_position = 0;
        for (const uint8_t symbol : rle) {
            for (const uint8_t bit : encoded_paths[symbol]) {
                if (bit != 0) {
                    rounded_bitstream[verify_position / 8U] |=
                        static_cast<uint8_t>(
                            0x80U >> (verify_position & 7U));
                }
                ++verify_position;
            }
        }
        for (std::size_t byte = 0;
             byte < rounded_bitstream.size(); ++byte) {
            const uint8_t source =
                encoded[bitstream_start + byte];
            uint8_t mask = 0xFFU;
            if (byte + 1U == rounded_bitstream.size() &&
                (bit_position & 7U) != 0) {
                mask = static_cast<uint8_t>(
                    0xFFU << (8U - (bit_position & 7U)));
            }
            if ((source & mask) !=
                (rounded_bitstream[byte] & mask)) {
                return fail(
                    "huffman bitstream mismatch byte=" +
                    std::to_string(byte) + " source=" +
                    std::to_string(source) + " encoded=" +
                    std::to_string(rounded_bitstream[byte]));
            }
        }
        const uint32_t bytes_per_pixel = bits / 8U;
        const std::size_t raw_size =
            static_cast<std::size_t>(width) * height *
            bytes_per_pixel;
        std::vector<uint8_t> filtered;
        filtered.reserve(raw_size);
        std::size_t rle_position = 0;
        bool literal = true;
        while (filtered.size() < raw_size) {
            uint32_t length = 0;
            uint32_t shift = 0;
            bool complete = false;
            while (rle_position < rle.size() && shift < 35U) {
                const uint8_t byte = rle[rle_position++];
                length |= static_cast<uint32_t>(byte & 0x7FU) << shift;
                shift += 7U;
                if ((byte & 0x80U) == 0) {
                    complete = true;
                    break;
                }
            }
            if (!complete || length > raw_size - filtered.size()) {
                return fail(
                    "RLE length position=" +
                    std::to_string(rle_position) +
                    " length=" + std::to_string(length) +
                    " output=" + std::to_string(filtered.size()) +
                    " target=" + std::to_string(raw_size) +
                    " stream=" + std::to_string(rle_size));
            }
            if (literal) {
                if (rle_position + length > rle.size()) {
                    return fail(
                        "RLE literal position=" +
                        std::to_string(rle_position) +
                        " length=" + std::to_string(length) +
                        " stream=" + std::to_string(rle.size()));
                }
                filtered.insert(
                    filtered.end(),
                    rle.begin() +
                        static_cast<std::ptrdiff_t>(rle_position),
                    rle.begin() + static_cast<std::ptrdiff_t>(
                        rle_position + length));
                rle_position += length;
            } else {
                filtered.insert(filtered.end(), length, 0);
            }
            literal = !literal;
        }
        const std::size_t row_bytes =
            static_cast<std::size_t>(width) * bytes_per_pixel;
        std::vector<uint8_t> pixels(filtered);
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < row_bytes; ++x) {
                const int32_t up =
                    y == 0
                        ? -1
                        : pixels[(y - 1U) * row_bytes + x];
                const int32_t left =
                    x < bytes_per_pixel
                        ? -1
                        : pixels[y * row_bytes + x - bytes_per_pixel];
                int32_t predictor = up;
                if (up < 0) {
                    predictor = left < 0 ? 0 : left;
                } else if (left >= 0) {
                    predictor = (up + left) >> 1;
                }
                pixels[y * row_bytes + x] =
                    static_cast<uint8_t>(
                        filtered[y * row_bytes + x] + predictor);
            }
        }
        return CompressedBgDecoded{
            width, height, bits, format, std::move(pixels)};
    }
    uint32_t encode_compressed_bg(
        uint32_t destination, uint32_t size_output, uint32_t bitmap,
        uint32_t mode, uint32_t parameter, uint32_t base) {
        const auto surface = graphics_surfaces_.find(bitmap);
        if (surface == graphics_surfaces_.end()) {
            return 0x8000000AU;
        }
        GraphicsSurface& input = surface->second;
        const uint64_t pixels =
            static_cast<uint64_t>(input.width) * input.height;
        if (pixels > 0x400000U) {
            return 0x80000006U;
        }
        if (mode > 1U) {
            return 0x80000017U;
        }
        if (mode == 1U) {
            if (parameter > 100U) {
                return 0x80000018U;
            }
            if (!compressed_bg_jpeg_encoder_) {
                return 0xFFFFFFFEU;
            }
            const auto encoded =
                compressed_bg_jpeg_encoder_(input, parameter);
            if (!encoded || encoded->empty()) {
                return 0xFFFFFFFEU;
            }
            if (destination != 0) {
                memory_.write_bytes(destination, *encoded, base);
            }
            memory_.write_u32(
                size_output, base,
                static_cast<uint32_t>(encoded->size()));
            return 0;
        }
        if (input.format != 1U && input.format != 2U) {
            return 0xFFFFFFFFU;
        }
        if (!materialize_graphics_pixels(input)) {
            return 0x80000008U;
        }
        const uint32_t bytes_per_pixel =
            input.format == 1U ? 3U : 4U;
        const std::size_t row_bytes =
            static_cast<std::size_t>(input.width) * bytes_per_pixel;
        std::vector<uint8_t> raw(
            static_cast<std::size_t>(pixels) * bytes_per_pixel);
        for (std::size_t pixel = 0;
             pixel < static_cast<std::size_t>(pixels); ++pixel) {
            for (uint32_t channel = 0; channel < bytes_per_pixel;
                 ++channel) {
                raw[pixel * bytes_per_pixel + channel] =
                    input.pixels[pixel * 4U + channel];
            }
        }
        std::vector<uint8_t> filtered(raw.size());
        for (std::size_t y = 0; y < input.height; ++y) {
            for (std::size_t x = 0; x < row_bytes; ++x) {
                const int32_t up =
                    y == 0
                        ? -1
                        : raw[(y - 1U) * row_bytes + x];
                const int32_t left =
                    x < bytes_per_pixel
                        ? -1
                        : raw[y * row_bytes + x - bytes_per_pixel];
                int32_t predictor = up;
                if (up < 0) {
                    predictor = left < 0 ? 0 : left;
                } else if (left >= 0) {
                    predictor = (up + left) >> 1;
                }
                filtered[y * row_bytes + x] =
                    static_cast<uint8_t>(
                        raw[y * row_bytes + x] - predictor);
            }
        }
        std::vector<uint8_t> rle = compressed_bg_rle(filtered);
        std::array<uint32_t, 256> frequencies{};
        for (const uint8_t byte : rle) {
            ++frequencies[byte];
        }
        const std::array<std::vector<uint8_t>, 256> paths =
            compressed_bg_huffman_paths(frequencies);
        std::vector<uint8_t> encoded_frequencies;
        for (uint32_t frequency : frequencies) {
            do {
                uint8_t byte =
                    static_cast<uint8_t>(frequency & 0x7FU);
                frequency >>= 7U;
                if (frequency != 0) {
                    byte |= 0x80U;
                }
                encoded_frequencies.push_back(byte);
            } while (frequency != 0);
        }
        std::vector<uint8_t> bitstream;
        uint8_t current = 0;
        uint32_t bit_count = 0;
        for (const uint8_t symbol : rle) {
            for (const uint8_t bit : paths[symbol]) {
                if (bit != 0) {
                    current |= static_cast<uint8_t>(
                        0x80U >> bit_count);
                }
                if (++bit_count == 8U) {
                    bitstream.push_back(current);
                    current = 0;
                    bit_count = 0;
                }
            }
        }
        if (bit_count != 0) {
            bitstream.push_back(current);
        }
        std::vector<uint8_t> output(
            0x30U + encoded_frequencies.size() + bitstream.size(), 0);
        static constexpr std::string_view magic =
            "CompressedBG___";
        std::copy(magic.begin(), magic.end(), output.begin());
        const auto write_word = [&output](
                                    std::size_t offset,
                                    uint16_t value) {
            output[offset] = static_cast<uint8_t>(value);
            output[offset + 1U] =
                static_cast<uint8_t>(value >> 8U);
        };
        write_word(0x10U, static_cast<uint16_t>(input.width));
        write_word(0x12U, static_cast<uint16_t>(input.height));
        write_word(0x14U,
                   static_cast<uint16_t>(bytes_per_pixel * 8U));
        write_word(0x18U, static_cast<uint16_t>(input.format));
        const auto write_u32_at = [&output](
                                      std::size_t offset,
                                      uint32_t value) {
            for (uint32_t byte = 0; byte < 4U; ++byte) {
                output[offset + byte] =
                    static_cast<uint8_t>(value >> (byte * 8U));
            }
        };
        const uint32_t timestamp =
            static_cast<uint32_t>(monotonic_ms());
        write_u32_at(0x20U, static_cast<uint32_t>(rle.size()));
        write_u32_at(0x24U, timestamp);
        write_u32_at(
            0x28U,
            static_cast<uint32_t>(encoded_frequencies.size()));
        std::copy(
            encoded_frequencies.begin(), encoded_frequencies.end(),
            output.begin() + 0x30);
        uint8_t sum = 0;
        uint8_t parity = 0;
        for (const uint8_t byte : encoded_frequencies) {
            sum = static_cast<uint8_t>(sum + byte);
            parity ^= byte;
        }
        output[0x2CU] = sum;
        output[0x2DU] = parity;
        write_word(0x2EU, 1);
        uint32_t seed = timestamp;
        for (std::size_t index = 0;
             index < encoded_frequencies.size(); ++index) {
            output[0x30U + index] = static_cast<uint8_t>(
                output[0x30U + index] +
                compressed_bg_prng_next(seed));
        }
        std::copy(bitstream.begin(), bitstream.end(),
                  output.begin() +
                      static_cast<std::ptrdiff_t>(
                          0x30U + encoded_frequencies.size()));
        if (destination != 0) {
            memory_.write_bytes(destination, output, base);
        }
        memory_.write_u32(
            size_output, base,
            static_cast<uint32_t>(output.size()));
        return 0;
    }
    uint32_t copy_bitmap_region(
        uint32_t destination_type, int32_t destination_x,
        int32_t destination_y, uint32_t source_type,
        int32_t source_x, int32_t source_y,
        uint32_t width, uint32_t height) {
        const auto destination =
            graphics_surfaces_.find(destination_type);
        if (destination == graphics_surfaces_.end()) {
            return 1;
        }
        const auto source = graphics_surfaces_.find(source_type);
        if (source == graphics_surfaces_.end()) {
            return 2;
        }
        if (width == 0 || height == 0) {
            return 3;
        }
        if (destination->second.format != source->second.format ||
            !materialize_graphics_pixels(destination->second) ||
            !materialize_graphics_pixels(source->second)) {
            return 0;
        }
        const int64_t source_left = source_x;
        const int64_t source_top = source_y;
        const int64_t source_right =
            source_left + static_cast<int64_t>(width) - 1;
        const int64_t source_bottom =
            source_top + static_cast<int64_t>(height) - 1;
        const int64_t destination_left = destination_x;
        const int64_t destination_top = destination_y;
        const int64_t destination_right =
            destination_left + static_cast<int64_t>(width) - 1;
        const int64_t destination_bottom =
            destination_top + static_cast<int64_t>(height) - 1;
        const int64_t left = std::max<int64_t>(
            source_left, 0);
        const int64_t top = std::max<int64_t>(
            source_top, 0);
        const int64_t right = std::min<int64_t>(
            source_right,
            static_cast<int64_t>(source->second.width) - 1);
        const int64_t bottom = std::min<int64_t>(
            source_bottom,
            static_cast<int64_t>(source->second.height) - 1);
        const int64_t clipped_left = std::max<int64_t>(
            left, -destination_left);
        const int64_t clipped_top = std::max<int64_t>(
            top, -destination_top);
        const int64_t destination_max_x =
            source_left +
            (static_cast<int64_t>(destination->second.width) - 1 -
             destination_left);
        const int64_t destination_max_y =
            source_top +
            (static_cast<int64_t>(destination->second.height) - 1 -
             destination_top);
        const int64_t copy_right = std::min<int64_t>(
            right, destination_max_x);
        const int64_t copy_bottom = std::min<int64_t>(
            bottom, destination_max_y);
        if (copy_right < clipped_left ||
            copy_bottom < clipped_top) {
            return 0;
        }
        const uint32_t bytes_per_pixel =
            source->second.bytes_per_pixel;
        if (bytes_per_pixel == 0 ||
            destination->second.stride <
                destination->second.width * bytes_per_pixel ||
            source->second.stride <
                source->second.width * bytes_per_pixel) {
            return 0;
        }
        const std::size_t row_bytes =
            static_cast<std::size_t>(
                copy_right - clipped_left + 1) *
            bytes_per_pixel;
        const int64_t destination_column =
            destination_left + (clipped_left - source_left);
        for (int64_t row = clipped_top; row <= copy_bottom; ++row) {
            const std::size_t source_offset =
                static_cast<std::size_t>(row) *
                    source->second.stride +
                static_cast<std::size_t>(clipped_left) *
                    bytes_per_pixel;
            const int64_t destination_row =
                destination_top + (row - source_top);
            const std::size_t destination_offset =
                static_cast<std::size_t>(destination_row) *
                    destination->second.stride +
                static_cast<std::size_t>(destination_column) *
                    bytes_per_pixel;
            if (source_offset + row_bytes >
                    source->second.pixels.size() ||
                destination_offset + row_bytes >
                    destination->second.pixels.size()) {
                return 0;
            }
            std::copy_n(
                source->second.pixels.begin() +
                    static_cast<std::ptrdiff_t>(source_offset),
                row_bytes,
                destination->second.pixels.begin() +
                    static_cast<std::ptrdiff_t>(
                        destination_offset));
        }
        destination->second.rgba.clear();
        display_dirty_ = true;
        return 0;
    }
    static uint8_t blend_channel(uint8_t destination, uint8_t source,
                                 int16_t factor) {
        const int16_t shifted = static_cast<int16_t>(
            static_cast<uint16_t>(
                static_cast<int32_t>(source) -
                static_cast<int32_t>(destination))
            << 4U);
        const int32_t adjustment =
            (static_cast<int32_t>(shifted) *
             static_cast<int32_t>(factor)) >> 16;
        const int32_t value =
            static_cast<int32_t>(destination) + adjustment;
        return static_cast<uint8_t>(std::clamp(value, 0, 255));
    }
    static std::array<int16_t, 256> masked_blend_lut(uint32_t mode) {
        const double odd = static_cast<double>(mode * 2U + 1U);
        const double period = 256.0 / odd;
        const double step = odd / 256.0;
        std::array<int16_t, 256> result{};
        for (uint32_t index = 0; index < result.size(); ++index) {
            const uint32_t block =
                static_cast<uint32_t>(
                    static_cast<double>(index) * step);
            double remainder =
                static_cast<double>(index) -
                static_cast<double>(block) * period;
            if ((block & 1U) != 0) {
                remainder = period - remainder;
            }
            result[index] = static_cast<int16_t>(
                static_cast<int>(remainder * odd) << 4);
        }
        return result;
    }
    uint32_t blend_bitmap_masked(
        uint32_t destination_type, int32_t x, int32_t y,
        uint32_t source_type, uint32_t mask_type, uint32_t mode,
        uint32_t opacity) {
        auto destination = graphics_surfaces_.find(destination_type);
        if (destination == graphics_surfaces_.end()) {
            return 1;
        }
        const auto source = graphics_surfaces_.find(source_type);
        if (source == graphics_surfaces_.end()) {
            return 2;
        }
        const auto mask = graphics_surfaces_.find(mask_type);
        GraphicsSurface& destination_surface = destination->second;
        if (!materialize_graphics_pixels(destination_surface)) {
            return 7;
        }
        GraphicsSurface source_surface = source->second;
        if (!materialize_graphics_pixels(source_surface)) {
            return 7;
        }
        if (mask == graphics_surfaces_.end()) {
            if (destination_surface.format != source_surface.format ||
                (destination_surface.format != 1U &&
                 destination_surface.format != 2U)) {
                return 0;
            }
            const int16_t factor = static_cast<int16_t>(opacity << 4U);
            const uint32_t destination_stride =
                destination_surface.bytes_per_pixel *
                destination_surface.width;
            const uint32_t source_stride =
                source_surface.bytes_per_pixel *
                source_surface.width;
            for (uint32_t source_y = 0;
                 source_y < source_surface.height; ++source_y) {
                const int32_t target_y = y +
                    static_cast<int32_t>(source_y);
                if (target_y < 0 ||
                    target_y >= static_cast<int32_t>(
                                      destination_surface.height)) {
                    continue;
                }
                for (uint32_t source_x = 0;
                     source_x < source_surface.width; ++source_x) {
                    const int32_t target_x = x +
                        static_cast<int32_t>(source_x);
                    if (target_x < 0 ||
                        target_x >= static_cast<int32_t>(
                                          destination_surface.width)) {
                        continue;
                    }
                    const std::size_t source_offset =
                        static_cast<std::size_t>(source_y) * source_stride +
                        static_cast<std::size_t>(source_x) * 4U;
                    const std::size_t destination_offset =
                        static_cast<std::size_t>(target_y) *
                            destination_stride +
                        static_cast<std::size_t>(target_x) * 4U;
                    for (std::size_t channel = 0; channel < 4; ++channel) {
                        destination_surface.pixels[
                            destination_offset + channel] =
                            blend_channel(
                                destination_surface.pixels[
                                    destination_offset + channel],
                                source_surface.pixels[
                                    source_offset + channel],
                                factor);
                    }
                }
            }
            return 0;
        }
        if (mask->second.format != 3U) {
            return 5;
        }
        if (source_surface.width != mask->second.width ||
            source_surface.height != mask->second.height) {
            return 6;
        }
        if (opacity > 0x100U) {
            return 8;
        }
        if (source_surface.format != destination_surface.format) {
            return 4;
        }
        if (source_surface.format != 1U) {
            return 0;
        }
        GraphicsSurface mask_surface = mask->second;
        if (!materialize_graphics_pixels(mask_surface)) {
            return 7;
        }
        static constexpr std::array<int16_t, 128> threshold_factors{
            19689, 29501, -6460, -27229, 31705, -11874, -16714, 14869,
            -15900, -5895, -22564, -26214, -1848, -20163, 4199, -11757,
            10965, 31893, 9172, -29069, 5315, -9874, 205, -17561,
            1968, -22217, -27957, 22417, -5802, 632, 23697, -22689,
            -22543, 1692, 32307, 27433, 29535, 30686, 8187, -16191,
            17038, -32743, 27186, -20280, -29799, -25543, -29668, -17037,
            -21969, -31780, 12940, 16268, -3420, 11695, -22430, -17623,
            -9427, -18043, 21310, 9665, -22891, 31689, 8140, -21970,
            -25097, 8881, 14454, 13283, 12111, 1825, -6124, 21379,
            -10530, -21476, 5227, -27132, -29862, 4551, -28811, -3687,
            30105, 16677, 31808, -13775, -8378, 18411, -25774, -8726,
            25183, -3120, 19688, 1555, -25936, 16948, 11105, -7908,
            19303, -17111, 1310, -29502, -1662, -4126, 815, -18512,
            28739, 18717, -15226, 24596, 7567, 11330, 30634, -5324,
            11973, 30850, -6470, -28549, 17120, -32615, 22986, 384,
            32663, -386, 6112, 16911, -24435, 28781, -30906, -4054,
        };
        const std::array<int16_t, 256> smooth_factors =
            masked_blend_lut(mode & 7U);
        const uint32_t destination_stride =
            destination_surface.bytes_per_pixel *
            destination_surface.width;
        const uint32_t source_stride =
            source_surface.bytes_per_pixel * source_surface.width;
        bool intersected = false;
        for (uint32_t source_y = 0;
             source_y < source_surface.height; ++source_y) {
            const int32_t target_y = y + static_cast<int32_t>(source_y);
            if (target_y < 0 ||
                target_y >= static_cast<int32_t>(
                                  destination_surface.height)) {
                continue;
            }
            for (uint32_t source_x = 0;
                 source_x < source_surface.width; ++source_x) {
                const int32_t target_x = x +
                    static_cast<int32_t>(source_x);
                if (target_x < 0 ||
                    target_x >= static_cast<int32_t>(
                                      destination_surface.width)) {
                    continue;
                }
                intersected = true;
                const std::size_t mask_offset =
                    static_cast<std::size_t>(source_y) *
                        mask_surface.stride +
                    source_x;
                const uint32_t mask_value =
                    mask_surface.pixels[mask_offset];
                int32_t threshold = 0;
                int16_t factor = 0;
                if (opacity < 8U) {
                    const uint32_t shift = opacity & 0x1FU;
                    threshold = static_cast<int32_t>(
                        (mask_value << shift) +
                        static_cast<uint32_t>(
                            (0x100 -
                             static_cast<int32_t>(mode << shift)) -
                            static_cast<int32_t>(mode)));
                    if (threshold <= 0) {
                        continue;
                    }
                    if (threshold >= 0x100) {
                        threshold = 0x100;
                    } else {
                        factor = threshold_factors[
                            static_cast<std::size_t>(threshold >> 1)];
                    }
                } else {
                    threshold = static_cast<int32_t>(mask_value) +
                        (0x80 - static_cast<int32_t>(opacity)) * 2;
                    if (threshold <= 0) {
                        continue;
                    }
                    if (threshold >= 0x100) {
                        threshold = 0x100;
                    } else {
                        factor = smooth_factors[
                            static_cast<std::size_t>(threshold)];
                    }
                }
                const std::size_t source_offset =
                    static_cast<std::size_t>(source_y) * source_stride +
                    static_cast<std::size_t>(source_x) * 4U;
                const std::size_t destination_offset =
                    static_cast<std::size_t>(target_y) *
                        destination_stride +
                    static_cast<std::size_t>(target_x) * 4U;
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    if (threshold >= 0x100) {
                        destination_surface.pixels[
                            destination_offset + channel] =
                            source_surface.pixels[
                                source_offset + channel];
                    } else {
                        destination_surface.pixels[
                            destination_offset + channel] =
                            blend_channel(
                                destination_surface.pixels[
                                    destination_offset + channel],
                                source_surface.pixels[
                                    source_offset + channel],
                                factor);
                    }
                }
            }
        }
        return intersected ? 0U : 7U;
    }
    [[nodiscard]] const std::map<uint32_t, GraphicsSurface>&
    graphics_surfaces() const {
        return graphics_surfaces_;
    }
    bool release_graphics_surface(uint32_t type) {
        return graphics_surfaces_.erase(type) != 0;
    }
    uint32_t release_graphics_texture(uint32_t type) {
        const bool released_surface =
            graphics_surfaces_.erase(type) != 0;
        const bool released_movie =
            movie_slots_.erase(type) != 0;
        return released_surface || released_movie ? 0U
                                                  : 0x80000004U;
    }
    bool query_graphics_texture(uint32_t type, uint32_t output,
                                uint32_t base) {
        if (graphics_surfaces_.count(type) == 0) {
            return false;
        }
        memory_.write_u32(output, base, 0x40000000U | type);
        return true;
    }
    bool load_bitmap_file(uint32_t type, std::string_view prefix,
                          std::string_view file) {
        if (type >= 0x4000U) {
            return false;
        }
        std::vector<std::string> candidates;
        if (!prefix.empty()) {
            const std::string base(prefix);
            candidates.push_back(
                base +
                (base.ends_with('/') || base.ends_with('\\') ? "" : "/") +
                std::string(file));
            candidates.push_back(
                base +
                (base.ends_with('/') || base.ends_with('\\') ? "" : "\\") +
                std::string(file));
        }
        candidates.emplace_back(file);
        std::optional<std::vector<uint8_t>> bytes;
        for (const std::string& candidate : candidates) {
            bytes = read_virtual_file(candidate);
            if (bytes) {
                break;
            }
        }
        if (!bytes || bytes->size() < 0x30U) {
            return false;
        }
        if (std::string_view(
                reinterpret_cast<const char*>(bytes->data()), 15U).starts_with(
            "CompressedBG_")) {
            const auto decoded = decode_compressed_bg(*bytes);
            if (!decoded || decoded->width == 0 ||
                decoded->height == 0 ||
                (decoded->bits_per_pixel != 8U &&
                 decoded->bits_per_pixel != 24U &&
                 decoded->bits_per_pixel != 32U)) {
                return false;
            }
            GraphicsSurface surface;
            surface.width = decoded->width;
            surface.height = decoded->height;
            surface.format = decoded->bits_per_pixel == 8U ? 3U :
                decoded->format == 1U || decoded->format == 2U || decoded->format == 7U
                    ? decoded->format
                    : (decoded->bits_per_pixel == 24U ? 1U : 2U);
            surface.bytes_per_pixel =
                bitmap_bytes_per_pixel(surface.format);
            surface.stride =
                surface.bytes_per_pixel * surface.width;
            // PE64 140042BD0 maps the decoded selector7 to manager
            // format1; 1400429C0/140041790 copy its already padded BGRX.
            // The decoder result itself must retain selector7.
            if (compact_64_opcodes_ && decoded->format == 7U) {
                surface.format = 1U;
                surface.pixels = decoded->pixels;
            } else {
                surface.pixels = bitmap_internal_pixels(
                    surface.format, surface.width, surface.height,
                    decoded->pixels);
            }
            refresh_bgra_mirror(surface);
            install_allocated_graphics_surface(type, std::move(surface));
            display_dirty_ = true;
            return true;
        }
        const auto read_word = [&bytes](std::size_t offset) {
            return static_cast<uint16_t>(
                static_cast<uint16_t>((*bytes)[offset]) |
                static_cast<uint16_t>(
                    static_cast<uint16_t>((*bytes)[offset + 1U])
                    << 8U));
        };
        const uint32_t width = read_word(0);
        const uint32_t height = read_word(2);
        if (width == 0 || height == 0 ||
            static_cast<uint64_t>(width) * height > 0x04000000U) {
            return false;
        }
        uint32_t format = 0;
        switch (read_word(4)) {
        case 8: format = 3; break;
        case 16: format = 0; break;
        case 24: format = 1; break;
        case 32: {
            const uint16_t selector = read_word(8);
            format = selector == 4U || selector == 5U ||
                             selector == 7U
                         ? selector
                         : 2U;
            break;
        }
        case 48: format = 6; break;
        default: return false;
        }
        GraphicsSurface surface;
        surface.width = width;
        surface.height = height;
        surface.format = format;
        surface.bytes_per_pixel = bitmap_bytes_per_pixel(format);
        surface.stride = surface.bytes_per_pixel * width;
        const bool has_aux = read_word(10) == 1U;
        const uint16_t aux_first = read_word(12);
        const uint16_t aux_second = read_word(14);
        surface.encoded = std::move(*bytes);
        install_allocated_graphics_surface(type, std::move(surface));
        if (has_aux) {
            bitmap_aux_values_[type] = {aux_first, aux_second};
        } else {
            bitmap_aux_values_.erase(type);
        }
        display_dirty_ = true;
        return true;
    }
    bool load_bitmap_resource(uint32_t type, std::string archive,
                              std::string resource) {
        std::vector<uint8_t> data = read_resource(archive, resource);
        return load_bitmap_resource_data(
            type, std::move(archive), std::move(resource), std::move(data));
    }
    bool load_bitmap_resource_data(
        uint32_t type, std::string archive, std::string resource,
        std::vector<uint8_t> data) {
        if (data.size() >= 15U &&
            std::string_view(
                reinterpret_cast<const char*>(data.data()), 15U).starts_with(
                "CompressedBG_")) {
            std::string decode_error;
            auto decoded = decode_compressed_bg(data, &decode_error, archive + ":" + resource);
            if (!decoded || decoded->width == 0 ||
                decoded->height == 0 ||
                (decoded->bits_per_pixel != 8U &&
                 decoded->bits_per_pixel != 24U &&
                 decoded->bits_per_pixel != 32U)) {
                bitmap_load_errors_[type] =
                    "CompressedBG payload is invalid: " + decode_error;
                return false;
            }
            GraphicsSurface surface;
            surface.width = decoded->width;
            surface.height = decoded->height;
            surface.format = decoded->bits_per_pixel == 8U ? 3U :
                decoded->format == 1U || decoded->format == 2U || decoded->format == 7U
                    ? decoded->format
                    : (decoded->bits_per_pixel == 24U ? 1U : 2U);
            surface.bytes_per_pixel =
                bitmap_bytes_per_pixel(surface.format);
            surface.stride =
                surface.bytes_per_pixel * surface.width;
            // PE64 140042BD0 maps the decoded selector7 to manager
            // format1; 1400429C0/140041790 copy its already padded BGRX.
            // The decoder result itself must retain selector7.
            if (compact_64_opcodes_ && decoded->format == 7U) {
                surface.format = 1U;
                surface.pixels = std::move(decoded->pixels);
            } else if (surface.format != 1U && decoded->pixels.size() ==
                       static_cast<std::size_t>(surface.stride) * surface.height) {
                // The decoder already produced this exact internal layout;
                // transfer its allocation rather than copying every pixel.
                surface.pixels = std::move(decoded->pixels);
            } else {
                surface.pixels = bitmap_internal_pixels(
                    surface.format, surface.width, surface.height,
                    decoded->pixels);
            }
            surface.encoded = std::move(data);
            surface.archive = std::move(archive);
            surface.resource = std::move(resource);
            refresh_bgra_mirror(surface);
            install_allocated_graphics_surface(type, std::move(surface));
            display_dirty_ = true;
            return true;
        }
        if (data.size() >= 8) {
            const uint16_t width =
                static_cast<uint16_t>(data[0] | (data[1] << 8U));
            const uint16_t height =
                static_cast<uint16_t>(data[2] | (data[3] << 8U));
            const auto format = bitmap_format_from_header(data);
            if (!format) {
                bitmap_load_errors_[type] =
                    "bitmap header uses an unsupported format";
                warn_decode_failure("bitmap", bitmap_load_errors_[type], data, archive + ":" + resource);
                return false;
            }
            const uint16_t compression =
                static_cast<uint16_t>(data[6] | (data[7] << 8U));
            GraphicsSurface surface;
            surface.height = height;
            surface.width = width;
            surface.format = *format;
            surface.bytes_per_pixel =
                bitmap_bytes_per_pixel(*format);
            surface.stride = surface.bytes_per_pixel * width;
            surface.encoded = std::move(data);
            surface.archive = std::move(archive);
            surface.resource = std::move(resource);
            if (compression == 0 && surface.encoded.size() >= 16) {
                const std::size_t pixel_bytes =
                    static_cast<std::size_t>(width) * height *
                    (*format == 1U ? 3U : surface.bytes_per_pixel);
                if (surface.encoded.size() - 16U >= pixel_bytes) {
                    surface.pixels = bitmap_internal_pixels(
                        surface.format, width, height,
                        std::span<const uint8_t>(
                            surface.encoded).subspan(16, pixel_bytes));
                }
            }
            install_allocated_graphics_surface(type, std::move(surface));
            return true;
        }
        bitmap_load_errors_[type] = "bitmap header is truncated";
        warn_decode_failure("bitmap", bitmap_load_errors_[type], data, archive + ":" + resource);
        return false;
    }
    bool cache_bitmap_resource(
        std::string archive, std::string resource,
        std::vector<uint8_t> data) {
        if (data.size() < 8 ||
            !bitmap_format_from_header(data)) {
            return false;
        }
        bitmap_resource_cache_[{std::move(archive),
                                std::move(resource)}] =
            std::move(data);
        return true;
    }
    bool load_cached_bitmap_resource(
        uint32_t type, const std::string& archive,
        const std::string& resource, bool cache_loaded) {
        const auto key = std::make_pair(archive, resource);
        const auto cached = bitmap_resource_cache_.find(key);
        if (cached != bitmap_resource_cache_.end()) {
            return load_bitmap_resource_data(
                type, archive, resource, cached->second);
        }
        std::vector<uint8_t> data = read_resource(archive, resource);
        if (data.empty()) {
            bitmap_load_errors_[type] = "bitmap resource is missing";
            return false;
        }
        const bool loaded = cache_loaded
            ? load_bitmap_resource_data(type, archive, resource, data)
            : load_bitmap_resource_data(type, archive, resource, std::move(data));
        if (loaded && cache_loaded) {
            bitmap_resource_cache_[key] = std::move(data);
        }
        return loaded;
    }
    uint32_t read_bitmap_pixels(uint32_t type, uint32_t capacity,
                                uint32_t destination,
                                uint32_t size_output,
                                uint32_t base) {
        const auto found = graphics_surfaces_.find(type);
        if (found == graphics_surfaces_.end()) {
            return 9;
        }
        const GraphicsSurface& bitmap = found->second;
        static constexpr std::array<uint32_t, 4> bits_per_pixel{
            16, 24, 32, 8,
        };
        if (bitmap.format >= bits_per_pixel.size()) {
            throw BpFault("bitmap pixel extraction format is unsupported");
        }
        const uint32_t output_bytes_per_pixel =
            bits_per_pixel[bitmap.format] / 8U;
        const uint32_t required =
            bitmap.width * bitmap.height * output_bytes_per_pixel;
        if (capacity < required) {
            return 10;
        }
        const std::vector<uint8_t> pixels = [&]() -> std::vector<uint8_t> {
            if (!bitmap.pixels.empty()) {
                return bitmap.pixels;
            }
            if (bitmap.encoded.size() >= 16) {
                const std::size_t source_size =
                    static_cast<std::size_t>(bitmap.width) *
                    bitmap.height *
                    (bitmap.format == 1U
                         ? 3U
                         : bitmap_bytes_per_pixel(bitmap.format));
                if (bitmap.encoded.size() - 16U >= source_size) {
                    return bitmap_internal_pixels(
                        bitmap.format, bitmap.width, bitmap.height,
                        std::span<const uint8_t>(
                            bitmap.encoded).subspan(16, source_size));
                }
            }
            return std::vector<uint8_t>();
        }();
        const uint32_t source_bytes_per_pixel =
            bitmap.bytes_per_pixel != 0
                ? bitmap.bytes_per_pixel
                : bitmap_bytes_per_pixel(bitmap.format);
        const uint32_t source_stride =
            bitmap.stride != 0
                ? bitmap.stride
                : source_bytes_per_pixel * bitmap.width;
        const std::size_t source_rows_size =
            static_cast<std::size_t>(source_stride) * bitmap.height;
        if (pixels.size() < source_rows_size) {
            throw BpFault("bitmap pixel buffer is incomplete");
        }
        std::vector<uint8_t> output;
        output.reserve(required);
        for (uint32_t y = 0; y < bitmap.height; ++y) {
            const std::size_t row =
                static_cast<std::size_t>(y) * source_stride;
            for (uint32_t x = 0; x < bitmap.width; ++x) {
                const std::size_t pixel =
                    row + static_cast<std::size_t>(x) *
                              source_bytes_per_pixel;
                output.insert(
                    output.end(), pixels.begin() +
                        static_cast<std::ptrdiff_t>(pixel),
                    pixels.begin() + static_cast<std::ptrdiff_t>(
                        pixel + output_bytes_per_pixel));
            }
        }
        memory_.write_bytes(destination, output, base);
        memory_.write_u32(size_output, base, required);
        return 0;
    }
    bool query_bitmap_info(uint32_t type, uint32_t address, uint32_t base) {
        if (address != 0) {
            memory_.write_u32(address, base, 0);
        }
        const auto found = graphics_surfaces_.find(type);
        if (found == graphics_surfaces_.end() || address == 0) {
            return false;
        }
        const GraphicsSurface& bitmap = found->second;
        const std::array<uint32_t, 5> fields{
            bitmap.stride != 0 ? bitmap.stride : bitmap.width * 4U,
            bitmap.width,
            bitmap.height,
            bitmap.format,
            bitmap.bytes_per_pixel != 0
                ? bitmap.bytes_per_pixel
                : 4U,
        };
        for (std::size_t index = 0; index < fields.size(); ++index) {
            memory_.write_u32(
                address + static_cast<uint32_t>((index + 1) * 4),
                base, fields[index]);
        }
        return true;
    }
    std::optional<DecodedBitmap> decoded_bitmap(uint32_t type) const {
        const auto found = graphics_surfaces_.find(type);
        if (found == graphics_surfaces_.end()) {
            return std::nullopt;
        }
        if (!found->second.rgba.empty()) {
            DecodedBitmap decoded;
            decoded.width = found->second.width;
            decoded.height = found->second.height;
            decoded.format = found->second.format;
            decoded.rgba = found->second.rgba;
            return decoded;
        }
        return decode_bgi_bitmap(found->second.encoded);
    }
    static std::optional<DecodedBitmap> decode_bitmap_payload(
        std::span<const uint8_t> encoded,
        std::string* error = nullptr, std::string_view resource = {});
    struct PrimarySurfaceSnapshot {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 2;
        std::vector<uint8_t> rgba;
    };
    using PrimarySurfaceCaptureProvider =
        std::function<std::optional<PrimarySurfaceSnapshot>()>;
    void set_primary_surface_capture_provider(
        PrimarySurfaceCaptureProvider provider) {
        primary_surface_capture_provider_ = std::move(provider);
    }
    bool capture_bitmap_from_primary(uint32_t type) {
        if (type >= 0x4000U) {
            return false;
        }
        PrimarySurfaceSnapshot snapshot;
        if (primary_surface_capture_provider_) {
            if (auto captured = primary_surface_capture_provider_()) {
                snapshot = std::move(*captured);
            }
        }
        if (snapshot.width == 0 || snapshot.height == 0) {
            const auto [width, height] = current_mode_dimensions();
            snapshot.width = width;
            snapshot.height = height;
        }
        if (snapshot.width == 0 || snapshot.height == 0) {
            return false;
        }
        uint32_t format = snapshot.format;
        if (format > 6U) {
            if (format != 7U) {
                return false;
            }
            format = 1;
        }
        const std::size_t pixel_count =
            static_cast<std::size_t>(snapshot.width) * snapshot.height;
        if (!snapshot.rgba.empty() &&
            snapshot.rgba.size() != pixel_count * 4U) {
            snapshot.rgba.clear();
        }
        GraphicsSurface surface;
        surface.width = snapshot.width;
        surface.height = snapshot.height;
        surface.format = format;
        surface.bytes_per_pixel = bitmap_bytes_per_pixel(format);
        surface.stride = surface.bytes_per_pixel * surface.width;
        surface.rgba = std::move(snapshot.rgba);
        surface.pixels.assign(
            static_cast<std::size_t>(surface.stride) * surface.height,
            0);
        if (surface.rgba.size() == pixel_count * 4U) {
            for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
                const std::size_t offset = pixel * 4U;
                if (format == 1U || format == 2U) {
                    surface.pixels[offset] = surface.rgba[offset + 2U];
                    surface.pixels[offset + 1U] =
                        surface.rgba[offset + 1U];
                    surface.pixels[offset + 2U] =
                        surface.rgba[offset];
                    surface.pixels[offset + 3U] =
                        format == 1U ? 0U : surface.rgba[offset + 3U];
                }
            }
        }
        install_allocated_graphics_surface(type, std::move(surface));
        return true;
    }
    bool invert_indexed_bitmap(uint32_t type) {
        const auto found = graphics_surfaces_.find(type);
        if (found == graphics_surfaces_.end() ||
            found->second.format != 3U ||
            !materialize_graphics_pixels(found->second)) {
            return false;
        }
        GraphicsSurface& surface = found->second;
        const std::size_t required =
            static_cast<std::size_t>(surface.stride) *
            surface.height;
        if (surface.pixels.size() < required) {
            return false;
        }
        for (std::size_t y = 0; y < surface.height; ++y) {
            for (std::size_t x = 0; x < surface.width; ++x) {
                uint8_t& value =
                    surface.pixels[y * surface.stride + x];
                value = static_cast<uint8_t>(~value);
            }
        }
        return true;
    }
    struct DisplayBitmapDraw {
        uint32_t bitmap = 0;
        uint32_t display_object = 0;
        uint32_t object_priority = 0;
        uint32_t dispatch_parameter = 0;
        int32_t source_left = 0;
        int32_t source_top = 0;
        int32_t source_right = 0;
        int32_t source_bottom = 0;
        int32_t destination_left = 0;
        int32_t destination_top = 0;
        int32_t destination_right = 0;
        int32_t destination_bottom = 0;
    };
    static constexpr uint32_t main_background_handle = 0x81000000U;
    bool compose_main_background_at(uint32_t destination, const DisplayBitmapDraw& callback);
    bool compose_background_f_at(uint32_t destination, const DisplayBitmapDraw& callback);
    bool compose_filter_at(uint32_t destination, uint32_t object,
                           uint32_t mask_left, uint32_t mask_top);
    void refresh_filter_display_geometry(uint32_t object);
    void refresh_main_background_geometry();
    bool execute_sprite_display_callback(const DisplayBitmapDraw& callback);
    bool execute_display_callback(const DisplayBitmapDraw& callback,
                                  std::optional<uint32_t> local_window = std::nullopt);
    using DisplayCallbackTrace = std::function<void(
        const DisplayBitmapDraw&, uint32_t target, bool executed)>;
    void set_display_callback_trace(DisplayCallbackTrace trace) {
        display_callback_trace_ = std::move(trace);
    }
    struct DisplayDirtyRegion {
        std::array<int32_t, 4> rectangle;
        uint32_t parameter = 0;
    };
    bool render_display_regions(uint32_t width, uint32_t height,
                                 std::span<const DisplayDirtyRegion> regions);
    [[nodiscard]] std::optional<DecodedBitmap> display_frame_snapshot() const;
    // Read-only host upload view. Consume synchronously before the next render.
    [[nodiscard]] const GraphicsSurface& display_frame_view() const;
    void offload_bitmap_cpu_pixels();
    [[nodiscard]] std::array<uint64_t,2> bitmap_pixel_storage_bytes() const;
    using DisplayFrameReadbackProvider=std::function<bool(GraphicsSurface&)>;
    void set_display_frame_readback_provider(DisplayFrameReadbackProvider provider) {
        display_frame_readback_provider_=std::move(provider);
    }
    void release_display_frame_cpu_pixels() {
        if(!display_frame_readback_provider_ || display_frame_.pixels.empty())return;
        std::vector<uint8_t>().swap(display_frame_.pixels);
        std::vector<uint8_t>().swap(display_frame_.rgba);
        display_frame_gpu_cached_=true;
    }
    void invalidate_display_frame_gpu_cache() { display_frame_gpu_cached_=false; }
    [[nodiscard]] size_t retained_display_frame_cpu_bytes() const {
        return display_frame_.pixels.capacity()+display_frame_.rgba.capacity();
    }
    [[nodiscard]] bool has_display_frame(uint32_t width,
                                         uint32_t height) const {
        return display_frame_.width == width &&
               display_frame_.height == height &&
               display_frame_.format == 1U &&
               (display_frame_gpu_cached_ || display_frame_.pixels.size() ==
                   static_cast<std::size_t>(width) * height * 4U);
    }

    [[nodiscard]] std::vector<DisplayBitmapDraw> build_display_dispatch(
        uint32_t bitmap, std::array<int32_t, 4> dirty, uint32_t parameter,
        uint32_t cutoff, bool capture = false,
        std::array<int32_t, 2> canvas_origin = {0, 0}) const;
    uint32_t capture_display_region_into_bitmap(uint32_t bitmap,
        int32_t x, int32_t y, uint32_t priority);
    bool register_auxiliary_display_producer(uint32_t object,
                                              std::span<const uint32_t> priorities);
    bool remove_auxiliary_display_producer(uint32_t object);
    bool draw_bitmap_to_display_objects(uint32_t type,
                                        uint32_t priority) {
        if (type >= 0x4000U || priority >= 0x10000U) {
            return false;
        }
        const auto [width, height] = current_mode_dimensions();
        if (width == 0 || height == 0) {
            return false;
        }
        // 0044A040 takes the format from the MAIN display descriptor,
        // not the previous capture slot. The executable main CPU format is1.
        const uint32_t format = 1;
        GraphicsSurface surface;
        surface.width = width;
        surface.height = height;
        surface.format = format;
        surface.bytes_per_pixel = bitmap_bytes_per_pixel(format);
        surface.stride = surface.bytes_per_pixel * surface.width;
        surface.pixels.assign(
            static_cast<std::size_t>(surface.stride) * surface.height,
            0);
        install_allocated_graphics_surface(type, std::move(surface));

        const auto callbacks = build_display_dispatch(type,
            {0, 0, static_cast<int32_t>(width) - 1, static_cast<int32_t>(height) - 1},
            0, (priority << 16U) | 0xFFFFU, true);
        if (debug_enabled()) display_bitmap_draws_.insert(display_bitmap_draws_.end(),
                                     callbacks.begin(), callbacks.end());
        for (const DisplayBitmapDraw& callback : callbacks) {
            const bool executed = execute_display_callback(callback);
            if (display_callback_trace_) {
                display_callback_trace_(callback, type, executed);
            }
        }
        return true;
    }
    [[nodiscard]] const std::vector<DisplayBitmapDraw>&
    display_bitmap_draws() const {
        return display_bitmap_draws_;
    }
    void set_display_priority_cutoff(uint32_t bitmap) {
        display_priority_cutoff_ = bitmap << 16U;
        display_dirty_ = true;
    }
    [[nodiscard]] uint32_t display_priority_cutoff() const {
        return display_priority_cutoff_;
    }
    [[nodiscard]] uint32_t background_type() const {
        return background_type_;
    }
    void set_background_type(uint32_t type) {
        background_type_ = type;
    }
    void select_background_class(uint32_t type) {
        // 0x00445480 retains the existing object when +0x144 matches.
        if (background_type_ != type) {
            background_type_ = type;
            background_bitmaps_.fill(-1);
            background_bitmap_widths_ = {};
            background_bitmap_tokens_.fill(0xFFFFFFFFU);
            reinsert_display_object(main_background_handle);
            background_effect_level_ = 0;
            background_center_ = {0, 0};
        }
        // The factory invalidates the display before the image setter runs.
        refresh_main_background_geometry();
        display_dirty_ = true;
    }
    uint32_t configure_background_bitmap(int32_t bitmap) {
        select_background_class(1);
        const auto surface = graphics_surfaces_.find(
            static_cast<uint32_t>(bitmap));
        if (bitmap < 0 || surface == graphics_surfaces_.end()) {
            return 0;
        }
        // 004216C0 -> 00421680 checks the main descriptor's W/H/format.
        const auto [width, height] = current_mode_dimensions();
        if (surface->second.width != width || surface->second.height != height ||
            surface->second.format != 1U) {
            return 0;
        }
        background_bitmaps_[0] = bitmap;
        background_bitmap_tokens_[0] = graphics_bitmap_token(static_cast<uint32_t>(bitmap));
        background_bitmap_widths_[0] = surface->second.width;
        display_dirty_ = true;
        return 1;
    }
    uint32_t configure_background_bitmap_pair(
        int32_t first, int32_t second, uint32_t effect_level) {
        // 0x004449A0 selects CDspObjBackB, applies vtable +0x48, then
        // configures its primary and optional pattern/second bitmap.
        select_background_class(2);
        background_effect_level_ = effect_level;

        const auto first_surface = graphics_surfaces_.find(
            static_cast<uint32_t>(first));
        if (first < 0 || first_surface == graphics_surfaces_.end()) {
            return 0;
        }
        if (second == 0x7000 || second == 0x7001) {
            background_bitmaps_[0] = first;
            background_bitmap_widths_[0] = first_surface->second.width;
            background_bitmaps_[1] = second;
            display_dirty_ = true;
            return 1;
        }
        const auto second_surface = graphics_surfaces_.find(
            static_cast<uint32_t>(second));
        if (second < 0 || second_surface == graphics_surfaces_.end()) {
            return 0;
        }
        background_bitmaps_[0] = first;
        background_bitmap_widths_[0] = first_surface->second.width;
        background_bitmaps_[1] = second;
        background_bitmap_widths_[1] = second_surface->second.width;
        display_dirty_ = true;
        return 1;
    }
    uint32_t configure_background_bitmap_quad(
        int32_t first, int32_t second, int32_t third, int32_t fourth,
        int32_t center_x, int32_t center_y) {
        // 0x004449E0 selects CDspObjBackS. Center validation runs before
        // the four image validations and therefore persists on status 2.
        select_background_class(3);

        const auto [width, height] = effective_client_dimensions();
        if (center_x < 0 || center_y < 0 ||
            static_cast<uint32_t>(center_x) > width ||
            static_cast<uint32_t>(center_y) > height) {
            return 1;
        }
        background_center_ = {center_x, center_y};
        const std::array<int32_t, 4> bitmaps{
            first, second, third, fourth};
        std::array<uint32_t, 4> widths{};
        for (std::size_t index = 0; index < bitmaps.size(); ++index) {
            const int32_t bitmap = bitmaps[index];
            const auto surface = graphics_surfaces_.find(
                static_cast<uint32_t>(bitmap));
            if (bitmap < 0 || surface == graphics_surfaces_.end()) {
                return 2;
            }
            widths[index] = surface->second.width;
        }
        background_bitmaps_ = bitmaps;
        background_bitmap_widths_ = widths;
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::array<int32_t, 4>&
    background_bitmaps() const {
        return background_bitmaps_;
    }
    [[nodiscard]] const std::array<uint32_t, 4>&
    background_bitmap_widths() const {
        return background_bitmap_widths_;
    }
    [[nodiscard]] uint32_t background_effect_level() const {
        return background_effect_level_;
    }
    [[nodiscard]] const std::array<int32_t, 2>&
    background_center() const {
        return background_center_;
    }
    struct BackgroundEffectSlot {
        uint32_t field_150 = 0;
        uint32_t field_154 = 0;
        uint32_t fixed_x = 0;                  // +0x158
        uint32_t fixed_y = 0;                  // +0x15C
        uint32_t mode = 0;                     // +0x160
        uint32_t field_164 = 0;
        int32_t bitmap = 0;                    // +0x168
        uint32_t bitmap_token = 0;             // +0x16C
        uint32_t field_170 = 0;
        uint32_t field_174 = 0;
        uint32_t field_178 = 0;
        uint32_t field_17C = 0;
        uint32_t field_180 = 0;
        uint32_t field_184 = 0;
        uint32_t field_188 = 0;
        uint32_t field_18C = 0;
        uint32_t field_190 = 0;
        uint32_t field_194 = 0;
        uint32_t field_198 = 0;
        uint32_t field_19C = 0;
        uint32_t field_1A0 = 0;
        uint32_t field_1A4 = 0;
    };
    uint32_t configure_background_layer(
        uint32_t x, uint32_t y, int32_t bitmap, uint32_t field_170,
        uint32_t field_174, uint32_t field_178, uint32_t field_17c,
        uint32_t field_180, uint32_t field_1a4) {
        // 0x00444F60 first rebuilds the main object as CDspObjBackML
        // (type 12), which clears +0x150..+0x40F.
        background_type_ = 12;
        background_effect_slots_ = {};
        background_effect_active_slot_ = 0;
        background_position_ = {0, 0};

        if (bitmap < 0) {
            return 3;
        }
        const auto surface = graphics_surfaces_.find(
            static_cast<uint32_t>(bitmap));
        if (surface == graphics_surfaces_.end()) {
            return 3;
        }
        BackgroundEffectSlot& slot = background_effect_slots_[0];
        slot.field_150 = 1;
        slot.bitmap = bitmap;
        slot.bitmap_token = graphics_bitmap_token(static_cast<uint32_t>(bitmap));
        slot.field_170 = field_170;
        slot.field_174 = field_174;
        slot.field_190 = 0;
        slot.field_194 = 0;

        if (field_17c == 0 || field_180 == 0) {
            return 4;
        }
        slot.field_178 = field_178;
        slot.field_17C = field_17c;
        slot.field_180 = field_180;
        slot.field_1A4 = field_1a4;
        slot.field_198 = 0;
        slot.field_19C = 0;
        slot.field_1A0 = 0;
        slot.field_154 = 1;
        background_effect_active_slot_ = 0;
        background_position_ = {
            static_cast<int32_t>(x), static_cast<int32_t>(y)};
        display_dirty_ = true;
        return 0;
    }
    uint32_t set_background_effect_active_slot(uint32_t slot) {
        if (background_type_ != 12U) {
            return 1;
        }
        if (slot >= background_effect_slots_.size()) {
            return 2;
        }
        background_effect_active_slot_ = slot;
        return 0;
    }
    uint32_t set_background_effect_enabled_value(uint32_t slot,
                                                 uint32_t value) {
        if (background_type_ != 12U) {
            return 1;
        }
        if (slot >= background_effect_slots_.size()) {
            return 2;
        }
        background_effect_slots_[slot].field_154 = value;
        return 0;
    }
    uint32_t set_background_effect_position(uint32_t slot, uint32_t x,
                                            uint32_t y) {
        if (background_type_ != 12U) {
            return 1;
        }
        if (slot >= background_effect_slots_.size()) {
            return 2;
        }
        BackgroundEffectSlot& effect = background_effect_slots_[slot];
        effect.fixed_x = (x + 0x8000U) & 0xFFFF0000U;
        effect.fixed_y = (y + 0x8000U) & 0xFFFF0000U;
        if (effect.field_154 != 0) {
            display_dirty_ = true;
        }
        return 0;
    }
    uint32_t set_background_effect_mode(uint32_t slot, uint32_t mode) {
        if (background_type_ != 12U) {
            return 1;
        }
        if (slot >= background_effect_slots_.size()) {
            return 2;
        }
        static constexpr std::array<uint32_t, 23> modes{
            0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
            0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
            0x40, 0x41, 0xC0, 0xF0, 0xFF,
        };
        if (std::find(modes.begin(), modes.end(), mode) == modes.end() &&
            mode != 0x80 && mode != 0xC1) {
            return mode;
        }
        BackgroundEffectSlot& effect = background_effect_slots_[slot];
        effect.mode = mode;
        if (effect.field_154 != 0) {
            display_dirty_ = true;
        }
        return 0;
    }
    uint32_t set_background_effect_field_164(uint32_t slot,
                                             uint32_t value) {
        if (background_type_ != 12U) {
            return 1;
        }
        if (slot >= background_effect_slots_.size()) {
            return 2;
        }
        BackgroundEffectSlot& effect = background_effect_slots_[slot];
        effect.field_164 = value;
        if (effect.field_154 != 0) {
            display_dirty_ = true;
        }
        return 0;
    }
    uint32_t set_background_effect_bitmap(
        uint32_t slot, int32_t bitmap, uint32_t field_170,
        uint32_t field_174) {
        if (background_type_ != 12U) {
            return 1;
        }
        if (slot >= background_effect_slots_.size()) {
            return 2;
        }
        BackgroundEffectSlot& effect = background_effect_slots_[slot];
        if (bitmap == -1) {
            effect.field_150 = 0;
            return 0;
        }
        const auto surface = graphics_surfaces_.find(
            static_cast<uint32_t>(bitmap));
        if (surface == graphics_surfaces_.end()) {
            return 3;
        }
        effect.field_150 = 1;
        effect.bitmap = bitmap;
        effect.bitmap_token = graphics_bitmap_token(static_cast<uint32_t>(bitmap));
        effect.field_170 = field_170;
        effect.field_174 = field_174;
        effect.field_190 = 0;
        effect.field_194 = 0;
        if (effect.field_154 != 0) {
            display_dirty_ = true;
        }
        return 0;
    }
    uint32_t set_background_effect_rectangle(
        uint32_t slot, uint32_t first, uint32_t width, uint32_t height,
        uint32_t tail) {
        if (background_type_ != 12U) {
            return 1;
        }
        if (slot >= background_effect_slots_.size()) {
            return 2;
        }
        if (width == 0 || height == 0) {
            return 4;
        }
        BackgroundEffectSlot& effect = background_effect_slots_[slot];
        effect.field_178 = first;
        effect.field_17C = width;
        effect.field_180 = height;
        effect.field_1A4 = tail;
        effect.field_198 = 0;
        effect.field_19C = 0;
        effect.field_1A0 = 0;
        if (effect.field_154 != 0) {
            display_dirty_ = true;
        }
        return 0;
    }
    uint32_t set_background_effect_pair_188(
        uint32_t slot, uint32_t first, uint32_t second) {
        if (background_type_ != 12U) {
            return 1;
        }
        if (slot >= background_effect_slots_.size()) {
            return 2;
        }
        background_effect_slots_[slot].field_188 = first;
        background_effect_slots_[slot].field_18C = second;
        return 0;
    }
    uint32_t set_background_effect_pair_190(
        uint32_t slot, uint32_t first, uint32_t second) {
        if (background_type_ != 12U) {
            return 1;
        }
        if (slot >= background_effect_slots_.size()) {
            return 2;
        }
        background_effect_slots_[slot].field_190 = first;
        background_effect_slots_[slot].field_194 = second;
        return 0;
    }
    uint32_t set_background_effect_animation(
        uint32_t slot, uint32_t first, uint32_t second, uint32_t third) {
        if (background_type_ != 12U) {
            return 1;
        }
        if (slot >= background_effect_slots_.size()) {
            return 2;
        }
        BackgroundEffectSlot& effect = background_effect_slots_[slot];
        effect.field_198 = first;
        effect.field_19C = second;
        effect.field_1A0 = third;
        return 0;
    }
    [[nodiscard]] const std::array<BackgroundEffectSlot, 8>&
    background_effect_slots() const {
        return background_effect_slots_;
    }
    [[nodiscard]] uint32_t background_effect_active_slot() const {
        return background_effect_active_slot_;
    }
    [[nodiscard]] const std::array<int32_t, 2>&
    background_position() const {
        return background_position_;
    }
    uint32_t validate_bitmap_format(uint32_t type, uint32_t expected_format) {
        const auto found = graphics_surfaces_.find(type);
        if (found == graphics_surfaces_.end()) {
            return 1;
        }
        if (found->second.format == expected_format) {
            return 0;
        }
        if (found->second.format == 2 && expected_format == 1) {
            found->second.format = 1;
            return 0;
        }
        return 2;
    }
    bool set_bitmap_aux_values(uint32_t bitmap, uint32_t first,
                               uint32_t second) {
        if (graphics_surfaces_.count(bitmap) == 0) {
            return false;
        }
        bitmap_aux_values_[bitmap] = {first, second};
        return true;
    }
    [[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>>
    bitmap_aux_values(uint32_t bitmap) const {
        const auto found = bitmap_aux_values_.find(bitmap);
        if (found == bitmap_aux_values_.end()) {
            return std::nullopt;
        }
        return found->second;
    }
    uint32_t replace_bitmap_color(uint32_t bitmap, uint32_t old_color,
                                  uint32_t new_color) {
        const auto found = graphics_surfaces_.find(bitmap);
        if (found == graphics_surfaces_.end()) {
            return 1;
        }
        GraphicsSurface& surface = found->second;
        if ((surface.format != 1U && surface.format != 2U) ||
            surface.bytes_per_pixel != 4U ||
            !materialize_graphics_pixels(surface)) {
            return 2;
        }
        const std::size_t required =
            static_cast<std::size_t>(surface.stride) * surface.height;
        if (surface.pixels.size() < required) {
            return 2;
        }
        for (uint32_t y = 0; y < surface.height; ++y) {
            for (uint32_t x = 0; x < surface.width; ++x) {
                const std::size_t offset =
                    static_cast<std::size_t>(y) * surface.stride +
                    static_cast<std::size_t>(x) * 4U;
                uint32_t pixel =
                    static_cast<uint32_t>(surface.pixels[offset]) |
                    (static_cast<uint32_t>(surface.pixels[offset + 1])
                     << 8U) |
                    (static_cast<uint32_t>(surface.pixels[offset + 2])
                     << 16U) |
                    (static_cast<uint32_t>(surface.pixels[offset + 3])
                     << 24U);
                if (surface.format == 1U) {
                    if ((pixel & 0x00FFFFFFU) ==
                        (old_color & 0x00FFFFFFU)) {
                        pixel = new_color & 0x00FFFFFFU;
                    }
                } else if ((old_color & 0xFF000000U) == 0) {
                    if ((pixel & 0x00FFFFFFU) ==
                        (old_color & 0x00FFFFFFU)) {
                        pixel = (pixel & 0xFF000000U) |
                                (new_color & 0x00FFFFFFU);
                    }
                } else if (pixel == old_color) {
                    pixel = new_color;
                }
                surface.pixels[offset] =
                    static_cast<uint8_t>(pixel);
                surface.pixels[offset + 1] =
                    static_cast<uint8_t>(pixel >> 8U);
                surface.pixels[offset + 2] =
                    static_cast<uint8_t>(pixel >> 16U);
                surface.pixels[offset + 3] =
                    static_cast<uint8_t>(pixel >> 24U);
            }
        }
        return 0;
    }
    uint32_t read_bitmap_pixel(uint32_t bitmap, int32_t x, int32_t y,
                               uint32_t& value) {
        const auto found = graphics_surfaces_.find(bitmap);
        if (found == graphics_surfaces_.end()) {
            return 1;
        }
        GraphicsSurface& surface = found->second;
        if (surface.bytes_per_pixel == 0 ||
            surface.bytes_per_pixel > 4U ||
            !materialize_graphics_pixels(surface)) {
            return 2;
        }
        if (x < 0 || y < 0 ||
            static_cast<uint32_t>(x) >= surface.width ||
            static_cast<uint32_t>(y) >= surface.height) {
            return 3;
        }
        const std::size_t offset =
            static_cast<std::size_t>(y) * surface.stride +
            static_cast<std::size_t>(x) * surface.bytes_per_pixel;
        if (offset + surface.bytes_per_pixel > surface.pixels.size()) {
            return 3;
        }
        value = 0;
        for (uint32_t index = 0; index < surface.bytes_per_pixel; ++index) {
            value |= static_cast<uint32_t>(
                         surface.pixels[offset + index])
                     << (index * 8U);
        }
        return 0;
    }
    uint32_t convert_bitmap_to_grayscale(uint32_t source,
                                         uint32_t destination) {
        const auto found = graphics_surfaces_.find(source);
        if (found == graphics_surfaces_.end()) {
            return 9;
        }
        GraphicsSurface& input = found->second;
        if ((input.format != 1U && input.format != 2U) ||
            input.bytes_per_pixel != 4U ||
            !materialize_graphics_pixels(input)) {
            return 9;
        }
        GraphicsSurface output;
        output.width = input.width;
        output.height = input.height;
        output.format = 3;
        output.bytes_per_pixel = 1;
        output.stride = input.width;
        output.pixels.assign(
            static_cast<std::size_t>(output.stride) * output.height, 0);
        if (output.pixels.empty()) {
            return 10;
        }
        const std::size_t input_required =
            static_cast<std::size_t>(input.stride) * input.height;
        if (input.pixels.size() < input_required) {
            return 10;
        }
        for (uint32_t y = 0; y < input.height; ++y) {
            const std::size_t source_row =
                static_cast<std::size_t>(y) * input.stride;
            const std::size_t destination_row =
                static_cast<std::size_t>(y) * output.stride;
            for (uint32_t x = 0; x < input.width; ++x) {
                const std::size_t offset =
                    source_row + static_cast<std::size_t>(x) * 4U;
                const uint32_t b = input.pixels[offset];
                const uint32_t g = input.pixels[offset + 1U];
                const uint32_t r = input.pixels[offset + 2U];
                const uint32_t a = input.pixels[offset + 3U];
                const uint32_t weighted =
                    g * 151U + r * 77U + b * 28U;
                const uint32_t gray =
                    input.format == 1U
                        ? weighted >> 8U
                        : (weighted * a) >> 16U;
                output.pixels[destination_row + x] =
                    static_cast<uint8_t>(gray);
            }
        }
        install_allocated_graphics_surface(destination, std::move(output));
        bitmap_aux_values_.erase(destination);
        return 0;
    }
    static void write_i16(std::vector<uint8_t>& pixels,
                          std::size_t offset, int32_t value) {
        const uint16_t encoded = static_cast<uint16_t>(value);
        pixels[offset] = static_cast<uint8_t>(encoded);
        pixels[offset + 1U] = static_cast<uint8_t>(encoded >> 8U);
    }
    uint32_t initialize_bitmap_normal_map(uint32_t bitmap,
                                          uint32_t mode) {
        const auto found = graphics_surfaces_.find(bitmap);
        if (found == graphics_surfaces_.end()) {
            return 0x80000001U;
        }
        GraphicsSurface& surface = found->second;
        if (surface.format != 6U || surface.bytes_per_pixel != 6U ||
            surface.width == 0 || surface.height == 0 ||
            !materialize_graphics_pixels(surface)) {
            return 0x80000003U;
        }
        if (mode > 3U) {
            return 0x8000000AU;
        }
        static constexpr std::array<int16_t, 4> x_values{0x7FFF, 0, 0x7FFF, 0};
        static constexpr std::array<int16_t, 4> y_values{0, 0x7FFF, 0, 0x7FFF};
        static constexpr std::array<bool, 4> depth_uses_y{
            true, false, false, true};
        for (uint32_t y = 0; y < surface.height; ++y) {
            for (uint32_t x = 0; x < surface.width; ++x) {
                const std::size_t offset =
                    static_cast<std::size_t>(y) * surface.stride +
                    static_cast<std::size_t>(x) * 6U;
                write_i16(surface.pixels, offset, x_values[mode]);
                write_i16(surface.pixels, offset + 2U, y_values[mode]);
                write_i16(
                    surface.pixels, offset + 4U,
                    static_cast<int32_t>(
                        (depth_uses_y[mode] ? y : x) * 4U));
            }
        }
        return 0;
    }
    uint32_t generate_bitmap_normal_map(
        uint32_t bitmap, uint32_t mode, int32_t center_x,
        int32_t center_y, uint32_t phase) {
        const auto found = graphics_surfaces_.find(bitmap);
        if (found == graphics_surfaces_.end()) {
            return 0x80000001U;
        }
        GraphicsSurface& surface = found->second;
        if (surface.format != 6U || surface.bytes_per_pixel != 6U ||
            surface.width == 0 || surface.height == 0 ||
            !materialize_graphics_pixels(surface)) {
            return 0x80000003U;
        }
        if (mode >= 2U) {
            return 0x80000009U;
        }
        const uint32_t divisor =
            phase == 0 ? 0xFFFFFFFFU : phase << 2U;
        for (uint32_t row = 0; row < surface.height; ++row) {
            const int32_t y =
                static_cast<int32_t>(row) - center_y;
            const int64_t scaled_y =
                static_cast<int64_t>(y) * 0x7FFF;
            for (uint32_t column = 0; column < surface.width; ++column) {
                const int32_t x =
                    static_cast<int32_t>(column) - center_x;
                const int64_t scaled_x =
                    static_cast<int64_t>(x) * 0x7FFF;
                const double distance = std::sqrt(
                    static_cast<double>(x) * static_cast<double>(x) +
                    static_cast<double>(y) * static_cast<double>(y));
                int32_t first = 0;
                int32_t second = 0;
                if (distance != 0.0) {
                    const double nx =
                        static_cast<double>(scaled_x) / distance;
                    const double ny =
                        static_cast<double>(scaled_y) / distance;
                    first = static_cast<int32_t>(
                        mode == 0 ? nx : ny);
                    second = static_cast<int32_t>(
                        mode == 0 ? ny : nx);
                }
                const uint32_t depth =
                    static_cast<uint32_t>(distance * 4.0) % divisor;
                const std::size_t offset =
                    static_cast<std::size_t>(row) * surface.stride +
                    static_cast<std::size_t>(column) * 6U;
                write_i16(surface.pixels, offset, first);
                write_i16(surface.pixels, offset + 2U, second);
                write_i16(
                    surface.pixels, offset + 4U,
                    static_cast<int32_t>(static_cast<uint16_t>(depth)));
            }
        }
        return 0;
    }
    uint32_t load_bmp_bitmap(uint32_t bitmap, std::string_view path) {
        const auto source = read_virtual_file(path);
        if (!source || source->empty()) {
            return 0xFFFFFFFFU;
        }
        const std::vector<uint8_t>& data = *source;
        const auto read_u16 = [&](std::size_t offset) -> uint32_t {
            if (offset + 2U > data.size()) {
                return 0;
            }
            return static_cast<uint32_t>(data[offset]) |
                   (static_cast<uint32_t>(data[offset + 1U]) << 8U);
        };
        const auto read_u32 = [&](std::size_t offset) -> uint32_t {
            if (offset + 4U > data.size()) {
                return 0;
            }
            return static_cast<uint32_t>(data[offset]) |
                   (static_cast<uint32_t>(data[offset + 1U]) << 8U) |
                   (static_cast<uint32_t>(data[offset + 2U]) << 16U) |
                   (static_cast<uint32_t>(data[offset + 3U]) << 24U);
        };
        if (data.size() < 2U || data[0] != 'B' || data[1] != 'M') {
            return 0x80000001U;
        }
        if (read_u32(14) != 40U) {
            return 0x80000002U;
        }
        if (read_u16(26) != 1U) {
            return 0x80000003U;
        }
        const uint32_t bits = read_u16(28);
        uint32_t format = 0;
        switch (bits) {
            case 8: format = 3; break;
            case 16: format = 0; break;
            case 24: format = 1; break;
            case 32: format = 2; break;
            default: return 0x80000004U;
        }
        if (read_u32(30) != 0) {
            return 0x80000005U;
        }
        const int32_t width =
            static_cast<int32_t>(read_u32(18));
        const int32_t height =
            static_cast<int32_t>(read_u32(22));
        if (width == 0 || height == 0) {
            return 0x80000006U;
        }
        if (width < 0 || height < 0) {
            return 0x80000007U;
        }
        const uint32_t source_bytes = bits / 8U;
        const uint32_t destination_bytes =
            bitmap_bytes_per_pixel(format);
        const uint64_t source_stride64 =
            (static_cast<uint64_t>(width) * source_bytes + 3U) &
            ~uint64_t{3};
        const uint64_t destination_stride64 =
            static_cast<uint64_t>(width) * destination_bytes;
        const uint32_t pixel_offset = read_u32(10);
        const uint64_t required64 =
            static_cast<uint64_t>(pixel_offset) +
            source_stride64 * static_cast<uint32_t>(height);
        if (destination_bytes == 0 ||
            required64 > data.size() ||
            destination_stride64 > std::numeric_limits<uint32_t>::max()) {
            return 0x80000007U;
        }
        GraphicsSurface surface;
        surface.width = static_cast<uint32_t>(width);
        surface.height = static_cast<uint32_t>(height);
        surface.format = format;
        surface.bytes_per_pixel = destination_bytes;
        surface.stride = static_cast<uint32_t>(destination_stride64);
        surface.pixels.assign(
            static_cast<std::size_t>(destination_stride64) *
                surface.height,
            0);
        const std::size_t source_stride =
            static_cast<std::size_t>(source_stride64);
        const std::size_t destination_stride =
            static_cast<std::size_t>(destination_stride64);
        const uint32_t copy_bytes =
            std::min(source_bytes, destination_bytes);
        for (uint32_t row = 0; row < surface.height; ++row) {
            const std::size_t source_row =
                static_cast<std::size_t>(pixel_offset) +
                static_cast<std::size_t>(
                    surface.height - row - 1U) * source_stride;
            const std::size_t destination_row =
                static_cast<std::size_t>(row) * destination_stride;
            for (uint32_t column = 0; column < surface.width; ++column) {
                std::copy_n(
                    data.begin() +
                        static_cast<std::ptrdiff_t>(
                            source_row +
                            static_cast<std::size_t>(column) *
                                source_bytes),
                    copy_bytes,
                    surface.pixels.begin() +
                        static_cast<std::ptrdiff_t>(
                            destination_row +
                            static_cast<std::size_t>(column) *
                                destination_bytes));
            }
        }
        install_allocated_graphics_surface(bitmap, std::move(surface));
        bitmap_aux_values_.erase(bitmap);
        return 0;
    }
    void store_render_cache_record(
        uint32_t id, std::vector<uint32_t> values) {
        render_cache_records_[id] = std::move(values);
    }
    [[nodiscard]] uint32_t render_cache_record_count(
        uint32_t id) const {
        const auto found = render_cache_records_.find(id);
        return found == render_cache_records_.end()
                   ? 0U
                   : static_cast<uint32_t>(found->second.size());
    }
    std::vector<uint32_t> take_render_cache_record(uint32_t id) {
        const auto found = render_cache_records_.find(id);
        if (found == render_cache_records_.end()) {
            return {};
        }
        std::vector<uint32_t> values = std::move(found->second);
        render_cache_records_.erase(found);
        return values;
    }
    struct WindowImageLayer {
        GraphicsSurface image;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t level = 0;
        uint32_t enabled = 0;
    };
    struct WindowObject {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t slot = 0;
        bool visible = false;
        uint32_t draw_enabled = 0;
        int32_t background_bitmap = -1;
        bool background_active = false;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t blend_mode = 0;
        uint32_t transparency = 0;
        uint32_t field_1c = 0;
        std::string font_name;
        uint32_t font_size = 0;
        uint32_t font_scale = 0;
        uint32_t font_field4 = 0;
        uint32_t font_field5 = 0;
        uint32_t font_field6 = 0;
        int32_t valid_x0 = 0;
        int32_t valid_y0 = 0;
        int32_t valid_x1 = 0;
        int32_t valid_y1 = 0;
        bool valid_region_set = false;
        int32_t update_left = 0;
        int32_t update_top = 0;
        int32_t update_right = -1;
        int32_t update_bottom = -1;
        uint32_t line_spacing_percent = 0;
        uint32_t isolated_composition = 0;
        uint32_t field_16c = 0;
        uint32_t field_18c = 0;
        uint32_t field_3a0 = 0;
        int32_t text_cursor_x_390 = 0;
        int32_t text_cursor_y_394 = 0;
        uint32_t text_boundary_mode_398 = 0;
        uint32_t text_field_39c = 0;
        std::array<uint32_t, 3> compose_order{0, 1, 2};
        bool selection_columns_enabled = false;
        std::array<uint32_t, 16> selection_columns{};
        uint32_t background_fill_color = 0;
        PixelBuffer background_plane;
        uint32_t plane_format = 2;
        uint32_t overlay_level = 0;
        std::array<WindowImageLayer, 8> image_layers;
        PixelBuffer overlay_plane;
        PixelBuffer text_saved_plane; // original +1C0 snapshot used by 00430A00
        PixelBuffer composite_plane;
    };
    struct WindowBitmapCommand {
        uint32_t bitmap = 0;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t record_value = 0;
        int32_t mask_bitmap = -1;
        uint32_t mode = 0;
        uint32_t opacity = 0xFFFFFFFFU;
        uint32_t layer = 0;
        bool plane_executed = false;
        bool icon_group_selection = false;
        uint32_t icon_group_owner = 0;
    };
    struct SelectionIconProcessor {
        bool extended = false;
        uint32_t mode = 0;
        uint32_t field_64c = 0;
        uint32_t mask_flag = 0;
        std::vector<std::array<uint32_t, 4>> basic_records;
        std::vector<std::array<uint32_t, 16>> extended_records;
        std::vector<uint32_t> highlight_ids;
    };
    uint32_t append_window_bitmap_records(
        uint32_t window, uint32_t count, uint32_t records,
        uint32_t base, bool extended_records) {
        if (count == 0 || count > 0x40U) {
            return 1;
        }
        const auto object = window_objects_.find(window);
        if (object == window_objects_.end()) {
            return 0xFF;
        }
        const uint32_t stride = extended_records ? 0x40U : 0x10U;
        // Original B5 compacts offsets 0/4/8 from each 0x40-byte record.
        const uint32_t x_offset = 0U;
        const uint32_t y_offset = 4U;
        const uint32_t bitmap_offset = 8U;
        std::fill(object->second.overlay_plane.begin(), object->second.overlay_plane.end(), 0);
        object->second.field_18c = 1;
        object->second.overlay_level = 0;
        for (auto& image : object->second.image_layers) image.enabled = 0;
        auto& commands = window_bitmap_commands_[window];
        for (uint32_t index = 0; index < count; ++index) {
            const uint32_t record = records + index * stride;
            const int32_t x = static_cast<int32_t>(
                memory_.read_u32(record + x_offset, base));
            const int32_t y = static_cast<int32_t>(
                memory_.read_u32(record + y_offset, base));
            const uint32_t bitmap =
                memory_.read_u32(record + bitmap_offset, base);
            const auto surface = graphics_surfaces_.find(bitmap);
            if (surface == graphics_surfaces_.end()) {
                continue;
            }
            const bool written = draw_window_plane(window,x,y,bitmap,0U,0U,true);
            commands.push_back({
                bitmap,
                x,
                y,
                surface->second.width,
                surface->second.height,
                extended_records ? 0xFFFFFFFFU
                                : memory_.read_u32(record + 12U, base),
                -1,
                0U,
                0U,
                1U,
                written,
            });
        }
        display_dirty_ = true;
        return 0;
    }
    uint32_t draw_window_bitmap(uint32_t window, int32_t x, int32_t y,
                                uint32_t bitmap, uint32_t mode,
                                uint32_t opacity, uint32_t layer = 0,
                                bool screen_coordinates = false) {
        const auto object = window_objects_.find(window);
        if (object == window_objects_.end()) {
            return 0xFF;
        }
        const auto surface = graphics_surfaces_.find(bitmap);
        if (surface == graphics_surfaces_.end()) {
            return 1;
        }
        if (screen_coordinates) {
            x -= object->second.x;
            y -= object->second.y;
        }
        if (layer <= 1 && !draw_window_plane(window, x, y, bitmap, mode, opacity, layer == 1)) {
            return 2;
        }
        window_bitmap_commands_[window].push_back({
            bitmap,
            x,
            y,
            surface->second.width,
            surface->second.height,
            0,
            -1,
            mode,
            opacity,
            layer,
            layer <= 1,
        });
        display_dirty_ = true;
        return 0;
    }
    uint32_t fill_window_background(uint32_t window, uint32_t color) {
        const auto object = window_objects_.find(window);
        if (object == window_objects_.end()) {
            return 0xFF;
        }
        WindowObject& value = object->second;
        if (value.width == 0 || value.height == 0) {
            return 1;
        }
        const uint32_t format = value.plane_format;
        const uint32_t bytes_per_pixel =
            bitmap_bytes_per_pixel(format);
        if (bytes_per_pixel == 0) {
            return 1;
        }
        value.background_plane.resize(
            static_cast<std::size_t>(value.width) *
                value.height * bytes_per_pixel,
            0);
        for (std::size_t pixel = 0;
             pixel <
             static_cast<std::size_t>(value.width) * value.height;
             ++pixel) {
            const std::size_t offset = pixel * bytes_per_pixel;
            switch (format) {
                case 0: {
                    const uint32_t stored = color & 0xFFFFU;
                    for (uint32_t byte = 0; byte < 2U; ++byte) {
                        value.background_plane[offset + byte] =
                            static_cast<uint8_t>(stored >> (byte * 8U));
                    }
                    break;
                }
                case 1:
                case 2: {
                    const uint32_t stored =
                        format == 1U ? color & 0xFFFFFFU : color;
                    for (uint32_t byte = 0; byte < 4U; ++byte) {
                        value.background_plane[offset + byte] =
                            static_cast<uint8_t>(stored >> (byte * 8U));
                    }
                    break;
                }
                case 3:
                    value.background_plane[offset] =
                        static_cast<uint8_t>(color);
                    break;
                default:
                    break;
            }
        }
        value.background_fill_color = color;
        value.background_bitmap = -1;
        value.background_active = false;
        display_dirty_ = true;
        return 0;
    }
    bool reset_window_composition(uint32_t window) {
        const auto object = window_objects_.find(window);
        if (object == window_objects_.end()) {
            return false;
        }
        std::fill(object->second.overlay_plane.begin(), object->second.overlay_plane.end(), 0);
        for (auto& layer : object->second.image_layers) {
            layer.enabled = 0;
        }
        window_bitmap_commands_.erase(window);
        selection_icon_processors_.erase(window);
        (void)set_window_text_direction(
            window, object->second.text_boundary_mode_398);
        display_dirty_ = true;
        return true;
    }
    [[nodiscard]] const std::vector<uint8_t>& window_background_plane(
        uint32_t window) const {
        static const std::vector<uint8_t> empty;
        const auto found = window_objects_.find(window);
        return found == window_objects_.end()
                   ? empty
                   : found->second.background_plane.cpu_pixels();
    }
    [[nodiscard]] uint32_t window_background_format() const {
        return current_capture_format_;
    }
    uint32_t create_selection_icon_processor(
        uint32_t window, uint32_t count, uint32_t records,
        uint32_t highlights, uint32_t mode, uint32_t field_64c,
        uint32_t mask_flag, uint32_t base, bool extended) {
        if (count == 0 || count > 0x40U) {
            return 1;
        }
        if (mode > 3U) {
            return 2;
        }
        const auto object = window_objects_.find(window);
        if (object == window_objects_.end()) {
            return 0xFF;
        }
        const uint32_t stride = extended ? 0x40U : 0x10U;
        SelectionIconProcessor processor;
        processor.extended = extended;
        processor.mode = mode;
        processor.field_64c = field_64c;
        processor.mask_flag = mask_flag;
        std::vector<WindowBitmapCommand> commands;
        for (uint32_t index = 0; index < count; ++index) {
            const uint32_t address = records + index * stride;
            std::array<uint32_t, 16> words{};
            const std::size_t word_count =
                extended ? words.size() : 4U;
            for (std::size_t word = 0; word < word_count; ++word) {
                words[word] = memory_.read_u32(
                    address + static_cast<uint32_t>(word * 4U),
                    base);
            }
            const uint32_t bitmap = words[2];
            const auto surface = graphics_surfaces_.find(bitmap);
            if (surface == graphics_surfaces_.end()) {
                return 2;
            }
            int32_t mask_bitmap = -1;
            if (extended && mask_flag != 0 &&
                static_cast<int32_t>(words[13]) != -1 &&
                graphics_surfaces_.count(words[13]) != 0) {
                mask_bitmap = static_cast<int32_t>(words[13]);
            }
            commands.push_back({
                bitmap,
                static_cast<int32_t>(words[0]),
                static_cast<int32_t>(words[1]),
                surface->second.width,
                surface->second.height,
                words[3],
                mask_bitmap,
                0x80U,
                0U,
            });
            if (highlights != 0) {
                processor.highlight_ids.push_back(
                    memory_.read_u32(
                        highlights + index * 4U, base));
            }
            if (extended) {
                processor.extended_records.push_back(words);
            } else {
                processor.basic_records.push_back({
                    words[0], words[1], words[2], words[3]});
            }
        }
        auto& queued = window_bitmap_commands_[window];
        queued.insert(
            queued.end(), commands.begin(), commands.end());
        selection_icon_processors_[window] = std::move(processor);
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::map<uint32_t, SelectionIconProcessor>&
    selection_icon_processors() const {
        return selection_icon_processors_;
    }
    [[nodiscard]] const std::map<
        uint32_t, std::vector<WindowBitmapCommand>>&
    window_bitmap_commands() const {
        return window_bitmap_commands_;
    }
    struct ControlObject {
        uint32_t id = 0;
        uint32_t slot = 0;
        uint32_t target = 0;
        bool enabled = true;
        int32_t absolute_x = 0;
        int32_t absolute_y = 0;
        int32_t interaction_x = 0;
        int32_t interaction_y = 0;
        uint32_t field_150 = 1;
        int32_t grab_x = 0;
        int32_t grab_y = 0;
        int32_t offset_x = 0;
        int32_t offset_y = 0;
        uint32_t size_x = 0;
        uint32_t size_y = 0;
        uint32_t scroll_left_164 = 0;
        uint32_t scroll_top_168 = 0;
        uint32_t scroll_right_16c = 0;
        uint32_t scroll_bottom_170 = 0;
        uint32_t query_174 = 0;
        uint32_t query_178 = 0;
        uint32_t query_17c = 0;
        uint32_t query_180 = 0;
    };
    struct EffectorEffectState {
        uint32_t mode = 0;
        uint32_t mode_value = 0;
        uint32_t primary_bitmap = 0;
        uint32_t secondary_bitmap = 0xFFFFFFFFU;
        uint32_t field_158 = 0;
        uint32_t effect = 0;
        uint32_t query = 0;
        std::array<uint32_t, 9> custom{};
        uint32_t vector_bitmap = 0;
        uint32_t vector_count = 0;
        uint32_t vector_source = 0;
        uint32_t vector_parameter_5 = 0;
        std::vector<std::array<uint32_t, 4>> vector_records;
        uint32_t primary_token = 0xFFFFFFFFU;
        uint32_t secondary_token = 0xFFFFFFFFU;
        uint32_t vector_token = 0xFFFFFFFFU;
        GraphicsSurface temporal_cache;
        uint32_t vector_phase = 0;
        uint32_t vector_bilinear = 0;
        std::array<int32_t, 6> custom_modulation{};
        std::array<int32_t, 5> custom_transform{};
    };
    struct ObjectAttachment {
        uint32_t parent = 0;
        int32_t offset_x = 0;
        int32_t offset_y = 0;
    };
    [[nodiscard]] bool has_effector_object(uint32_t handle) const {
        return effector_exists(handle);
    }
    bool configure_effector_bitmaps(
        uint32_t handle, uint32_t primary, uint32_t secondary,
        uint32_t field_158, uint32_t effect, uint32_t query) ;
    bool configure_effector_mode(uint32_t handle, uint32_t mode,
                                 uint32_t effect, uint32_t query) ;
    void refresh_effector_display_geometry(uint32_t handle);
    bool compose_effector_at(uint32_t destination,uint32_t handle,
                             uint32_t left=0,uint32_t top=0);
    void refresh_effector_parameters(uint32_t handle);
    void finish_effector_configuration(uint32_t handle,uint32_t effect,uint32_t query);
    uint32_t update_effector_vector_phase(uint32_t handle,uint32_t phase,uint32_t effect);
    bool set_effector_enabled(uint32_t handle, uint32_t enabled) {
        if (!effector_exists(handle)) return false;
        const bool before = object_draw_visible(handle);
        const bool previous_dirty = display_dirty_;
        if (!set_base_draw_enabled(handle, enabled)) return false;
        refresh_effector_display_geometry(handle);
        // PE64 84D00 compares effective visibility before/after virtual+08.
        display_dirty_ = previous_dirty || before != object_draw_visible(handle);
        return true;
    }
    bool configure_effector_custom(
        uint32_t handle, const std::array<uint32_t, 9>& values) ;
    uint32_t configure_effector_vector_records(
        uint32_t handle, uint32_t bitmap, uint32_t count,
        uint32_t source, uint32_t parameter_5, uint32_t query,
        uint32_t base) ;
    bool clear_effector_effect(uint32_t handle, uint32_t effect,
                               uint32_t query) ;
    [[nodiscard]] const std::map<uint32_t, EffectorEffectState>&
    effector_effects() const {
        return effector_effects_;
    }
    uint32_t attach_display_object(uint32_t parent, uint32_t child,
                                   int32_t offset_x, int32_t offset_y) {
        if (!object_exists(parent)) {
            return 0xFF;
        }
        if (!object_exists(child)) {
            return 6;
        }
        if (parent == child) {
            return 7;
        }
        if (object_owners_.count(child) != 0 &&
            object_class_id(parent) != 8U) {
            return 8;
        }

        // 0x0041FBF0 always allocates a new 0x10-byte link node. The
        // only repeated-child gate is the parent +0x18 class check above.
        group_children_[parent].push_back(child);
        object_owners_[child] = parent;
        group_child_offsets_[std::make_pair(parent, child)] = {
            offset_x, offset_y};
        object_attachments_[child] = {
            parent, offset_x, offset_y};

        const auto parent_position =
            object_absolute_positions_.find(parent);
        const std::array<uint32_t, 3> base =
            parent_position == object_absolute_positions_.end()
                ? std::array<uint32_t, 3>{0, 0, 0}
                : parent_position->second;
        const uint32_t x =
            base[0] + static_cast<uint32_t>(offset_x);
        const uint32_t y =
            base[1] + static_cast<uint32_t>(offset_y);
        if (!set_object_fixed_position(child, x, y, base[2])) {
            return 8;
        }
        display_dirty_ = true;
        return 0;
    }
    uint32_t detach_display_object(uint32_t parent, uint32_t child) {
        if (!object_exists(parent)) {
            return 0xFF;
        }
        if (!object_exists(child)) {
            return 6;
        }
        const auto children = group_children_.find(parent);
        if (children == group_children_.end()) {
            return 9;
        }
        const auto link = std::find(
            children->second.begin(), children->second.end(), child);
        if (link == children->second.end()) {
            return 9;
        }

        // 0x0041FCE0 removes the first matching node and unconditionally
        // clears the child's +0x12C parent pointer, even for a prior link.
        children->second.erase(link);
        group_child_offsets_.erase(std::make_pair(parent, child));
        object_owners_.erase(child);
        object_attachments_.erase(child);
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::map<uint32_t, ObjectAttachment>&
    object_attachments() const {
        return object_attachments_;
    }
    void register_control_pointer(uint32_t handle);
    void unregister_control_pointer(uint32_t handle);
    bool control_pointer_button(int32_t x, int32_t y, bool down);
    void control_pointer_motion(int32_t x, int32_t y);
    uint32_t create_control_object(uint32_t target) {
        const auto active = std::count_if(
            control_objects_.begin(), control_objects_.end(),
            [](const auto& slot) { return slot.has_value(); });
        if (active >= 32) {
            return 1;
        }
        if (!object_exists(target)) {
            return 2;
        }
        if (object_class_id(target) > 7U) {
            return 3;
        }
        uint32_t slot = 0;
        while (slot < control_objects_.size() &&
               control_objects_[slot].has_value()) {
            ++slot;
        }
        if (slot >= 32U) {
            return 1;
        }
        ControlObject control;
        control.id = next_control_object_id_++;
        control.slot = slot;
        control.target = target;
        if (const auto window = window_objects_.find(target);
            window != window_objects_.end()) {
            control.size_x = window->second.width;
            control.size_y = window->second.height;
        } else if (const auto display =
                       display_object_states_.find(target);
                   display != display_object_states_.end()) {
            control.size_x = display->second.width;
            control.size_y = display->second.height;
        }
        if (const auto position = object_position(target)) {
            control.absolute_x = position->first;
            control.absolute_y = position->second;
        }
        control.scroll_right_16c = control.size_x - 1U;
        control.scroll_bottom_170 = control.size_y - 1U;
        control_objects_[slot] = control;
        last_created_control_handle_ = 0xF0000000U | slot;
        return 0;
    }
    [[nodiscard]] uint32_t last_created_control_handle() const {
        return last_created_control_handle_;
    }
    // 0046E410/0046E3C0: DAT_00540A98 stores control pointers, not
    // display/input-region nodes. Instance ids preserve pointer identity when
    // the manager later reuses an F0 slot; duplicates are intentional.
    bool activate_control_candidate(uint32_t handle) {
        const auto* control = lookup_control(handle);
        if (!control) return false;
        active_control_candidates_.insert(active_control_candidates_.begin(), control->id);
        return true;
    }
    bool deactivate_control_candidate(uint32_t handle) {
        const auto* control = lookup_control(handle);
        if (!control) return false;
        const auto node = std::find(active_control_candidates_.begin(),
                                    active_control_candidates_.end(), control->id);
        if (node == active_control_candidates_.end()) return false;
        active_control_candidates_.erase(node);
        return true;
    }
    [[nodiscard]] const std::vector<uint32_t>& active_control_candidates() const {
        return active_control_candidates_;
    }
    bool destroy_control_object(uint32_t handle) {
        ControlObject* control = lookup_control(handle);
        if (control == nullptr) {
            return false;
        }
        unregister_control_pointer(handle);
        control_objects_[control->slot].reset();
        return true;
    }
    bool set_control_enabled(uint32_t handle, uint32_t enabled) {
        ControlObject* control = lookup_control(handle);
        if (control == nullptr) {
            return false;
        }
        control->enabled = enabled != 0;
        // CDspObjKnob::virtual+4 (00426360) forwards to its target.
        set_base_draw_enabled(control->target, enabled);
        return true;
    }
    bool set_control_absolute_position(uint32_t handle, int32_t x,
                                       int32_t y) {
        ControlObject* control = lookup_control(handle);
        if (control == nullptr) {
            return false;
        }
        control->absolute_x = x;
        control->absolute_y = y;
        // 00426390 recomputes the current interaction offset after moving
        // the control base; the script deliberately calls D6 before D5.
        uint32_t width=control->size_x, height=control->size_y;
        if (const auto target=window_objects_.find(control->target); target!=window_objects_.end()) {
            width=target->second.width; height=target->second.height;
        } else if (const auto target=display_object_states_.find(control->target); target!=display_object_states_.end()) {
            width=target->second.width; height=target->second.height;
        }
        const auto horizontal=control_axis_geometry(control->scroll_left_164,control->scroll_right_16c,
            static_cast<int32_t>(width-1U),control->offset_x);
        const auto vertical=control_axis_geometry(control->scroll_top_168,control->scroll_bottom_170,
            static_cast<int32_t>(height-1U),control->offset_y);
        set_object_position(control->target,
            static_cast<int32_t>(static_cast<uint32_t>(x)+static_cast<uint32_t>(
                control_axis_pixel_offset(control->interaction_x,control->offset_x,horizontal))),
            static_cast<int32_t>(static_cast<uint32_t>(y)+static_cast<uint32_t>(
                control_axis_pixel_offset(control->interaction_y,control->offset_y,vertical))));
        return true;
    }
    bool set_control_interaction_position(uint32_t handle, int32_t x,
                                          int32_t y) {
        ControlObject* control = lookup_control(handle);
        if (control == nullptr) return false;
        uint32_t target_width = control->size_x, target_height = control->size_y;
        if (const auto window = window_objects_.find(control->target); window != window_objects_.end()) {
            target_width = window->second.width;
            target_height = window->second.height;
        } else if (const auto display = display_object_states_.find(control->target); display != display_object_states_.end()) {
            target_width = display->second.width;
            target_height = display->second.height;
        }
        const auto horizontal = control_axis_geometry(control->scroll_left_164,control->scroll_right_16c,
            static_cast<int32_t>(target_width-1U),control->offset_x);
        const auto vertical = control_axis_geometry(control->scroll_top_168,control->scroll_bottom_170,
            static_cast<int32_t>(target_height-1U),control->offset_y);
        const bool valid_x = x >= 0 && x <= horizontal.limit;
        const bool valid_y = y >= 0 && y <= vertical.limit;
        // 004266A0 stores each valid axis even if the other axis is invalid.
        if (valid_x) control->interaction_x = x;
        if (valid_y) control->interaction_y = y;
        if (!valid_x || !valid_y) return false;
        const int32_t pixel_x = control_axis_pixel_offset(x,control->offset_x,horizontal);
        const int32_t pixel_y = control_axis_pixel_offset(y,control->offset_y,vertical);
        set_object_position(control->target,
            static_cast<int32_t>(static_cast<uint32_t>(control->absolute_x)+static_cast<uint32_t>(pixel_x)),
            static_cast<int32_t>(static_cast<uint32_t>(control->absolute_y)+static_cast<uint32_t>(pixel_y)));
        return true;
    }
    std::optional<std::pair<int32_t, int32_t>>
    control_interaction_position(uint32_t handle) const {
        const ControlObject* control = lookup_control(handle);
        if (control == nullptr) {
            return std::nullopt;
        }
        return std::pair<int32_t, int32_t>{
            control->interaction_x, control->interaction_y};
    }
    bool set_control_offset(uint32_t handle, int32_t x, int32_t y) {
        ControlObject* control = lookup_control(handle);
        if (control == nullptr || x < 0 || y < 0) {
            return false;
        }
        control->offset_x = x;
        control->offset_y = y;
        return true;
    }
    bool set_control_size(uint32_t handle, int32_t width, int32_t height) {
        ControlObject* control = lookup_control(handle);
        if (control == nullptr) return false;
        // CDspObjKnob +20 (00426340) forwards to the current target rectangle.
        uint32_t target_width = control->size_x, target_height = control->size_y;
        if (const auto target = window_objects_.find(control->target); target != window_objects_.end()) {
            target_width = target->second.width;
            target_height = target->second.height;
        } else if (const auto target = display_object_states_.find(control->target); target != display_object_states_.end()) {
            target_width = target->second.width;
            target_height = target->second.height;
        }
        if (width < static_cast<int32_t>(target_width) || height < static_cast<int32_t>(target_height)) return false;
        control->scroll_left_164 = 0;
        control->scroll_top_168 = 0;
        control->scroll_right_16c = static_cast<uint32_t>(width) - 1U;
        control->scroll_bottom_170 = static_cast<uint32_t>(height) - 1U;
        return true;
    }
    std::optional<uint32_t> take_control_query(uint32_t handle) {
        ControlObject* control = lookup_control(handle);
        if (control == nullptr) {
            return std::nullopt;
        }
        const uint32_t value = control->query_178;
        control->query_174 = 0;
        control->query_178 = 0;
        control->query_17c = 0;
        control->query_180 = 0;
        return value;
    }
    bool set_control_field_150(uint32_t handle, uint32_t value) {
        ControlObject* control = lookup_control(handle);
        if (control == nullptr) {
            return false;
        }
        control->field_150 = value;
        return true;
    }
    [[nodiscard]] const std::array<std::optional<ControlObject>, 32>&
    control_objects() const {
        return control_objects_;
    }
    std::optional<uint32_t> create_window_object(uint32_t width,
                                                 uint32_t height) {
        if (window_objects_.size() >= 16) {
            return std::nullopt;
        }
        const uint32_t object_id = next_window_object_id_++;
        const uint32_t priority = 0x6000U + (object_id & 0x1FFFU);
        const uint32_t scaled_width =
            width <= 31U ? (width << 5U) : width;
        const uint32_t scaled_height =
            height <= 19U ? (height << 5U) : height;
        if (scaled_width == 0 || scaled_width > 0x2000U ||
            scaled_height == 0 || scaled_height > 0x8000U) {
            return std::nullopt;
        }
        uint32_t slot = 0;
        while (window_objects_.count(0xB0000000U + slot) != 0) {
            ++slot;
        }
        const uint32_t handle = 0xB0000000U + slot;
        WindowObject window;
        // 004300C0 -> 00421190: default 32-bit RGB planes retain alpha.
        window.plane_format = current_capture_format_ == 1U ? 2U : current_capture_format_;
        window.blend_mode = 1;
        window.width = scaled_width;
        window.height = scaled_height;
        window.slot = slot;
        window.update_right = static_cast<int32_t>(scaled_width) - 1;
        window.update_bottom = static_cast<int32_t>(scaled_height) - 1;
        window_objects_[handle] = std::move(window);
        object_enabled_[handle] = 1;
        object_base_draw_enabled_[handle] = 0;
        object_child_propagation_[handle] = 0;
        object_feature_mask_[handle] = 0xFFFFFFFFU;
        object_absolute_position_dispatch_[handle] = 1;
        object_effect_level_[handle] = 0;
        object_visibility_threshold_[handle] = 0;
        object_positions_[handle] = {0, 0};
        object_absolute_positions_[handle] = {0, 0, 0};
        object_pair_3c_40_[handle] = {0, 0};
        object_pair_44_48_[handle] = {0, 0};
        for (auto it = object_raw_fields_.begin(); it != object_raw_fields_.end();) {
            if (it->first.first == handle) {
                it = object_raw_fields_.erase(it);
            } else {
                ++it;
            }
        }
        object_raw_fields_[{handle, 0x18U}] = 3;
        object_raw_fields_[{handle, 0x20U}] = object_id;
        object_raw_fields_[{handle, 0x84U}] = 1;
        object_raw_fields_[{handle, 0xB0U}] = 1;
        object_raw_fields_[{handle, 0xBCU}] = 256;
        object_raw_fields_[{handle, 0x110U}] = handle;
        object_display_classes_[handle] = DisplayObjectClass::CDspObjWindow;
        register_display_object(handle, priority, handle);
        display_object_states_.at(handle).evaluate_priority = true;
        update_display_object_geometry(
            handle, 0, 0, scaled_width, scaled_height, false);
        return handle;
    }
    bool destroy_window_object(uint32_t handle) {
        if (window_objects_.count(handle) == 0) {
            return false;
        }
        for (const auto& [id, processor] : icon_input_processors_) {
            (void)id;
            if (processor.window == handle) {
                return false;
            }
        }
        unregister_display_object(handle);
        window_objects_.erase(handle);
        window_bitmap_commands_.erase(handle);
        selection_icon_processors_.erase(handle);
        object_display_classes_.erase(handle);
        object_absolute_positions_.erase(handle);
        object_absolute_position_dispatch_.erase(handle);
        icon_layouts_.erase(handle);
        const auto owner = object_owners_.find(handle);
        if (owner != object_owners_.end()) {
            auto children = group_children_.find(owner->second);
            if (children != group_children_.end()) {
                std::erase(children->second, handle);
            }
            object_owners_.erase(owner);
        }
        display_dirty_ = true;
        return true;
    }
    struct DisplayObjectState {
        uint32_t priority = 0;
        uint32_t value = 0;
        uint64_t sequence = 0;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 0;
        uint32_t range_start = 0;
        uint32_t range_end = 0;
        uint32_t animation_frame = 0;
        uint32_t transform_opacity = 0;
        bool available = true;
        bool evaluate_priority = false;
        bool auxiliary = false;
    };
    struct ActiveDisplayNode {
        uint32_t id = 0;
        uint32_t priority = 0;
        bool event = false;
    };
    void register_display_object(uint32_t id, uint32_t priority,
                                 uint32_t value = 0) {
        DisplayObjectState state;
        state.priority = priority;
        state.value = value;
        state.sequence = next_display_sequence_++;
        display_object_states_[id] = state;
    }
    [[nodiscard]] std::vector<uint32_t> ordered_display_objects() const;
    bool reinsert_display_object(uint32_t id);
    [[nodiscard]] uint32_t base_display_priority(uint32_t id) const;
    void refresh_sprite_display_geometry(uint32_t id);
    void initialize_affine_sprite_mode5(uint32_t id, std::span<const uint32_t> operands);
    void refresh_affine_sprite_mode5(uint32_t id);
    bool unregister_display_list_object(uint32_t id);
    void update_display_object_geometry(
        uint32_t id, int32_t x, int32_t y, uint32_t width,
        uint32_t height, bool available) {
        const auto found = display_object_states_.find(id);
        if (found == display_object_states_.end()) {
            return;
        }
        found->second.x = x;
        found->second.y = y;
        found->second.width = width;
        found->second.height = height;
        found->second.available = available;
        for (InputRegion& region : input_regions_) {
            if (region.object != id) {
                continue;
            }
            region.left = x;
            region.top = y;
            region.right = x + static_cast<int32_t>(width) - 1;
            region.bottom = y + static_cast<int32_t>(height) - 1;
        }
    }
    void update_display_object_priority(uint32_t id, uint32_t priority) {
        const auto found = display_object_states_.find(id);
        if (found != display_object_states_.end()) {
            found->second.priority = priority;
            found->second.evaluate_priority = false;
            reinsert_display_object(id);
        }
    }
    void update_display_object_value(uint32_t id, uint32_t value) {
        const auto found = display_object_states_.find(id);
        if (found != display_object_states_.end()) {
            found->second.value = value;
        }
    }
    void register_display_object_region(uint32_t id, std::optional<uint32_t> input_key = std::nullopt) {
        const auto object = display_object_states_.find(id);
        if (object == display_object_states_.end()) {
            return;
        }
        remove_input_regions_for_object(id);
        const DisplayObjectState& state = object->second;
        if (state.width == 0 || state.height == 0) {
            return;
        }
        add_input_region({
            input_key.value_or(state.priority),
            state.x,
            state.y,
            state.x + static_cast<int32_t>(state.width) - 1,
            state.y + static_cast<int32_t>(state.height) - 1,
            id,
        });
    }
    void add_display_object_region(uint32_t id) {
        const auto object = display_object_states_.find(id);
        if (object == display_object_states_.end()) {
            return;
        }
        const DisplayObjectState& state = object->second;
        if (state.width == 0 || state.height == 0) {
            return;
        }
        add_input_region({
            state.priority,
            state.x,
            state.y,
            state.x + static_cast<int32_t>(state.width) - 1,
            state.y + static_cast<int32_t>(state.height) - 1,
            id,
        });
    }
    void remove_input_regions_for_object(uint32_t object) {
        std::erase_if(input_regions_, [object](const InputRegion& region) {
            return region.object == object;
        });
    }
    void unregister_display_object(uint32_t id) {
        remove_input_regions_for_object(id);
        display_objects_.erase(id);
        object_display_classes_.erase(id);
        unregister_display_list_object(id);
        std::erase(active_display_candidates_, id);
        std::erase_if(active_display_objects_, [id](
                                                  const ActiveDisplayNode& node) {
            return node.id == id;
        });
        std::erase_if(display_bitmap_draws_, [id](
                                                const DisplayBitmapDraw& draw) {
            return draw.display_object == id;
        });
        if (active_display_id_ == id) {
            active_display_id_ = 0;
        }
    }
    bool activate_display_candidate(uint32_t id) {
        if (display_object_states_.count(id) == 0) {
            return false;
        }
        active_display_candidates_.insert(
            active_display_candidates_.begin(), id);
        add_display_object_region(id);
        return true;
    }
    bool deactivate_display_candidate(uint32_t id) {
        if (display_object_states_.count(id) == 0) {
            return false;
        }
        const auto found =
            std::find(active_display_candidates_.begin(),
                      active_display_candidates_.end(), id);
        if (found == active_display_candidates_.end()) {
            return false;
        }
        active_display_candidates_.erase(found);
        return true;
    }
    bool publish_active_display_object(uint32_t id) {
        const auto object = display_object_states_.find(id);
        if (object == display_object_states_.end()) {
            return false;
        }
        const auto position = std::find_if(
            active_display_objects_.begin(), active_display_objects_.end(),
            [&object](const ActiveDisplayNode& node) {
                return node.priority < object->second.priority;
            });
        active_display_objects_.insert(
            position,
            ActiveDisplayNode{id, object->second.priority, false});
        return true;
    }
    [[nodiscard]] bool has_active_display_object(uint32_t id) const {
        return std::any_of(
            active_display_objects_.begin(), active_display_objects_.end(),
            [id](const ActiveDisplayNode& node) { return node.id == id; });
    }
    void mark_active_display_object(uint32_t id) {
        for (ActiveDisplayNode& node : active_display_objects_) {
            if (node.id == id) {
                node.event = true;
            }
        }
    }
    uint32_t take_active_display_event() {
        uint32_t result = 0;
        for (ActiveDisplayNode& node : active_display_objects_) {
            if (node.event) {
                result = node.id;
                node.event = false;
            }
        }
        return result;
    }
    bool set_current_active_display_object(uint32_t id) {
        if (id != 0 && display_object_states_.count(id) == 0) {
            return false;
        }
        active_display_id_ = id;
        return true;
    }
    [[nodiscard]] const std::map<uint32_t, DisplayObjectState>&
    display_object_states() const {
        return display_object_states_;
    }
    struct TouchWindowNode {
        uint32_t id = 0;
        uint32_t object = 0;
        uint32_t value = 0;
    };
    bool register_touch_window(uint32_t object) {
        if (!object_exists(object)) {
            return false;
        }
        touch_windows_.insert(
            touch_windows_.begin(),
            TouchWindowNode{next_touch_window_id_++, object, 0});
        add_display_object_region(object);
        return true;
    }
    bool unregister_touch_window(uint32_t object) {
        const auto found = std::find_if(
            touch_windows_.begin(), touch_windows_.end(),
            [object](const TouchWindowNode& node) {
                return node.object == object;
            });
        if (found == touch_windows_.end()) {
            return false;
        }
        touch_windows_.erase(found);
        remove_input_regions_for_object(object);
        return true;
    }
    void clear_touch_windows() {
        for (const TouchWindowNode& node : touch_windows_) {
            remove_input_regions_for_object(node.object);
        }
        touch_windows_.clear();
        next_touch_window_id_ = 0;
    }
    [[nodiscard]] const std::vector<TouchWindowNode>& touch_windows() const {
        return touch_windows_;
    }
    uint32_t hit_touch_window(int32_t x, int32_t y) const {
        for (const TouchWindowNode& node : touch_windows_) {
            const auto display = display_object_states_.find(node.object);
            if (display != display_object_states_.end()) {
                const DisplayObjectState& state = display->second;
                if (x >= state.x &&
                    x < state.x + static_cast<int32_t>(state.width) &&
                    y >= state.y &&
                    y < state.y + static_cast<int32_t>(state.height)) {
                    return node.id;
                }
                continue;
            }
            const auto window = window_objects_.find(node.object);
            if (window != window_objects_.end() &&
                x >= window->second.x &&
                x < window->second.x +
                        static_cast<int32_t>(window->second.width) &&
                y >= window->second.y &&
                y < window->second.y +
                        static_cast<int32_t>(window->second.height)) {
                return node.id;
            }
        }
        return 0xFFFFFFFFU;
    }
    std::optional<uint32_t> touch_window_value(uint32_t id) const {
        const auto found = std::find_if(
            touch_windows_.begin(), touch_windows_.end(),
            [id](const TouchWindowNode& node) {
                return node.id == id;
            });
        if (found == touch_windows_.end()) {
            return std::nullopt;
        }
        return found->value;
    }
    [[nodiscard]] uint32_t active_display_object_value() const {
        const auto object = display_object_states_.find(active_display_id_);
        return object == display_object_states_.end()
                   ? 0U
                   : object->second.value;
    }
    [[nodiscard]] const std::vector<uint32_t>& active_display_candidates()
        const {
        return active_display_candidates_;
    }
    [[nodiscard]] const std::map<uint32_t, WindowObject>&
    window_objects() const {
        return window_objects_;
    }
    [[nodiscard]] std::optional<uint32_t> window_field_3a0(
        uint32_t handle) const {
        const auto found = window_objects_.find(handle);
        if (found == window_objects_.end()) {
            return std::nullopt;
        }
        return found->second.field_3a0;
    }
    bool set_window_field_3a0(uint32_t handle, uint32_t value) {
        const auto found = window_objects_.find(handle);
        if (found == window_objects_.end()) {
            return false;
        }
        found->second.field_3a0 = value;
        return true;
    }
    bool set_window_text_cursor(
        uint32_t handle, int32_t x, int32_t y) {
        const auto found = window_objects_.find(handle);
        if (found == window_objects_.end()) {
            return false;
        }
        found->second.text_cursor_x_390 = x;
        found->second.text_cursor_y_394 = y;
        return true;
    }
    bool set_window_text_boundary_mode(
        uint32_t handle, uint32_t mode) {
        const auto found = window_objects_.find(handle);
        if (found == window_objects_.end()) {
            return false;
        }
        found->second.text_boundary_mode_398 = mode;
        return true;
    }
    std::optional<bool> set_window_text_direction(
        uint32_t handle, uint32_t mode) {
        const auto found = window_objects_.find(handle);
        if (found == window_objects_.end()) {
            return std::nullopt;
        }
        if (mode >= 2U) {
            return false;
        }
        WindowObject& window = found->second;
        window.text_boundary_mode_398 = mode;
        window.text_cursor_y_394 = window.update_top;
        window.text_cursor_x_390 =
            mode == 0 ? window.update_left : window.update_right;
        return true;
    }
    std::optional<bool> set_window_text_field_39c(
        uint32_t handle, uint32_t value) {
        const auto found = window_objects_.find(handle);
        if (found == window_objects_.end()) {
            return std::nullopt;
        }
        if (value >= 3U) {
            return false;
        }
        found->second.text_field_39c = value;
        return true;
    }
    [[nodiscard]] std::optional<std::pair<int32_t, int32_t>>
    window_text_cursor(uint32_t handle) const {
        const auto found = window_objects_.find(handle);
        if (found == window_objects_.end()) {
            return std::nullopt;
        }
        return std::pair<int32_t, int32_t>{
            found->second.text_cursor_x_390,
            found->second.text_cursor_y_394};
    }
    [[nodiscard]] std::optional<bool> window_text_cursor_at_boundary(
        uint32_t handle) const {
        const auto found = window_objects_.find(handle);
        if (found == window_objects_.end()) {
            return std::nullopt;
        }
        const WindowObject& window = found->second;
        const uint32_t metric = text_metrics_[4];
        if (window.text_boundary_mode_398 == 0U) {
            return static_cast<uint32_t>(window.text_cursor_x_390) ==
                   metric + static_cast<uint32_t>(window.update_left);
        }
        if (window.text_boundary_mode_398 == 1U) {
            return static_cast<uint32_t>(window.text_cursor_y_394) ==
                   metric + static_cast<uint32_t>(window.update_top);
        }
        return false;
    }
    bool query_window_update_rect(uint32_t handle, uint32_t destination,
                                  uint32_t base) {
        const auto window = window_objects_.find(handle);
        if (window == window_objects_.end()) {
            return false;
        }
        memory_.write_u32(destination, base,
                          static_cast<uint32_t>(window->second.update_left));
        memory_.write_u32(destination + 4U, base,
                          static_cast<uint32_t>(window->second.update_top));
        memory_.write_u32(destination + 8U, base,
                          static_cast<uint32_t>(window->second.update_right));
        memory_.write_u32(destination + 12U, base,
                          static_cast<uint32_t>(window->second.update_bottom));
        return true;
    }
    bool set_window_update_rect(uint32_t handle, int32_t left,
                                int32_t top, int32_t right,
                                int32_t bottom) {
        const auto window = window_objects_.find(handle);
        if (window == window_objects_.end()) {
            return false;
        }
        window->second.update_left = left;
        window->second.update_top = top;
        window->second.update_right = right;
        window->second.update_bottom = bottom;
        return true;
    }
    struct WindowCaptureSnapshot {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 2;
        std::vector<uint8_t> pixels;
    };
    using WindowCaptureProvider =
        std::function<std::optional<WindowCaptureSnapshot>(uint32_t)>;
    void set_window_capture_provider(WindowCaptureProvider provider) {
        window_capture_provider_ = std::move(provider);
    }
    bool capture_window_to_bitmap(uint32_t type, uint32_t handle) {
        if (type >= 0x4000U) {
            return false;
        }
        const auto window = window_objects_.find(handle);
        if (window == window_objects_.end()) return false;
        std::optional<WindowCaptureSnapshot> captured;
        if (window_capture_provider_) {
            captured = window_capture_provider_(handle);
        } else {
            // 004318F0 copies the window's owned +154 composite descriptor.
            // The host display/SDL backbuffer is not the source of this native.
            if (!recompose_window_basic_planes(handle)) return false;
            captured = WindowCaptureSnapshot{window->second.width,window->second.height,
                2U,std::as_const(window->second.composite_plane).cpu_pixels()};
        }
        if (!captured || captured->width == 0 ||
            captured->height == 0 || captured->format != 2U) {
            return false;
        }
        uint32_t format = current_capture_format_;
        if (format == 1U) {
            format = 2U;
        }
        const std::size_t destination_size =
            static_cast<std::size_t>(window->second.width) *
            window->second.height * 4U;
        const std::size_t source_stride =
            static_cast<std::size_t>(captured->width) * 4U;
        if (captured->pixels.size() <
            source_stride * captured->height) {
            return false;
        }
        GraphicsSurface surface;
        surface.width = window->second.width;
        surface.height = window->second.height;
        surface.format = format;
        surface.bytes_per_pixel = 4;
        surface.stride = surface.width * 4U;
        const bool exact_storage=captured->width==surface.width && captured->height==surface.height &&
            captured->pixels.size()==destination_size;
        if(exact_storage)surface.pixels=std::move(captured->pixels);
        else surface.pixels.assign(destination_size, 0);
        const uint32_t copy_width =
            std::min(surface.width, captured->width);
        const uint32_t copy_height =
            std::min(surface.height, captured->height);
        if (!exact_storage && copy_width != 0 && copy_height != 0) {
            for (uint32_t row = 0; row < copy_height; ++row) {
                std::copy_n(
                    captured->pixels.begin() +
                        static_cast<std::ptrdiff_t>(
                            static_cast<std::size_t>(row) *
                            source_stride),
                    static_cast<std::size_t>(copy_width) * 4U,
                    surface.pixels.begin() +
                        static_cast<std::ptrdiff_t>(
                            static_cast<std::size_t>(row) *
                            surface.stride));
            }
        }
        // Resolve lazy buffers once. Capture's presentation mirror is opaque
        // even when its native format2 bytes carry nonopaque alpha.
        auto* const rgba=surface.rgba.overwrite(destination_size);
        const auto* const pixels=std::as_const(surface.pixels).data();
        for (std::size_t index = 0;
             index < destination_size / 4U; ++index) {
            rgba[index * 4U] = pixels[index * 4U + 2U];
            rgba[index * 4U + 1U] = pixels[index * 4U + 1U];
            rgba[index * 4U + 2U] = pixels[index * 4U];
            rgba[index * 4U + 3U] = 0xFF;
        }
        install_allocated_graphics_surface(type, std::move(surface));
        return true;
    }
    uint32_t set_window_background(uint32_t handle, int32_t bitmap);
    bool recompose_window_basic_planes(uint32_t window);
    bool set_window_overlay_level(uint32_t window, uint32_t level);
    bool set_window_image_layer(uint32_t window, uint32_t index, uint32_t bitmap);
    bool set_window_image_layer_position(uint32_t window, uint32_t index,
                                         int32_t x, int32_t y, uint32_t level);
    bool set_window_image_layer_enabled(uint32_t window, uint32_t index, uint32_t enabled);


    bool configure_window(uint32_t handle, int32_t x, int32_t y,
                          uint32_t blend_mode, uint32_t transparency,
                          uint32_t field_1c) {
        const auto window = window_objects_.find(handle);
        if (window == window_objects_.end()) {
            return false;
        }
        window->second.x = x;
        window->second.y = y;
        window->second.blend_mode = blend_mode;
        window->second.transparency = transparency;
        object_raw_fields_[{handle, 0xB0U}] = blend_mode;
        set_raw_field_recursive(handle, 0xB4U, transparency);
        if (field_1c < 0x10000U) {
            window->second.field_1c = field_1c;
            object_raw_fields_[{handle, 0x1CU}] = field_1c;
        }
        update_display_object_geometry(
            handle, x, y, window->second.width, window->second.height,
            window->second.visible);
        reinsert_display_object(handle);
        display_dirty_ = true;
        return true;
    }
    bool configure_window_font(uint32_t handle, uint32_t font_id,
                               uint32_t font_size, uint32_t font_scale,
                               uint32_t field4, uint32_t field5,
                               uint32_t field6) {
        const auto window = window_objects_.find(handle);
        if (window == window_objects_.end()) {
            return false;
        }
        const std::string name = font_name(font_id);
        if ((font_id != 0 && name.empty()) ||
            font_size < 4 || font_size > 200 ||
            font_scale < 25 || font_scale > 200) {
            return false;
        }
        window->second.font_name = name;
        window->second.font_size = font_size;
        window->second.font_scale = font_scale;
        window->second.font_field4 = field4;
        window->second.font_field5 = field5;
        window->second.font_field6 = field6;
        display_dirty_ = true;
        return true;
    }
    bool set_window_valid_region(uint32_t handle, int32_t x, int32_t y,
                                 int32_t width, int32_t height) {
        const auto window = window_objects_.find(handle);
        if (window == window_objects_.end()) {
            return false;
        }
        auto& state = window->second;
        // PE32 00430880 -> 00430820 / PE64 140068CF0 -> 140068D30.
        // Endpoints use wrapped dword arithmetic; inverted rectangles are
        // accepted when all four endpoints remain inside the client surface.
        const auto right = static_cast<int32_t>(static_cast<uint32_t>(x) +
            static_cast<uint32_t>(width) - 1U);
        const auto bottom = static_cast<int32_t>(static_cast<uint32_t>(y) +
            static_cast<uint32_t>(height) - 1U);
        const auto inside = [](int32_t value, uint32_t limit) {
            return value >= 0 && static_cast<uint32_t>(value) < limit;
        };
        if (!inside(x, state.width) || !inside(right, state.width) ||
            !inside(y, state.height) || !inside(bottom, state.height)) return false;
        state.valid_x0 = state.update_left = x;
        state.valid_y0 = state.update_top = y;
        state.valid_x1 = state.update_right = right;
        state.valid_y1 = state.update_bottom = bottom;
        state.valid_region_set = true;
        // Original setter invokes 00431550 / 140067CA0, not just clipping.
        // Other raw direction values retain the previous cursor.
        if (state.text_boundary_mode_398 == 0U) {
            state.text_cursor_x_390 = x;
            state.text_cursor_y_394 = y;
        } else if (state.text_boundary_mode_398 == 1U) {
            state.text_cursor_x_390 = right;
            state.text_cursor_y_394 = y;
        }
        display_dirty_ = true;
        return true;
    }
    bool set_window_line_spacing(uint32_t handle, uint32_t percent) {
        const auto window = window_objects_.find(handle);
        if (window == window_objects_.end() || percent > 800U) {
            return false;
        }
        window->second.line_spacing_percent = percent;
        display_dirty_ = true;
        return true;
    }
    bool set_window_isolated_composition(uint32_t handle, uint32_t value) {
        const auto window = window_objects_.find(handle);
        if (window == window_objects_.end()) {
            return false;
        }
        window->second.isolated_composition = value;
        display_dirty_ = true;
        return true;
    }
    bool set_window_composition_order(uint32_t handle, uint32_t mode) {
        static constexpr std::array<std::array<uint32_t, 3>, 6> orders{{
            {{0, 1, 2}},
            {{0, 2, 1}},
            {{1, 0, 2}},
            {{1, 2, 0}},
            {{2, 0, 1}},
            {{2, 1, 0}},
        }};
        const auto window = window_objects_.find(handle);
        if (window == window_objects_.end() || mode >= orders.size()) {
            return false;
        }
        window->second.compose_order = orders[mode];
        display_dirty_ = true;
        return true;
    }
    bool set_window_draw_enabled(uint32_t handle, uint32_t enabled) {
        const auto window = window_objects_.find(handle);
        if (window == window_objects_.end()) {
            return false;
        }
        window->second.draw_enabled = enabled;
        set_base_draw_enabled(handle, enabled);
        refresh_window_display_geometry(handle);
        display_dirty_ = true;
        return true;
    }
    bool set_window_field_16c(uint32_t handle, uint32_t value) {
        const auto window = window_objects_.find(handle);
        if (window == window_objects_.end()) {
            return false;
        }
        window->second.field_16c = value;
        display_dirty_ = true;
        return true;
    }
    bool set_window_field_18c(uint32_t handle, uint32_t value) {
        const auto window = window_objects_.find(handle);
        if (window == window_objects_.end()) {
            return false;
        }
        window->second.field_18c = value;
        return true;
    }
    bool set_window_selection_columns(
        uint32_t handle,
        const std::optional<std::array<uint32_t, 16>>& columns) {
        const auto window = window_objects_.find(handle);
        if (window == window_objects_.end()) {
            return false;
        }
        window->second.selection_columns_enabled = columns.has_value();
        if (columns) {
            window->second.selection_columns = *columns;
        }
        return true;
    }
    struct IconInputLayout {
        uint32_t window_count = 0;
        uint32_t icon_count = 0;
        std::vector<uint32_t> group_counts;
    };
    uint32_t apply_icon_input_layout(uint32_t window, uint32_t descriptor,
                                     uint32_t base) {
        if (window_objects_.count(window) == 0) {
            return 1;
        }
        if (descriptor == 0) {
            return 2;
        }
        const uint32_t window_count =
            memory_.read_u32(descriptor, base);
        if (window_count == 0 || window_count > 256U) {
            return 2;
        }
        const uint32_t descriptors =
            memory_.read_u32(descriptor + 4, base);
        if (descriptors == 0) {
            return 2;
        }
        IconInputLayout layout;
        layout.window_count = window_count;
        layout.group_counts.reserve(window_count);
        struct StaticIcon {
            uint32_t bitmap, depth;
            int32_t x, y;
        };
        std::vector<StaticIcon> static_icons;
        uint32_t sequence = 0;
        for (uint32_t group = 0; group < window_count; ++group) {
            const uint32_t entry =
                descriptors + group * 0x40U;
            const uint32_t count = memory_.read_u32(entry, base);
            if (count == 0 || count > (compact_64_opcodes_ ? 1024U : 256U)) {
                return 3;
            }
            if (memory_.read_u32(entry + 8, base) == 0) {
                return 3;
            }
            layout.group_counts.push_back(count);
            layout.icon_count += count;
            const uint32_t records = memory_.read_u32(entry + 8U, base);
            const uint32_t selected = memory_.read_u32(entry + 12U, base);
            for (uint32_t index = 0; index < count; ++index, ++sequence) {
                const uint32_t record = records + index * 0xC4U;
                const uint32_t enabled = memory_.read_u32(record + 4U, base);
                const uint32_t flags = memory_.read_u32(record + 0xC0U, base);
                if (enabled == 0 || (flags & 0x40U) != 0) continue;
                uint32_t bitmap = memory_.read_u32(record + 0x20U, base);
                const uint32_t highlighted = memory_.read_u32(record + 0x28U, base);
                if (bitmap != 0xFFFFFFFFU && index == selected && highlighted != 0xFFFFFFFFU)
                    bitmap = highlighted;
                const auto image = graphics_surfaces_.find(bitmap);
                if (image == graphics_surfaces_.end() || image->second.width <= 1U ||
                    image->second.height <= 1U) continue;
                const uint32_t depth = (flags & 0x10U) != 0 ? enabled :
                    (flags & 2U) != 0 ? memory_.read_u32(record + 12U, base) : sequence;
                // 00430E20/140068460 adds the anchor to the child position;
                // its unity-scale sprite transform subtracts that same anchor.
                static_icons.push_back({bitmap, depth,
                    static_cast<int32_t>(memory_.read_u32(record + 8U, base)),
                    static_cast<int32_t>(memory_.read_u32(record + 12U, base))});
            }
        }
        // B7 is a static window drawing operation, independent of B8/B9's
        // live input processor. 00452EB0 / 140092F20 reset the old window-local
        // children/overlay and commit each selected child into that overlay.
        auto& owned = window_objects_.at(window);
        std::fill(owned.overlay_plane.begin(), owned.overlay_plane.end(), 0);
        owned.field_18c = 1;
        owned.overlay_level = 0;
        for (auto& image : owned.image_layers) image.enabled = 0;
        auto& commands = window_bitmap_commands_[window];
        commands.clear();
        selection_icon_processors_.erase(window);
        std::stable_sort(static_icons.begin(), static_icons.end(),
            [](const StaticIcon& left, const StaticIcon& right) { return left.depth < right.depth; });
        for (const auto& icon : static_icons) {
            const auto& image = graphics_surfaces_.at(icon.bitmap);
            const bool written = draw_window_plane(window, icon.x, icon.y, icon.bitmap, 1U, 0U, true);
            commands.push_back({icon.bitmap, icon.x, icon.y, image.width, image.height,
                icon.depth, -1, 1U, 0U, 1U, written});
        }
        icon_layouts_[window] = std::move(layout);
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::map<uint32_t, IconInputLayout>&
    icon_layouts() const {
        return icon_layouts_;
    }
    std::optional<uint32_t> create_group_object() {
        for (uint32_t index = 1; index < 512; ++index) {
            const uint32_t handle = 0xF1000000U | index;
            if (group_objects_.insert(handle).second) {
                // 00426090 -> 0041F390: +84 starts1, +14 starts0.
                object_enabled_[handle] = 1;
                object_base_draw_enabled_[handle] = 0;
                object_raw_fields_[{handle, 0x84U}] = 1;
                object_absolute_positions_[handle] = {0, 0, 0};
                object_display_classes_[handle] =
                    DisplayObjectClass::CDspObjGroup;
                return handle;
            }
        }
        return std::nullopt;
    }
    bool destroy_group_object(uint32_t group) {
        if (group_objects_.erase(group) == 0) {
            return false;
        }
        const auto children = group_children_.find(group);
        if (children != group_children_.end()) {
            for (const uint32_t child : children->second) {
                const auto owner = object_owners_.find(child);
                if (owner != object_owners_.end() &&
                    owner->second == group) {
                    object_owners_.erase(owner);
                }
            }
            group_children_.erase(children);
        }
        std::erase_if(group_child_offsets_, [group](const auto& entry) {
            return entry.first.first == group;
        });
        group_states_.erase(group);
        object_positions_.erase(group);
        object_display_classes_.erase(group);
        object_absolute_positions_.erase(group);
        object_absolute_position_dispatch_.erase(group);
        display_dirty_ = true;
        return true;
    }
    [[nodiscard]] const std::set<uint32_t>& group_objects() const {
        return group_objects_;
    }
    struct IconAnimationPathRecord {
        int32_t angle_delta_q16 = 0;
        uint32_t duration_ms = 0;
        uint32_t parameter = 0;
    };
    struct IconInputIconRecord {
        std::array<uint32_t, 0x31> words{};
        uint32_t display_object_id = 0;
        bool runtime_present = false; // Original per-icon detail +0.
        uint32_t runtime_enabled = 0; // Detail +4 / child CDspObj +4.
        int32_t render_x = 0;
        int32_t render_y = 0;
        uint32_t render_depth = 0;
        uint32_t render_effect = 0;
        // Target descriptor +0x18/+0x1C. The legacy names are kept because
        // the property-message API exposes the same pair as a frame range.
        uint32_t range_start = 0;
        uint32_t range_end = 0;
        uint32_t animation_frame = 0;
        uint64_t animation_deadline_ms = 0;
        std::vector<IconAnimationPathRecord> animation_paths;
        uint32_t animation_angle_q16 = 0;
        uint32_t animation_path_angle_q16 = 0;
        uint32_t animation_path_index = 0;
        uint32_t animation_subframe = 0;
        uint32_t animation_subframe_count = 0;
        uint64_t animation_path_deadline_ms = 0;
    };
    struct IconInputGroupRecord {
        std::array<uint32_t, 0x10> words{};
        std::vector<IconInputIconRecord> icons;
    };
    struct ExtendedIconInputLayout {
        std::array<uint32_t, 10> words{};
        std::vector<IconInputGroupRecord> groups;
    };
    struct BasicIconInputIconRecord {
        std::array<uint32_t, 15> words{};
        uint32_t display_object_id = 0;
        int32_t render_x = 0;
        int32_t render_y = 0;
    };
    struct BasicIconInputGroupRecord {
        std::array<uint32_t, 13> words{};
        std::vector<BasicIconInputIconRecord> icons;
    };
    struct BasicIconInputLayout {
        std::array<uint32_t, 8> words{};
        std::vector<BasicIconInputGroupRecord> groups;
    };
    struct IconMotion {
        uint32_t group=0, icon=0, state=0;
        uint32_t effect_first=0, effect_last=0;
        uint32_t reverse_easing=0, forward_easing=0;
        uint32_t duration=0, delay=0;
        uint64_t started=0;
        std::array<uint32_t,3> final_point{};
        std::vector<std::array<uint32_t,3>> points;
    };
    struct IconInputProcessor {
        uint32_t id = 0;
        uint32_t window = 0;
        uint32_t mode = 0;
        uint32_t value_3c = 0;
        std::map<int32_t,IconMotion> motions;
        ExtendedIconInputLayout layout;
        std::optional<BasicIconInputLayout> basic_layout;
        std::vector<uint32_t> basic_group_values;
        std::deque<std::array<uint32_t, 3>> events;
        std::array<uint32_t, 6> state{};
        uint32_t value_88 = 0;
        int32_t selected_group = -1;
        int32_t selected_icon = -1;
        uint32_t selected_state = 0;
        std::optional<uint32_t> pressed_icon;
        std::optional<uint32_t> pending_icon; // release-inside action confirmation (+0x90/+0xC8)
        std::optional<uint32_t> hovered_icon; // original flattened hover +0x74
    };
    uint32_t configure_icon_motion(uint32_t id,const std::array<uint32_t,10>& arguments,uint32_t base);
    void set_icon_motion_transform(IconInputProcessor&,IconInputIconRecord&,int32_t x,int32_t y,int32_t z,uint32_t effect);
    bool transition_icon_motion(IconInputProcessor&,uint32_t group,uint32_t icon,uint32_t flag,uint32_t delay,uint64_t now);
    void advance_icon_motions(IconInputProcessor&,uint64_t now);
    using IconInputTrace = std::function<void(
        uint32_t id, uint32_t window, uint32_t mode)>;
    void set_icon_input_trace(IconInputTrace trace) {
        icon_input_trace_ = std::move(trace);
    }
    using StepTrace = std::function<void(
        uint32_t ip, uint32_t opcode, uint32_t sub_opcode)>;
    void set_step_trace(StepTrace trace) {
        step_trace_ = std::move(trace);
    }
    uint32_t icon_input_window_key(const IconInputProcessor& processor) const;
    void register_icon_processor_input(IconInputProcessor& processor);
    void release_icon_processor_input(const IconInputProcessor& processor);
    bool icon_input_priority_hit(const IconInputProcessor& processor, int32_t x, int32_t y) const;
    bool input_object_mask_hit(uint32_t object, int32_t x, int32_t y,
                               uint32_t width, uint32_t height) const;
    uint32_t create_icon_input_processor(uint32_t window, uint32_t mode) {
        if (window_objects_.count(window) == 0) {
            throw BpFault("icon input processor requires a window object");
        }
        const uint32_t id = ++next_icon_input_processor_id_;
        IconInputProcessor processor;
        processor.id = id;
        processor.window = window;
        processor.mode = mode;
        icon_input_processors_[id] = std::move(processor);
        if (icon_input_trace_) {
            icon_input_trace_(id, window, mode);
        }
        return id;
    }
    bool query_icon_input_value(uint32_t id, uint32_t destination,
                                uint32_t base) {
        const auto processor = icon_input_processors_.find(id);
        if (processor == icon_input_processors_.end()) {
            return false;
        }
        memory_.write_u32(
            destination, base, processor->second.value_3c);
        return true;
    }
    bool read_icon_input_group_values(uint32_t id, uint32_t destination,
                                      uint32_t base) {
        const auto processor = icon_input_processors_.find(id);
        if (processor == icon_input_processors_.end()) {
            return false;
        }
        for (std::size_t index = 0;
             index < processor->second.basic_group_values.size();
             ++index) {
            memory_.write_u32(
                destination + static_cast<uint32_t>(index * 4U), base,
                processor->second.basic_group_values[index]);
        }
        return true;
    }
    uint32_t configure_basic_icon_input(uint32_t id, uint32_t descriptor,
                                        uint32_t base) {
        const auto processor = icon_input_processors_.find(id);
        if (processor == icon_input_processors_.end()) {
            return 1;
        }
        if (processor->second.mode != 0) {
            return 4;
        }
        if (descriptor == 0) {
            return 2;
        }
        BasicIconInputLayout layout;
        for (std::size_t index = 0; index < layout.words.size(); ++index) {
            layout.words[index] = memory_.read_u32(
                descriptor + static_cast<uint32_t>(index * 4U), base);
        }
        const uint32_t group_count = layout.words[0];
        const uint32_t group_source = layout.words[1];
        if (group_count == 0 || group_count > 256U ||
            group_source == 0) {
            return 2;
        }
        layout.groups.reserve(group_count);
        std::vector<uint32_t> group_values;
        group_values.reserve(group_count);
        for (uint32_t group_index = 0; group_index < group_count;
             ++group_index) {
            const uint32_t source =
                group_source + group_index * 0x34U;
            BasicIconInputGroupRecord group;
            for (std::size_t index = 0; index < group.words.size();
                 ++index) {
                group.words[index] = memory_.read_u32(
                    source + static_cast<uint32_t>(index * 4U), base);
            }
            const uint32_t icon_count = group.words[0] & 0xFFFFU;
            const uint32_t icon_source = group.words[1];
            if (icon_count == 0 || icon_count > 256U ||
                icon_source == 0) {
                return 3;
            }
            group.icons.reserve(icon_count);
            for (uint32_t icon_index = 0; icon_index < icon_count;
                 ++icon_index) {
                const uint32_t icon_address =
                    icon_source + icon_index * 0x3CU;
                BasicIconInputIconRecord icon;
                for (std::size_t index = 0;
                     index < icon.words.size(); ++index) {
                    icon.words[index] = memory_.read_u32(
                        icon_address +
                            static_cast<uint32_t>(index * 4U),
                        base);
                }
                group.icons.push_back(icon);
            }
            group_values.push_back(group.words[2]);
            layout.groups.push_back(std::move(group));
        }
        // Basic vtable+34 is also 00451C00: retire its old virtual children
        // before installing replacement records and their cached masks.
        if (processor->second.basic_layout) {
            for (const auto& group : processor->second.basic_layout->groups)
                for (const auto& icon : group.icons)
                    if (icon.display_object_id != 0) {
                        unregister_display_object(icon.display_object_id);
                        object_masks_.erase(icon.display_object_id);
                    }
        }
        release_icon_processor_input(processor->second);
        processor->second.basic_layout = std::move(layout);
        processor->second.basic_group_values = std::move(group_values);
        processor->second.state[0] = 1;
        const uint32_t painted = apply_basic_icon_window(processor->second.window,descriptor,base);
        if (painted != 0) return painted;
        refresh_basic_icon_input_regions(id);
        // 0044F770 constructs the virtual child once, then treats word6 as
        // its explicit input mask. The displayed bitmap's alpha is unrelated.
        for (const auto& group : processor->second.basic_layout->groups) {
            for (const auto& icon : group.icons) {
                const int32_t mask = static_cast<int32_t>(icon.words[6]);
                if (mask == -2) {
                    cache_object_input_mask(icon.display_object_id, -2);
                } else if (mask >= 0) {
                    auto source = graphics_surfaces_.find(static_cast<uint32_t>(mask));
                    if (source != graphics_surfaces_.end() && source->second.format <= 3U &&
                        materialize_graphics_pixels(source->second))
                        cache_object_input_mask(icon.display_object_id, mask);
                }
            }
        }
        register_icon_processor_input(processor->second);
        return 0;
    }
    void refresh_basic_icon_input_regions(uint32_t id) {
        auto processor = icon_input_processors_.find(id);
        if (processor == icon_input_processors_.end() ||
            !processor->second.basic_layout) {
            return;
        }
        const auto window =
            window_objects_.find(processor->second.window);
        const int32_t window_x =
            window == window_objects_.end() ? 0 : window->second.x;
        const int32_t window_y =
            window == window_objects_.end() ? 0 : window->second.y;
        uint32_t sequence = 0;
        for (std::size_t group_index = 0;
             group_index < processor->second.basic_layout->groups.size();
             ++group_index) {
            BasicIconInputGroupRecord& group =
                processor->second.basic_layout->groups[group_index];
            for (std::size_t icon_index = 0;
                 icon_index < group.icons.size(); ++icon_index) {
                BasicIconInputIconRecord& icon = group.icons[icon_index];
                if (icon.display_object_id == 0) {
                    icon.display_object_id = ++next_display_object_id_;
                }
                icon.render_x =
                    static_cast<int32_t>(icon.words[1]);
                icon.render_y =
                    static_cast<int32_t>(icon.words[2]);
                uint32_t bitmap = icon.words[3];
                if (icon_index == group.words[2] &&
                    static_cast<int32_t>(icon.words[4]) != -1) {
                    bitmap = icon.words[4];
                }
                const auto surface = graphics_surfaces_.find(bitmap);
                const bool available =
                    icon.words[0] != 0 &&
                    surface != graphics_surfaces_.end();
                const uint32_t width =
                    available ? surface->second.width : 0;
                const uint32_t height =
                    available ? surface->second.height : 0;
                const uint32_t priority = 0x8000U + sequence;
                if (display_object_states_.count(
                        icon.display_object_id) == 0) {
                    register_display_object(
                        icon.display_object_id, priority);
                } else {
                    update_display_object_priority(
                        icon.display_object_id, priority);
                }
                update_display_object_geometry(
                    icon.display_object_id,
                    window_x + icon.render_x,
                    window_y + icon.render_y,
                    width, height, available);
                update_display_object_value(
                    icon.display_object_id,
                    (static_cast<uint32_t>(group_index) << 16U) |
                        static_cast<uint32_t>(icon_index));
                // 00452510/00451750 register ordinary icon regions only.
                // 0046E1D0's separate active list accepts CDspObjKnob handles.
                register_display_object_region(icon.display_object_id,
                    icon_input_window_key(processor->second) + 0x8000U);
                ++sequence;
            }
        }
        input_regions_active_ = true;
    }
    void apply_icon_group_selection_layers(
        IconInputProcessor& processor, std::size_t group_index) {
        auto& commands =
            window_bitmap_commands_[processor.window];
        auto owned_window = window_objects_.find(processor.window);
        if (owned_window != window_objects_.end()) {
            owned_window->second.image_layers[0].enabled = 0;
            owned_window->second.image_layers[1].enabled = 0;
        }
        std::erase_if(
            commands,
            [&processor](const WindowBitmapCommand& command) {
                return command.icon_group_selection &&
                       command.icon_group_owner == processor.id;
            });
        const bool extended = processor.mode != 0;
        if ((extended &&
             group_index >= processor.layout.groups.size()) ||
            (!extended &&
             (!processor.basic_layout ||
              group_index >= processor.basic_layout->groups.size()))) {
            display_dirty_ = true;
            return;
        }
        const std::array<uint32_t, 0x10>* extended_words = nullptr;
        const std::array<uint32_t, 13>* basic_words = nullptr;
        if (extended) {
            extended_words =
                &processor.layout.groups[group_index].words;
        } else {
            basic_words =
                &processor.basic_layout->groups[group_index].words;
        }
        const auto word = [&](std::size_t index) {
            if (extended) {
                return (*extended_words)[index + 2U];
            }
            return (*basic_words)[index];
        };
        for (uint32_t layer = 0; layer < 2U; ++layer) {
            const std::size_t base = 7U + layer * 3U;
            const uint32_t bitmap = word(base + 2U);
            const auto surface = graphics_surfaces_.find(bitmap);
            if (surface == graphics_surfaces_.end()) {
                continue;
            }
            if (owned_window != window_objects_.end()) {
                auto& image = owned_window->second.image_layers[layer];
                image.image = surface->second;
                image.x = static_cast<int32_t>(word(base));
                image.y = static_cast<int32_t>(word(base+1U));
                image.level = 0;
                image.enabled = 1;
            }
            commands.push_back({
                bitmap,
                static_cast<int32_t>(word(base)),
                static_cast<int32_t>(word(base + 1U)),
                surface->second.width,
                surface->second.height,
                processor.id,
                -1,
                1U,
                0U,
                layer,
                true,
                true,
                processor.id,
            });
        }
        display_dirty_ = true;
    }
    uint32_t apply_basic_icon_window(uint32_t window,
                                     uint32_t descriptor,
                                     uint32_t base) {
        const auto object = window_objects_.find(window);
        if (object == window_objects_.end()) {
            return 1;
        }
        if (descriptor == 0) {
            return 2;
        }
        std::array<uint32_t, 8> header{};
        for (std::size_t index = 0; index < header.size(); ++index) {
            header[index] = memory_.read_u32(
                descriptor + static_cast<uint32_t>(index * 4U), base);
        }
        const uint32_t group_count = header[0];
        const uint32_t group_source = header[1];
        if (group_count == 0 || group_count > 256U ||
            group_source == 0) {
            return 2;
        }
        std::fill(object->second.overlay_plane.begin(), object->second.overlay_plane.end(), 0);
        object->second.field_18c = 1;
        object->second.overlay_level = 0;
        for (auto& image : object->second.image_layers) image.enabled = 0;
        auto& commands = window_bitmap_commands_[window];
        for (uint32_t group_index = 0; group_index < group_count;
             ++group_index) {
            const uint32_t group_address =
                group_source + group_index * 0x34U;
            BasicIconInputGroupRecord group;
            for (std::size_t index = 0; index < group.words.size();
                 ++index) {
                group.words[index] = memory_.read_u32(
                    group_address +
                        static_cast<uint32_t>(index * 4U),
                    base);
            }
            const uint32_t icon_count = group.words[0] & 0xFFFFU;
            const uint32_t icon_source = group.words[1];
            if (icon_count == 0 || icon_count > 256U ||
                icon_source == 0) {
                return 3;
            }
            for (uint32_t icon_index = 0; icon_index < icon_count;
                 ++icon_index) {
                const uint32_t icon_address =
                    icon_source + icon_index * 0x3CU;
                BasicIconInputIconRecord icon;
                for (std::size_t index = 0;
                     index < icon.words.size(); ++index) {
                    icon.words[index] = memory_.read_u32(
                        icon_address +
                            static_cast<uint32_t>(index * 4U),
                        base);
                }
                if (icon.words[0] == 0) {
                    continue;
                }
                int32_t bitmap =
                    static_cast<int32_t>(icon.words[3]);
                if (bitmap != -1 &&
                    icon_index == group.words[2] &&
                    static_cast<int32_t>(icon.words[4]) != -1) {
                    bitmap = static_cast<int32_t>(icon.words[4]);
                }
                if (bitmap < 0) {
                    continue;
                }
                const auto surface = graphics_surfaces_.find(
                    static_cast<uint32_t>(bitmap));
                if (surface == graphics_surfaces_.end()) {
                    continue;
                }
                const bool written = draw_window_plane(window,
                    static_cast<int32_t>(icon.words[1]), static_cast<int32_t>(icon.words[2]),
                    static_cast<uint32_t>(bitmap), 0U, 0U, true);
                commands.push_back({
                    static_cast<uint32_t>(bitmap),
                    static_cast<int32_t>(icon.words[1]),
                    static_cast<int32_t>(icon.words[2]),
                    surface->second.width,
                    surface->second.height,
                    group_index,
                    -1,
                    0U,
                    0U,
                    1U,
                    written,
                });
            }
        }
        display_dirty_ = true;
        return 0;
    }
    bool destroy_icon_input_processor(uint32_t id) {
        const auto processor = icon_input_processors_.find(id);
        if (processor == icon_input_processors_.end()) {
            return false;
        }
        release_icon_processor_input(processor->second);
        for (const IconInputGroupRecord& group :
             processor->second.layout.groups) {
            for (const IconInputIconRecord& icon : group.icons) {
                if (icon.display_object_id != 0) {
                    unregister_display_object(icon.display_object_id);
                    object_masks_.erase(icon.display_object_id);
                }
            }
        }
        if (processor->second.basic_layout) {
            for (const BasicIconInputGroupRecord& group :
                 processor->second.basic_layout->groups) {
                for (const BasicIconInputIconRecord& icon : group.icons) {
                    if (icon.display_object_id != 0) {
                        unregister_display_object(icon.display_object_id);
                    object_masks_.erase(icon.display_object_id);
                    }
                }
            }
        }
        if (auto commands =
                window_bitmap_commands_.find(processor->second.window);
            commands != window_bitmap_commands_.end()) {
            std::erase_if(
                commands->second,
                [id](const WindowBitmapCommand& command) {
                    return command.icon_group_selection &&
                           command.icon_group_owner == id;
                });
        }
        icon_input_processors_.erase(processor);
        return true;
    }
    static constexpr uint32_t icon_full_turn_q16 = 0x1680000U;
    static uint32_t wrap_icon_angle(int64_t angle) {
        constexpr int64_t full_turn =
            static_cast<int64_t>(icon_full_turn_q16);
        angle %= full_turn;
        if (angle < 0) {
            angle += full_turn;
        }
        return static_cast<uint32_t>(angle);
    }
    static bool icon_index_is_selected(
        const IconInputGroupRecord& group, std::size_t icon_index) {
        return group.words[3] ==
               static_cast<uint32_t>(icon_index);
    }
    [[nodiscard]] uint32_t icon_render_bitmap(
        const IconInputGroupRecord& group,
        std::size_t icon_index) const {
        if (icon_index >= group.icons.size()) {
            return 0;
        }
        const IconInputIconRecord& icon = group.icons[icon_index];
        bool hovered = false;
        for (const auto& [id, processor] : icon_input_processors_) {
            (void)id;
            if (!processor.hovered_icon) continue;
            const uint32_t packed = *processor.hovered_icon;
            const std::size_t hovered_group = packed >> 16U;
            if (hovered_group < processor.layout.groups.size() &&
                &processor.layout.groups[hovered_group] == &group &&
                (packed & 0xFFFFU) == icon_index) {
                hovered = true;
                break;
            }
        }
        return select_extended_icon_bitmap(icon.words, icon.animation_frame,
            icon_index_is_selected(group, icon_index), hovered);
    }
    [[nodiscard]] double icon_render_angle_degrees(
        const IconInputIconRecord& icon) const {
        return static_cast<double>(icon.animation_angle_q16) /
               65536.0;
    }
    struct IconRenderBounds {
        int32_t left = 0;
        int32_t top = 0;
        int32_t right = -1;
        int32_t bottom = -1;
    };
    [[nodiscard]] IconRenderBounds icon_render_bounds(
        const IconInputGroupRecord& group,
        std::size_t icon_index) const {
        if (icon_index >= group.icons.size()) {
            return {};
        }
        const IconInputIconRecord& icon = group.icons[icon_index];
        const auto surface =
            graphics_surfaces_.find(icon_render_bitmap(group, icon_index));
        if (surface == graphics_surfaces_.end() ||
            surface->second.width == 0 ||
            surface->second.height == 0) {
            return {};
        }
        const double radians =
            icon_render_angle_degrees(icon) * std::numbers::pi / 180.0;
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        const double left = static_cast<double>(icon.render_x);
        const double top = static_cast<double>(icon.render_y);
        const double right =
            left + static_cast<double>(surface->second.width - 1U);
        const double bottom =
            top + static_cast<double>(surface->second.height - 1U);
        const std::array<std::pair<double, double>, 4> corners{{
            {left, top},
            {right, top},
            {right, bottom},
            {left, bottom},
        }};
        double minimum_x = std::numeric_limits<double>::infinity();
        double minimum_y = std::numeric_limits<double>::infinity();
        double maximum_x = -std::numeric_limits<double>::infinity();
        double maximum_y = -std::numeric_limits<double>::infinity();
        for (const auto& [x, y] : corners) {
            const double local_x = x - left;
            const double local_y = y - top;
            const double rotated_x =
                left + local_x * cosine - local_y * sine;
            const double rotated_y =
                top + local_x * sine + local_y * cosine;
            minimum_x = std::min(minimum_x, rotated_x);
            minimum_y = std::min(minimum_y, rotated_y);
            maximum_x = std::max(maximum_x, rotated_x);
            maximum_y = std::max(maximum_y, rotated_y);
        }
        return {
            static_cast<int32_t>(std::floor(minimum_x)),
            static_cast<int32_t>(std::floor(minimum_y)),
            static_cast<int32_t>(std::ceil(maximum_x)),
            static_cast<int32_t>(std::ceil(maximum_y)),
        };
    }
    static std::optional<std::pair<uint32_t, uint32_t>>
    icon_source_pixel(double x, double y,
                      uint32_t width, uint32_t height) {
        const int32_t fixed_x =
            static_cast<int32_t>(x * 65536.0);
        const int32_t fixed_y =
            static_cast<int32_t>(y * 65536.0);
        if (fixed_x < 0 || fixed_y < 0 ||
            static_cast<uint32_t>(fixed_x >> 16) >= width ||
            static_cast<uint32_t>(fixed_y >> 16) >= height) {
            return std::nullopt;
        }
        return std::pair{
            static_cast<uint32_t>(fixed_x >> 16),
            static_cast<uint32_t>(fixed_y >> 16),
        };
    }
    [[nodiscard]] std::optional<DecodedBitmap> render_icon_frame(
        const IconInputGroupRecord& group,
        std::size_t icon_index) const {
        if (icon_index >= group.icons.size()) {
            return std::nullopt;
        }
        const IconInputIconRecord& icon = group.icons[icon_index];
        const uint32_t bitmap = icon_render_bitmap(group, icon_index);
        const auto source = decoded_bitmap(bitmap);
        if (!source || source->width == 0 || source->height == 0) {
            return std::nullopt;
        }
        if (icon.animation_angle_q16 == 0) {
            return source;
        }
        const IconRenderBounds bounds =
            icon_render_bounds(group, icon_index);
        if (bounds.right < bounds.left ||
            bounds.bottom < bounds.top) {
            return std::nullopt;
        }
        const uint32_t width =
            static_cast<uint32_t>(bounds.right - bounds.left + 1);
        const uint32_t height =
            static_cast<uint32_t>(bounds.bottom - bounds.top + 1);
        DecodedBitmap output;
        output.width = width;
        output.height = height;
        output.format = 2;
        output.rgba.assign(
            static_cast<std::size_t>(width) * height * 4U, 0);
        const double radians =
            icon_render_angle_degrees(icon) *
            std::numbers::pi / 180.0;
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        const auto to_fixed = [](double value) {
            return static_cast<int32_t>(value * 65536.0);
        };
        const double left =
            static_cast<double>(bounds.left - icon.render_x);
        const double top =
            static_cast<double>(bounds.top - icon.render_y);
        const uint32_t start_x = static_cast<uint32_t>(to_fixed(
            left * cosine + top * sine));
        const uint32_t start_y = static_cast<uint32_t>(to_fixed(
            -left * sine + top * cosine));
        const uint32_t column_step_x =
            static_cast<uint32_t>(to_fixed(cosine));
        const uint32_t column_step_y =
            static_cast<uint32_t>(to_fixed(-sine));
        const uint32_t row_step_x =
            static_cast<uint32_t>(to_fixed(sine));
        const uint32_t row_step_y =
            static_cast<uint32_t>(to_fixed(cosine));
        const auto sample = [&](int32_t x, int32_t y,
                                std::array<uint8_t, 4>& pixel) {
            pixel.fill(0);
            if (x < 0 || y < 0 ||
                static_cast<uint32_t>(x) >= source->width ||
                static_cast<uint32_t>(y) >= source->height) {
                return;
            }
            const std::size_t offset =
                (static_cast<std::size_t>(y) *
                     source->width +
                 static_cast<uint32_t>(x)) *
                4U;
            std::copy_n(
                source->rgba.begin() +
                    static_cast<std::ptrdiff_t>(offset),
                4U, pixel.begin());
        };
        for (uint32_t row = 0; row < height; ++row) {
            const uint32_t row_x =
                start_x + row * row_step_x;
            const uint32_t row_y =
                start_y + row * row_step_y;
            for (uint32_t column = 0; column < width; ++column) {
                const int32_t fixed_x =
                    static_cast<int32_t>(
                        row_x + column * column_step_x);
                const int32_t fixed_y =
                    static_cast<int32_t>(
                        row_y + column * column_step_y);
                const int32_t source_x = fixed_x >> 16;
                const int32_t source_y = fixed_y >> 16;
                const int32_t fraction_x =
                    (static_cast<uint32_t>(fixed_x) >> 12U) & 0xFU;
                const int32_t fraction_y =
                    (static_cast<uint32_t>(fixed_y) >> 12U) & 0xFU;
                std::array<uint8_t, 4> top_left{};
                std::array<uint8_t, 4> top_right{};
                std::array<uint8_t, 4> bottom_left{};
                std::array<uint8_t, 4> bottom_right{};
                sample(source_x, source_y, top_left);
                sample(source_x + 1, source_y, top_right);
                sample(source_x, source_y + 1, bottom_left);
                sample(source_x + 1, source_y + 1, bottom_right);
                const std::size_t destination_offset =
                    (static_cast<std::size_t>(row) * width +
                     column) *
                    4U;
                for (std::size_t channel = 0; channel < 4;
                     ++channel) {
                    const int32_t top_value =
                        static_cast<int32_t>(top_left[channel]) +
                        ((static_cast<int32_t>(
                              top_right[channel]) -
                          static_cast<int32_t>(
                              top_left[channel])) *
                         fraction_x >>
                         4);
                    const int32_t bottom_value =
                        static_cast<int32_t>(
                            bottom_left[channel]) +
                        ((static_cast<int32_t>(
                              bottom_right[channel]) -
                          static_cast<int32_t>(
                              bottom_left[channel])) *
                         fraction_x >>
                         4);
                    const int32_t value =
                        top_value +
                        ((bottom_value - top_value) *
                         fraction_y >>
                         4);
                    output.rgba[destination_offset + channel] =
                        static_cast<uint8_t>(
                            std::clamp(value, 0, 255));
                }
            }
        }
        return output;
    }
    void register_icon_input_region(
        uint32_t id, const IconInputGroupRecord& group,
        std::size_t icon_index, int32_t window_x, int32_t window_y,
        uint32_t input_key) {
        const auto object = display_object_states_.find(id);
        if (object == display_object_states_.end()) {
            return;
        }
        remove_input_regions_for_object(id);
        const IconRenderBounds bounds =
            icon_render_bounds(group, icon_index);
        if (bounds.right < bounds.left || bounds.bottom < bounds.top) {
            return;
        }
        add_input_region({
            input_key,
            window_x + bounds.left,
            window_y + bounds.top,
            window_x + bounds.right,
            window_y + bounds.bottom,
            id,
        });
    }
    uint32_t configure_extended_icon_input(uint32_t id, uint32_t descriptor,
                                           uint32_t base) {
        const auto processor = icon_input_processors_.find(id);
        if (processor == icon_input_processors_.end()) {
            return 1;
        }
        if (processor->second.mode != 1 && processor->second.mode != 2) {
            return 4;
        }
        if (descriptor == 0) {
            return 2;
        }
        ExtendedIconInputLayout layout;
        for (std::size_t index = 0; index < layout.words.size(); ++index) {
            layout.words[index] = memory_.read_u32(
                descriptor + static_cast<uint32_t>(index * 4U), base);
        }
        const uint32_t group_count = layout.words[0];
        if (group_count == 0 || group_count > 256U ||
            layout.words[1] == 0) {
            return 2;
        }
        layout.groups.reserve(group_count);
        for (uint32_t group_index = 0; group_index < group_count;
             ++group_index) {
            const uint32_t source =
                layout.words[1] + group_index * 0x40U;
            IconInputGroupRecord group;
            for (std::size_t index = 0; index < group.words.size();
                 ++index) {
                group.words[index] = memory_.read_u32(
                    source + static_cast<uint32_t>(index * 4U), base);
            }
            const uint32_t icon_count = group.words[0] & 0xFFFFU;
            const uint32_t icon_source = group.words[2];
            if (icon_count == 0 || icon_count > 256U ||
                icon_source == 0) {
                return 3;
            }
            group.icons.reserve(icon_count);
            for (uint32_t icon_index = 0; icon_index < icon_count;
                 ++icon_index) {
                const uint32_t icon_address =
                    icon_source + icon_index * 0xC4U;
                IconInputIconRecord icon;
                for (std::size_t index = 0;
                     index < icon.words.size(); ++index) {
                    icon.words[index] = memory_.read_u32(
                        icon_address +
                            static_cast<uint32_t>(index * 4U),
                        base);
                }
                icon.render_x =
                    static_cast<int32_t>(
                        icon.words[2] + icon.words[4]);
                icon.render_y =
                    static_cast<int32_t>(
                        icon.words[3] + icon.words[5]);
                icon.range_start = icon.words[6];
                icon.range_end = icon.words[7];
                const std::size_t path_count =
                    static_cast<int32_t>(icon.words[14]) > 0
                        ? std::min<std::size_t>(
                              icon.words[14],
                              (icon.words.size() - 15U) / 3U)
                        : 0U;
                icon.animation_paths.reserve(path_count);
                for (std::size_t path = 0;
                     path < path_count; ++path) {
                    const std::size_t first = 15U + path * 3U;
                    icon.animation_paths.push_back({
                        static_cast<int32_t>(icon.words[first]),
                        icon.words[first + 1U],
                        icon.words[first + 2U],
                    });
                }
                const uint32_t initial_bitmap = group.words[3] == icon_index && icon.words[10] != 0xFFFFFFFFU
                    ? icon.words[10] : icon.words[8];
                icon.runtime_present = icon.words[1] != 0 && graphics_surfaces_.count(initial_bitmap) != 0;
                icon.runtime_enabled = (icon.words[48] & 0x40U) == 0 ? 1U : 0U;
                icon.animation_angle_q16 = 0;
                icon.animation_path_angle_q16 =
                    wrap_icon_angle(icon.words[39]);
                group.icons.push_back(icon);
            }
            layout.groups.push_back(std::move(group));
        }
        if (auto owned = window_objects_.find(processor->second.window); owned != window_objects_.end()) {
            std::fill(owned->second.overlay_plane.begin(),owned->second.overlay_plane.end(),0);
            owned->second.field_18c = 0;
            owned->second.overlay_level = 0;
            for (auto& image : owned->second.image_layers) image.enabled = 0;
        }
        // 00452510 begins with virtual+34 (00454010 -> 00451C00):
        // unregister and delete the previous window-local display children.
        // Their IDs cannot outlive the layout records used by callbacks.
        for (const auto& old_group : processor->second.layout.groups)
            for (const auto& old_icon : old_group.icons)
                if (old_icon.display_object_id != 0) {
                    unregister_display_object(old_icon.display_object_id);
                    object_masks_.erase(old_icon.display_object_id);
                }
        release_icon_processor_input(processor->second);
        processor->second.pressed_icon.reset();
        processor->second.pending_icon.reset();
        processor->second.hovered_icon.reset();
        processor->second.layout = std::move(layout);
        refresh_icon_input_regions(id);
        // 00452510 constructs each child with word12 as its independent mask.
        for (const auto& group : processor->second.layout.groups)
            for (const auto& icon : group.icons) {
                const int32_t mask = static_cast<int32_t>(icon.words[12]);
                if (mask == -2) cache_object_input_mask(icon.display_object_id, -2);
                else if (mask >= 0) {
                    auto source = graphics_surfaces_.find(static_cast<uint32_t>(mask));
                    if (source != graphics_surfaces_.end() && source->second.format <= 3U &&
                        materialize_graphics_pixels(source->second))
                        cache_object_input_mask(icon.display_object_id, mask);
                }
            }
        register_icon_processor_input(processor->second);
        if (debug_enabled()) icon_input_layout_history_.push_back(processor->second.layout);
        processor->second.state[0] = 1;
        return 0;
    }
    uint32_t set_extended_icon_input_item_state(
        uint32_t id, uint32_t group_index, uint32_t icon_index,
        uint32_t value) {
        const auto processor = icon_input_processors_.find(id);
        if (processor == icon_input_processors_.end()) {
            return 1;
        }
        if (processor->second.mode != 1 && processor->second.mode != 2) {
            return 4;
        }
        if (group_index >= processor->second.layout.groups.size()) {
            return 0;
        }
        IconInputGroupRecord& group =
            processor->second.layout.groups[group_index];
        if (icon_index >= group.icons.size()) {
            return 0;
        }
        IconInputIconRecord& icon = group.icons[icon_index];
        if (!icon.runtime_present) {
            return 0;
        }
        icon.runtime_enabled = value;
        refresh_icon_input_regions(id);
        display_dirty_ = true;
        return 0;
    }
    bool dequeue_icon_input_event(uint32_t id, uint32_t destination,
                                  uint32_t base) {
        const auto processor = icon_input_processors_.find(id);
        if (processor == icon_input_processors_.end()) {
            return false;
        }
        if (!processor->second.events.empty()) {
            const auto event = processor->second.events.front();
            processor->second.events.pop_front();
            memory_.write_u32(destination, base, event[0]);
            memory_.write_u32(destination + 4, base, event[1]);
            memory_.write_u32(destination + 8, base, event[2]);
            return true;
        }
        memory_.write_u32(destination, base, 0);
        memory_.write_u32(destination + 4, base, 0);
        memory_.write_u32(destination + 8, base, 0);
        return false;
    }
    void enqueue_icon_input_event(uint32_t id,
                                  const std::array<uint32_t, 3>& event) {
        const auto processor = icon_input_processors_.find(id);
        if (processor != icon_input_processors_.end()) {
            processor->second.events.push_back(event);
        }
    }
    static uint32_t icon_input_priority(
        const IconInputIconRecord& icon, uint32_t sequence) {
        const uint32_t flags = icon.words[0x30];
        uint32_t priority = sequence;
        if ((flags & 0x10U) != 0) {
            priority = icon.words[1];
        } else if ((flags & 0x2U) != 0) {
            priority = icon.words[3];
        }
        return priority + 0x8000U;
    }
    static uint32_t icon_motion_render_priority(const IconInputIconRecord& icon, uint32_t sequence);
    static bool icon_accepts_input(const IconInputIconRecord& icon) {
        return icon.runtime_present && icon.runtime_enabled != 0 && icon.words[0] != 0 &&
               (icon.words[0x30] & 4U) == 0;
    }
    static bool bitmap_pixel_is_opaque(
        const GraphicsSurface& surface, uint32_t x, uint32_t y) {
        if (surface.bytes_per_pixel == 0 ||
            surface.bytes_per_pixel > 4U || x >= surface.width ||
            y >= surface.height) {
            return false;
        }
        const std::size_t offset =
            static_cast<std::size_t>(y) * surface.stride +
            static_cast<std::size_t>(x) * surface.bytes_per_pixel;
        if (offset + surface.bytes_per_pixel > surface.pixels.size()) {
            return false;
        }
        uint32_t pixel = 0;
        for (uint32_t index = 0; index < surface.bytes_per_pixel; ++index) {
            pixel |= static_cast<uint32_t>(
                         surface.pixels[offset + index])
                     << (index * 8U);
        }
        switch (surface.format) {
            case 0: return (pixel & 0xFFFFU) != 0;
            case 1: return (pixel & 0x00FFFFFFU) != 0;
            case 2: return (pixel & 0xFF000000U) != 0;
            case 3: return (pixel & 0xFFU) != 0;
            default: return false;
        }
    }
    void refresh_icon_input_regions(uint32_t id) {
        auto processor = icon_input_processors_.find(id);
        if (processor == icon_input_processors_.end()) {
            return;
        }
        const auto window =
            window_objects_.find(processor->second.window);
        const int32_t window_x =
            window == window_objects_.end() ? 0 : window->second.x;
        const int32_t window_y =
            window == window_objects_.end() ? 0 : window->second.y;
        uint32_t sequence = 0;
        for (std::size_t group_index = 0;
             group_index < processor->second.layout.groups.size();
             ++group_index) {
            IconInputGroupRecord& group =
                processor->second.layout.groups[group_index];
            for (std::size_t icon_index = 0;
                 icon_index < group.icons.size(); ++icon_index) {
                IconInputIconRecord& icon = group.icons[icon_index];
                if (icon.display_object_id == 0) {
                    icon.display_object_id = ++next_display_object_id_;
                }
                uint32_t priority = processor->second.mode == 2
                    ? icon_motion_render_priority(icon, sequence)
                    : icon_input_priority(icon, sequence);
                if (processor->second.mode == 2) {
                    // 913A0 changes XYZ/effect, but leaves the layer assigned
                    // by 68460 at child creation intact (including flags&2).
                    if (const auto previous = display_object_states_.find(icon.display_object_id);
                        previous != display_object_states_.end()) {
                        priority = (previous->second.priority & 0xFFFF0000U) |
                                   (priority & 0xFFFFU);
                    }
                }
                if (display_object_states_.count(icon.display_object_id) == 0) {
                    register_display_object(
                        icon.display_object_id, priority);
                } else {
                    update_display_object_priority(
                        icon.display_object_id, priority);
                }
                const uint32_t render_bitmap =
                    icon_render_bitmap(group, icon_index);
                const auto surface =
                    graphics_surfaces_.find(render_bitmap);
                const bool available = icon.runtime_present && icon.runtime_enabled != 0 &&
                    surface != graphics_surfaces_.end();
                const bool rotated =
                    icon.animation_angle_q16 != 0;
                const IconRenderBounds bounds =
                    rotated
                        ? icon_render_bounds(group, icon_index)
                        : IconRenderBounds{};
                const uint32_t width =
                    !available ? 0U
                    : rotated
                        ? static_cast<uint32_t>(
                              bounds.right - bounds.left + 1)
                        : surface->second.width;
                const uint32_t height =
                    !available ? 0U
                    : rotated
                        ? static_cast<uint32_t>(
                              bounds.bottom - bounds.top + 1)
                        : surface->second.height;
                update_display_object_geometry(
                    icon.display_object_id,
                    window_x +
                        (rotated ? bounds.left : icon.render_x),
                    window_y +
                        (rotated ? bounds.top : icon.render_y),
                    width, height, available);
                update_display_object_value(
                    icon.display_object_id,
                    (static_cast<uint32_t>(group_index) << 16U) |
                        static_cast<uint32_t>(icon_index));
                const auto display =
                    display_object_states_.find(icon.display_object_id);
                if (display != display_object_states_.end()) {
                    display->second.depth = icon.render_depth;
                    display->second.range_start = icon.range_start;
                    display->second.range_end = icon.range_end;
                    display->second.animation_frame =
                        icon.animation_frame;
                    display->second.transform_opacity = 1;
                }
                // 00452510/00451750 register ordinary icon regions only.
                // 0046E1D0's separate active list accepts CDspObjKnob handles.
                register_icon_input_region(
                    icon.display_object_id, group, icon_index,
                    window_x, window_y,
                    icon_input_window_key(processor->second) + 0x8000U);
                ++sequence;
            }
        }
        input_regions_active_ = true;
        // 00452510 invalidates the layout's window; 004538C0 submits
        // old/new icon rectangles through 0044A350 -> 00435BC0.
        // The host frame cache must observe those live geometry changes.
        display_dirty_ = true;
    }
    std::optional<uint32_t> extended_icon_input_hit_at(
        IconInputProcessor& processor, int32_t x, int32_t y) {
        if (!icon_input_priority_hit(processor, x, y)) return std::nullopt;
        const auto window = window_objects_.find(processor.window);
        const int32_t window_x = window == window_objects_.end() ? 0 : window->second.x;
        const int32_t window_y = window == window_objects_.end() ? 0 : window->second.y;
            std::optional<std::size_t> best_group;
            std::optional<std::size_t> best_icon;
            uint32_t best_priority = 0;
            uint32_t sequence = 0;
            for (std::size_t group = 0;
                 group < processor.layout.groups.size(); ++group) {
                const auto& record = processor.layout.groups[group];
                for (std::size_t icon = 0; icon < record.icons.size();
                     ++icon) {
                    const uint32_t priority = icon_input_priority(
                        record.icons[icon], sequence++);
                    const uint32_t bitmap =
                        icon_render_bitmap(record, icon);
                    if (!icon_accepts_input(record.icons[icon])) {
                        continue;
                    }
                    auto surface = graphics_surfaces_.find(bitmap);
                    if (surface == graphics_surfaces_.end()) {
                        continue;
                    }
                    const int32_t left = record.icons[icon].render_x;
                    const int32_t top = record.icons[icon].render_y;
                    const double radians =
                        icon_render_angle_degrees(record.icons[icon]) *
                        std::numbers::pi / 180.0;
                    const double cosine = std::cos(radians);
                    const double sine = std::sin(radians);
                    const double destination_x =
                        static_cast<double>(x - window_x - left);
                    const double destination_y =
                        static_cast<double>(y - window_y - top);
                    double source_x =
                        destination_x * cosine +
                        destination_y * sine;
                    double source_y =
                        -destination_x * sine +
                        destination_y * cosine;
                    const auto source_pixel =
                        icon_source_pixel(
                            source_x, source_y,
                            surface->second.width,
                            surface->second.height);
                    if (!source_pixel) {
                        continue;
                    }
                    const auto bounds = icon_render_bounds(record, icon);
                    if (!input_object_mask_hit(record.icons[icon].display_object_id,
                            x - window_x - bounds.left, y - window_y - bounds.top,
                            static_cast<uint32_t>(bounds.right - bounds.left + 1),
                            static_cast<uint32_t>(bounds.bottom - bounds.top + 1))) continue;
                    if (!best_group ||
                        priority >= best_priority) {
                        best_group = group;
                        best_icon = icon;
                        best_priority = priority;
                    }
                }
            }
        if (!best_group || !best_icon) return std::nullopt;
        return (static_cast<uint32_t>(*best_group) << 16U) | static_cast<uint32_t>(*best_icon);
    }
    void update_extended_icon_hover_at(int32_t x, int32_t y) {
        for (auto& [id, processor] : icon_input_processors_) {
            const auto hovered = extended_icon_input_hit_at(processor,x,y);
            if (hovered == processor.hovered_icon) continue;
            // 004538C0 resets the old icon's inactive frame for flags1/8.
            if (processor.hovered_icon) {
                const std::size_t group = *processor.hovered_icon >> 16U;
                const std::size_t index = *processor.hovered_icon & 0xFFFFU;
                if (group < processor.layout.groups.size() && index < processor.layout.groups[group].icons.size()) {
                    auto& icon = processor.layout.groups[group].icons[index];
                    if ((icon.words[48] & 1U) != 0 ||
                        ((icon.words[48] & 8U) != 0 && !icon_index_is_selected(processor.layout.groups[group],index))) {
                        icon.animation_frame = 0;
                        icon.animation_deadline_ms = 0;
                    }
                }
            }
            processor.hovered_icon = hovered;
            const uint32_t item = hovered.value_or(0xFFFFFFFFU);
            const uint32_t alternate = hovered
                ? processor.layout.groups[item >> 16U].icons[item & 0xFFFFU].words[9]
                : 0xFFFFFFFFU;
            processor.events.push_back({0x10000002U, item, alternate != 0xFFFFFFFFU ? 1U : 0U});
            refresh_icon_input_regions(id);
            display_dirty_ = true;
        }
    }
    bool queue_icon_input_at(int32_t x, int32_t y, bool pressed);
    void accept_extended_icon_input(IconInputProcessor& processor, uint32_t packed,
                                    int32_t x, int32_t y);
    void queue_icon_motion_at(int32_t x, int32_t y) {
        update_extended_icon_hover_at(x,y);
        const uint32_t packed =
            (static_cast<uint32_t>(y) << 16U) |
            (static_cast<uint32_t>(x) & 0xFFFFU);
        for (auto& [id, processor] : icon_input_processors_) {
            (void)id;
            processor.events.push_back(
                {0x10000007U, 0xFFFFFFFFU, packed});
        }
    }
    void advance_icon_input_animations(uint64_t now_ms) {
        for(auto& [id,processor]:icon_input_processors_) {
            (void)id;if(processor.mode==2)advance_icon_motions(processor,now_ms);
        }
        update_extended_icon_hover_at(pointer_x_,pointer_y_);
        bool changed = false;
        std::vector<uint32_t> changed_processors;
        for (auto& [id, processor] : icon_input_processors_) {
            bool processor_changed = false;
            for (std::size_t group_index = 0;
                 group_index < processor.layout.groups.size();
                 ++group_index) {
                IconInputGroupRecord& group =
                    processor.layout.groups[group_index];
                for (std::size_t icon_index = 0;
                     icon_index < group.icons.size(); ++icon_index) {
                    IconInputIconRecord& icon = group.icons[icon_index];
                    if (!icon.runtime_present) continue;
                    const int32_t frame_count =
                        static_cast<int32_t>(icon.range_start);
                    const int32_t period_ms =
                        static_cast<int32_t>(icon.range_end);
                    const bool selected =
                        icon_index_is_selected(group, icon_index);
                    const uint32_t flags = icon.words[0x30];
                    const bool hovered = processor.hovered_icon &&
                        (*processor.hovered_icon >> 16U) == group_index &&
                        (*processor.hovered_icon & 0xFFFFU) == icon_index;
                    const bool frames_active =
                        hovered || selected ||
                        ((flags & 1U) == 0 &&
                         ((flags & 8U) == 0 || selected));
                    if (frames_active && frame_count > 1 &&
                        period_ms > 0 &&
                        icon.animation_deadline_ms == 0) {
                        icon.animation_deadline_ms =
                            now_ms +
                            static_cast<uint64_t>(period_ms);
                        if (icon.animation_frame >=
                            static_cast<uint32_t>(frame_count)) {
                            icon.animation_frame = 0;
                            changed = true;
                            processor_changed = true;
                        }
                    } else if (
                        frames_active && frame_count > 1 &&
                        period_ms > 0 &&
                        now_ms >= icon.animation_deadline_ms) {
                        icon.animation_frame =
                            (icon.animation_frame + 1U) %
                            static_cast<uint32_t>(frame_count);
                        icon.animation_deadline_ms +=
                            static_cast<uint64_t>(period_ms);
                        if (const auto display =
                                display_object_states_.find(
                                    icon.display_object_id);
                            display != display_object_states_.end()) {
                            display->second.animation_frame =
                                icon.animation_frame;
                        }
                        changed = true;
                        processor_changed = true;
                    }

                    const bool path_active =
                        icon.words[40] == 0 ||
                        (group.words[3] ==
                             static_cast<uint32_t>(icon_index) &&
                         icon.words[13] == 0);
                    if (icon.animation_paths.empty()) {
                        icon.animation_angle_q16 = 0;
                        icon.animation_path_angle_q16 = 0;
                        icon.animation_path_index = 0;
                        icon.animation_subframe = 0;
                        icon.animation_subframe_count = 0;
                        icon.animation_path_deadline_ms = 0;
                        continue;
                    }
                    if (!path_active) {
                        if (icon.animation_path_deadline_ms != 0) {
                            if (icon.words[41] == 0) {
                                icon.animation_angle_q16 = 0;
                                icon.animation_path_deadline_ms = 0;
                            } else {
                                icon.animation_path_deadline_ms =
                                    now_ms + 4U;
                            }
                            changed = true;
                            processor_changed = true;
                        }
                        continue;
                    }
                    if (icon.animation_path_deadline_ms == 0) {
                        icon.animation_path_angle_q16 =
                            wrap_icon_angle(icon.words[39]);
                        icon.animation_path_index = 0;
                        icon.animation_subframe = 0;
                        icon.animation_subframe_count = 0;
                        icon.animation_path_deadline_ms =
                            now_ms + 4U;
                        changed = true;
                        processor_changed = true;
                        continue;
                    }
                    if (now_ms >=
                        icon.animation_path_deadline_ms) {
                        if (icon.animation_path_index >=
                            icon.animation_paths.size()) {
                            icon.animation_path_index = 0;
                            icon.animation_subframe = 0;
                        }
                        const IconAnimationPathRecord& path =
                            icon.animation_paths[
                                icon.animation_path_index];
                        if (icon.animation_subframe == 0) {
                            icon.animation_subframe_count =
                                static_cast<uint32_t>(std::max(
                                    static_cast<int32_t>(
                                        path.duration_ms) / 4,
                                    1));
                        }
                        ++icon.animation_subframe;
                        const int64_t interpolated =
                            static_cast<int64_t>(path.angle_delta_q16) *
                                icon.animation_subframe /
                            static_cast<int64_t>(
                                icon.animation_subframe_count);
                        icon.animation_angle_q16 = wrap_icon_angle(
                            static_cast<int64_t>(
                                icon.animation_path_angle_q16) +
                            interpolated);
                        if (icon.animation_subframe >=
                            icon.animation_subframe_count) {
                            icon.animation_path_angle_q16 =
                                wrap_icon_angle(
                                    static_cast<int64_t>(
                                        icon.animation_path_angle_q16) +
                                    path.angle_delta_q16);
                            icon.animation_angle_q16 =
                                icon.animation_path_angle_q16;
                            icon.animation_path_index =
                                (icon.animation_path_index + 1U) %
                                icon.animation_paths.size();
                            icon.animation_subframe = 0;
                        }
                        icon.animation_path_deadline_ms += 4U;
                        changed = true;
                        processor_changed = true;
                    }
                }
            }
            if (processor_changed) {
                changed_processors.push_back(id);
            }
        }
        for (const uint32_t id : changed_processors) {
            refresh_icon_input_regions(id);
        }
        if (changed) {
            display_dirty_ = true;
        }
    }
    bool query_icon_input_state(uint32_t id, uint32_t destination,
                                uint32_t base) {
        const auto processor = icon_input_processors_.find(id);
        if (processor == icon_input_processors_.end()) {
            return false;
        }
        for (std::size_t index = 0;
             index < processor->second.state.size(); ++index) {
            memory_.write_u32(
                destination + static_cast<uint32_t>(index * 4U), base,
                processor->second.state[index]);
        }
        return true;
    }
    [[nodiscard]] const std::map<uint32_t, IconInputProcessor>&
    icon_input_processors() const {
        return icon_input_processors_;
    }
    [[nodiscard]] const std::vector<ExtendedIconInputLayout>&
    icon_input_layout_history() const {
        return icon_input_layout_history_;
    }
    [[nodiscard]] bool object_exists(uint32_t object) const {
        return object == 0 ||
               group_objects_.count(object) != 0 ||
               window_objects_.count(object) != 0 ||
               display_objects_.count(object) != 0 ||
               sprites_.count(object) != 0 ||
               icon_input_processors_.count(object) != 0;
    }
    bool set_object_base_value(uint32_t object, uint32_t value) {
        if (!object_exists(object)) {
            return false;
        }
        object_base_values_[object] = value;
        return true;
    }
    [[nodiscard]] bool object_draw_visible(uint32_t object) const {
        return object_enabled(object) != 0 && object_base_draw_enabled(object) != 0 &&
               object_raw_field(object, 0x0CU) == 0 &&
               object_visibility_threshold(object) < 0x100U &&
               object_raw_field(object, 0xBCU) != 0;
    }
    bool set_display_object_enabled(uint32_t object, uint32_t enabled) {
        if (!object_exists(object)) {
            return false;
        }
        object_enabled_[object] = enabled;
        if (group_objects_.count(object) != 0) {
            return set_group_enabled(object, enabled);
        }
        if (const auto window = window_objects_.find(object);
            window != window_objects_.end()) {
            window->second.visible =
                object_draw_visible(object);
            update_display_object_geometry(
                object, window->second.x, window->second.y,
                window->second.width, window->second.height,
                window->second.visible);
        } else if (const auto display =
                       display_object_states_.find(object);
                   display != display_object_states_.end()) {
            display->second.available =
                object_draw_visible(object);
        }
        display_dirty_ = true;
        return true;
    }
    bool set_filter_enabled(uint32_t object, uint32_t enabled) {
        const auto display_class = object_display_classes_.find(object);
        if (display_class == object_display_classes_.end() ||
            display_class->second != DisplayObjectClass::CDspObjFilter) {
            return false;
        }
        return set_base_draw_enabled(object, enabled);
    }
    uint32_t clear_filter(uint32_t object, uint32_t field_14c,
                          uint32_t effect, uint32_t query_field) {
        const auto display_class = object_display_classes_.find(object);
        if (display_class == object_display_classes_.end() ||
            display_class->second != DisplayObjectClass::CDspObjFilter) {
            return 0xFFU;
        }
        FilterState& filter = filter_states_[object];
        filter.field_148 = 0;
        filter.field_14c = field_14c;
        filter.query_field = query_field;
        object_raw_fields_[{object,0x1CU}] = query_field;
        refresh_filter_display_geometry(object);
        filter.active = false;
        set_raw_field_recursive(object, 0xB4U, effect);
        display_dirty_ = true;
        return 0;
    }
    uint32_t configure_filter(uint32_t object, uint32_t field_148,
                              uint32_t field_14c, uint32_t bitmap,
                              uint32_t field_154, uint32_t effect,
                              uint32_t query_field) {
        const auto display_class = object_display_classes_.find(object);
        if (display_class == object_display_classes_.end() ||
            display_class->second != DisplayObjectClass::CDspObjFilter) {
            return 0xFFU;
        }
        FilterState& filter = filter_states_[object];
        filter.field_148 = field_148;
        filter.field_14c = field_14c;
        filter.query_field = query_field;
        object_raw_fields_[{object,0x1CU}] = query_field;
        refresh_filter_display_geometry(object);
        filter.active = false;
        set_raw_field_recursive(object, 0xB4U, effect);
        if (bitmap == 0xFFFFFFFFU) {
            display_dirty_ = true;
            return 0;
        }
        const auto surface = graphics_surfaces_.find(bitmap);
        if (surface == graphics_surfaces_.end()) {
            return 1;
        }
        if (surface->second.format != 3U) {
            return 2;
        }
        const auto [width, height] = current_mode_dimensions();
        if (surface->second.width != width ||
            surface->second.height != height) {
            return 3;
        }
        filter.bitmap = bitmap;
        filter.field_154 = field_154;
        filter.bitmap_token = graphics_bitmap_token(bitmap);
        filter.active = true;
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::map<uint32_t, FilterState>&
    filter_states() const {
        return filter_states_;
    }
    bool set_map_enabled(uint32_t object, uint32_t enabled) {
        const auto display_class = object_display_classes_.find(object);
        if (display_class == object_display_classes_.end() ||
            display_class->second != DisplayObjectClass::CDspObjMap) {
            return false;
        }
        return set_display_object_enabled(object, enabled);
    }
    bool set_base_draw_enabled(uint32_t object, uint32_t enabled) {
        if (!object_exists(object)) {
            return false;
        }
        std::set<uint32_t> visited;
        const auto apply = [&](const auto& self, uint32_t current) -> void {
            if (!visited.insert(current).second) {
                return;
            }
            object_base_draw_enabled_[current] = enabled;
            if (const auto window = window_objects_.find(current);
                window != window_objects_.end()) {
                window->second.visible =
                    object_enabled(current) != 0 && enabled != 0 &&
                    object_visibility_threshold(current) < 0x100U;
                update_display_object_geometry(
                    current, window->second.x, window->second.y,
                    window->second.width, window->second.height,
                    window->second.visible);
            } else if (const auto display =
                           display_object_states_.find(current);
                       display != display_object_states_.end()) {
                display->second.available =
                    object_enabled(current) != 0 && enabled != 0 &&
                    object_visibility_threshold(current) < 0x100U;
            }
            if (object_child_propagation(current) == 0 ||
                group_objects_.count(current) == 0) {
                return;
            }
            const auto children = group_children_.find(current);
            if (children == group_children_.end()) {
                return;
            }
            for (const uint32_t child : children->second) {
                self(self, child);
            }
        };
        apply(apply, object);
        display_dirty_ = true;
        return true;
    }
    bool set_object_child_propagation(uint32_t object, uint32_t value) {
        if (!object_exists(object)) {
            return false;
        }
        object_child_propagation_[object] = value;
        return true;
    }
    [[nodiscard]] uint32_t object_base_draw_enabled(
        uint32_t object) const {
        if (!object_exists(object)) {
            return 0;
        }
        const auto found = object_base_draw_enabled_.find(object);
        return found == object_base_draw_enabled_.end() ? 1U
                                                        : found->second;
    }
    [[nodiscard]] uint32_t object_child_propagation(
        uint32_t object) const {
        const auto found = object_child_propagation_.find(object);
        return found == object_child_propagation_.end() ? 0U
                                                        : found->second;
    }
    bool set_object_effect_level(uint32_t object, uint32_t value) {
        if (!object_exists(object) || value > 0x100U) {
            return false;
        }
        std::set<uint32_t> visited;
        const auto apply = [&](const auto& self, uint32_t current) -> void {
            if (!visited.insert(current).second) {
                return;
            }
            // Sprite virtual +48 (0042D4D0 / PE64 1400641E0) selects
            // source mixing independently from the base transparency.
            uint32_t selector = 0U;
            if (const auto type = object_display_classes_.find(current);
                type != object_display_classes_.end() &&
                type->second == DisplayObjectClass::CDspObjSprite) {
                selector = object_raw_field(current, 0x254U);
                if (selector > 3U && selector != 0xFFFFFFFFU) {
                    return;
                }
            }
            if (selector != 1U) {
                object_effect_level_[current] = value;
                object_raw_fields_[{current, 0xB4U}] = value;
                refresh_effector_parameters(current);
                if (const auto sprite = sprites_.find(current); sprite != sprites_.end()) {
                    sprite->second.alpha = value;
                }
                if (const auto window = window_objects_.find(current); window != window_objects_.end()) {
                    window->second.transparency = value;
                }
                const auto children = group_children_.find(current);
                if (children != group_children_.end()) {
                    for (const uint32_t child : children->second) {
                        self(self, child);
                    }
                }
            }
            if (selector == 1U || selector == 2U) {
                object_raw_fields_[{current, 0x250U}] = value;
                refresh_sprite_display_geometry(current);
            }
        };
        apply(apply, object);
        display_dirty_ = true;
        return true;
    }
    [[nodiscard]] uint32_t object_effect_level(uint32_t object) const {
        const auto found = object_effect_level_.find(object);
        return found == object_effect_level_.end() ? 0U : found->second;
    }
    bool set_object_position(uint32_t object, int32_t x, int32_t y) {
        if (!object_exists(object)) {
            return false;
        }
        std::set<uint32_t> visited;
        const auto apply = [&](const auto& self, uint32_t current,
                               int32_t current_x,
                               int32_t current_y) -> void {
            if (!visited.insert(current).second) {
                return;
            }
            object_positions_[current] = {current_x, current_y};
            if (const auto sprite = sprites_.find(current);
                sprite != sprites_.end()) {
                sprite->second.x = current_x;
                sprite->second.y = current_y;
            }
            if (group_objects_.count(current) != 0) {
                GroupState& group = group_states_[current];
                group.x = current_x;
                group.y = current_y;
            }
            if (const auto window = window_objects_.find(current);
                window != window_objects_.end()) {
                window->second.x = current_x;
                window->second.y = current_y;
                update_display_object_geometry(
                    current, current_x, current_y, window->second.width,
                    window->second.height, window->second.visible);
            } else if (const auto display =
                           display_object_states_.find(current);
                       display != display_object_states_.end()) {
                display->second.x = current_x;
                display->second.y = current_y;
            }
            refresh_sprite_display_geometry(current);
            refresh_window_display_geometry(current);
            if (group_objects_.count(current) == 0) {
                return;
            }
            const auto children = group_children_.find(current);
            if (children == group_children_.end()) {
                return;
            }
            for (const uint32_t child : children->second) {
                const auto offset = group_child_offsets_.find(
                    std::make_pair(current, child));
                int32_t offset_x = 0;
                int32_t offset_y = 0;
                if (offset != group_child_offsets_.end()) {
                    offset_x = offset->second.first;
                    offset_y = offset->second.second;
                }
                self(
                    self, child,
                    static_cast<int32_t>(
                        static_cast<uint32_t>(current_x) +
                        static_cast<uint32_t>(offset_x)),
                    static_cast<int32_t>(
                        static_cast<uint32_t>(current_y) +
                        static_cast<uint32_t>(offset_y)));
            }
        };
        apply(apply, object, x, y);
        display_dirty_ = true;
        return true;
    }
    [[nodiscard]] std::optional<std::pair<int32_t, int32_t>>
    object_position(uint32_t object) const {
        const auto found = object_positions_.find(object);
        if (found == object_positions_.end()) {
            return std::nullopt;
        }
        return found->second;
    }
    bool set_object_fixed_position(uint32_t object, uint32_t x,
                                   uint32_t y, uint32_t z,
                                   bool dispatch_position = true) {
        if (!object_exists(object)) {
            return false;
        }
        auto& position = object_absolute_positions_[object];
        const std::array<uint32_t, 3> previous = position;
        position = {x, y, z};

        if (dispatch_position &&
            object_absolute_position_dispatches_to_base(object) &&
            !set_object_position(
                object, static_cast<int32_t>(x) >> 16,
                static_cast<int32_t>(y) >> 16)) {
            return false;
        }

        const auto children = group_children_.find(object);
        if (children != group_children_.end()) {
            for (const uint32_t child : children->second) {
                const auto current = object_absolute_positions_.find(child);
                const std::array<uint32_t, 3> child_position =
                    current == object_absolute_positions_.end()
                        ? std::array<uint32_t, 3>{0, 0, 0}
                        : current->second;
                if (!set_object_fixed_position(
                        child,
                        child_position[0] + x - previous[0],
                        child_position[1] + y - previous[1],
                        child_position[2] + z - previous[2], false)) {
                    return false;
                }
            }
        }
        reinsert_display_object(object);
        display_dirty_ = true;
        return true;
    }
    bool set_object_absolute_position(uint32_t object, uint32_t x,
                                      uint32_t y, uint32_t z) {
        if (!object_exists(object)) {
            return false;
        }
        if (object_raw_field(object, 0x88U) != 0 &&
            (z == 0 || object_raw_field(object, 0x8CU) == 1)) {
            x = (x + 0x8000U) & 0xFFFF0000U;
            y = (y + 0x8000U) & 0xFFFF0000U;
        }
        return set_object_fixed_position(object, x, y, z);
    }
    bool set_object_absolute_position_dispatch(uint32_t object,
                                               uint32_t enabled) {
        if (!object_exists(object)) {
            return false;
        }
        object_absolute_position_dispatch_[object] = enabled;
        return true;
    }
    [[nodiscard]] bool object_absolute_position_dispatches_to_base(
        uint32_t object) const {
        const auto found =
            object_absolute_position_dispatch_.find(object);
        if (found != object_absolute_position_dispatch_.end()) {
            return found->second != 0;
        }
        const auto display_class = object_display_classes_.find(object);
        return display_class == object_display_classes_.end() ||
               display_class->second != DisplayObjectClass::CDspObjBackML;
    }
    [[nodiscard]] std::optional<std::array<uint32_t, 3>>
    object_absolute_position(uint32_t object) const {
        const auto found = object_absolute_positions_.find(object);
        if (found == object_absolute_positions_.end()) {
            return std::nullopt;
        }
        return found->second;
    }
    [[nodiscard]] std::optional<std::pair<int32_t, int32_t>>
    group_child_offset(uint32_t group, uint32_t object) const {
        const auto found =
            group_child_offsets_.find(std::make_pair(group, object));
        if (found == group_child_offsets_.end()) {
            return std::nullopt;
        }
        return found->second;
    }
    bool set_object_visibility_threshold(uint32_t object, uint32_t value) {
        if (!object_exists(object) || value > 0x100U) {
            return false;
        }
        std::set<uint32_t> visited;
        const auto apply = [&](const auto& self, uint32_t current) -> void {
            if (!visited.insert(current).second) {
                return;
            }
            if ((object_feature_mask(current) & 1U) != 0) {
                object_visibility_threshold_[current] = value;
                object_raw_fields_[{current, 0xB8U}] = value;
            }
            if (const auto window = window_objects_.find(current);
                window != window_objects_.end()) {
                window->second.visible =
                    object_draw_visible(current);
                update_display_object_geometry(
                    current, window->second.x, window->second.y,
                    window->second.width, window->second.height,
                    window->second.visible);
            } else if (const auto display =
                           display_object_states_.find(current);
                       display != display_object_states_.end()) {
                display->second.available =
                    object_draw_visible(current);
            }
            if (group_objects_.count(current) == 0) {
                return;
            }
            const auto children = group_children_.find(current);
            if (children == group_children_.end()) {
                return;
            }
            for (const uint32_t child : children->second) {
                self(self, child);
            }
        };
        apply(apply, object);
        display_dirty_ = true;
        return true;
    }
    [[nodiscard]] uint32_t object_visibility_threshold(
        uint32_t object) const {
        const auto found = object_visibility_threshold_.find(object);
        return found == object_visibility_threshold_.end() ? 0U
                                                           : found->second;
    }
    bool set_object_visibility_gate(uint32_t object, uint32_t value) {
        if (!object_exists(object)) {
            return false;
        }
        std::set<uint32_t> visited;
        const auto apply = [&](const auto& self, uint32_t current) -> void {
            if (!visited.insert(current).second) {
                return;
            }
            object_raw_fields_[{current, 0xBCU}] = value;
            const bool visible = object_draw_visible(current);
            if (const auto window = window_objects_.find(current);
                window != window_objects_.end()) {
                window->second.visible = visible;
                update_display_object_geometry(
                    current, window->second.x, window->second.y,
                    window->second.width, window->second.height, visible);
            } else if (const auto display =
                           display_object_states_.find(current);
                       display != display_object_states_.end()) {
                display->second.available = visible;
            }
            const auto children = group_children_.find(current);
            if (children == group_children_.end()) {
                return;
            }
            for (const uint32_t child : children->second) {
                self(self, child);
            }
        };
        apply(apply, object);
        display_dirty_ = true;
        return true;
    }
    bool set_object_feature_mask(uint32_t object, uint32_t mask) {
        if (!object_exists(object)) {
            return false;
        }
        object_feature_mask_[object] = mask;
        return true;
    }
    [[nodiscard]] uint32_t object_feature_mask(uint32_t object) const {
        const auto found = object_feature_mask_.find(object);
        return found == object_feature_mask_.end() ? 0xFFFFFFFFU
                                                   : found->second;
    }
    bool set_object_scaled_level(uint32_t object, uint32_t value) {
        if (!object_exists(object) || value > 0x100U) {
            return false;
        }
        std::set<uint32_t> visited;
        const uint32_t scaled = value << 16U;
        const auto apply = [&](const auto& self, uint32_t current) -> void {
            if (!visited.insert(current).second) {
                return;
            }
            object_scaled_level_[current] = scaled;
            refresh_effector_parameters(current);
            if (group_objects_.count(current) == 0) {
                return;
            }
            const auto children = group_children_.find(current);
            if (children == group_children_.end()) {
                return;
            }
            for (const uint32_t child : children->second) {
                self(self, child);
            }
        };
        apply(apply, object);
        display_dirty_ = true;
        return true;
    }
    template <typename Map>
    bool set_object_base_pair(uint32_t object, Map& fields,
                              int32_t first, int32_t second) {
        if (!object_exists(object)) {
            return false;
        }
        std::set<uint32_t> visited;
        const auto apply = [&](const auto& self, uint32_t current) -> void {
            if (!visited.insert(current).second) {
                return;
            }
            fields[current] = {first, second};
            refresh_sprite_display_geometry(current);
            refresh_window_display_geometry(current);
            if (group_objects_.count(current) == 0) {
                return;
            }
            const auto children = group_children_.find(current);
            if (children == group_children_.end()) {
                return;
            }
            for (const uint32_t child : children->second) {
                self(self, child);
            }
        };
        apply(apply, object);
        display_dirty_ = true;
        return true;
    }
    bool set_object_pair_44_48(uint32_t object, int32_t first,
                               int32_t second) {
        return set_object_base_pair(
            object, object_pair_44_48_, first, second);
    }
    bool set_object_pair_3c_40(uint32_t object, int32_t first,
                               int32_t second) {
        return set_object_base_pair(
            object, object_pair_3c_40_, first, second);
    }
    [[nodiscard]] uint32_t object_scaled_level(uint32_t object) const {
        const auto found = object_scaled_level_.find(object);
        return found == object_scaled_level_.end() ? 0U : found->second;
    }
    [[nodiscard]] std::optional<std::pair<int32_t, int32_t>>
    object_pair_44_48(uint32_t object) const {
        const auto found = object_pair_44_48_.find(object);
        if (found == object_pair_44_48_.end()) {
            return std::nullopt;
        }
        return found->second;
    }
    [[nodiscard]] std::optional<std::pair<int32_t, int32_t>>
    object_pair_3c_40(uint32_t object) const {
        const auto found = object_pair_3c_40_.find(object);
        if (found == object_pair_3c_40_.end()) {
            return std::nullopt;
        }
        return found->second;
    }
    struct PointerObjectSelection {
        bool found = false;
        uint32_t class_id = 0;
        uint32_t value = 0;
    };
    [[nodiscard]] uint32_t object_class_id(uint32_t object) const {
        const auto found = object_display_classes_.find(object);
        return found == object_display_classes_.end()
                   ? 0U
                   : display_class_info(found->second).type_level;
    }
    [[nodiscard]] std::optional<DisplayObjectClass>
    display_object_class(uint32_t object) const {
        const auto found = object_display_classes_.find(object);
        if (found == object_display_classes_.end()) {
            return std::nullopt;
        }
        return found->second;
    }
    [[nodiscard]] bool object_pointer_hit(uint32_t object,
                                          int32_t pointer_x,
                                          int32_t pointer_y) const {
        const auto state = display_object_states_.find(object);
        if (state == display_object_states_.end() ||
            !state->second.available || state->second.width == 0 ||
            state->second.height == 0) {
            return false;
        }
        const auto base = object_positions_.find(object);
        int32_t absolute_x = state->second.x;
        int32_t absolute_y = state->second.y;
        if (base != object_positions_.end()) {
            absolute_x = base->second.first;
            absolute_y = base->second.second;
        }
        if (const auto offset = object_pair_3c_40_.find(object);
            offset != object_pair_3c_40_.end()) {
            absolute_x += offset->second.first;
            absolute_y += offset->second.second;
        }
        if (const auto offset = object_pair_44_48_.find(object);
            offset != object_pair_44_48_.end()) {
            absolute_x += offset->second.first;
            absolute_y += offset->second.second;
        }
        if (object_raw_field(object, 0x4CU) != 0) {
            absolute_x += display_transform_origin_x_;
            absolute_y += display_transform_origin_y_;
        }
        const int32_t local_x = pointer_x - absolute_x;
        const int32_t local_y = pointer_y - absolute_y;
        return local_x >= 0 && local_y >= 0 &&
               static_cast<uint32_t>(local_x) < state->second.width &&
               static_cast<uint32_t>(local_y) < state->second.height;
    }
    uint32_t object_pointer_hit_value(uint32_t object) const {
        if (!object_exists(object)) {
            return 0xFFU;
        }
        const auto [pointer_x, pointer_y] = pointer_position();
        return object_pointer_hit(object, pointer_x, pointer_y) ? 1U : 0U;
    }
    [[nodiscard]] PointerObjectSelection
    select_display_object_at_pointer() const {
        if (!pointer_available_) {
            return {};
        }
        const auto objects = ordered_display_objects();
        for (auto iterator = objects.rbegin();
             iterator != objects.rend(); ++iterator) {
            const uint32_t object = *iterator;
            if (object_raw_field(object, 0x50U) == 0 ||
                !object_pointer_hit(
                    object, pointer_x_, pointer_y_)) {
                continue;
            }
            const auto state = display_object_states_.find(object);
            return {
                true,
                object_class_id(object),
                state == display_object_states_.end()
                    ? 0U
                    : state->second.value,
            };
        }
        return {};
    }
    uint32_t set_object_property(uint32_t object, uint32_t property,
                                 uint32_t value, uint32_t extra) {
        if (!object_exists(object)) {
            return 0xFFU;
        }
        if (effector_exists(object)) {
            auto& state=effector_effects_[object];
            if (property>=0x80U && property<=0x82U) {
                if (state.mode==3) {
                    const size_t index=property==0x80U?0:property==0x81U?2:3;
                    state.custom_modulation[index]=static_cast<int32_t>(value);
                    if (property!=0x81U)state.custom_modulation[index+1]=static_cast<int32_t>(extra);
                }
                return 0;
            }
            if (property==0x8FU) {state.custom_modulation[5]=static_cast<int32_t>(value);return 0;}
            if (property==0xFFU) {
                state.vector_bilinear=value;state.field_158=value;state.custom[6]=value;return 0;
            }
            if (property==0x100U)return update_effector_vector_phase(object,value,extra)==0?0:0xFFFF0002U;
        }
        const auto display_class = object_display_classes_.find(object);
        if (display_class != object_display_classes_.end() &&
            display_class->second == DisplayObjectClass::CDspObjSprite) {
            // Sprite vtable50FE38+5C is0042D700. In raw modes0/1/3/4
            // these selectors are recognized no-ops, not base-class errors.
            const uint32_t mode = object_raw_field(object, 0x144U);
            const bool affine_mode = mode == 2U || mode == 5U || mode == 6U;
            if (affine_mode && property == 0x40U) {
                object_raw_fields_[{object, 0x258U}] = value << 16U;
                object_raw_fields_[{object, 0x25CU}] = extra << 16U;
                return 0;
            }
            if (mode == 5U && (property == 0x41U || property == 0x42U)) {
                if (property == 0x41U) {
                    object_raw_fields_[{object, 0x260U}] = value;
                } else {
                    const uint32_t scale = value == 0 ? 1U : value;
                    object_raw_fields_[{object, 0x27CU}] = scale;
                    object_raw_fields_[{object, 0x280U}] = extra == 0 ? scale : extra;
                    object_raw_fields_[{object, 0x284U}] = extra != 0;
                }
                refresh_affine_sprite_mode5(object);
                return 0;
            }
            if (!affine_mode &&
                (property == 0x40U || property == 0x41U || property == 0x42U)) return 0;
            if (mode != 6U && property == 0x43U) return 0;
            if (property >= 0x80U && property <= 0x83U) {
                if (affine_mode && property == 0x80U) {
                    object_raw_fields_[{object, 0x2C4U}] = value;
                    object_raw_fields_[{object, 0x2C8U}] = extra;
                } else if (affine_mode && property == 0x81U) {
                    object_raw_fields_[{object, mode == 6U ? 0x2D8U : 0x2CCU}] = value;
                    object_raw_fields_[{object, 0x2E4U}] = extra;
                } else if (affine_mode && property == 0x82U) {
                    object_raw_fields_[{object, 0x2DCU}] = value;
                    object_raw_fields_[{object, 0x2E0U}] = extra;
                } else if (mode == 6U && property == 0x83U) {
                    object_raw_fields_[{object, 0x2D0U}] = value;
                    object_raw_fields_[{object, 0x2D4U}] = extra;
                }
                return 0;
            }
            if (property == 0x8FU) {
                object_raw_fields_[{object, 0x2E8U}] = value;
                return 0;
            }
        }
        // 0044A370 resolves public object0 to the current main background.
        // CDspObjBackML overrides +5C at00423360; its recognized selectors
        // deliberately ignore the slot setter's status and return success.
        if (object == 0 && background_type_ == 12U) {
            bool handled = true;
            switch (property) {
                case 0x41: {
                    const auto& slot = background_effect_slots_[background_effect_active_slot_];
                    (void)set_background_effect_rectangle(background_effect_active_slot_, value,
                        slot.field_17C, slot.field_180, slot.field_1A4);
                    break;
                }
                case 0x80:
                    (void)set_background_effect_pair_190(background_effect_active_slot_, value, extra);
                    break;
                case 0x81:
                    if (background_effect_active_slot_ < background_effect_slots_.size())
                        background_effect_slots_[background_effect_active_slot_].field_198 = value;
                    break;
                case 0x82:
                    if (background_effect_active_slot_ < background_effect_slots_.size()) {
                        background_effect_slots_[background_effect_active_slot_].field_19C = value;
                        background_effect_slots_[background_effect_active_slot_].field_1A0 = extra;
                    }
                    break;
                case 0x8E:
                    if (background_effect_active_slot_ < background_effect_slots_.size())
                        background_effect_slots_[background_effect_active_slot_].field_184 = value;
                    break;
                case 0x8F:
                    (void)set_background_effect_pair_188(background_effect_active_slot_, value, extra);
                    break;
                default:
                    handled = false;
                    break;
            }
            if (handled) {
                display_dirty_ = true;
                return 0;
            }
        }
        switch (property) {
            case 0:
                if (!set_object_position(
                        object, static_cast<int32_t>(value),
                        static_cast<int32_t>(extra))) {
                    return 0xFFU;
                }
                break;
            case 1:
                object_raw_fields_[{object, 0xB0U}] = value;
                if (const auto sprite = sprites_.find(object);
                    sprite != sprites_.end()) {
                    sprite->second.blend_mode = value;
                }
                if (const auto window = window_objects_.find(object); window != window_objects_.end()) {
                    window->second.blend_mode = value;
                }
                break;
            case 2:
                set_raw_field_recursive(object, 0xB4U, value);
                break;
            case 0xC0:
                object_child_propagation_[object] = value;
                break;
            case 0xC1:
                object_raw_fields_[{object, 0x10U}] = value;
                break;
            case 0xC4:
                object_raw_fields_[{object, 0x4CU}] = value;
                break;
            case 0xC5:
                object_raw_fields_[{object, 0x50U}] = value;
                break;
            case 0x8000:
                object_raw_fields_[{object, 0x88U}] = value;
                object_raw_fields_[{object, 0x8CU}] = extra;
                break;
            case 0x8001:
                object_raw_fields_[{object, 0x90U}] = value;
                break;
            case 0x8100:
                object_raw_fields_[{object, 0x24U}] = value;
                break;
            case 0x8101:
                if (value >= 2U) {
                    return 0xFEU;
                }
                object_raw_fields_[{object, 0x28U}] = value;
                break;
            case 0x7FFF0000U:
                object_raw_fields_[{object, 0xC4U}] = value;
                break;
            case 0x7FFFFFFFU:
                if (value >= 0x10U) {
                    return 0xFEU;
                }
                object_indexed_fields_[{object, value}] = extra;
                break;
            case 0xFFFFU: {
                const uint32_t mask = value == 0 ? 0xFFFFFFFFU : value;
                const uint32_t selected = extra == 0 ? 0U : mask;
                object_feature_mask_[object] =
                    selected | (~mask & object_feature_mask(object));
                break;
            }
            default:
                // 00420A50 returns-FFFF; manager0044A920 maps it to5.
                return 5;
        }
        reinsert_display_object(object);
        refresh_sprite_display_geometry(object);
        refresh_window_display_geometry(object);
        display_dirty_ = true;
        return 0;
    }
    uint32_t query_object_property(uint32_t object, uint32_t output,
                                   uint32_t selector, uint32_t base) {
        if (!object_exists(object)) {
            return 0xFFU;
        }
        const auto write_pair = [&](int32_t first, int32_t second) {
            memory_.write_u32(output, base, static_cast<uint32_t>(first));
            memory_.write_u32(output + 4U, base,
                              static_cast<uint32_t>(second));
        };
        // Sprite virtual+60 is0042DA50, which handles selector10 before base00420C80.
        if (selector == 0x10U && sprites_.count(object) != 0) {
            memory_.write_u32(output,base,object_raw_field(object,0x160U));
            return 0;
        }
        // Sprite query virtual+60: legacy0042DA50 ->0042D170;
        // PE64 140063AE0 ->1400645D0. Read the stored base angle,
        // not the current animated angle or a stale caller output word.
        if (selector == 0x41U && sprites_.count(object) != 0) {
            const uint32_t mode = object_raw_field(object, 0x144U);
            if (mode != 2U && mode != 5U && mode != 6U) return 0xFFFF0001U;
            memory_.write_u32(output, base,
                object_raw_field(object, mode == 6U ? 0x26CU : 0x260U));
            return 0;
        }
        switch (selector) {
            case 0: {
                const auto position = object_positions_.find(object);
                if (position == object_positions_.end()) {
                    write_pair(0, 0);
                } else {
                    write_pair(position->second.first,
                               position->second.second);
                }
                break;
            }
            case 1:
                memory_.write_u32(
                    output, base, object_raw_field(object, 0xB0U));
                break;
            case 2:
                memory_.write_u32(
                    output, base, object_effect_level(object));
                break;
            case 3:
            case 0xFFFFFFFEU:
                memory_.write_u32(
                    output, base, object_raw_field(object, 0x1CU));
                break;
            case 0x20: {
                const auto position =
                    object_absolute_positions_.find(object);
                if (position == object_absolute_positions_.end()) {
                    const std::array<uint32_t, 3> zero{0, 0, 0};
                    for (std::size_t index = 0; index < zero.size();
                         ++index) {
                        memory_.write_u32(
                            output + static_cast<uint32_t>(index * 4U),
                            base, zero[index]);
                    }
                } else {
                    for (std::size_t index = 0;
                         index < position->second.size(); ++index) {
                        memory_.write_u32(
                            output + static_cast<uint32_t>(index * 4U),
                            base, position->second[index]);
                    }
                }
                break;
            }
            case 0x7FFFFFFFU: {
                const uint32_t index = memory_.read_u32(output, base);
                if (index < 0x10U) {
                    memory_.write_u32(
                        output, base,
                        object_indexed_field(object, index));
                }
                break;
            }
            case 0xFFFFFFFFU:
                memory_.write_u32(
                    output, base, object_raw_field(object, 0x10CU));
                break;
            default:
                break;
        }
        return 0;
    }
    uint32_t query_object_origin(uint32_t object, uint32_t output,
                                 uint32_t base) {
        if (!object_exists(object)) {
            return 0xFFU;
        }
        int32_t x = 0;
        int32_t y = 0;
        if (const auto position = object_positions_.find(object);
            position != object_positions_.end()) {
            x = position->second.first;
            y = position->second.second;
        } else if (const auto display =
                       display_object_states_.find(object);
                   display != display_object_states_.end()) {
            x = display->second.x;
            y = display->second.y;
        } else if (const auto window = window_objects_.find(object);
                   window != window_objects_.end()) {
            x = window->second.x;
            y = window->second.y;
        }
        if (const auto offset = object_pair_3c_40_.find(object);
            offset != object_pair_3c_40_.end()) {
            x += offset->second.first;
            y += offset->second.second;
        }
        if (const auto offset = object_pair_44_48_.find(object);
            offset != object_pair_44_48_.end()) {
            x += offset->second.first;
            y += offset->second.second;
        }
        if (object_raw_field(object, 0x4CU) != 0) {
            x += display_transform_origin_x_;
            y += display_transform_origin_y_;
        }
        memory_.write_u32(output, base, static_cast<uint32_t>(x));
        memory_.write_u32(output + 4U, base, static_cast<uint32_t>(y));
        return 0;
    }
    void set_raw_field_recursive(uint32_t object, uint32_t offset,
                                 uint32_t value) {
        std::set<uint32_t> visited;
        const auto apply = [&](const auto& self, uint32_t current) -> void {
            if (!visited.insert(current).second) {
                return;
            }
            object_raw_fields_[{current, offset}] = value;
            if (offset == 0xB4U) {
                object_effect_level_[current] = value;
                refresh_effector_parameters(current);
                if (const auto sprite = sprites_.find(current);
                    sprite != sprites_.end()) {
                    sprite->second.alpha = value;
                }
                if (const auto window = window_objects_.find(current); window != window_objects_.end()) {
                    window->second.transparency = value;
                }
            }
            if (offset == 0xB8U) {
                object_visibility_threshold_[current] = value;
            }
            refresh_sprite_display_geometry(current);
            refresh_window_display_geometry(current);
            if (group_objects_.count(current) == 0) {
                return;
            }
            const auto children = group_children_.find(current);
            if (children == group_children_.end()) {
                return;
            }
            for (const uint32_t child : children->second) {
                self(self, child);
            }
        };
        apply(apply, object);
    }
    bool set_object_suppression(uint32_t object, uint32_t value) {
        if (!object_exists(object)) {
            return false;
        }
        std::set<uint32_t> visited;
        const auto apply = [&](const auto& self, uint32_t current) -> void {
            if (!visited.insert(current).second) {
                return;
            }
            object_raw_fields_[{current, 0x0CU}] = value;
            const bool visible = object_draw_visible(current);
            if (const auto window = window_objects_.find(current);
                window != window_objects_.end()) {
                window->second.visible = visible;
                update_display_object_geometry(
                    current, window->second.x, window->second.y,
                    window->second.width, window->second.height, visible);
            } else if (const auto display =
                           display_object_states_.find(current);
                       display != display_object_states_.end()) {
                display->second.available = visible;
            }
            const auto base = object_base_values_.find(current);
            if (base == object_base_values_.end() || base->second == 0) {
                return;
            }
            const auto children = group_children_.find(current);
            if (children == group_children_.end()) {
                return;
            }
            for (const uint32_t child : children->second) {
                self(self, child);
            }
        };
        apply(apply, object);
        display_dirty_ = true;
        return true;
    }
    bool set_object_raw_triplet_recursive(
        uint32_t object, uint32_t first_offset, uint32_t second_offset,
        uint32_t third_offset, uint32_t first, uint32_t second,
        uint32_t third) {
        if (!object_exists(object)) {
            return false;
        }
        std::set<uint32_t> visited;
        const auto apply = [&](const auto& self, uint32_t current) -> void {
            if (!visited.insert(current).second) {
                return;
            }
            object_raw_fields_[{current, first_offset}] = first;
            object_raw_fields_[{current, second_offset}] = second;
            object_raw_fields_[{current, third_offset}] = third;
            reinsert_display_object(current);
            const auto children = group_children_.find(current);
            if (children == group_children_.end()) {
                return;
            }
            for (const uint32_t child : children->second) {
                self(self, child);
            }
        };
        apply(apply, object);
        display_dirty_ = true;
        return true;
    }
    [[nodiscard]] uint32_t object_raw_field(uint32_t object,
                                            uint32_t offset) const {
        if (offset == 0xBCU) {
            const auto found = object_raw_fields_.find({object, offset});
            return found == object_raw_fields_.end() ? 0x100U
                                                     : found->second;
        }
        if (offset == 0x50U) {
            const auto found = object_raw_fields_.find({object, offset});
            return found == object_raw_fields_.end() ? 1U
                                                     : found->second;
        }
        const auto found = object_raw_fields_.find({object, offset});
        return found == object_raw_fields_.end() ? 0U : found->second;
    }
    [[nodiscard]] uint32_t object_indexed_field(uint32_t object,
                                                uint32_t index) const {
        const auto found = object_indexed_fields_.find({object, index});
        return found == object_indexed_fields_.end() ? 0U : found->second;
    }
    std::optional<bool> set_object_priority_bias(
        uint32_t object, uint32_t value) {
        if (!object_exists(object)) {
            return std::nullopt;
        }
        if (value >= 0x10000U) {
            return false;
        }
        const uint32_t previous = object_raw_field(object, 0x1CU);
        object_raw_fields_[{object, 0x1CU}] = value;
        if (const auto sprite = sprites_.find(object);
            sprite != sprites_.end()) {
            sprite->second.priority = value;
        }
        const auto display = display_object_states_.find(object);
        if (display != display_object_states_.end()) {
            display->second.priority += (value - previous) << 16U;
            reinsert_display_object(object);
        }
        return true;
    }
    [[nodiscard]] uint32_t object_enabled(uint32_t object) const {
        const auto found = object_enabled_.find(object);
        return found == object_enabled_.end() ? 0U : found->second;
    }
    bool get_object_base_value(uint32_t object, uint32_t& value) const {
        if (!object_exists(object)) {
            return false;
        }
        const auto found = object_base_values_.find(object);
        value = found == object_base_values_.end() ? 0U : found->second;
        return true;
    }
    bool append_object_property_array(
        uint32_t object, std::span<const uint32_t> values) {
        if (!object_exists(object) || values.empty() ||
            values.size() > 256U) {
            return false;
        }
        object_property_arrays_[object].emplace_back(
            values.begin(), values.end());
        return true;
    }
    bool dispatch_icon_input_property_array(
        IconInputProcessor& processor,
        std::span<const uint32_t> values) {
        if (values.empty()) {
            return false;
        }
        if(processor.mode==2 && values[0]==0x30000000U) {
            return values.size()==5 && transition_icon_motion(processor,values[1],values[2],values[3],values[4],monotonic_ms());
        }
        const bool extended = processor.mode != 0;
        const auto group_count = [&]() -> std::size_t {
            if (extended) {
                return processor.layout.groups.size();
            }
            return processor.basic_layout
                       ? processor.basic_layout->groups.size()
                       : 0U;
        };
        const auto icon_count = [&](std::size_t group) -> std::size_t {
            if (extended) {
                return processor.layout.groups[group].icons.size();
            }
            return processor.basic_layout->groups[group].icons.size();
        };
        const auto valid_group = [&](std::size_t group) {
            return group < group_count();
        };
        const auto valid_icon = [&](std::size_t group,
                                    std::size_t icon) {
            return valid_group(group) && icon < icon_count(group);
        };
        const auto selected_index = [&](std::size_t group) -> int32_t {
            if (extended) {
                return static_cast<int32_t>(
                    processor.layout.groups[group].words[3]);
            }
            return static_cast<int32_t>(
                processor.basic_layout->groups[group].words[2]);
        };
        const auto set_selected_index = [&](std::size_t group,
                                            uint32_t icon) {
            if (extended) {
                processor.layout.groups[group].words[3] = icon;
            } else {
                processor.basic_layout->groups[group].words[2] = icon;
            }
        };
        const auto radio_group = [&](std::size_t group) -> uint32_t {
            if (extended) {
                return processor.layout.groups[group].words[8];
            }
            return processor.basic_layout->groups[group].words[6];
        };
        const auto clear_radio_group = [&](std::size_t selected_group) {
            const uint32_t radio = radio_group(selected_group);
            if (radio == 0xFFFFFFFFU) {
                return;
            }
            for (std::size_t group = 0; group < group_count(); ++group) {
                if (group != selected_group &&
                    radio_group(group) == radio) {
                    set_selected_index(group, 0xFFFFFFFFU);
                }
            }
        };
        const auto select_item = [&](std::size_t group,
                                     uint32_t raw_icon) -> bool {
            if (!valid_group(group)) {
                return false;
            }
            if (raw_icon == 0xFFFFFFFFU) {
                set_selected_index(group, raw_icon);
                processor.selected_icon = -1;
                if (extended) {
                    refresh_icon_input_regions(processor.id);
                } else {
                    refresh_basic_icon_input_regions(processor.id);
                }
                return true;
            }
            const std::size_t icon = raw_icon;
            if (!valid_icon(group, icon)) {
                return false;
            }
            if (static_cast<uint32_t>(selected_index(group)) == raw_icon) {
                return false;
            }
            if (extended &&
                processor.layout.groups[group]
                        .icons[icon]
                        .words[0] == 0) {
                return false;
            }
            if (!extended &&
                static_cast<int32_t>(
                    processor.basic_layout->groups[group]
                        .icons[icon]
                        .words[3]) == -1) {
                return false;
            }
            set_selected_index(group, raw_icon);
            processor.value_3c = static_cast<uint32_t>(group);
            processor.selected_group = static_cast<int32_t>(group);
            processor.selected_icon = static_cast<int32_t>(icon);
            processor.state[1] = static_cast<uint32_t>(group);
            processor.state[2] = raw_icon;
            clear_radio_group(group);
            if (extended) {
                refresh_icon_input_regions(processor.id);
            } else {
                refresh_basic_icon_input_regions(processor.id);
            }
            display_dirty_ = true;
            return true;
        };
        switch (values[0]) {
            case 0x10000000U: {
                if (values.size() != 3U) {
                    return false;
                }
                const uint32_t packed = values[1];
                const uint32_t raw_group = packed >> 16U;
                const uint32_t raw_icon = packed & 0xFFFFU;
                if (!extended) {
                    std::optional<std::pair<std::size_t, std::size_t>>
                        location;
                    std::size_t flat = raw_group;
                    for (std::size_t group = 0;
                         group < group_count() && !location; ++group) {
                        if (flat < icon_count(group)) {
                            location = std::pair(group, flat);
                        } else {
                            flat -= icon_count(group);
                        }
                    }
                    if (!location) {
                        return false;
                    }
                    processor.selected_group =
                        static_cast<int32_t>(location->first);
                    processor.selected_icon =
                        static_cast<int32_t>(location->second);
                    processor.selected_state = raw_icon;
                    processor.state[0] = 0;
                    return true;
                }
                if (raw_group == 0xFFFFU && raw_icon == 0xFFFFU) {
                    processor.selected_group = -1;
                    processor.selected_icon = -1;
                    processor.selected_state = values[2];
                    if (values[2] != 0) {
                        processor.events.push_back(
                            {0x10000007U, 0xFFFFFFFFU,
                             (static_cast<uint32_t>(pointer_y_) << 16U) |
                                 (static_cast<uint32_t>(pointer_x_) &
                                  0xFFFFU)});
                    }
                    processor.events.push_back(
                        {0x10000006U, 0xFFFFFFFFU, values[2]});
                    return true;
                }
                if (!valid_icon(raw_group, raw_icon)) {
                    return false;
                }
                processor.selected_group =
                    static_cast<int32_t>(raw_group);
                processor.selected_icon =
                    static_cast<int32_t>(raw_icon);
                processor.selected_state = values[2];
                const auto& icon =
                    processor.layout.groups[raw_group].icons[raw_icon];
                if (values[2] != 0) {
                    const int32_t local_x = pointer_x_ - icon.render_x;
                    const int32_t local_y = pointer_y_ - icon.render_y;
                    processor.state[4] =
                        static_cast<uint32_t>(local_x);
                    processor.state[5] =
                        static_cast<uint32_t>(local_y);
                    processor.events.push_back(
                        {0x10000007U, 0xFFFFFFFFU,
                         (static_cast<uint32_t>(local_y) << 16U) |
                             (static_cast<uint32_t>(local_x) &
                              0xFFFFU)});
                }
                processor.events.push_back(
                    {0x10000006U, packed, values[2]});
                return true;
            }
            case 0x10000001U:
                if (values.size() != 2U ||
                    !valid_group(values[1])) {
                    return false;
                }
                {
                    const std::size_t group = values[1];
                    const uint32_t gate = extended
                        ? processor.layout.groups[group].words[5]
                        : processor.basic_layout->groups[group].words[3];
                    if (gate == 0) {
                        return false;
                    }
                    processor.value_3c = values[1];
                    if (static_cast<uint32_t>(selected_index(group)) !=
                        0xFFFFFFFFU) {
                        clear_radio_group(group);
                    }
                    apply_icon_group_selection_layers(processor, group);
                    display_dirty_ = true;
                }
                return true;
            case 0x10000002U:
                if (values.size() != 3U) {
                    return false;
                }
                return select_item(values[1], values[2]);
            case 0x10000003U:
                if (values.size() != 2U) {
                    return false;
                }
                processor.value_88 = values[1];
                return true;
            case 0x10000004U: {
                if (values.size() != 5U ||
                    !valid_icon(values[1], values[2])) {
                    return false;
                }
                const std::size_t group = values[1];
                const std::size_t icon = values[2];
                const uint32_t field = values[3];
                const uint32_t value = values[4];
                if (extended) {
                    auto& record =
                        processor.layout.groups[group].icons[icon];
                    if (field < 4U) {
                        record.words[8U + field] = value;
                        refresh_icon_input_regions(processor.id);
                    } else if (field == 4U) {
                        record.words[6] = value;
                    } else {
                        return false;
                    }
                } else {
                    auto& record =
                        processor.basic_layout->groups[group]
                            .icons[icon];
                    switch (field) {
                        case 0: record.words[3] = value; break;
                        case 1: record.words[5] = value; break;
                        case 2: record.words[4] = value; break;
                        case 4: record.words[6] = value; break;
                        default: return false;
                    }
                    refresh_basic_icon_input_regions(processor.id);
                }
                display_dirty_ = true;
                return true;
            }
            case 0x10000005U:
                if (values.size() != 4U ||
                    !valid_icon(values[1], values[2])) {
                    return false;
                }
                if (extended) {
                    auto& record =
                        processor.layout.groups[values[1]]
                            .icons[values[2]];
                    record.render_x =
                        static_cast<int32_t>(record.words[2]);
                    record.render_y =
                        static_cast<int32_t>(record.words[3]);
                    record.render_depth = values[3];
                    refresh_icon_input_regions(processor.id);
                }
                return true;
            case 0x10000006U:
                if (values.size() != 5U ||
                    !valid_icon(values[1], values[2])) {
                    return false;
                }
                if (extended) {
                    auto& record =
                        processor.layout.groups[values[1]]
                            .icons[values[2]];
                    record.words[6] = values[3];
                    record.words[7] = values[4];
                    record.range_start = values[3];
                    record.range_end = values[4];
                    record.animation_frame = 0;
                    record.animation_deadline_ms = 0;
                    refresh_icon_input_regions(processor.id);
                }
                return true;
            case 0x10000007U:
                if (values.size() != 6U ||
                    !valid_icon(values[1], values[2])) {
                    return false;
                }
                if (extended) {
                    auto& record =
                        processor.layout.groups[values[1]]
                            .icons[values[2]];
                    record.render_x =
                        static_cast<int32_t>(values[3]);
                    record.render_y =
                        static_cast<int32_t>(values[4]);
                    record.render_depth = values[5];
                    refresh_icon_input_regions(processor.id);
                }
                return true;
            case 0x10000008U:
                if (values.size() != 4U ||
                    !valid_icon(values[1], values[2])) {
                    return false;
                }
                if (extended) {
                    set_extended_icon_input_item_state(processor.id,values[1],values[2],values[3]);
                }
                return true;
            default:
                return false;
        }
    }
    uint32_t service_object_property_arrays() {
        uint32_t serviced = 0;
        for (auto& [object, arrays] : object_property_arrays_) {
            while (!arrays.empty()) {
                const std::vector<uint32_t>& values = arrays.front();
                if (values.size() == 2U && values[0] == 0U) {
                    set_object_base_value(object, values[1]);
                    arrays.erase(arrays.begin());
                    ++serviced;
                    continue;
                }
                if (const auto processor =
                        icon_input_processors_.find(object);
                    processor != icon_input_processors_.end()) {
                    (void)dispatch_icon_input_property_array(
                        processor->second, values);
                    arrays.erase(arrays.begin());
                    ++serviced;
                    continue;
                }
                if (values.size() != 2U) {
                    break;
                }
                break;
            }
        }
        return serviced;
    }
    [[nodiscard]] std::optional<uint32_t>
    icon_input_processor_value_88(uint32_t id) const {
        const auto processor = icon_input_processors_.find(id);
        if (processor == icon_input_processors_.end()) {
            return std::nullopt;
        }
        return processor->second.value_88;
    }
    [[nodiscard]] const std::map<
        uint32_t, std::vector<std::vector<uint32_t>>>&
    object_property_arrays() const {
        return object_property_arrays_;
    }
    struct DisplayProcedure {
        uint32_t object = 0;
        std::array<int32_t, 6> parameters{};
    };
    struct ControlDisplayProcedureState {
        uint32_t object = 0;
        std::array<int32_t, 11> core_arguments{};
        std::array<int32_t, 6> wrapper_parameters{};
        int32_t start_x = 0;
        int32_t start_y = 0;
        int32_t target_x = 0;
        int32_t target_y = 0;
        int32_t frame_rate = 0;
        int32_t frame_count = 0;
        uint32_t input_enabled = 0;
        uint32_t input_mask = 0;
        uint64_t started_ms = 0;
        uint64_t duration_ms = 0;
        uint32_t start_effect = 0;
        uint32_t target_effect = 0;
        uint32_t start_time = 0;
        uint32_t target_time = 0;
        uint32_t effect_easing = 0;
        bool absolute_coordinates = false;
        std::array<uint32_t,3> absolute_start{};
        std::array<uint32_t,3> absolute_target{};
        uint32_t tick_count = 0;
        uint32_t reported_elapsed = 0;
        uint32_t period_limit_ms = 0;
        uint64_t next_update_ms = 0;
        int32_t last_x = std::numeric_limits<int32_t>::min();
        int32_t last_y = std::numeric_limits<int32_t>::min();
        uint32_t last_effect = 0xFFFFFFFFU;
        uint32_t last_time = 0xFFFFFFFFU;
        bool complete = false;
    };
    struct ShakeDisplayProcedureState {
        uint32_t object = 0;
        std::array<uint32_t, 9> operands{};
        uint32_t mode = 0;
        int32_t scale = 0;
        int32_t frame_span = 0;
        int32_t amplitude_steps = 0;
        int32_t decay_percent = 0;
        int32_t interval = 0;
        int32_t frame_rate = 0;
        int32_t subframe_rate = 0;
        std::vector<int32_t> amplitude_table;
        std::vector<int32_t> oscillator_table;
    };
    enum class FramedControlProcedureKind : uint8_t {
        Spline = 0,
        Framed = 1,
        Recorded = 2,
    };
    struct FramedControlProcedureState {
        FramedControlProcedureKind kind =
            FramedControlProcedureKind::Framed;
        uint32_t object = 0;
        std::array<uint32_t, 12> operands{};
        int32_t frame_count = 0;
        int32_t frame_period = 0;
        std::vector<std::array<uint32_t, 4>> records;
    };
    uint32_t configure_framed_control_procedure(
        FramedControlProcedureKind kind,
        const std::array<uint32_t, 12>& operands, uint32_t base) {
        const uint32_t object = operands[0];
        if (!object_exists(object)) {
            return 0xFFFFFFFFU;
        }
        if (operands[8] == 0) {
            return 1;
        }
        FramedControlProcedureState state;
        state.kind = kind;
        state.object = object;
        state.operands = operands;
        if (kind == FramedControlProcedureKind::Spline) {
            const uint64_t product =
                static_cast<uint64_t>(
                    static_cast<uint32_t>(operands[7])) *
                operands[8];
            const uint32_t count =
                static_cast<uint32_t>(product / 1000U);
            state.frame_count =
                static_cast<int32_t>(count == 0 ? 1 : count);
            state.frame_period =
                state.frame_count > 1
                    ? static_cast<int32_t>(
                          1000U /
                          static_cast<uint32_t>(operands[8]))
                    : static_cast<int32_t>(operands[7]);
        } else if (kind ==
                   FramedControlProcedureKind::Framed) {
            state.frame_count = std::max(
                static_cast<int32_t>(operands[7]), 1);
            state.frame_period = static_cast<int32_t>(
                static_cast<uint32_t>(operands[9]) * 1000U /
                operands[8]);
        } else {
            state.frame_count = std::max(
                static_cast<int32_t>(operands[7]), 1);
            state.frame_period = static_cast<int32_t>(
                static_cast<uint32_t>(operands[9]) * 1000U /
                operands[8]);
            const uint32_t count = operands[1];
            const uint32_t source = operands[2];
            try {
                state.records.reserve(count);
                for (uint32_t index = 0; index < count; ++index) {
                    std::array<uint32_t, 4> record{};
                    for (std::size_t word = 0;
                         word < record.size(); ++word) {
                        record[word] = memory_.read_u32(
                            source + index * 16U +
                                static_cast<uint32_t>(word * 4U),
                            base);
                    }
                    state.records.push_back(record);
                }
            } catch (const BpFault&) {
                return 2;
            }
        }
        framed_control_procedures_.push_back(std::move(state));
        return 0;
    }
    [[nodiscard]] const std::vector<FramedControlProcedureState>&
    framed_control_procedures() const {
        return framed_control_procedures_;
    }
    uint32_t configure_shake_display_procedure(
        const std::array<uint32_t, 9>& operands) {
        const uint32_t object = operands[0];
        if (!object_exists(object)) {
            return 0xFFFFFFFFU;
        }
        const uint32_t mode = operands[1];
        const int32_t scale = static_cast<int32_t>(operands[2]);
        const int32_t frame_span = static_cast<int32_t>(operands[3]);
        const int32_t amplitude_steps =
            static_cast<int32_t>(operands[4]);
        const int32_t decay_percent =
            static_cast<int32_t>(operands[5]);
        const int32_t interval = static_cast<int32_t>(operands[6]);
        if (mode > 5U) {
            return 1;
        }
        if (frame_span == 0) {
            return 2;
        }
        if (amplitude_steps == 0) {
            return 3;
        }
        if (interval == 0 || frame_span >= interval) {
            return 4;
        }
        const int32_t frame_rate = 1000 / interval;
        if (frame_rate == 0 ||
            frame_rate * frame_span == 0) {
            throw BpFault("shake display procedure period underflow");
        }
        const int32_t subframe_rate =
            1000 / (frame_rate * frame_span);
        ShakeDisplayProcedureState state;
        state.object = object;
        state.operands = operands;
        state.mode = mode;
        state.scale = scale;
        state.frame_span = frame_span;
        state.amplitude_steps = amplitude_steps;
        state.decay_percent = decay_percent;
        state.interval = interval;
        state.frame_rate = frame_rate;
        state.subframe_rate = subframe_rate;
        int64_t amplitude =
            static_cast<int64_t>(scale) << 16U;
        state.amplitude_table.reserve(
            static_cast<std::size_t>(amplitude_steps));
        for (int32_t index = 0; index < amplitude_steps; ++index) {
            state.amplitude_table.push_back(
                static_cast<int32_t>(amplitude));
            amplitude =
                amplitude * (100 - decay_percent) / 100;
        }
        const std::size_t oscillator_count =
            static_cast<std::size_t>(subframe_rate) *
            static_cast<std::size_t>(amplitude_steps);
        state.oscillator_table.reserve(oscillator_count);
        const int32_t base = mode < 4U ? 0 : 0x8000;
        int32_t step = 0x20000 / subframe_rate;
        int32_t position = base;
        for (std::size_t index = 0; index < oscillator_count; ++index) {
            const int32_t next = step + position;
            int32_t next_step = -step;
            if (next < 0x10000) {
                next_step = step;
            }
            int32_t magnitude = 0x20000 - next;
            if (next < 0x10000) {
                magnitude = next;
            }
            if (magnitude < 1) {
                next_step = -next_step;
                magnitude = -magnitude;
            }
            const int32_t word =
                mode == 0U || mode == 2U
                    ? base - magnitude
                    : magnitude - base;
            state.oscillator_table.push_back(word);
            step = next_step;
            position = magnitude;
        }
        shake_display_procedures_.push_back(std::move(state));
        return 0;
    }
    [[nodiscard]] const std::vector<ShakeDisplayProcedureState>&
    shake_display_procedures() const {
        return shake_display_procedures_;
    }
    bool record_display_procedure(
        uint32_t object, const std::array<int32_t, 6>& parameters) {
        if (!object_exists(object) || parameters[2] == 0) {
            return false;
        }
        display_procedures_.push_back({object, parameters});
        return true;
    }
    [[nodiscard]] const std::vector<DisplayProcedure>&
    display_procedures() const {
        return display_procedures_;
    }
    bool install_control_display_procedure(
        Thread& thread, const std::array<int32_t, 11>& core_arguments,
        const std::array<int32_t, 6>& wrapper_parameters);
    bool install_framed_control_display_procedure(Thread& thread, const std::array<uint32_t,12>& operands);
    bool install_recorded_linear_display_procedure(Thread& thread, const std::array<uint32_t,12>& operands);
    WaitProcedureResult advance_control_display_procedure(std::size_t index, uint64_t now_ms);
    [[nodiscard]] const std::vector<ControlDisplayProcedureState>&
    control_display_procedures() const {
        return control_display_procedures_;
    }
    uint32_t add_object_to_group(uint32_t group, uint32_t object,
                                 uint32_t offset_x, uint32_t offset_y) {
        if (group_objects_.count(group) == 0) {
            return 0xFFFFFFFFU;
        }
        const bool valid_object =
            group_objects_.count(object) != 0 ||
            window_objects_.count(object) != 0 ||
            display_objects_.count(object) != 0 ||
            sprites_.count(object) != 0;
        if (!valid_object) {
            return 1;
        }
        if (group == object) {
            return 3;
        }
        const auto owner = object_owners_.find(object);
        if (owner != object_owners_.end() && owner->second != group) {
            return 4;
        }
        auto& children = group_children_[group];
        if (std::find(children.begin(), children.end(), object) ==
            children.end()) {
            children.push_back(object);
            object_owners_[object] = group;
        }
        group_child_offsets_[std::make_pair(group, object)] = {
            static_cast<int32_t>(offset_x),
            static_cast<int32_t>(offset_y)};
        display_dirty_ = true;
        return 0;
    }
    uint32_t remove_object_from_group(uint32_t group, uint32_t object) {
        if (group_objects_.count(group) == 0) {
            return 0xFFFFFFFFU;
        }
        if (!object_exists(object)) {
            return 1;
        }
        auto children = group_children_.find(group);
        if (children == group_children_.end()) {
            return 2;
        }
        const auto member =
            std::find(children->second.begin(), children->second.end(), object);
        if (member == children->second.end()) {
            return 2;
        }
        children->second.erase(member);
        group_child_offsets_.erase(std::make_pair(group, object));
        const auto owner = object_owners_.find(object);
        if (owner != object_owners_.end() && owner->second == group) {
            object_owners_.erase(owner);
        }
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::map<uint32_t, std::vector<uint32_t>>&
    group_children() const {
        return group_children_;
    }
    [[nodiscard]] const std::map<uint32_t, uint32_t>&
    object_owners() const {
        return object_owners_;
    }
    struct WaitWindowProcedure {
        uint32_t procedure_id = 0;
        uint32_t argument = 0;
    };
    uint32_t create_wait_window_procedure(uint32_t argument) {
        const uint32_t id = next_procedure_id_++;
        const uint32_t token = 0xE1000000U | id;
        wait_window_procedures_[token] = {id, argument};
        return token;
    }
    [[nodiscard]] const std::map<uint32_t, WaitWindowProcedure>&
    wait_window_procedures() const {
        return wait_window_procedures_;
    }
    struct MovieProcedure {
        uint32_t procedure_id = 0;
        std::string source;
        std::string extra;
        uint32_t field_30 = 0;
        bool source_present = true;
        bool extra_present = true;
        uint32_t state = 0;
        uint32_t result = 0xFFFFFFFFU;
        bool complete = false;
    };
    void install_mf_movie_wait(Thread& thread, std::string directory,
        std::string resource, uint32_t volume, bool directory_present,
        bool resource_present);
    WaitProcedureResult advance_mf_movie_procedure(uint32_t token);
    using MfMovieOpenProvider = std::function<uint32_t(const MovieSlotState&)>;
    void set_mf_movie_open_provider(MfMovieOpenProvider provider) {
        mf_movie_open_provider_ = std::move(provider);
    }
    void set_mf_movie_stop_provider(std::function<bool()> provider) {
        mf_movie_stop_provider_ = std::move(provider);
    }
    uint32_t set_mf_movie_volume(uint32_t value) {
        if (!mf_movie_) return 0xFFFFFFFEU;
        mf_movie_->volume = static_cast<uint32_t>(std::clamp(
            static_cast<int32_t>(value), 0, 128));
        return 0;
    }
    [[nodiscard]] const std::optional<MovieSlotState>& mf_movie() const {
        return mf_movie_;
    }
    [[nodiscard]] uint32_t mf_movie_handle() const { return mf_movie_handle_; }
    [[nodiscard]] bool mf_movie_active() const {
        // 1400F2560 -> 140111720: Started, or a pending Start command.
        return mf_movie_ && mf_movie_state_ == 3U;
    }
    uint32_t stop_mf_movie() {
        if (!mf_movie_) return 0xFFFFFFFEU;
        const bool stopped = !mf_movie_stop_provider_ || mf_movie_stop_provider_();
        mf_movie_.reset();
        mf_movie_state_ = 0;
        mf_movie_handle_ = 0;
        display_dirty_ = true;
        return stopped ? 0U : 0xFFFFFFFFU;
    }
    void update_mf_movie_clock(uint32_t position_ms, uint32_t duration_ms,
        bool playing) {
        if (!mf_movie_) return;
        mf_movie_->position_ms = position_ms;
        mf_movie_->duration_ms = duration_ms;
        mf_movie_->playing = playing;
        mf_movie_state_ = playing ? 3U : 5U;
    }
    uint32_t create_movie_procedure(
        std::string source, std::string extra,
        uint32_t field_30 = 0) {
        const uint32_t id = next_procedure_id_++;
        const uint32_t token = 0xE2000000U | id;
        movie_procedures_[token] = {
            id, std::move(source), std::move(extra), field_30};
        return token;
    }
    [[nodiscard]] const std::map<uint32_t, MovieProcedure>&
    movie_procedures() const {
        return movie_procedures_;
    }
    struct RegisterBmpProcedure {
        uint32_t procedure_id = 0;
        uint32_t field_28 = 0;
        uint32_t decode_source = 0;
        uint32_t decode_mode = 0;
        std::string field_2c;
        std::string field_30;
        uint32_t field_34 = 0;
        bool worker_started = true;
    };
    uint32_t create_register_bmp_procedure(
        uint32_t field_28, uint32_t decode_source,
        uint32_t decode_mode, std::string field_2c,
        std::string field_30, uint32_t field_34) {
        const uint32_t id = next_procedure_id_++;
        const uint32_t token = 0xE3000000U | id;
        register_bmp_procedures_[token] = {
            id, field_28, decode_source, decode_mode,
            std::move(field_2c), std::move(field_30), field_34, true};
        return token;
    }
    [[nodiscard]] const std::map<uint32_t, RegisterBmpProcedure>&
    register_bmp_procedures() const {
        return register_bmp_procedures_;
    }
    struct BurikoMovieProcedure {
        uint32_t procedure_id = 0;
        uint32_t field_650 = 0;
        uint32_t field_654 = 0;
        std::string string_a;
        std::string string_b;
        uint32_t field_658 = 1;
        bool load_started = true;
    };
    uint32_t create_buriko_movie_procedure(
        uint32_t field_650, uint32_t field_654,
        std::string string_a, std::string string_b) {
        const uint32_t id = next_procedure_id_++;
        const uint32_t token = 0xE4000000U | id;
        buriko_movie_procedures_[token] = {
            id, field_650, field_654, std::move(string_a),
            std::move(string_b), 1, true};
        return token;
    }
    [[nodiscard]] const std::map<uint32_t, BurikoMovieProcedure>&
    buriko_movie_procedures() const {
        return buriko_movie_procedures_;
    }
    struct HeaderMovieProcedure {
        uint32_t procedure_id = 0;
        uint32_t field_20 = 0;
        uint32_t field_24 = 0;
        std::string string_a;
        std::string string_b;
        uint32_t field_30 = 0;
        uint32_t field_74 = 0;
    };
    uint32_t create_header_movie_procedure(
        uint32_t field_20, uint32_t field_24,
        std::string string_a, std::string string_b) {
        const uint32_t id = next_procedure_id_++;
        const uint32_t token = 0xE5000000U | id;
        HeaderMovieProcedure procedure;
        procedure.procedure_id = id;
        if (!string_b.empty()) {
            procedure.field_20 = field_20;
            procedure.field_24 = field_24;
            procedure.string_a = std::move(string_a);
            procedure.string_b = std::move(string_b);
            procedure.field_30 = 1;
        }
        header_movie_procedures_[token] = std::move(procedure);
        return token;
    }
    [[nodiscard]] const std::map<uint32_t, HeaderMovieProcedure>&
    header_movie_procedures() const {
        return header_movie_procedures_;
    }
    void cache_movie_resource(std::string name, uint32_t handle) {
        movie_cache_[std::move(name)] = handle;
    }
    void clear_movie_cache() {
        movie_cache_.clear();
    }
    [[nodiscard]] const std::map<std::string, uint32_t>&
    movie_cache() const {
        return movie_cache_;
    }
    struct BinaryLoadProcedure {
        uint32_t procedure_id = 0;
        uint32_t destination = 0;
        std::string directory;
        std::string resource;
        uint32_t offset = 0;
        uint32_t size = 0;
        int32_t status = 0;
        bool complete = false;
    };
    uint32_t create_binary_load_procedure(
        uint32_t destination, std::string directory,
        std::string resource, uint32_t offset, uint32_t size) {
        const uint32_t id = next_procedure_id_++;
        const uint32_t token = 0xE7000000U | id;
        binary_load_procedures_[token] = {
            id, destination, std::move(directory), std::move(resource),
            offset, size, 0, false};
        return token;
    }
    int32_t complete_binary_load_procedure(
        uint32_t token, uint32_t base) {
        const auto found = binary_load_procedures_.find(token);
        if (found == binary_load_procedures_.end()) {
            return 3;
        }
        if (found->second.complete) {
            return found->second.status;
        }
        found->second.status = load_binary_span(
            found->second.destination, found->second.directory,
            found->second.resource, found->second.offset,
            found->second.size, base);
        found->second.complete = true;
        return found->second.status;
    }
    [[nodiscard]] const std::map<uint32_t, BinaryLoadProcedure>&
    binary_load_procedures() const {
        return binary_load_procedures_;
    }
    enum class CodecProcedureKind {
        EncodeData,
        EncodeStruct,
        DecodeData,
    };
    struct CodecProcedure {
        uint32_t procedure_id = 0;
        CodecProcedureKind kind = CodecProcedureKind::EncodeData;
        std::string string_a;
        std::string string_b;
        uint32_t field_a = 0;
        uint32_t field_b = 0;
        bool worker_started = true;
        bool complete = false;
    };
    uint32_t create_codec_procedure(
        CodecProcedureKind kind, std::string string_a,
        std::string string_b, uint32_t field_a,
        uint32_t field_b = 0) {
        const uint32_t id = next_procedure_id_++;
        const uint32_t token = 0xE8000000U | id;
        codec_procedures_[token] = {
            id, kind, std::move(string_a), std::move(string_b),
            field_a, field_b, true, false};
        return token;
    }
    // C0/C4 take raw buffer addresses and install a wait; the eventual
    // result is a byte count, never the internal procedure identity.
    void install_codec_buffer_wait(Thread& thread, CodecProcedureKind kind,
        uint32_t output, uint32_t input, uint32_t row_bytes, uint32_t rows=0);
    WaitProcedureResult advance_codec_procedure(uint32_t token);
    [[nodiscard]] const std::map<uint32_t, CodecProcedure>&
    codec_procedures() const {
        return codec_procedures_;
    }
    struct GroupState {
        uint32_t enabled = 1;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t priority = 0;
    };
    bool set_group_enabled(uint32_t group, uint32_t enabled) {
        if (group_objects_.count(group) == 0) {
            return false;
        }
        // 00449800 dispatches virtual+4 (0041FF30), which writes +14
        // and visits every +13C child link. +84 is a separate field.
        std::set<uint32_t> visited;
        const auto apply = [&](const auto& self, uint32_t object) -> void {
            if (!visited.insert(object).second) return;
            object_base_draw_enabled_[object] = enabled;
            if (group_objects_.count(object) != 0)
                group_states_[object].enabled = enabled;
            refresh_sprite_display_geometry(object);
            refresh_window_display_geometry(object);
            if (const auto display = display_object_states_.find(object);
                display != display_object_states_.end())
                display->second.available = object_draw_visible(object);
            if (const auto children = group_children_.find(object);
                children != group_children_.end())
                for (const uint32_t child : children->second) self(self, child);
        };
        apply(apply, group);
        display_dirty_ = true;
        return true;
    }
    bool configure_group(uint32_t group, int32_t x, int32_t y,
                         uint32_t effect) {
        if (group_objects_.count(group) == 0) {
            return false;
        }
        // 00449860 dispatches +2C (00420300 -> 00420320) and +48
        // (00420780), propagating position and effect through child links.
        // The fourth argument is an effect value; no +54 priority call occurs.
        if (!set_object_position(group, x, y)) return false;
        set_raw_field_recursive(group, 0xB4U, effect);
        display_dirty_ = true;
        return true;
    }
    struct SpriteMaskConfig {
        uint32_t field1 = 0;
        uint32_t field2 = 0;
        uint32_t primary_bitmap = 0;
        uint32_t field4 = 0;
        uint32_t field5 = 0;
        uint32_t secondary_bitmap = 0;
        uint32_t mask_bitmap = 0;
        uint32_t field8 = 0;
    };
    struct ObjectMaskState {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride = 0;
        std::vector<uint8_t> bits;
    };
    void set_object_mask(uint32_t object, int32_t bitmap) {
        if (!object_exists(object) || object_class_id(object) > 7U) {
            return;
        }
        cache_object_input_mask(object, bitmap);
    }
    // Virtual input children use the same mask storage internally, although
    // the public Grp0 mask wrapper intentionally accepts only classes 0..7.
    void cache_object_input_mask(uint32_t object, int32_t bitmap) {
        if (bitmap == -1) {
            object_masks_.erase(object);
            return;
        }
        ObjectMaskState mask;
        if (bitmap == -2) {
            if (const auto window = window_objects_.find(object);
                window != window_objects_.end()) {
                mask.width = window->second.width;
                mask.height = window->second.height;
            } else if (const auto display =
                           display_object_states_.find(object);
                       display != display_object_states_.end()) {
                mask.width = display->second.width;
                mask.height = display->second.height;
            } else if (sprite_mask_configs_.count(object) != 0) {
                const auto& config = sprite_mask_configs_.at(object);
                const auto primary = graphics_surfaces_.find(
                    config.primary_bitmap);
                if (primary != graphics_surfaces_.end()) {
                    mask.width = primary->second.width;
                    mask.height = primary->second.height;
                }
            } else {
                return;
            }
            mask.stride = (mask.width + 7U) >> 3U;
            mask.bits.assign(
                static_cast<std::size_t>(mask.stride) * mask.height,
                0);
            object_masks_[object] = std::move(mask);
            return;
        }
        if (bitmap < 0) {
            return;
        }
        const auto surface = graphics_surfaces_.find(
            static_cast<uint32_t>(bitmap));
        if (surface == graphics_surfaces_.end() ||
            surface->second.format > 3U) {
            return;
        }
        mask.width = surface->second.width;
        mask.height = surface->second.height;
        mask.stride = (mask.width + 7U) >> 3U;
        mask.bits.assign(
            static_cast<std::size_t>(mask.stride) * mask.height, 0);
        const uint32_t bytes_per_pixel =
            surface->second.bytes_per_pixel;
        for (uint32_t y = 0; y < mask.height; ++y) {
            for (uint32_t x = 0; x < mask.width; ++x) {
                const std::size_t offset =
                    static_cast<std::size_t>(y) *
                        surface->second.stride +
                    static_cast<std::size_t>(x) * bytes_per_pixel;
                if (offset + bytes_per_pixel >
                    surface->second.pixels.size()) {
                    continue;
                }
                uint32_t pixel = 0;
                for (uint32_t byte = 0; byte < bytes_per_pixel;
                     ++byte) {
                    pixel |= static_cast<uint32_t>(
                                 surface->second.pixels[offset + byte])
                             << (byte * 8U);
                }
                bool set = false;
                switch (surface->second.format) {
                    case 0:
                        set = static_cast<uint16_t>(pixel) != 0;
                        break;
                    case 1:
                        set = (pixel & 0xFFFFFFU) != 0;
                        break;
                    case 2:
                        set = (pixel & 0xFF000000U) != 0;
                        break;
                    case 3:
                        set = static_cast<uint8_t>(pixel) != 0;
                        break;
                    default:
                        break;
                }
                if (set) {
                    mask.bits[
                        static_cast<std::size_t>(y) * mask.stride +
                        (x >> 3U)] |= static_cast<uint8_t>(1U << (x & 7U));
                }
            }
        }
        object_masks_[object] = std::move(mask);
    }
    [[nodiscard]] const std::map<uint32_t, ObjectMaskState>&
    object_masks() const {
        return object_masks_;
    }
    // Legacy API name retained; original Grp0:43 always targets BackF.
    // The ninth native operand is the effect value, never an object ID.
    uint32_t configure_sprite_mask(uint32_t effect, const SpriteMaskConfig& in);
    [[nodiscard]] const std::map<uint32_t, SpriteMaskConfig>&
    sprite_mask_configs() const {
        return sprite_mask_configs_;
    }
    [[nodiscard]] const std::map<uint32_t, GroupState>& group_states() const {
        return group_states_;
    }
    struct BitmapBlit {
        uint32_t destination = 0;
        uint32_t source = 0;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t mode = 0;
        uint32_t parameter = 0;
    };
    static uint8_t blended_byte(uint8_t destination, uint8_t source,
                                uint32_t factor) {
        const int32_t difference =
            static_cast<int32_t>(source) -
            static_cast<int32_t>(destination);
        const int32_t product = difference *
            static_cast<int32_t>(factor);
        const int32_t adjusted =
            product >= 0
                ? product / 128
                : -((-product + 127) / 128);
        return static_cast<uint8_t>(
            std::clamp(
                static_cast<int32_t>(destination) + adjusted,
                0, 255));
    }
    static int32_t floor_shift_right(int32_t value, uint32_t shift) {
        const int32_t divisor = static_cast<int32_t>(1U << shift);
        return value >= 0
                   ? value / divisor
                   : -((-value + divisor - 1) / divisor);
    }
    static int16_t signed_multiply_high(int16_t first, int16_t second) {
        return static_cast<int16_t>(
            floor_shift_right(
                static_cast<int32_t>(first) *
                    static_cast<int32_t>(second),
                16U));
    }
    static void refresh_bgra_mirror(GraphicsSurface& surface) {
        if(surface.defer_rgba_mirror){surface.rgba.clear();return;}
        const size_t count=static_cast<size_t>(surface.width)*surface.height;
        auto* rgba=surface.rgba.overwrite(count*4U);
        if(surface.format!=1U && surface.format!=2U) {
            std::fill_n(rgba,count*4U,uint8_t{0});return;
        }
        // Resolve a CPU view once; per-byte lazy-access checks prevent SIMD
        // and needlessly touch the ownership state for every color component.
        const auto& pixels=std::as_const(surface.pixels).cpu_pixels();
        for(size_t pixel=0;pixel<count;++pixel) {
            const size_t at=pixel*4U;
            rgba[at]=pixels[at+2U];rgba[at+1U]=pixels[at+1U];rgba[at+2U]=pixels[at];
            rgba[at+3U]=surface.format==1U?0xffU:pixels[at+3U];
        }
    }
    bool accelerate_bitmap_transfer(uint32_t destination,uint32_t source,int32_t x,int32_t y,
                                    uint32_t mode,uint32_t parameter,bool direct_copy,bool source_fade);
    bool record_bitmap_mode0_alpha(uint32_t destination, uint32_t source,
                                    int32_t x, int32_t y, uint32_t mode,
                                    uint32_t parameter);
    bool record_bitmap_blit(uint32_t destination, uint32_t source, int32_t x,
                            int32_t y, uint32_t mode, uint32_t parameter) {
        auto target = graphics_surfaces_.find(destination);
        auto input = graphics_surfaces_.find(source);
        if (target == graphics_surfaces_.end() ||
            input == graphics_surfaces_.end()) {
            return false;
        }
        if (mode == 0x80U && target->second.format <= 3U && input->second.format <= 3U) {
            const auto df = target->second.format, sf = input->second.format;
            // 0040B710's mode80 preflight: 0 only pairs with0;
            // 1/2 accept1..3; 3 accepts2/3.
            if (!(df == sf || ((df == 1U || df == 2U) && sf >= 1U) ||
                  (df == 3U && sf == 2U))) return false;
        }
        // 0040B710 accepts these pairs, but mode80's0040BDD0 has
        // no conversion arm for them: return success without touching pixels.
        if (mode == 0x80U && parameter <= 0x100U &&
            ((target->second.format == 3U && input->second.format == 2U) ||
             (target->second.format == 1U && input->second.format == 3U))) {
            if (debug_enabled()) bitmap_blits_.push_back({destination, source, x, y, mode, parameter});
            display_dirty_ = true;
            return true;
        }
        const bool mode_ff_packed =
            mode == 0xFFU &&
            parameter < 4U &&
            (input->second.format == 1U ||
             input->second.format == 2U);
        const bool mode0 =
            mode == 0U ||
            (mode == 0xFFU && !mode_ff_packed);
        const bool format1_mode0 =
            mode0 &&
            input->second.format == 1U;
        const bool mode5 =
            mode == 5U || mode == 0xC0U;
        const bool mode80_direct =
            mode == 0x80U ||
            (mode5 && parameter == 0U) ||
            (mode == 0xC1U && parameter == 0U) ||
            (mode == 0xF0U && parameter == 0U);
        const bool matching_direct_copy =
            (mode80_direct || format1_mode0) &&
            target->second.format == input->second.format;
        const bool format1_to_format2 =
            (mode80_direct || format1_mode0) &&
            target->second.format == 2U &&
            input->second.format == 1U;
        const bool format3_to_format2 =
            mode80_direct &&
            target->second.format == 2U &&
            input->second.format == 3U;
        const bool format2_to_format1 =
            mode80_direct &&
            target->second.format == 1U &&
            input->second.format == 2U;
        const bool format0_transparent_copy =
            mode0 &&
            target->second.format == 0U &&
            input->second.format == 0U;
        if ((mode0 || ((mode == 1U || mode == 0x20U) && parameter == 0U)) &&
            input->second.format == 2U &&
            (target->second.format == 1U || target->second.format == 2U)) {
            return record_bitmap_mode0_alpha(destination, source, x, y, mode, parameter);
        }
        if ((mode == 1U || mode == 0x20U) && parameter>0U && parameter<0x100U &&
            input->second.format==2U && target->second.format==2U) {
            if(!materialize_graphics_pixels(target->second) || !materialize_graphics_pixels(input->second))return false;
            auto& out=target->second;auto& in=input->second;
            if(!blend_original_glyph_opacity({out.pixels,0,out.stride,out.width,out.height,2,4},
                {in.pixels,0,in.stride,in.width,in.height,2,4},x,y,parameter))return false;
            refresh_bgra_mirror(out);if (debug_enabled()) bitmap_blits_.push_back({destination,source,x,y,mode,parameter});
            display_dirty_=true;return true;
        }
        if(accelerate_bitmap_transfer(destination,source,x,y,mode,parameter,
                matching_direct_copy || format1_to_format2 || format2_to_format1,mode5))return true;
        const bool clear_intersection = mode == 0x41U;
        const bool clear_source_key = mode == 0x40U;
        if (mode_ff_packed) {
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            const int64_t clipped_left = std::max<int64_t>(x, 0);
            const int64_t clipped_top = std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) + input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) + input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            if (clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    const uint32_t source_y =
                        static_cast<uint32_t>(target_y - y);
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const uint32_t source_x =
                            static_cast<uint32_t>(target_x - x);
                        const std::size_t source_offset =
                            static_cast<std::size_t>(source_y) *
                                input->second.stride +
                            static_cast<std::size_t>(source_x) * 4U;
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            static_cast<std::size_t>(target_x) *
                                target->second.bytes_per_pixel;
                        if (source_offset + 4U >
                                input->second.pixels.size() ||
                            target_offset + 4U >
                                target->second.pixels.size()) {
                            continue;
                        }
                        const uint32_t source_pixel =
                            static_cast<uint32_t>(
                                input->second.pixels[source_offset]) |
                            static_cast<uint32_t>(
                                input->second.pixels[
                                    source_offset + 1U])
                                << 8U |
                            static_cast<uint32_t>(
                                input->second.pixels[
                                    source_offset + 2U])
                                << 16U |
                            static_cast<uint32_t>(
                                input->second.pixels[
                                    source_offset + 3U])
                                << 24U;
                        uint32_t output = 0;
                        switch (parameter) {
                            case 0:
                                output =
                                    (source_pixel & 0xFFU) |
                                    0xFF000000U;
                                break;
                            case 1:
                                output =
                                    (source_pixel & 0xFF00U) |
                                    0xFF000000U;
                                break;
                            case 2:
                                output =
                                    (source_pixel & 0xFFFF0000U) |
                                    0xFF000000U;
                                break;
                            case 3: {
                                const uint8_t value =
                                    static_cast<uint8_t>(
                                        source_pixel >> 24U);
                                output =
                                    static_cast<uint32_t>(value) *
                                    0x01010101U;
                                break;
                            }
                            default:
                                break;
                        }
                        for (uint32_t byte = 0; byte < 4U; ++byte) {
                            target->second.pixels[
                                target_offset + byte] =
                                static_cast<uint8_t>(
                                    output >> (byte * 8U));
                        }
                    }
                }
            }
            if (target->second.format == 1U ||
                target->second.format == 2U) {
                refresh_bgra_mirror(target->second);
            }
        } else if (matching_direct_copy || format1_to_format2 ||
            format2_to_format1) {
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            if (target->second.rgba.size() !=
                static_cast<std::size_t>(target->second.width) *
                    target->second.height * 4U) {
                target->second.rgba.assign(
                    static_cast<std::size_t>(
                        target->second.width) *
                        target->second.height * 4U,
                    0);
            }
            const int64_t clipped_left =
                std::max<int64_t>(x, 0);
            const int64_t clipped_top =
                std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) +
                    input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) +
                    input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            if (clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    const uint32_t source_y =
                        static_cast<uint32_t>(target_y - y);
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const uint32_t source_x =
                            static_cast<uint32_t>(target_x - x);
                        const std::size_t source_offset =
                            static_cast<std::size_t>(source_y) *
                                input->second.stride +
                            static_cast<std::size_t>(source_x) *
                                input->second.bytes_per_pixel;
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            static_cast<std::size_t>(target_x) *
                                target->second.bytes_per_pixel;
                        if (format2_to_format1) {
                            const uint32_t alpha_half =
                                input->second.pixels[source_offset + 3U] >> 1U;
                            // 0040BED0 uses530650, whose entry127 is
                            // overwritten with128 by00408653..0040868A.
                            const uint32_t factor = alpha_half == 127U ? 128U : alpha_half;
                            for (uint32_t byte = 0; byte < 3U;
                                 ++byte) {
                                target->second.pixels[
                                    target_offset + byte] =
                                    static_cast<uint8_t>(
                                        (static_cast<uint32_t>(
                                             input->second.pixels[
                                                 source_offset +
                                                 byte]) *
                                         factor) >>
                                        7U);
                            }
                            target->second.pixels[
                                target_offset + 3U] = 0;
                        } else {
                            for (uint32_t byte = 0;
                                 byte <
                                 target->second.bytes_per_pixel;
                                 ++byte) {
                                target->second.pixels[
                                    target_offset + byte] =
                                    input->second.pixels[
                                        source_offset + byte];
                            }
                        }
                        if (format1_to_format2) {
                            target->second.pixels[
                                target_offset + 3U] = 0xFFU;
                        }
                        if (target->second.bytes_per_pixel == 4U) {
                            target->second.rgba[target_offset] =
                                target->second.pixels[
                                    target_offset + 2U];
                            target->second.rgba[
                                target_offset + 1U] =
                                target->second.pixels[
                                    target_offset + 1U];
                            target->second.rgba[
                                target_offset + 2U] =
                                target->second.pixels[
                                    target_offset];
                            target->second.rgba[
                                target_offset + 3U] =
                                target->second.format == 1U
                                    ? 0xFFU
                                    : target->second.pixels[
                                          target_offset + 3U];
                        }
                    }
                }
            }
        } else if (format3_to_format2) {
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            if (target->second.rgba.size() !=
                static_cast<std::size_t>(target->second.width) *
                    target->second.height * 4U) {
                target->second.rgba.assign(
                    static_cast<std::size_t>(
                        target->second.width) *
                        target->second.height * 4U,
                    0);
            }
            const int64_t clipped_left =
                std::max<int64_t>(x, 0);
            const int64_t clipped_top =
                std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) +
                    input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) +
                    input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            if (clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    const uint32_t source_y =
                        static_cast<uint32_t>(target_y - y);
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const uint32_t source_x =
                            static_cast<uint32_t>(target_x - x);
                        const uint8_t index =
                            input->second.pixels[
                                static_cast<std::size_t>(source_y) *
                                    input->second.stride +
                                source_x];
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            static_cast<std::size_t>(target_x) * 4U;
                        target->second.pixels[target_offset] = 0;
                        target->second.pixels[target_offset + 1U] = 0;
                        target->second.pixels[target_offset + 2U] = 0;
                        target->second.pixels[target_offset + 3U] =
                            index;
                        target->second.rgba[target_offset] = 0;
                        target->second.rgba[target_offset + 1U] = 0;
                        target->second.rgba[target_offset + 2U] = 0;
                        target->second.rgba[target_offset + 3U] = index;
                    }
                }
            }
        } else if (format0_transparent_copy) {
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            const int64_t clipped_left =
                std::max<int64_t>(x, 0);
            const int64_t clipped_top =
                std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) +
                    input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) +
                    input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            if (clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    const uint32_t source_y =
                        static_cast<uint32_t>(target_y - y);
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const uint32_t source_x =
                            static_cast<uint32_t>(target_x - x);
                        const std::size_t source_offset =
                            static_cast<std::size_t>(source_y) *
                                input->second.stride +
                            static_cast<std::size_t>(source_x) * 2U;
                        const uint16_t source_pixel =
                            static_cast<uint16_t>(
                                input->second.pixels[
                                    source_offset]) |
                            static_cast<uint16_t>(
                                static_cast<uint16_t>(
                                    input->second.pixels[
                                        source_offset + 1U])
                                << 8U);
                        if (source_pixel == 0) {
                            continue;
                        }
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            static_cast<std::size_t>(target_x) * 2U;
                        target->second.pixels[target_offset] =
                            static_cast<uint8_t>(source_pixel);
                        target->second.pixels[
                            target_offset + 1U] =
                            static_cast<uint8_t>(
                                source_pixel >> 8U);
                    }
                }
            }
        } else if (clear_intersection) {
            if (!materialize_graphics_pixels(target->second)) {
                return false;
            }
            if (target->second.rgba.size() !=
                static_cast<std::size_t>(target->second.width) *
                    target->second.height * 4U) {
                target->second.rgba.assign(
                    static_cast<std::size_t>(
                        target->second.width) *
                        target->second.height * 4U,
                    0);
            }
            const uint32_t clear_width =
                std::min(target->second.width,
                         input->second.width);
            const uint32_t clear_height =
                std::min(target->second.height,
                         input->second.height);
            const uint32_t clear_bytes =
                clear_width * target->second.bytes_per_pixel;
            for (uint32_t y = 0; y < clear_height; ++y) {
                const std::size_t offset =
                    static_cast<std::size_t>(y) *
                    target->second.stride;
                std::fill_n(
                    target->second.pixels.begin() +
                        static_cast<std::ptrdiff_t>(offset),
                    clear_bytes, uint8_t{0});
                std::fill_n(
                    target->second.rgba.begin() +
                        static_cast<std::ptrdiff_t>(
                            static_cast<std::size_t>(y) *
                            target->second.width * 4U),
                    static_cast<std::size_t>(clear_width) * 4U,
                    uint8_t{0});
            }
        } else if (clear_source_key) {
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            if (target->second.rgba.size() !=
                static_cast<std::size_t>(target->second.width) *
                    target->second.height * 4U) {
                target->second.rgba.assign(
                    static_cast<std::size_t>(
                        target->second.width) *
                        target->second.height * 4U,
                    0);
            }
            const int64_t clipped_left =
                std::max<int64_t>(x, 0);
            const int64_t clipped_top =
                std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) +
                    input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) +
                    input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            if (clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    const uint32_t source_y =
                        static_cast<uint32_t>(target_y - y);
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const uint32_t source_x =
                            static_cast<uint32_t>(target_x - x);
                        const std::size_t source_offset =
                            static_cast<std::size_t>(source_y) *
                                input->second.stride +
                            static_cast<std::size_t>(source_x) *
                                input->second.bytes_per_pixel;
                        bool clear = false;
                        if (input->second.format == 0U &&
                            target->second.format == 0U) {
                            clear =
                                input->second.pixels[source_offset] != 0 ||
                                input->second.pixels[
                                    source_offset + 1U] != 0;
                        } else if (input->second.format == 1U &&
                                   (target->second.format == 1U ||
                                    target->second.format == 2U)) {
                            clear =
                                input->second.pixels[source_offset] != 0 ||
                                input->second.pixels[
                                    source_offset + 1U] != 0 ||
                                input->second.pixels[
                                    source_offset + 2U] != 0;
                        } else if (input->second.format == 2U &&
                                   (target->second.format == 1U ||
                                    target->second.format == 2U)) {
                            const uint8_t alpha =
                                input->second.pixels[
                                    source_offset + 3U];
                            clear = parameter == 0U
                                        ? alpha == 0xFFU
                                        : alpha != 0U;
                        }
                        if (!clear) {
                            continue;
                        }
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            static_cast<std::size_t>(target_x) *
                                target->second.bytes_per_pixel;
                        std::fill_n(
                            target->second.pixels.begin() +
                                static_cast<std::ptrdiff_t>(
                                    target_offset),
                            target->second.bytes_per_pixel,
                            uint8_t{0});
                        if (target->second.bytes_per_pixel == 4U) {
                            std::fill_n(
                                target->second.rgba.begin() +
                                    static_cast<std::ptrdiff_t>(
                                        target_offset),
                                4U, uint8_t{0});
                        }
                    }
                }
            }
        } else if (mode == 0xC1U) {
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            const int64_t clipped_left = std::max<int64_t>(x, 0);
            const int64_t clipped_top = std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) + input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) + input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            const uint32_t target_format = target->second.format;
            if (parameter != 0U && parameter < 0x100U &&
                target_format == input->second.format &&
                (target_format == 0U ||
                 target_format == 1U ||
                 target_format == 2U) &&
                clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            static_cast<std::size_t>(target_x) *
                                target->second.bytes_per_pixel;
                        if (target_format == 0U) {
                            const uint16_t destination_pixel =
                                static_cast<uint16_t>(
                                    target->second.pixels[
                                        target_offset]) |
                                static_cast<uint16_t>(
                                    static_cast<uint16_t>(
                                        target->second.pixels[
                                            target_offset + 1U])
                                    << 8U);
                            const uint32_t inverse =
                                0x100U - parameter;
                            const auto to_white =
                                [inverse, parameter](
                                    uint16_t destination,
                                    uint16_t mask) {
                                    return static_cast<uint16_t>(
                                        (((destination & mask) *
                                              inverse +
                                          mask * parameter) >>
                                         8U) &
                                        mask);
                                };
                            const uint16_t output =
                                static_cast<uint16_t>(
                                    to_white(
                                        destination_pixel,
                                        0x7C00U) |
                                    to_white(
                                        destination_pixel,
                                        0x03E0U) |
                                    to_white(
                                        destination_pixel,
                                        0x001FU));
                            target->second.pixels[target_offset] =
                                static_cast<uint8_t>(output);
                            target->second.pixels[
                                target_offset + 1U] =
                                static_cast<uint8_t>(output >> 8U);
                        } else {
                            const uint32_t factor = parameter >> 1U;
                            for (uint32_t byte = 0; byte < 3U; ++byte) {
                                target->second.pixels[
                                    target_offset + byte] =
                                    blended_byte(
                                        target->second.pixels[
                                            target_offset + byte],
                                        0xFFU, factor);
                            }
                            if (target_format == 2U) {
                                target->second.pixels[
                                    target_offset + 3U] =
                                    input->second.pixels[
                                        static_cast<std::size_t>(
                                            target_y - y) *
                                            input->second.stride +
                                        static_cast<std::size_t>(
                                            target_x - x) *
                                            input->second.bytes_per_pixel +
                                        3U];
                            }
                        }
                    }
                }
            }
            if (target_format == 1U || target_format == 2U) {
                refresh_bgra_mirror(target->second);
            }
        } else if (mode == 0xF0U) {
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            const int64_t clipped_left = std::max<int64_t>(x, 0);
            const int64_t clipped_top = std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) + input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) + input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            const uint32_t source_format = input->second.format;
            const uint32_t target_format = target->second.format;
            if (parameter != 0U && parameter < 0x100U &&
                clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    const uint32_t source_y =
                        static_cast<uint32_t>(target_y - y);
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const uint32_t source_x =
                            static_cast<uint32_t>(target_x - x);
                        const std::size_t source_offset =
                            static_cast<std::size_t>(source_y) *
                                input->second.stride +
                            static_cast<std::size_t>(source_x) *
                                input->second.bytes_per_pixel;
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            static_cast<std::size_t>(target_x) *
                                target->second.bytes_per_pixel;
                        if (source_format == 0U &&
                            target_format == 0U) {
                            const uint16_t source_pixel =
                                static_cast<uint16_t>(
                                    input->second.pixels[source_offset]) |
                                static_cast<uint16_t>(
                                    static_cast<uint16_t>(
                                        input->second.pixels[
                                            source_offset + 1U])
                                    << 8U);
                            const uint16_t destination_pixel =
                                static_cast<uint16_t>(
                                    target->second.pixels[
                                        target_offset]) |
                                static_cast<uint16_t>(
                                    static_cast<uint16_t>(
                                        target->second.pixels[
                                            target_offset + 1U])
                                    << 8U);
                            const auto blend_channel =
                                [parameter](uint16_t source,
                                            uint16_t destination,
                                            uint16_t mask) {
                                    return static_cast<uint16_t>(
                                        ((source & mask) *
                                             (0x100U - parameter) +
                                         (destination & mask) *
                                             parameter) >>
                                        8U);
                                };
                            const uint16_t output =
                                static_cast<uint16_t>(
                                    blend_channel(
                                        source_pixel,
                                        destination_pixel,
                                        0x7C00U) |
                                    blend_channel(
                                        source_pixel,
                                        destination_pixel,
                                        0x03E0U) |
                                    blend_channel(
                                        source_pixel,
                                        destination_pixel,
                                        0x001FU));
                            target->second.pixels[target_offset] =
                                static_cast<uint8_t>(output);
                            target->second.pixels[
                                target_offset + 1U] =
                                static_cast<uint8_t>(output >> 8U);
                        } else if ((source_format == 1U ||
                                    source_format == 2U) &&
                                   (target_format == 1U ||
                                    target_format == 2U)) {
                            const uint32_t factor = parameter >> 1U;
                            for (uint32_t byte = 0; byte < 4U; ++byte) {
                                target->second.pixels[
                                    target_offset + byte] =
                                    blended_byte(
                                        target->second.pixels[
                                            target_offset + byte],
                                        input->second.pixels[
                                            source_offset + byte],
                                        factor);
                            }
                        }
                    }
                }
            }
            if (target_format == 1U || target_format == 2U) {
                refresh_bgra_mirror(target->second);
            }
        } else if (mode == 8U || mode == 9U ||
                   mode == 0x26U || mode == 0x27U) {
            const bool threshold_source =
                mode == 9U || mode == 0x27U;
            const uint32_t effective_parameter =
                (mode == 0x26U || mode == 0x27U)
                    ? 0x100U - parameter
                    : parameter;
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            const int64_t clipped_left = std::max<int64_t>(x, 0);
            const int64_t clipped_top = std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) + input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) + input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            const uint32_t source_format = input->second.format;
            if (effective_parameter <= 0x100U &&
                (source_format == 1U || source_format == 2U) &&
                target->second.format == 1U &&
                clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    const uint32_t source_y =
                        static_cast<uint32_t>(target_y - y);
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const uint32_t source_x =
                            static_cast<uint32_t>(target_x - x);
                        const std::size_t source_offset =
                            static_cast<std::size_t>(source_y) *
                                input->second.stride +
                            static_cast<std::size_t>(source_x) * 4U;
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            static_cast<std::size_t>(target_x) * 4U;
                        for (uint32_t byte = 0; byte < 4U; ++byte) {
                            const int32_t source_value =
                                input->second.pixels[
                                    source_offset + byte];
                            const int32_t destination_value =
                                target->second.pixels[
                                    target_offset + byte];
                            const int32_t threshold_value =
                                threshold_source
                                    ? source_value
                                    : destination_value;
                            const uint16_t sign_mask =
                                threshold_value > 0x7F
                                    ? 0xFFFFU
                                    : 0x0000U;
                            const uint16_t doubled_sum =
                                static_cast<uint16_t>(
                                    static_cast<uint16_t>(
                                        destination_value +
                                        source_value)
                                    << 1U);
                            const uint16_t product =
                                static_cast<uint16_t>(
                                    static_cast<uint16_t>(
                                        destination_value *
                                        source_value) >>
                                    7U);
                            const int16_t delta =
                                static_cast<int16_t>(
                                    static_cast<uint16_t>(
                                        (doubled_sum & sign_mask) -
                                        (sign_mask >> 8U) +
                                        (product ^ sign_mask) +
                                        (sign_mask != 0U ? 1U : 0U) -
                                        static_cast<uint16_t>(
                                            source_value)));
                            int32_t value = 0;
                            if (source_format == 2U) {
                                const uint32_t source_alpha =
                                    input->second.pixels[
                                        source_offset + 3U];
                                const int16_t alpha_weight =
                                    static_cast<int16_t>(
                                        (source_alpha *
                                         effective_parameter) >>
                                        4U);
                                value =
                                    destination_value +
                                    signed_multiply_high(
                                        static_cast<int16_t>(
                                            static_cast<uint16_t>(
                                                static_cast<uint16_t>(
                                                    delta)
                                                << 4U)),
                                        alpha_weight);
                            } else if (effective_parameter < 0x100U) {
                                const int16_t scaled_delta =
                                    static_cast<int16_t>(
                                        static_cast<uint16_t>(
                                            static_cast<uint16_t>(
                                                delta)
                                            << 4U));
                                const int16_t scaled_parameter =
                                    static_cast<int16_t>(
                                        static_cast<uint16_t>(
                                            effective_parameter
                                            << 4U));
                                value =
                                    destination_value +
                                    signed_multiply_high(
                                        scaled_delta,
                                        scaled_parameter);
                            } else {
                                value = delta;
                            }
                            target->second.pixels[
                                target_offset + byte] =
                                static_cast<uint8_t>(
                                    std::clamp(value, 0, 255));
                        }
                    }
                }
            }
            refresh_bgra_mirror(target->second);
        } else if (mode == 7U || mode == 0x25U) {
            const uint32_t effective_parameter =
                mode == 0x25U ? 0x100U - parameter : parameter;
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            const int64_t clipped_left = std::max<int64_t>(x, 0);
            const int64_t clipped_top = std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) + input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) + input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            const uint32_t target_format = target->second.format;
            const bool supported =
                input->second.format == 2U &&
                (target_format == 1U ||
                 target_format == 2U ||
                 target_format == 3U);
            if (effective_parameter <= 0x100U &&
                supported &&
                clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    const uint32_t source_y =
                        static_cast<uint32_t>(target_y - y);
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const uint32_t source_x =
                            static_cast<uint32_t>(target_x - x);
                        const std::size_t source_offset =
                            static_cast<std::size_t>(source_y) *
                                input->second.stride +
                            static_cast<std::size_t>(source_x) * 4U;
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            static_cast<std::size_t>(target_x) *
                                target->second.bytes_per_pixel;
                        const uint32_t source_alpha =
                            input->second.pixels[
                                source_offset + 3U];
                        const uint32_t factor =
                            0x100U -
                            ((source_alpha * effective_parameter) >> 8U);
                        if (target_format == 1U) {
                            for (uint32_t byte = 0; byte < 4U; ++byte) {
                                target->second.pixels[
                                    target_offset + byte] =
                                    static_cast<uint8_t>(
                                        (static_cast<uint32_t>(
                                             target->second.pixels[
                                                 target_offset + byte]) *
                                         factor) >>
                                        8U);
                            }
                        } else if (target_format == 2U) {
                            target->second.pixels[
                                target_offset + 3U] =
                                static_cast<uint8_t>(
                                    (static_cast<uint32_t>(
                                         target->second.pixels[
                                             target_offset + 3U]) *
                                     factor) >>
                                    8U);
                        } else {
                            target->second.pixels[target_offset] =
                                static_cast<uint8_t>(
                                    (static_cast<uint32_t>(
                                         target->second.pixels[
                                             target_offset]) *
                                     factor) >>
                                    8U);
                        }
                    }
                }
            }
            if (target_format == 1U || target_format == 2U) {
                refresh_bgra_mirror(target->second);
            }
        } else if (mode == 6U || mode == 0x24U) {
            const uint32_t effective_parameter =
                mode == 0x24U ? 0x100U - parameter : parameter;
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            const int64_t clipped_left = std::max<int64_t>(x, 0);
            const int64_t clipped_top = std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) + input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) + input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            const uint32_t source_format = input->second.format;
            const bool supported =
                (source_format == 1U &&
                 target->second.format == 1U) ||
                (source_format == 2U &&
                 target->second.format == 1U);
            if (effective_parameter != 0U &&
                effective_parameter <= 0x100U &&
                supported &&
                clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    const uint32_t source_y =
                        static_cast<uint32_t>(target_y - y);
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const uint32_t source_x =
                            static_cast<uint32_t>(target_x - x);
                        const std::size_t source_offset =
                            static_cast<std::size_t>(source_y) *
                                input->second.stride +
                            static_cast<std::size_t>(source_x) * 4U;
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            static_cast<std::size_t>(target_x) * 4U;
                        const uint32_t source_alpha =
                            input->second.pixels[
                                source_offset + 3U];
                        const uint32_t factor =
                            source_format == 1U
                                ? effective_parameter
                                : ((source_alpha >> 1U) *
                                   effective_parameter) >>
                                      8U;
                        for (uint32_t byte = 0; byte < 4U; ++byte) {
                            const uint32_t weighted_source =
                                (static_cast<uint32_t>(
                                     input->second.pixels[
                                         source_offset + byte]) *
                                 factor) >>
                                8U;
                            const uint32_t destination =
                                target->second.pixels[
                                    target_offset + byte];
                            const int32_t value =
                                static_cast<int32_t>(
                                    weighted_source + destination) -
                                static_cast<int32_t>(
                                    (weighted_source * destination) >>
                                    8U);
                            target->second.pixels[
                                target_offset + byte] =
                                static_cast<uint8_t>(
                                    std::clamp(value, 0, 255));
                        }
                    }
                }
            }
            if (target->second.format == 1U ||
                target->second.format == 2U) {
                refresh_bgra_mirror(target->second);
            }
        } else if (mode5) {
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            const int64_t clipped_left = std::max<int64_t>(x, 0);
            const int64_t clipped_top = std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) + input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) + input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            const uint32_t source_format = input->second.format;
            const uint32_t target_format = target->second.format;
            if (parameter <= 0x100U &&
                clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    const uint32_t source_y =
                        static_cast<uint32_t>(target_y - y);
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const uint32_t source_x =
                            static_cast<uint32_t>(target_x - x);
                        const std::size_t source_offset =
                            static_cast<std::size_t>(source_y) *
                                input->second.stride +
                            static_cast<std::size_t>(source_x) *
                                input->second.bytes_per_pixel;
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            static_cast<std::size_t>(target_x) *
                                target->second.bytes_per_pixel;
                        if (source_format == 0U &&
                            target_format == 0U) {
                            const uint16_t destination_pixel =
                                static_cast<uint16_t>(
                                    target->second.pixels[
                                        target_offset]) |
                                static_cast<uint16_t>(
                                    static_cast<uint16_t>(
                                        target->second.pixels[
                                            target_offset + 1U])
                                    << 8U);
                            const auto fade_channel =
                                [parameter](uint16_t destination,
                                            uint16_t mask) {
                                    return static_cast<uint16_t>(
                                        ((destination & mask) *
                                         (0x100U - parameter)) >>
                                        8U);
                                };
                            const uint16_t output =
                                static_cast<uint16_t>(
                                    fade_channel(
                                        destination_pixel,
                                        0x7C00U) |
                                    fade_channel(
                                        destination_pixel,
                                        0x03E0U) |
                                    fade_channel(
                                        destination_pixel,
                                        0x001FU));
                            target->second.pixels[target_offset] =
                                static_cast<uint8_t>(output);
                            target->second.pixels[
                                target_offset + 1U] =
                                static_cast<uint8_t>(output >> 8U);
                        } else if (source_format == 1U &&
                                   target_format == 1U) {
                            for (uint32_t byte = 0; byte < 4U; ++byte) {
                                target->second.pixels[
                                    target_offset + byte] =
                                    parameter == 0x100U
                                        ? 0U
                                        : static_cast<uint8_t>(
                                              (static_cast<uint32_t>(
                                                   input->second.pixels[
                                                       source_offset +
                                                       byte]) *
                                               (0x100U - parameter)) >>
                                              8U);
                            }
                        } else if (source_format == 2U &&
                                   target_format == 1U) {
                            const uint32_t factor =
                                static_cast<uint32_t>(
                                    input->second.pixels[
                                        source_offset + 3U]) >>
                                1U;
                            for (uint32_t byte = 0; byte < 4U; ++byte) {
                                const uint32_t faded_source =
                                    (static_cast<uint32_t>(
                                         input->second.pixels[
                                             source_offset + byte]) *
                                     (0x100U - parameter)) >>
                                    8U;
                                const int32_t difference =
                                    static_cast<int32_t>(
                                        faded_source) -
                                    static_cast<int32_t>(
                                        target->second.pixels[
                                            target_offset + byte]);
                                target->second.pixels[
                                    target_offset + byte] =
                                    static_cast<uint8_t>(
                                        std::clamp<int32_t>(
                                            static_cast<int32_t>(
                                                target->second.pixels[
                                                    target_offset +
                                                    byte]) +
                                                floor_shift_right(
                                                    difference *
                                                        static_cast<int32_t>(
                                                            factor),
                                                    7U),
                                            0, 255));
                            }
                        } else if (source_format == 2U &&
                                   target_format == 2U) {
                            const uint32_t source_alpha =
                                input->second.pixels[
                                    source_offset + 3U];
                            if (source_alpha == 0U) {
                                continue;
                            }
                            const uint32_t destination_alpha =
                                target->second.pixels[
                                    target_offset + 3U];
                            const uint32_t destination_term =
                                (0x100U - source_alpha) *
                                destination_alpha;
                            const uint32_t denominator =
                                source_alpha * 0x100U +
                                destination_term;
                            const uint32_t source_weight =
                                (source_alpha *
                                 (0x100U - parameter) *
                                 65536U) /
                                denominator;
                            const uint32_t destination_weight =
                                (destination_term * 65536U) /
                                denominator;
                            for (uint32_t byte = 0; byte < 3U; ++byte) {
                                const uint32_t value =
                                    static_cast<uint32_t>(
                                        input->second.pixels[
                                            source_offset + byte]) *
                                        source_weight +
                                    static_cast<uint32_t>(
                                        target->second.pixels[
                                            target_offset + byte]) *
                                        destination_weight;
                                target->second.pixels[
                                    target_offset + byte] =
                                    static_cast<uint8_t>(
                                        std::min<uint32_t>(
                                            value >> 16U, 255U));
                            }
                            target->second.pixels[
                                target_offset + 3U] =
                                static_cast<uint8_t>(
                                    denominator >> 8U);
                        }
                    }
                }
            }
            if (target_format == 1U || target_format == 2U) {
                refresh_bgra_mirror(target->second);
            }
        } else if (mode == 4U || mode == 0x23U) {
            const uint32_t effective_parameter =
                mode == 0x23U ? 0x100U - parameter : parameter;
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            const int64_t clipped_left = std::max<int64_t>(x, 0);
            const int64_t clipped_top = std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) + input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) + input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            const uint32_t source_format = input->second.format;
            const uint32_t target_format = target->second.format;
            const bool supported =
                (source_format == 1U &&
                 (target_format == 1U || target_format == 2U)) ||
                (source_format == 2U && target_format == 1U) ||
                (source_format == 3U && target_format == 3U);
            if (effective_parameter != 0U &&
                effective_parameter <= 0x100U &&
                supported &&
                clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    const uint32_t source_y =
                        static_cast<uint32_t>(target_y - y);
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const uint32_t source_x =
                            static_cast<uint32_t>(target_x - x);
                        const std::size_t source_offset =
                            static_cast<std::size_t>(source_y) *
                                input->second.stride +
                            static_cast<std::size_t>(source_x) *
                                input->second.bytes_per_pixel;
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            static_cast<std::size_t>(target_x) *
                                target->second.bytes_per_pixel;
                        if (source_format == 1U &&
                            target_format == 1U) {
                            const int16_t factor = static_cast<int16_t>(
                                effective_parameter >> 1U);
                            for (uint32_t byte = 0; byte < 4U; ++byte) {
                                const int16_t source_value =
                                    static_cast<int16_t>(
                                        input->second.pixels[
                                            source_offset + byte]);
                                const int16_t destination_value =
                                    static_cast<int16_t>(
                                        target->second.pixels[
                                            target_offset + byte]);
                                const int16_t centered_source =
                                    static_cast<int16_t>(
                                        byte < 3U
                                            ? source_value - 1
                                            : source_value);
                                const int16_t term =
                                    static_cast<int16_t>(
                                        centered_source * factor);
                                const int16_t adjustment =
                                    signed_multiply_high(
                                        term,
                                        static_cast<int16_t>(
                                            destination_value * 2));
                                target->second.pixels[
                                    target_offset + byte] =
                                    static_cast<uint8_t>(
                                        std::clamp<int32_t>(
                                            source_value + adjustment,
                                            0, 255));
                            }
                        } else if (source_format == 1U &&
                                   target_format == 2U) {
                            const int32_t factor =
                                static_cast<int32_t>(
                                    effective_parameter >> 1U);
                            for (uint32_t byte = 0; byte < 3U; ++byte) {
                                const int32_t source_value =
                                    input->second.pixels[
                                        source_offset + byte];
                                const uint32_t scaled_product =
                                    (static_cast<uint32_t>(
                                         target->second.pixels[
                                             target_offset + byte]) *
                                     static_cast<uint32_t>(
                                         source_value)) >>
                                    8U;
                                const int32_t difference =
                                    static_cast<int32_t>(
                                        scaled_product) -
                                    source_value;
                                target->second.pixels[
                                    target_offset + byte] =
                                    static_cast<uint8_t>(
                                        std::clamp<int32_t>(
                                            source_value +
                                                floor_shift_right(
                                                    difference * factor,
                                                    7U),
                                            0, 255));
                            }
                            target->second.pixels[
                                target_offset + 3U] =
                                input->second.pixels[
                                    source_offset + 3U];
                        } else if (source_format == 2U &&
                                   target_format == 1U) {
                            const uint32_t factor =
                                ((static_cast<uint32_t>(
                                      input->second.pixels[
                                          source_offset + 3U]) >>
                                  1U) *
                                 effective_parameter) >>
                                8U;
                            for (uint32_t byte = 0; byte < 4U; ++byte) {
                                const uint32_t destination_value =
                                    target->second.pixels[
                                        target_offset + byte];
                                const uint32_t scaled_product =
                                    (static_cast<uint32_t>(
                                         input->second.pixels[
                                             source_offset + byte]) *
                                     destination_value) >>
                                    8U;
                                const int32_t difference =
                                    static_cast<int32_t>(
                                        scaled_product) -
                                    static_cast<int32_t>(
                                        destination_value);
                                target->second.pixels[
                                    target_offset + byte] =
                                    static_cast<uint8_t>(
                                        std::clamp<int32_t>(
                                            static_cast<int32_t>(
                                                destination_value) +
                                                floor_shift_right(
                                                    difference *
                                                        static_cast<int32_t>(
                                                            factor),
                                                    7U),
                                            0, 255));
                            }
                        } else if (source_format == 3U &&
                                   target_format == 3U) {
                            const uint32_t source_value =
                                input->second.pixels[source_offset];
                            const uint32_t destination_value =
                                target->second.pixels[target_offset];
                            const uint32_t product =
                                ((destination_value * source_value + 1U) *
                                 0x101U) >>
                                16U;
                            const int32_t difference =
                                static_cast<int32_t>(product) -
                                static_cast<int32_t>(
                                    destination_value);
                            target->second.pixels[target_offset] =
                                static_cast<uint8_t>(
                                    std::clamp<int32_t>(
                                        static_cast<int32_t>(
                                            destination_value) +
                                            floor_shift_right(
                                                difference *
                                                    static_cast<int32_t>(
                                                        effective_parameter),
                                                8U),
                                        0, 255));
                        }
                    }
                }
            }
            if (target_format == 1U || target_format == 2U) {
                refresh_bgra_mirror(target->second);
            }
        } else if (mode == 3U || mode == 0x22U) {
            const uint32_t effective_parameter =
                mode == 0x22U ? 0x100U - parameter : parameter;
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            const int64_t clipped_left = std::max<int64_t>(x, 0);
            const int64_t clipped_top = std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) + input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) + input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            if (effective_parameter != 0U &&
                effective_parameter <= 0x100U &&
                input->second.format == 2U &&
                target->second.format == 1U &&
                clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    const uint32_t source_y =
                        static_cast<uint32_t>(target_y - y);
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const uint32_t source_x =
                            static_cast<uint32_t>(target_x - x);
                        const std::size_t source_offset =
                            static_cast<std::size_t>(source_y) *
                                input->second.stride +
                            static_cast<std::size_t>(source_x) * 4U;
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            static_cast<std::size_t>(target_x) * 4U;
                        const uint32_t factor =
                            ((static_cast<uint32_t>(
                                  input->second.pixels[
                                      source_offset + 3U]) >>
                              1U) *
                             effective_parameter) >>
                            8U;
                        for (uint32_t byte = 0; byte < 4U; ++byte) {
                            const uint32_t weighted_source =
                                (static_cast<uint32_t>(
                                     input->second.pixels[
                                         source_offset + byte]) *
                                 factor) >>
                                7U;
                            target->second.pixels[
                                target_offset + byte] =
                                static_cast<uint8_t>(
                                    weighted_source <
                                            target->second.pixels[
                                                target_offset + byte]
                                        ? target->second.pixels[
                                              target_offset + byte] -
                                              weighted_source
                                        : 0U);
                        }
                    }
                }
            }
            if (target->second.format == 1U ||
                target->second.format == 2U) {
                refresh_bgra_mirror(target->second);
            }
        } else if (mode == 2U || mode == 0x21U) {
            const uint32_t effective_parameter =
                mode == 0x21U ? 0x100U - parameter : parameter;
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            const int64_t clipped_left = std::max<int64_t>(x, 0);
            const int64_t clipped_top = std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) + input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) + input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            if (effective_parameter != 0U &&
                effective_parameter < 0x100U &&
                input->second.format == 0U &&
                target->second.format == 0U &&
                clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    const uint32_t source_y =
                        static_cast<uint32_t>(target_y - y);
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const uint32_t source_x =
                            static_cast<uint32_t>(target_x - x);
                        const std::size_t source_offset =
                            static_cast<std::size_t>(source_y) *
                                input->second.stride +
                            source_x * 2U;
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            target_x * 2U;
                        const uint16_t source_pixel =
                            static_cast<uint16_t>(
                                input->second.pixels[source_offset]) |
                            static_cast<uint16_t>(
                                static_cast<uint16_t>(
                                    input->second.pixels[
                                        source_offset + 1U])
                                << 8U);
                        if (source_pixel == 0U) {
                            continue;
                        }
                        const uint16_t destination_pixel =
                            static_cast<uint16_t>(
                                target->second.pixels[target_offset]) |
                            static_cast<uint16_t>(
                                static_cast<uint16_t>(
                                    target->second.pixels[
                                        target_offset + 1U])
                                << 8U);
                        const uint32_t red_sum =
                            ((source_pixel & 0x7C00U) *
                             effective_parameter >>
                             8U) +
                            (destination_pixel & 0x7C00U);
                        const uint32_t green_sum =
                            ((source_pixel & 0x03E0U) *
                             effective_parameter >>
                             8U) +
                            (destination_pixel & 0x03E0U);
                        uint32_t blue_sum =
                            ((source_pixel & 0x001FU) *
                             effective_parameter >>
                             8U) +
                            (destination_pixel & 0x001FU);
                        const uint16_t red =
                            static_cast<uint16_t>(
                                red_sum > 0x7FFFU
                                    ? 0x7C00U
                                    : (red_sum & 0x7C00U));
                        const uint16_t green =
                            static_cast<uint16_t>(
                                green_sum > 0x03FFU
                                    ? 0x03E0U
                                    : (green_sum & 0x03E0U));
                        blue_sum =
                            blue_sum < 0x20U ? blue_sum : 0x1FU;
                        const uint16_t output =
                            static_cast<uint16_t>(
                                red + green + blue_sum);
                        target->second.pixels[target_offset] =
                            static_cast<uint8_t>(output);
                        target->second.pixels[target_offset + 1U] =
                            static_cast<uint8_t>(output >> 8U);
                    }
                }
            }
        } else if (mode == 1U || mode == 0x20U) {
            if (!materialize_graphics_pixels(target->second) ||
                !materialize_graphics_pixels(input->second)) {
                return false;
            }
            const int64_t clipped_left = std::max<int64_t>(x, 0);
            const int64_t clipped_top = std::max<int64_t>(y, 0);
            const int64_t clipped_right = std::min<int64_t>(
                static_cast<int64_t>(x) + input->second.width - 1,
                static_cast<int64_t>(target->second.width) - 1);
            const int64_t clipped_bottom = std::min<int64_t>(
                static_cast<int64_t>(y) + input->second.height - 1,
                static_cast<int64_t>(target->second.height) - 1);
            const uint32_t source_format = input->second.format;
            const uint32_t target_format = target->second.format;
            if (parameter < 0x100U &&
                clipped_left <= clipped_right &&
                clipped_top <= clipped_bottom) {
                for (int64_t target_y = clipped_top;
                     target_y <= clipped_bottom; ++target_y) {
                    const uint32_t source_y =
                        static_cast<uint32_t>(target_y - y);
                    for (int64_t target_x = clipped_left;
                         target_x <= clipped_right; ++target_x) {
                        const uint32_t source_x =
                            static_cast<uint32_t>(target_x - x);
                        const std::size_t source_offset =
                            static_cast<std::size_t>(source_y) *
                                input->second.stride +
                            static_cast<std::size_t>(source_x) *
                                input->second.bytes_per_pixel;
                        const std::size_t target_offset =
                            static_cast<std::size_t>(target_y) *
                                target->second.stride +
                            static_cast<std::size_t>(target_x) *
                                target->second.bytes_per_pixel;
                        if (parameter == 0U &&
                            source_format == 0U &&
                            target_format == 0U) {
                                const uint16_t source_pixel =
                                    static_cast<uint16_t>(
                                        input->second.pixels[source_offset]) |
                                    static_cast<uint16_t>(
                                        static_cast<uint16_t>(
                                            input->second.pixels[
                                                source_offset + 1U])
                                        << 8U);
                                if (source_pixel != 0U) {
                                    target->second.pixels[target_offset] =
                                        static_cast<uint8_t>(source_pixel);
                                    target->second.pixels[
                                        target_offset + 1U] =
                                        static_cast<uint8_t>(
                                            source_pixel >> 8U);
                                }
                        } else if (parameter == 0U &&
                                   source_format == 1U &&
                                   (target_format == 1U ||
                                    target_format == 2U)) {
                                for (uint32_t byte = 0; byte < 4U; ++byte) {
                                    target->second.pixels[
                                        target_offset + byte] =
                                        input->second.pixels[
                                            source_offset + byte];
                                }
                                if (target_format == 2U) {
                                    target->second.pixels[
                                        target_offset + 3U] = 0xFFU;
                                }
                        } else if (parameter != 0U &&
                                   source_format == 0U &&
                                   target_format == 0U) {
                            const uint16_t source_pixel =
                                static_cast<uint16_t>(
                                    input->second.pixels[source_offset]) |
                                static_cast<uint16_t>(
                                    static_cast<uint16_t>(
                                        input->second.pixels[
                                            source_offset + 1U])
                                    << 8U);
                            if (source_pixel != 0U) {
                                const uint16_t destination_pixel =
                                    static_cast<uint16_t>(
                                        target->second.pixels[
                                            target_offset]) |
                                    static_cast<uint16_t>(
                                        static_cast<uint16_t>(
                                            target->second.pixels[
                                                target_offset + 1U])
                                        << 8U);
                                const auto blend_565_channel =
                                    [parameter](uint16_t source,
                                                uint16_t destination,
                                                uint16_t mask) {
                                        return static_cast<uint16_t>(
                                            ((source & mask) *
                                                 (256U - parameter) +
                                             (destination & mask) *
                                                 parameter) >>
                                            8U);
                                    };
                                const uint16_t red =
                                    blend_565_channel(
                                        source_pixel,
                                        destination_pixel, 0x7C00U);
                                const uint16_t green =
                                    blend_565_channel(
                                        source_pixel,
                                        destination_pixel, 0x03E0U);
                                const uint16_t blue =
                                    blend_565_channel(
                                        source_pixel,
                                        destination_pixel, 0x001FU);
                                const uint16_t output =
                                    static_cast<uint16_t>(
                                        (red | green | blue) & 0xFFFFU);
                                target->second.pixels[target_offset] =
                                    static_cast<uint8_t>(output);
                                target->second.pixels[
                                    target_offset + 1U] =
                                    static_cast<uint8_t>(output >> 8U);
                            }
                        } else if (parameter != 0U &&
                                   source_format == 1U &&
                                   target_format == 1U) {
                            // Original 0040C5E0: source + ((destination-source)*factor >> 7).
                            const uint32_t factor = parameter >> 1U;
                            for (uint32_t byte = 0; byte < 4U; ++byte) {
                                target->second.pixels[
                                    target_offset + byte] =
                                    blended_byte(
                                        input->second.pixels[
                                            source_offset + byte],
                                        target->second.pixels[
                                            target_offset + byte],
                                        factor);
                            }
                        } else if (parameter != 0U &&
                                   source_format == 1U &&
                                   target_format == 2U) {
                            const uint32_t inverse = 256U - parameter;
                            const uint32_t destination_alpha =
                                target->second.pixels[
                                    target_offset + 3U];
                            const uint32_t alpha_term =
                                (destination_alpha *
                                 (256U + 255U * parameter)) >>
                                8U;
                            const uint32_t denominator =
                                inverse * 255U + alpha_term;
                            const uint32_t source_weight =
                                (inverse * 255U * 65536U) /
                                denominator;
                            const uint32_t destination_weight =
                                (alpha_term * 65536U) /
                                denominator;
                            for (uint32_t byte = 0; byte < 3U; ++byte) {
                                const uint32_t value =
                                    static_cast<uint32_t>(
                                        input->second.pixels[
                                            source_offset + byte]) *
                                        source_weight +
                                    static_cast<uint32_t>(
                                        target->second.pixels[
                                            target_offset + byte]) *
                                        destination_weight;
                                target->second.pixels[
                                    target_offset + byte] =
                                    static_cast<uint8_t>(value >> 16U);
                            }
                            target->second.pixels[
                                target_offset + 3U] =
                                static_cast<uint8_t>(
                                    denominator >> 8U);
                        } else if (source_format == 2U &&
                                   target_format == 1U) {
                            const uint32_t source_alpha =
                                input->second.pixels[
                                    source_offset + 3U];
                            if (parameter == 0U &&
                                (source_alpha >> 1U) == 0x7FU) {
                                for (uint32_t byte = 0; byte < 3U; ++byte) {
                                    target->second.pixels[
                                        target_offset + byte] =
                                        input->second.pixels[
                                            source_offset + byte];
                                }
                                target->second.pixels[
                                    target_offset + 3U] = 0;
                                continue;
                            }
                            const uint32_t factor =
                                ((source_alpha >> 1U) *
                                 (256U - parameter)) >>
                                8U;
                            for (uint32_t byte = 0; byte < 4U; ++byte) {
                                target->second.pixels[
                                    target_offset + byte] =
                                    blended_byte(
                                        target->second.pixels[
                                            target_offset + byte],
                                        input->second.pixels[
                                            source_offset + byte],
                                        factor);
                            }
                        } else if (source_format == 2U &&
                                   target_format == 2U) {
                            const uint32_t source_alpha =
                                input->second.pixels[
                                    source_offset + 3U];
                            if (source_alpha == 0U) {
                                continue;
                            }
                            if (parameter == 0U &&
                                source_alpha == 0xFFU) {
                                for (uint32_t byte = 0; byte < 4U; ++byte) {
                                    target->second.pixels[
                                        target_offset + byte] =
                                        input->second.pixels[
                                            source_offset + byte];
                                }
                                continue;
                            }
                            const uint32_t scaled_source_alpha =
                                source_alpha * (256U - parameter);
                            const uint32_t destination_component =
                                (static_cast<uint32_t>(
                                     target->second.pixels[
                                         target_offset + 3U]) *
                                 (65536U - scaled_source_alpha)) >>
                                8U;
                            const uint32_t total =
                                destination_component +
                                scaled_source_alpha;
                            const uint32_t destination_weight =
                                (destination_component << 8U) / total;
                            const uint32_t source_weight =
                                (scaled_source_alpha << 8U) / total;
                            for (uint32_t byte = 0; byte < 4U; ++byte) {
                                const uint32_t value =
                                    static_cast<uint32_t>(
                                        target->second.pixels[
                                            target_offset + byte]) *
                                        destination_weight +
                                    static_cast<uint32_t>(
                                        input->second.pixels[
                                            source_offset + byte]) *
                                        source_weight;
                                target->second.pixels[
                                    target_offset + byte] =
                                    static_cast<uint8_t>(
                                        std::min<uint32_t>(
                                            value >> 8U, 255U));
                            }
                        }
                    }
                }
            }
            if (target_format == 1U || target_format == 2U) {
                refresh_bgra_mirror(target->second);
            }
        }
        if (debug_enabled()) bitmap_blits_.push_back(
            {destination, source, x, y, mode, parameter});
        display_dirty_ = true;
        return true;
    }
    [[nodiscard]] const std::vector<BitmapBlit>& bitmap_blits() const {
        return bitmap_blits_;
    }
    enum class BitmapKernelKind : uint8_t {
        Composite = 0,
        ScaledCopy = 1,
        RotatedScale = 2,
        IsometricRotate = 3,
        HalfDownsample = 4,
    };
    struct BitmapKernelOperation {
        BitmapKernelKind kind = BitmapKernelKind::Composite;
        uint32_t destination = 0;
        std::array<uint32_t, 3> sources{};
        uint32_t mask = 0xFFFFFFFFU;
        uint32_t opacity = 0;
        uint32_t mode = 0;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t angle_x = 0;
        uint32_t angle_y = 0;
        uint32_t scale_x = 0;
        uint32_t scale_y = 0;
    };
    uint32_t record_composite_bitmap_kernel(
        uint32_t first, uint32_t second, uint32_t third,
        uint32_t mask, uint32_t opacity, uint32_t mode) {
        const auto first_surface = graphics_surfaces_.find(first);
        if (first_surface == graphics_surfaces_.end()) {
            return 1;
        }
        const auto second_surface = graphics_surfaces_.find(second);
        if (second_surface == graphics_surfaces_.end()) {
            return 2;
        }
        const auto third_surface = graphics_surfaces_.find(third);
        if (third_surface == graphics_surfaces_.end()) {
            return 4;
        }
        if (opacity > 0x100U) {
            return 3;
        }
        if (third_surface->second.format != 4U ||
            first_surface->second.format !=
                second_surface->second.format ||
            third_surface->second.width <
                first_surface->second.width ||
            third_surface->second.height <
                first_surface->second.height) {
            return 8;
        }
        if (mask != 0xFFFFFFFFU) {
            const auto mask_surface = graphics_surfaces_.find(mask);
            if (mask_surface == graphics_surfaces_.end()) {
                return 6;
            }
            if (mask_surface->second.format != 4U ||
                mask_surface->second.width !=
                    third_surface->second.width ||
                mask_surface->second.height !=
                    third_surface->second.height) {
                return 7;
            }
        }
        BitmapKernelOperation operation;
        operation.kind = BitmapKernelKind::Composite;
        operation.sources = {first, second, third};
        operation.mask = mask;
        operation.opacity = opacity;
        operation.mode = mode;
        if (debug_enabled()) bitmap_kernel_operations_.push_back(operation);
        display_dirty_ = true;
        return 0;
    }
    uint32_t record_scaled_bitmap_kernel(
        uint32_t destination, uint32_t source,
        uint32_t mode, uint32_t opacity) {
        auto destination_surface =
            graphics_surfaces_.find(destination);
        if (destination_surface == graphics_surfaces_.end()) {
            return 1;
        }
        auto source_surface = graphics_surfaces_.find(source);
        if (source_surface == graphics_surfaces_.end()) {
            return 2;
        }
        if (mode >= 2U) {
            return 4;
        }
        if (opacity > 0x100U) {
            return 5;
        }
        if (destination_surface->second.format !=
                source_surface->second.format ||
            destination_surface->second.width !=
                source_surface->second.width ||
            destination_surface->second.height !=
                source_surface->second.height) {
            return 3;
        }
        if (opacity == 0 &&
            destination_surface->second.width ==
                source_surface->second.width &&
            destination_surface->second.height ==
                source_surface->second.height) {
            if (!materialize_graphics_pixels(
                    destination_surface->second) ||
                !materialize_graphics_pixels(
                    source_surface->second)) {
                return 5;
            }
            if (destination_surface->second.rgba.size() !=
                static_cast<std::size_t>(
                    destination_surface->second.width) *
                    destination_surface->second.height * 4U) {
                destination_surface->second.rgba.assign(
                    static_cast<std::size_t>(
                        destination_surface->second.width) *
                        destination_surface->second.height * 4U,
                    0);
            }
            const uint32_t row_bytes =
                destination_surface->second.width *
                destination_surface->second.bytes_per_pixel;
            for (uint32_t y = 0;
                 y < destination_surface->second.height; ++y) {
                const std::size_t source_offset =
                    static_cast<std::size_t>(y) *
                    source_surface->second.stride;
                const std::size_t destination_offset =
                    static_cast<std::size_t>(y) *
                    destination_surface->second.stride;
                std::copy_n(
                    source_surface->second.pixels.begin() +
                        static_cast<std::ptrdiff_t>(source_offset),
                    row_bytes,
                    destination_surface->second.pixels.begin() +
                        static_cast<std::ptrdiff_t>(
                            destination_offset));
            }
            for (uint32_t y = 0;
                 y < destination_surface->second.height; ++y) {
                for (uint32_t x = 0;
                     x < destination_surface->second.width; ++x) {
                    const std::size_t pixel =
                        static_cast<std::size_t>(y) *
                            destination_surface->second.width +
                        x;
                    const std::size_t offset = pixel * 4U;
                    destination_surface->second.rgba[offset] =
                        destination_surface->second.pixels[offset + 2U];
                    destination_surface->second.rgba[offset + 1U] =
                        destination_surface->second.pixels[offset + 1U];
                    destination_surface->second.rgba[offset + 2U] =
                        destination_surface->second.pixels[offset];
                    destination_surface->second.rgba[offset + 3U] =
                        destination_surface->second.format == 1U
                            ? 0xFFU
                            : destination_surface->second.pixels[
                                  offset + 3U];
                }
            }
        } else if (opacity != 0 &&
                   (source_surface->second.format == 1U ||
                    source_surface->second.format == 2U)) {
            if (!materialize_graphics_pixels(
                    destination_surface->second) ||
                !materialize_graphics_pixels(
                    source_surface->second)) {
                return 5;
            }
            const GraphicsSurface source_copy =
                source_surface->second;
            const uint32_t effective_opacity =
                std::min<uint32_t>(opacity, 0xFFU);
            const uint32_t radius =
                effective_opacity * 2U + 1U;
            const uint32_t half_radius = radius >> 1U;
            const uint32_t factor =
                (radius * 2U + 0x20000U) / radius;
            for (uint32_t y = 0;
                 y < destination_surface->second.height; ++y) {
                const std::size_t source_row =
                    static_cast<std::size_t>(y) *
                    source_copy.stride;
                const std::size_t destination_row =
                    static_cast<std::size_t>(y) *
                    destination_surface->second.stride;
                for (uint32_t x = 0;
                     x < destination_surface->second.width; ++x) {
                    const std::size_t destination_offset =
                        destination_row +
                        static_cast<std::size_t>(x) * 4U;
                    for (uint32_t channel = 0; channel < 4U;
                         ++channel) {
                        uint32_t sum = 0;
                        for (int64_t delta =
                                 -static_cast<int64_t>(
                                     half_radius);
                             delta <=
                             static_cast<int64_t>(half_radius);
                             ++delta) {
                            int64_t source_x =
                                static_cast<int64_t>(x) + delta;
                            if (source_x < 0 ||
                                source_x >=
                                    static_cast<int64_t>(
                                        source_copy.width)) {
                                if (mode == 0U) {
                                    continue;
                                }
                                source_x = std::clamp<int64_t>(
                                    source_x, 0,
                                    static_cast<int64_t>(
                                        source_copy.width) -
                                        1);
                            }
                            const std::size_t source_offset =
                                source_row +
                                static_cast<std::size_t>(
                                    source_x) *
                                    4U +
                                channel;
                            sum +=
                                static_cast<uint32_t>(
                                    source_copy.pixels[
                                        source_offset]) >>
                                1U;
                        }
                        destination_surface->second.pixels[
                            destination_offset + channel] =
                            static_cast<uint8_t>(
                                std::min<uint32_t>(
                                    (sum * factor) >> 16U,
                                    0xFFU));
                    }
                }
            }
            if (destination_surface->second.rgba.size() !=
                static_cast<std::size_t>(
                    destination_surface->second.width) *
                    destination_surface->second.height * 4U) {
                destination_surface->second.rgba.assign(
                    static_cast<std::size_t>(
                        destination_surface->second.width) *
                        destination_surface->second.height * 4U,
                    0);
            }
            for (std::size_t pixel = 0;
                 pixel <
                 static_cast<std::size_t>(
                     destination_surface->second.width) *
                     destination_surface->second.height;
                 ++pixel) {
                const std::size_t offset = pixel * 4U;
                destination_surface->second.rgba[offset] =
                    destination_surface->second.pixels[
                        offset + 2U];
                destination_surface->second.rgba[offset + 1U] =
                    destination_surface->second.pixels[
                        offset + 1U];
                destination_surface->second.rgba[offset + 2U] =
                    destination_surface->second.pixels[offset];
                destination_surface->second.rgba[offset + 3U] =
                    destination_surface->second.format == 1U
                        ? 0xFFU
                        : destination_surface->second.pixels[
                              offset + 3U];
            }
        }
        BitmapKernelOperation operation;
        operation.kind = BitmapKernelKind::ScaledCopy;
        operation.destination = destination;
        operation.sources[0] = source;
        operation.mode = mode;
        operation.opacity = opacity;
        if (debug_enabled()) bitmap_kernel_operations_.push_back(operation);
        display_dirty_ = true;
        return 0;
    }
    uint32_t record_rotated_bitmap_kernel(
        uint32_t source, uint32_t destination,
        int32_t x, int32_t y, uint32_t width, uint32_t height,
        uint32_t angle_x, uint32_t angle_y, uint32_t scale_x,
        uint32_t scale_y) {
        const auto source_surface = graphics_surfaces_.find(source);
        if (source_surface == graphics_surfaces_.end()) {
            return 1;
        }
        const auto destination_surface =
            graphics_surfaces_.find(destination);
        if (destination_surface == graphics_surfaces_.end()) {
            return 2;
        }
        if (source_surface->second.format !=
            destination_surface->second.format) {
            return 3;
        }
        if (width < 2U || height < 2U ||
            scale_x < 2U || scale_y < 2U) {
            return 5;
        }
        BitmapKernelOperation operation;
        operation.kind = BitmapKernelKind::RotatedScale;
        operation.destination = destination;
        operation.sources[0] = source;
        operation.x = x;
        operation.y = y;
        operation.width = width;
        operation.height = height;
        operation.angle_x = angle_x;
        operation.angle_y = angle_y;
        operation.scale_x = scale_x;
        operation.scale_y = scale_y;
        if (debug_enabled()) bitmap_kernel_operations_.push_back(operation);
        display_dirty_ = true;
        return 0;
    }
    uint32_t record_isometric_bitmap_kernel(
        uint32_t source, uint32_t destination,
        uint32_t scale, uint32_t angle) {
        const auto source_surface = graphics_surfaces_.find(source);
        if (source_surface == graphics_surfaces_.end()) {
            return 1;
        }
        const auto destination_surface =
            graphics_surfaces_.find(destination);
        if (destination_surface == graphics_surfaces_.end()) {
            return 2;
        }
        if (source_surface->second.format !=
            destination_surface->second.format) {
            return 3;
        }
        if (scale == 0) {
            return 4;
        }
        BitmapKernelOperation operation;
        operation.kind = BitmapKernelKind::IsometricRotate;
        operation.destination = destination;
        operation.sources[0] = source;
        operation.mode = angle;
        operation.scale_x = scale;
        operation.scale_y = scale;
        if (debug_enabled()) bitmap_kernel_operations_.push_back(operation);
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::vector<BitmapKernelOperation>&
    bitmap_kernel_operations() const {
        return bitmap_kernel_operations_;
    }
    uint32_t generate_linear_vector_map(
        uint32_t target, int16_t origin_x, int16_t origin_y,
        int32_t step_x, int32_t step_y) {
        const auto surface = graphics_surfaces_.find(target);
        if (surface == graphics_surfaces_.end()) {
            return 1;
        }
        GraphicsSurface& output = surface->second;
        if (output.format != 4U || output.width == 0 ||
            output.height == 0) {
            return 3;
        }
        if (step_x == 0 || step_y == 0) {
            return 5;
        }
        if (!materialize_graphics_pixels(output)) {
            return 3;
        }
        const auto write_word = [&output](uint32_t x, uint32_t y,
                                          int16_t value) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * output.stride +
                static_cast<std::size_t>(x) * 4U;
            output.pixels[offset] =
                static_cast<uint8_t>(static_cast<uint16_t>(value));
            output.pixels[offset + 1U] =
                static_cast<uint8_t>(
                    static_cast<uint16_t>(value) >> 8U);
        };
        int64_t y_source = 0;
        for (uint32_t y = 0; y < output.height; ++y) {
            int64_t x_source = 0;
            for (uint32_t x = 0; x < output.width; ++x) {
                const int64_t x_word =
                    x_source / static_cast<int64_t>(output.width) +
                    (static_cast<int64_t>(origin_x) - x) * 16;
                const int64_t y_word =
                    y_source / static_cast<int64_t>(output.height) +
                    (static_cast<int64_t>(origin_y) - y) * 16;
                write_word(
                    x, y, static_cast<int16_t>(
                              static_cast<uint16_t>(x_word)));
                const std::size_t offset =
                    static_cast<std::size_t>(y) * output.stride +
                    static_cast<std::size_t>(x) * 4U;
                output.pixels[offset + 2U] =
                    static_cast<uint8_t>(
                        static_cast<uint16_t>(y_word));
                output.pixels[offset + 3U] =
                    static_cast<uint8_t>(
                        static_cast<uint16_t>(y_word) >> 8U);
                x_source += step_x;
            }
            y_source += step_y;
        }
        display_dirty_ = true;
        return 0;
    }
    uint32_t generate_random_vector_map(uint32_t target,
                                        int32_t radius) {
        const auto surface = graphics_surfaces_.find(target);
        if (surface == graphics_surfaces_.end()) {
            return 1;
        }
        GraphicsSurface& output = surface->second;
        if (output.format != 4U || output.width == 0 ||
            output.height == 0) {
            return 3;
        }
        if (!materialize_graphics_pixels(output)) {
            return 3;
        }
        const uint32_t modulus =
            static_cast<uint32_t>(radius * 2 + 1);
        if (modulus == 0) {
            throw BpFault("random vector map radius overflows modulus");
        }
        const auto next_word = [this, radius, modulus]() {
            const uint32_t first = random_next();
            const uint32_t second = random_next();
            const uint32_t combined =
                (second << 15U) | first;
            return static_cast<int16_t>(
                static_cast<uint32_t>(radius) -
                combined % modulus);
        };
        for (uint32_t y = 0; y < output.height; ++y) {
            for (uint32_t x = 0; x < output.width; ++x) {
                const int16_t x_word = next_word();
                const int16_t y_word = next_word();
                const std::size_t offset =
                    static_cast<std::size_t>(y) * output.stride +
                    static_cast<std::size_t>(x) * 4U;
                output.pixels[offset] =
                    static_cast<uint8_t>(
                        static_cast<uint16_t>(x_word));
                output.pixels[offset + 1U] =
                    static_cast<uint8_t>(
                        static_cast<uint16_t>(x_word) >> 8U);
                output.pixels[offset + 2U] =
                    static_cast<uint8_t>(
                        static_cast<uint16_t>(y_word));
                output.pixels[offset + 3U] =
                    static_cast<uint8_t>(
                        static_cast<uint16_t>(y_word) >> 8U);
            }
        }
        display_dirty_ = true;
        return 0;
    }
    uint32_t generate_half_downsample_bitmap(
        uint32_t destination, uint32_t source) {
        auto source_surface = graphics_surfaces_.find(source);
        if (source_surface == graphics_surfaces_.end()) {
            return 10;
        }
        if (source_surface->second.width <= 1U ||
            source_surface->second.height <= 1U) {
            return 6;
        }
        if (destination >= 0x4000U) {
            return 9;
        }
        GraphicsSurface output;
        output.width = (source_surface->second.width + 1U) / 2U;
        output.height = (source_surface->second.height + 1U) / 2U;
        output.format = source_surface->second.format;
        output.bytes_per_pixel =
            source_surface->second.bytes_per_pixel;
        output.stride = output.width * output.bytes_per_pixel;
        output.pixels.assign(
            static_cast<std::size_t>(output.stride) *
                output.height,
            0);
        output.rgba.assign(
            static_cast<std::size_t>(output.width) *
                output.height * 4U,
            0);
        if (source_surface->second.format == 1U ||
            source_surface->second.format == 2U) {
            if (!materialize_graphics_pixels(source_surface->second)) {
                return 3;
            }
            const auto vertical_average = [](uint8_t first,
                                             uint8_t second) {
                return static_cast<uint8_t>(
                    (static_cast<uint16_t>(first) +
                     static_cast<uint16_t>(second) + 1U) >>
                    1U);
            };
            for (uint32_t y = 0; y < output.height; ++y) {
                const uint32_t source_y0 = y * 2U;
                const bool has_bottom = source_y0 + 1U <
                    source_surface->second.height;
                const uint32_t source_y1 =
                    has_bottom ? source_y0 + 1U : source_y0;
                for (uint32_t x = 0; x < output.width; ++x) {
                    const uint32_t source_x0 = x * 2U;
                    const bool has_right = source_x0 + 1U <
                        source_surface->second.width;
                    const uint32_t source_x1 =
                        has_right ? source_x0 + 1U : source_x0;
                    const std::size_t top_left =
                        static_cast<std::size_t>(source_y0) *
                            source_surface->second.stride +
                        static_cast<std::size_t>(source_x0) *
                            source_surface->second.bytes_per_pixel;
                    const std::size_t top_right =
                        static_cast<std::size_t>(source_y0) *
                            source_surface->second.stride +
                        static_cast<std::size_t>(source_x1) *
                            source_surface->second.bytes_per_pixel;
                    const std::size_t bottom_left =
                        static_cast<std::size_t>(source_y1) *
                            source_surface->second.stride +
                        static_cast<std::size_t>(source_x0) *
                            source_surface->second.bytes_per_pixel;
                    const std::size_t bottom_right =
                        static_cast<std::size_t>(source_y1) *
                            source_surface->second.stride +
                        static_cast<std::size_t>(source_x1) *
                            source_surface->second.bytes_per_pixel;
                    const std::size_t destination_offset =
                        static_cast<std::size_t>(y) * output.stride +
                        static_cast<std::size_t>(x) *
                            output.bytes_per_pixel;
                    const std::size_t rgba_offset =
                        (static_cast<std::size_t>(y) *
                             output.width +
                         x) *
                        4U;
                    for (uint32_t channel = 0; channel < 4U;
                         ++channel) {
                        uint8_t value = 0;
                        if (has_bottom && has_right) {
                            const uint8_t left = vertical_average(
                                source_surface->second
                                    .pixels[top_left + channel],
                                source_surface->second
                                    .pixels[bottom_left + channel]);
                            const uint8_t right = vertical_average(
                                source_surface->second
                                    .pixels[top_right + channel],
                                source_surface->second
                                    .pixels[bottom_right + channel]);
                            value = vertical_average(left, right);
                        } else if (has_bottom) {
                            value = vertical_average(
                                source_surface->second
                                    .pixels[top_left + channel],
                                source_surface->second
                                    .pixels[bottom_left + channel]);
                        } else if (has_right) {
                            value = vertical_average(
                                source_surface->second
                                    .pixels[top_left + channel],
                                source_surface->second
                                    .pixels[top_right + channel]);
                        } else {
                            value = source_surface->second
                                .pixels[top_left + channel];
                        }
                        output.pixels[destination_offset + channel] =
                            value;
                    }
                    if (output.format == 1U) {
                        output.rgba[rgba_offset] =
                            output.pixels[destination_offset + 2U];
                        output.rgba[rgba_offset + 1U] =
                            output.pixels[destination_offset + 1U];
                        output.rgba[rgba_offset + 2U] =
                            output.pixels[destination_offset];
                        output.rgba[rgba_offset + 3U] = 0xFFU;
                    } else {
                        output.rgba[rgba_offset] =
                            output.pixels[destination_offset + 2U];
                        output.rgba[rgba_offset + 1U] =
                            output.pixels[destination_offset + 1U];
                        output.rgba[rgba_offset + 2U] =
                            output.pixels[destination_offset];
                        output.rgba[rgba_offset + 3U] =
                            output.pixels[destination_offset + 3U];
                    }
                }
            }
        }
        install_allocated_graphics_surface(destination, std::move(output));
        BitmapKernelOperation operation;
        operation.kind = BitmapKernelKind::HalfDownsample;
        operation.destination = destination;
        operation.sources[0] = source;
        if (debug_enabled()) bitmap_kernel_operations_.push_back(operation);
        display_dirty_ = true;
        return 0;
    }
    struct BitmapWarpOperation {
        std::array<uint32_t, 18> operands{};
    };
    uint32_t record_bitmap_warp(
        const std::array<uint32_t, 18>& operands) {
        if (graphics_surfaces_.count(operands[0]) == 0) {
            return 9;
        }
        if (graphics_surfaces_.count(operands[3]) == 0) {
            return 10;
        }
        if (debug_enabled()) bitmap_warp_operations_.push_back({operands});
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::vector<BitmapWarpOperation>&
    bitmap_warp_operations() const {
        return bitmap_warp_operations_;
    }
    enum class BitmapCompositeKind : uint8_t {
        AlphaAtOffset = 0,
        FilteredExpand = 1,
    };
    struct BitmapCompositeOperation {
        BitmapCompositeKind kind =
            BitmapCompositeKind::AlphaAtOffset;
        uint32_t destination = 0;
        uint32_t source = 0;
        uint32_t mask = 0xFFFFFFFFU;
        uint32_t mode = 0;
        int32_t x = 0;
        int32_t y = 0;
    };
    uint32_t compose_bitmap_alpha_at_offset(
        uint32_t destination, int32_t x, int32_t y,
        uint32_t source, uint32_t mask, uint32_t mode) {
        if (destination >= 0x4000U) {
            return 7;
        }
        const auto source_surface = graphics_surfaces_.find(source);
        if (source_surface == graphics_surfaces_.end()) {
            return 6;
        }
        if (source_surface->second.format != 2U) {
            return 7;
        }
        const auto mask_surface = mask == 0xFFFFFFFFU
                                      ? graphics_surfaces_.end()
                                      : graphics_surfaces_.find(mask);
        if (mask != 0xFFFFFFFFU &&
            mask_surface == graphics_surfaces_.end()) {
            return 5;
        }
        if (mask_surface != graphics_surfaces_.end() &&
            mask_surface->second.format != 2U) {
            return 7;
        }
        const auto [width, height] = effective_client_dimensions();
        GraphicsSurface output;
        output.width = width;
        output.height = height;
        output.format = 3;
        output.bytes_per_pixel = 1;
        output.stride = width;
        output.pixels.assign(
            static_cast<std::size_t>(width) * height, 0);
        output.rgba.assign(
            static_cast<std::size_t>(width) * height * 4U, 0);
        GraphicsSurface source_copy = source_surface->second;
        if (!materialize_graphics_pixels(source_copy)) {
            return 7;
        }
        std::optional<GraphicsSurface> mask_copy;
        if (mask_surface != graphics_surfaces_.end()) {
            mask_copy = mask_surface->second;
            if (!materialize_graphics_pixels(*mask_copy)) {
                return 7;
            }
        }
        const int64_t left = x;
        const int64_t top = y;
        int64_t clipped_left = std::max<int64_t>(left, 0);
        int64_t clipped_top = std::max<int64_t>(top, 0);
        int64_t clipped_right = std::min<int64_t>(
            left + static_cast<int64_t>(source_copy.width) - 1,
            static_cast<int64_t>(width) - 1);
        int64_t clipped_bottom = std::min<int64_t>(
            top + static_cast<int64_t>(source_copy.height) - 1,
            static_cast<int64_t>(height) - 1);
        if (mask_copy) {
            clipped_right = std::min<int64_t>(
                clipped_right,
                left + static_cast<int64_t>(mask_copy->width) - 1);
            clipped_bottom = std::min<int64_t>(
                clipped_bottom,
                top + static_cast<int64_t>(mask_copy->height) - 1);
        }
        if (clipped_left <= clipped_right &&
            clipped_top <= clipped_bottom) {
            for (int64_t destination_y = clipped_top;
                 destination_y <= clipped_bottom; ++destination_y) {
                const uint32_t source_y =
                    static_cast<uint32_t>(destination_y - top);
                for (int64_t destination_x = clipped_left;
                     destination_x <= clipped_right;
                     ++destination_x) {
                    const uint32_t source_x =
                        static_cast<uint32_t>(destination_x - left);
                    const std::size_t source_offset =
                        static_cast<std::size_t>(source_y) *
                            source_copy.stride +
                        static_cast<std::size_t>(source_x) * 4U;
                    const uint8_t source_alpha =
                        source_copy.pixels[source_offset + 3U];
                    uint8_t alpha = source_alpha;
                    if (mask_copy) {
                        alpha = static_cast<uint8_t>(
                            static_cast<int32_t>(source_alpha) +
                            ((static_cast<int32_t>(
                                  mask_copy->pixels[source_offset + 3U]) -
                              static_cast<int32_t>(source_alpha)) *
                                 static_cast<int32_t>(mode) >>
                             8));
                    }
                    const std::size_t destination_offset =
                        static_cast<std::size_t>(destination_y) *
                            output.stride +
                        static_cast<std::size_t>(destination_x);
                    output.pixels[destination_offset] = alpha;
                    output.rgba[destination_offset * 4U] = alpha;
                    output.rgba[destination_offset * 4U + 1U] = alpha;
                    output.rgba[destination_offset * 4U + 2U] = alpha;
                    output.rgba[destination_offset * 4U + 3U] = 0xFFU;
                }
            }
        }
        install_allocated_graphics_surface(destination, std::move(output));
        if (debug_enabled()) bitmap_composite_operations_.push_back({
            BitmapCompositeKind::AlphaAtOffset,
            destination, source, mask, mode, x, y});
        display_dirty_ = true;
        return 0;
    }
    uint32_t record_filtered_bitmap_expand(
        uint32_t destination, uint32_t source, uint32_t mode) {
        if (destination >= 0x4000U) {
            return 9;
        }
        const auto input = graphics_surfaces_.find(source);
        if (input == graphics_surfaces_.end()) {
            return 10;
        }
        if (mode < 1U || mode > 3U) {
            return 0x18U;
        }
        if (input->second.format != 2U) {
            return 0x80000011U;
        }
        if (input->second.width == 0 || input->second.height == 0) {
            return 0xFFFFFFFFU;
        }
        GraphicsSurface source_copy = input->second;
        if (!materialize_graphics_pixels(source_copy)) {
            return 0xFFFFFFFFU;
        }
        GraphicsSurface output;
        output.width = source_copy.width + 16U;
        output.height = source_copy.height + 16U;
        output.format = 3;
        output.bytes_per_pixel = 1;
        output.stride = output.width;
        output.pixels.assign(
            static_cast<std::size_t>(output.width) *
                output.height,
            0);
        output.rgba.assign(
            static_cast<std::size_t>(output.width) *
                output.height * 4U,
            0);
        const uint32_t radius = mode;
        for (uint32_t source_y = 0;
             source_y < source_copy.height; ++source_y) {
            for (uint32_t source_x = 0;
                 source_x < source_copy.width; ++source_x) {
                const std::size_t source_offset =
                    static_cast<std::size_t>(source_y) *
                        source_copy.stride +
                    static_cast<std::size_t>(source_x) * 4U;
                const uint32_t alpha =
                    source_copy.pixels[source_offset + 3U];
                if (alpha == 0) {
                    continue;
                }
                for (int32_t dy = -static_cast<int32_t>(radius);
                     dy <= static_cast<int32_t>(radius); ++dy) {
                    for (int32_t dx =
                             -static_cast<int32_t>(radius);
                         dx <= static_cast<int32_t>(radius); ++dx) {
                        const uint32_t destination_x =
                            source_x + 8U +
                            static_cast<uint32_t>(dx);
                        const uint32_t destination_y =
                            source_y + 8U +
                            static_cast<uint32_t>(dy);
                        const std::size_t destination_offset =
                            static_cast<std::size_t>(destination_y) *
                                output.stride +
                            destination_x;
                        output.pixels[destination_offset] =
                            static_cast<uint8_t>(
                                std::min<uint32_t>(
                                    static_cast<uint32_t>(
                                        output.pixels[
                                            destination_offset]) +
                                        alpha,
                                    0xFFU));
                    }
                }
            }
        }
        for (uint32_t source_y = 0;
             source_y < source_copy.height; ++source_y) {
            for (uint32_t source_x = 0;
                 source_x < source_copy.width; ++source_x) {
                const std::size_t source_offset =
                    static_cast<std::size_t>(source_y) *
                        source_copy.stride +
                    static_cast<std::size_t>(source_x) * 4U;
                const uint32_t alpha =
                    source_copy.pixels[source_offset + 3U];
                if (alpha == 0) {
                    continue;
                }
                const std::size_t destination_offset =
                    static_cast<std::size_t>(source_y + 8U) *
                        output.stride +
                    source_x + 8U;
                output.pixels[destination_offset] =
                    static_cast<uint8_t>(
                        (static_cast<uint32_t>(
                             output.pixels[destination_offset]) *
                         (0x100U - alpha)) >>
                        8U);
            }
        }
        for (std::size_t index = 0;
             index < output.pixels.size(); ++index) {
            const uint8_t alpha = output.pixels[index];
            output.rgba[index * 4U] = alpha;
            output.rgba[index * 4U + 1U] = alpha;
            output.rgba[index * 4U + 2U] = alpha;
            output.rgba[index * 4U + 3U] = 0xFFU;
        }
        install_allocated_graphics_surface(destination, std::move(output));
        if (debug_enabled()) bitmap_composite_operations_.push_back({
            BitmapCompositeKind::FilteredExpand,
            destination, source, 0xFFFFFFFFU, mode, 0, 0});
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::vector<BitmapCompositeOperation>&
    bitmap_composite_operations() const {
        return bitmap_composite_operations_;
    }
    struct ExternalImageData {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 2;
        std::vector<uint8_t> rgba;
    };
    using ExternalImageLoadProvider = std::function<
        std::optional<ExternalImageData>(
            std::string_view, uint32_t)>;
    using ExternalImageSaveProvider = std::function<
        bool(std::string_view, uint32_t,
             const GraphicsSurface&)>;
    void set_external_image_load_provider(
        ExternalImageLoadProvider provider) {
        external_image_load_provider_ = std::move(provider);
    }
    void set_external_image_save_provider(
        ExternalImageSaveProvider provider) {
        external_image_save_provider_ = std::move(provider);
    }
    uint32_t load_external_image(
        uint32_t type, std::string_view path, uint32_t format) {
        if (type >= 0x4000U) {
            return 1;
        }
        if (!external_image_load_provider_) {
            return 1;
        }
        std::optional<ExternalImageData> image =
            external_image_load_provider_(path, format);
        if (!image || image->width == 0 || image->height == 0 ||
            image->rgba.size() !=
                static_cast<std::size_t>(image->width) *
                    image->height * 4U) {
            return 1;
        }
        if (image->format > 7U ||
            bitmap_bytes_per_pixel(image->format) == 0) {
            return 2;
        }
        GraphicsSurface surface;
        surface.width = image->width;
        surface.height = image->height;
        surface.format = image->format;
        surface.bytes_per_pixel =
            bitmap_bytes_per_pixel(image->format);
        surface.stride = surface.width * surface.bytes_per_pixel;
        surface.rgba = image->rgba;
        if (surface.format == 2U) {
            surface.pixels.resize(image->rgba.size());
            for (std::size_t index = 0;
                 index < image->rgba.size() / 4U; ++index) {
                surface.pixels[index * 4U] =
                    image->rgba[index * 4U + 2U];
                surface.pixels[index * 4U + 1U] =
                    image->rgba[index * 4U + 1U];
                surface.pixels[index * 4U + 2U] =
                    image->rgba[index * 4U];
                surface.pixels[index * 4U + 3U] =
                    image->rgba[index * 4U + 3U];
            }
        } else {
            surface.pixels.assign(
                static_cast<std::size_t>(surface.stride) *
                    surface.height,
                0);
        }
        install_allocated_graphics_surface(type, std::move(surface));
        display_dirty_ = true;
        return 0;
    }
    uint32_t save_external_image(
        std::string_view path, uint32_t format, uint32_t extra,
        uint32_t bitmap) {
        const auto source = graphics_surfaces_.find(bitmap);
        if (source == graphics_surfaces_.end()) {
            return 4;
        }
        if (format > 4U) {
            return 5;
        }
        if (!external_image_save_provider_ ||
            !external_image_save_provider_(
                path, format, source->second)) {
            return 1;
        }
        (void)extra;
        return 0;
    }
    struct TextProcedureState {
        std::array<uint32_t, 6> wrapper_operands{};
        std::array<int32_t, 13> core_arguments{};
        int32_t type = -1;
        std::string text;
        uint32_t window = 0;
    };
    uint32_t configure_text_display_procedure(
        const std::array<uint32_t, 6>& operands, std::string text);
    [[nodiscard]] const std::vector<TextProcedureState>&
    text_procedure_states() const {
        return text_procedure_states_;
    }
    struct RichTextProcedureState {
        uint8_t family = 0;
        uint8_t subopcode = 0;
        std::vector<uint32_t> wrapper_operands;
        uint32_t window = 0;
        std::string text;
        bool plane_executed = false;
        bool complete = false;
        std::string unsupported_reason;
        PlainTextLayout layout;
        std::vector<TextProcedureNode> nodes;
        uint32_t force_completion = 0; // procedure+30
        uint32_t input_force = 0; // procedure+34
        uint32_t input_key = 2;
        uint32_t enable_high_input = 0;
        uint32_t enable_mask_input = 0;
        uint64_t last_clock_ms = 0;
        uint64_t fixed_clock = 0;
        uint32_t clock_origin = 0;
        bool first_elapsed_poll = true;
        uint32_t post_display_wait = 0; // procedure+38
        bool click_wait_entered = false; // original+64 != 0
        bool pending_line_break = false; // original+60, 00438D50
        uint32_t icon_frame = 0;
        uint32_t wait_due_ms = 0;
        uint32_t auto_deadline_ms = 0;
        bool auto_finished = false; // original+74
        int32_t icon_baseline = 0; // original43E4C0 ruby baseline, +114=0 branch
    };
    uint32_t configure_rich_text_procedure(
        uint8_t family, uint8_t subopcode,
        std::span<const uint32_t> operands, std::string text);
    WaitProcedureResult advance_rich_text_procedure(std::size_t index);
    WaitProcedureResult advance_rich_text_procedure_body(std::size_t index);
    [[nodiscard]] const std::vector<RichTextProcedureState>&
    rich_text_procedure_states() const {
        return rich_text_procedure_states_;
    }
    struct DirectTextDrawState {
        uint8_t family = 0;
        uint8_t subopcode = 0;
        std::vector<uint32_t> operands;
        uint32_t window = 0;
        std::string text;
        bool plane_executed = false;
        std::string unsupported_reason;
    };
    struct WindowGlyphRequest {
        uint32_t window = 0;
        std::string face_name;
        uint32_t logical_height = 0;
        uint32_t width_percent = 0;
        uint32_t bold = 0;
        uint32_t proportional = 0;
        uint32_t font_field6 = 0;
        GlyphFontRequest font;
    };
    GlyphOutlineProvider with_character_bitmaps(GlyphOutlineProvider host, uint32_t color,
        uint32_t logical_height=0, uint32_t width_percent=100,
        uint32_t padding_x=0, uint32_t padding_y=0);
    using WindowGlyphProviderFactory = std::function<
        std::optional<GlyphOutlineProvider>(const WindowGlyphRequest&)>;
    void set_window_glyph_provider_factory(WindowGlyphProviderFactory factory) {
        window_glyph_provider_factory_ = std::move(factory);
    }
    uint32_t draw_direct_text(
        uint8_t family, uint8_t subopcode,
        std::span<const uint32_t> operands, std::string text);
    [[nodiscard]] const std::vector<DirectTextDrawState>&
    direct_text_draw_states() const {
        return direct_text_draw_states_;
    }
    struct TextMeasureState {
        std::vector<uint32_t> operands;
        uint32_t handle = 0;
        uint32_t font_id = 0;
        std::string text;
        uint32_t mode = 0;
    };
    uint32_t create_text_measure(uint32_t output,
        std::span<const uint32_t> operands, std::string text);
    [[nodiscard]] const std::vector<TextMeasureState>&
    text_measure_states() const {
        return text_measure_states_;
    }
    struct RichTextObjectState {
        uint8_t family = 0;
        uint8_t subopcode = 0;
        std::vector<uint32_t> operands;
        uint32_t handle = 0;
        uint32_t target = 0;
        std::string text;
    };
    uint32_t create_rich_text_object(
        uint8_t family, uint8_t subopcode,
        std::span<const uint32_t> operands, std::string text, std::string auxiliary = {});
    bool apply_original_text_ruby(PlainTextLayout& layout, const WindowGlyphRequest& main_font,
        uint32_t color, uint32_t style, uint32_t percent_x, uint32_t percent_y,
        uint32_t effect_color, uint32_t effect_parameter);
    bool apply_original_text_style(PlainTextLayout& layout, uint32_t style,
        int32_t style_x, int32_t style_y, uint32_t effect_color,
        uint32_t effect_parameter, std::span<const GlyphMask> style_sources = {},
        uint32_t private_cache_width=UINT32_MAX, uint32_t private_cache_height=UINT32_MAX,
        bool proportional=true);
    [[nodiscard]] const std::vector<RichTextObjectState>&
    rich_text_object_states() const {
        return rich_text_object_states_;
    }
    struct ImageEffectOperation {
        uint8_t subopcode = 0;
        std::vector<uint32_t> operands;
    };
    uint32_t record_image_effect(
        uint8_t subopcode, std::span<const uint32_t> operands) {
        if (operands.empty()) {
            return 1;
        }
        const auto target = graphics_surfaces_.find(operands[0]);
        if (target == graphics_surfaces_.end()) {
            return subopcode == 0x12U || subopcode == 0x14U ||
                           subopcode == 0x15U || subopcode == 0x16U ||
                           subopcode == 0x17U
                       ? 0x80000001U
                       : 1U;
        }
        const bool vector_required =
            subopcode >= 0x12U && subopcode <= 0x17U;
        if (vector_required && target->second.format != 4U) {
            return subopcode == 0x12U ? 0x80000003U : 3U;
        }
        if (subopcode == 0x12U && operands[1] == 0) {
            return 0x80000006U;
        }
        if ((subopcode == 0x14U || subopcode == 0x15U) &&
            (operands[3] == 0 || operands[4] == 0)) {
            return 0x80000008U;
        }
        if (subopcode == 0x18U || subopcode == 0x19U) {
            if (graphics_surfaces_.count(operands[3]) == 0) {
                return 2;
            }
        } else if (subopcode == 0x1AU || subopcode == 0x1BU ||
                   subopcode == 0x1DU || subopcode == 0x1EU) {
            if (graphics_surfaces_.count(operands[1]) == 0) {
                return 2;
            }
        }
        if (subopcode == 0x1DU && operands[2] > 5U) {
            return 6;
        }
        if (subopcode == 0x1EU &&
            target->second.format !=
                graphics_surfaces_.at(operands[1]).format) {
            return 3;
        }
        image_effect_operations_.push_back({
            subopcode,
            std::vector<uint32_t>(operands.begin(), operands.end())});
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::vector<ImageEffectOperation>&
    image_effect_operations() const {
        return image_effect_operations_;
    }
    uint32_t apply_tone_curve_effect(
        std::span<const uint32_t> operands) {
        if (operands.size() != 8) {
            return 0xFFFFFFFFU;
        }
        const auto destination =
            graphics_surfaces_.find(operands[0]);
        if (destination == graphics_surfaces_.end()) {
            return 0x80000009U;
        }
        const auto source = graphics_surfaces_.find(operands[1]);
        if (source == graphics_surfaces_.end()) {
            return 0x8000000AU;
        }
        if (tone_curves_.count(operands[4]) == 0) {
            return 0x8000000FU;
        }
        if (destination->second.format != source->second.format ||
            (source->second.format != 1U &&
             source->second.format != 2U)) {
            return 0x80000011U;
        }
        if (operands[3] > 0x100U) {
            return 0x80000014U;
        }
        if (operands[6] != 8U && operands[6] != 0x26U) {
            return 0x80000013U;
        }
        if (operands[7] > 0x100U) {
            return 0x80000015U;
        }
        image_effect_operations_.push_back({
            0xCDU,
            std::vector<uint32_t>(operands.begin(), operands.end())});
        display_dirty_ = true;
        return 0;
    }
    struct TextListProcessorState {
        uint32_t variant = 0;
        std::vector<uint32_t> operands;
        std::vector<std::string> entries;
        uint32_t mode = 0;
        uint32_t initial_index = 0;
    };
    uint32_t configure_text_list_processor(
        uint32_t variant, std::span<const uint32_t> operands,
        uint32_t base) {
        const std::size_t expected = variant == 0 ? 8U : 6U;
        if (operands.size() != expected ||
            window_objects_.count(operands[0]) == 0) {
            return 0xFFFFFFFFU;
        }
        const uint32_t count = operands[1];
        if (count == 0 || count > 16U) {
            return 0x80000001U;
        }
        const uint32_t mode = operands[3];
        if (mode > 0x10U) {
            return 0x80000003U;
        }
        const uint32_t initial =
            variant == 0U ? operands[6] : 0U;
        if (initial >= count) {
            return 0x80000002U;
        }
        TextListProcessorState state;
        state.variant = variant;
        state.operands.assign(operands.begin(), operands.end());
        state.mode = mode;
        state.initial_index = initial;
        const uint32_t table = operands[2];
        for (uint32_t index = 0; index < count; ++index) {
            const uint32_t address = memory_.read_u32(
                table + index * 4U, base);
            state.entries.push_back(
                memory_.read_cstring(address, base));
        }
        text_list_processors_.push_back(std::move(state));
        return 0;
    }
    [[nodiscard]] const std::vector<TextListProcessorState>&
    text_list_processors() const {
        return text_list_processors_;
    }
    uint32_t configure_enhanced_text_list_processor(
        uint32_t variant, std::span<const uint32_t> operands,
        uint32_t base) {
        if (variant > 1U || operands.size() != 14U ||
            window_objects_.count(operands[0]) == 0) {
            return 0xFFFFFFFFU;
        }
        const uint32_t count = operands[1];
        if (count == 0 || count > 16U) {
            return 0x80000001U;
        }
        const uint32_t mode = operands[3];
        if (mode > 0x10U) {
            return 0x80000003U;
        }
        const uint32_t initial = operands[6];
        if (initial >= count) {
            return 0x80000002U;
        }
        TextListProcessorState state;
        state.variant = 2U + variant;
        state.operands.assign(operands.begin(), operands.end());
        state.mode = mode;
        state.initial_index = initial;
        const uint32_t table = operands[2];
        for (uint32_t index = 0; index < count; ++index) {
            const uint32_t address = memory_.read_u32(
                table + index * 4U, base);
            state.entries.push_back(
                memory_.read_cstring(address, base));
        }
        text_list_processors_.push_back(std::move(state));
        return 0;
    }
    struct TextDrawRequest {
        uint32_t target = 0;
        int32_t x = 0;
        int32_t y = 0;
        std::string text;
        uint32_t font_address = 0;
        std::array<uint32_t, 6> parameters{};
        uint8_t parameter_count = 0;
        bool wrapped = false;
    };
    using TextDrawProvider = std::function<
        std::optional<uint32_t>(const TextDrawRequest&)>;
    void set_text_draw_provider(TextDrawProvider provider) {
        text_draw_provider_ = std::move(provider);
    }
    std::optional<uint32_t> record_text_draw(
        uint32_t target, int32_t x, int32_t y,
        uint32_t text_address, uint32_t font_address,
        std::span<const uint32_t> parameters, bool wrapped,
        uint32_t base) {
        if (graphics_surfaces_.count(target) == 0 ||
            !text_draw_provider_ || parameters.size() > 6U) {
            return std::nullopt;
        }
        TextDrawRequest request;
        request.target = target;
        request.x = x;
        request.y = y;
        request.text = memory_.read_cstring(text_address, base);
        request.font_address = font_address;
        request.parameter_count =
            static_cast<uint8_t>(parameters.size());
        std::copy(
            parameters.begin(), parameters.end(),
            request.parameters.begin());
        request.wrapped = wrapped;
        const std::optional<uint32_t> width =
            text_draw_provider_(request);
        if (width) {
            text_draw_operations_.push_back(std::move(request));
            display_dirty_ = true;
        }
        return width;
    }
    [[nodiscard]] const std::vector<TextDrawRequest>&
    text_draw_operations() const {
        return text_draw_operations_;
    }
    struct TextLayoutState {
        std::string font;
        uint32_t parameter_2 = 0;
        uint32_t parameter_3 = 0;
        uint32_t parameter_4 = 0;
        uint32_t parameter_5 = 0;
    };
    using TextStyleProvider = std::function<uint32_t(
        std::string_view, uint32_t, uint32_t, uint32_t, uint32_t)>;
    void set_text_style_provider(TextStyleProvider provider) {
        text_style_provider_ = std::move(provider);
    }
    uint32_t configure_text_layout(
        std::string_view font, uint32_t parameter_2,
        uint32_t parameter_3, uint32_t parameter_4,
        uint32_t parameter_5) {
        uint32_t status = 0;
        if (text_style_provider_) {
            status = text_style_provider_(
                font, parameter_2, parameter_3,
                parameter_4, parameter_5);
        }
        if (status == 4U) {
            return 1;
        }
        if (status == 5U) {
            return 2;
        }
        if (status == 6U) {
            return 3;
        }
        if (status != 0) {
            return 0xFFFFFFFFU;
        }
        text_layout_ = {
            std::string(font), parameter_2, parameter_3,
            parameter_4, parameter_5};
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const TextLayoutState& text_layout() const {
        return text_layout_;
    }
    struct GlyphPosition {
        std::array<uint32_t, 3> words{};
    };
    using GlyphLayoutProvider = std::function<
        std::optional<std::vector<GlyphPosition>>(
            std::string_view, uint32_t, uint32_t,
            uint32_t, uint32_t)>;
    void set_glyph_layout_provider(
        GlyphLayoutProvider provider) {
        glyph_layout_provider_ = std::move(provider);
    }
    uint32_t query_glyph_positions(
        uint32_t output, uint32_t count_output,
        uint32_t text_address, uint32_t p4, uint32_t p5,
        uint32_t p6, uint32_t p7, uint32_t base) {
        if (!glyph_layout_provider_) {
            return 0xFFFFFFFFU;
        }
        const std::string text =
            memory_.read_cstring(text_address, base);
        std::optional<std::vector<GlyphPosition>> positions =
            glyph_layout_provider_(text, p4, p5, p6, p7);
        if (!positions) {
            return 0xFFFFFFFFU;
        }
        if (output != 0) {
            for (std::size_t index = 0;
                 index < positions->size(); ++index) {
                for (std::size_t word = 0; word < 3U; ++word) {
                    memory_.write_u32(
                        output + static_cast<uint32_t>(
                                     (index * 3U + word) * 4U),
                        base, positions->at(index).words[word]);
                }
            }
        }
        if (count_output != 0) {
            memory_.write_u32(
                count_output, base,
                static_cast<uint32_t>(positions->size()));
        }
        glyph_positions_ = std::move(*positions);
        return 0;
    }
    [[nodiscard]] const std::vector<GlyphPosition>&
    glyph_positions() const {
        return glyph_positions_;
    }
    struct DirectBitmapDraw {
        uint32_t bitmap = 0;
        int32_t x = 0;
        int32_t y = 0;
    };
    uint32_t record_direct_bitmap_draw(uint32_t bitmap, int32_t x,
                                       int32_t y) {
        if (graphics_surfaces_.count(bitmap) == 0) {
            return 2;
        }
        direct_bitmap_draws_.push_back({bitmap, x, y});
        return 0;
    }
    [[nodiscard]] const std::vector<DirectBitmapDraw>&
    direct_bitmap_draws() const {
        return direct_bitmap_draws_;
    }
    struct ScreenShakeState {
        bool active = false;
        bool wait_for_tick = false;
        uint32_t mode = 0;
        int32_t amplitude = 0;
        int32_t frames_per_stage = 0;
        int32_t stages = 0;
        int32_t decay_percent = 0;
        int32_t interval_frames = 0;
        int32_t stage = 0;
        int32_t frame = 0;
        int32_t random_x = 0;
        int32_t random_y = 0;
        int32_t delta_x = 0;
        int32_t delta_y = 0;
        int32_t oscillation = 0;
        int32_t velocity = 0;
        int32_t offset_x = 0;
        int32_t offset_y = 0;
        uint32_t wait_token = 0;
        uint64_t next_update_ms = 0;
    };
    uint32_t start_screen_shake(const std::array<int32_t, 7>& parameters,
                                uint64_t now_ms) {
        const int32_t mode = parameters[0];
        const int32_t amplitude =
            static_cast<int32_t>(static_cast<uint32_t>(parameters[1])
                                 << 11U);
        const int32_t frames_per_stage = parameters[2];
        const int32_t stages = parameters[3];
        const int32_t decay_percent = parameters[4];
        const int32_t interval_frames = parameters[5];
        if (mode > 3) {
            return 1;
        }
        if (frames_per_stage < 1) {
            return 2;
        }
        if (stages < 1) {
            return 3;
        }
        if (interval_frames <= 0 ||
            frames_per_stage > interval_frames) {
            return 4;
        }
        screen_shake_ = {};
        screen_shake_.active = true;
        screen_shake_.wait_for_tick = parameters[6] != 0;
        screen_shake_.wait_token = sys0_counter_542e90_;
        screen_shake_.mode = static_cast<uint32_t>(mode);
        screen_shake_.amplitude = amplitude;
        screen_shake_.frames_per_stage = frames_per_stage;
        screen_shake_.stages = stages;
        screen_shake_.decay_percent = decay_percent;
        screen_shake_.interval_frames = interval_frames;
        screen_shake_.next_update_ms =
            now_ms +
            static_cast<uint64_t>(1000 / interval_frames);
        return 0;
    }
    void install_screen_shake_wait(Thread& thread) {
        thread.snapshot_wait_kind = SnapshotWaitKind::ScreenShake;
        thread.snapshot_wait_index = 0;
        thread.wait_procedure = [this]() {
            advance_screen_shake(monotonic_ms());
            return WaitProcedureResult{
                screen_shake_.active ? 0 : 1, {}};
        };
        thread.status_flags |= 1U;
        thread.state = ThreadState::Waiting;
    }
    void advance_screen_shake(uint64_t now_ms) {
        if (!screen_shake_.active) {
            return;
        }
        if (screen_shake_.wait_for_tick &&
            screen_shake_.wait_token != sys0_counter_542e90_) {
            screen_shake_.active = false;
            screen_shake_.wait_for_tick = false;
            screen_shake_.offset_x = 0;
            screen_shake_.offset_y = 0;
            return;
        }
        if (now_ms < screen_shake_.next_update_ms) {
            return;
        }
        screen_shake_.next_update_ms =
            now_ms +
            static_cast<uint64_t>(1000 / screen_shake_.interval_frames);
        if (screen_shake_.frame == 0) {
            screen_shake_.oscillation = 0;
            screen_shake_.velocity =
                (screen_shake_.amplitude << 2) /
                screen_shake_.frames_per_stage;
            if (screen_shake_.mode == 3) {
                const int32_t x =
                    screen_shake_random(screen_shake_.amplitude);
                const int32_t y =
                    screen_shake_random(screen_shake_.amplitude);
                const uint32_t direction =
                    static_cast<uint32_t>(screen_shake_.stage) & 3U;
                const int32_t previous_x = screen_shake_.random_x;
                const int32_t previous_y = screen_shake_.random_y;
                screen_shake_.random_x =
                    (direction == 0U || direction == 1U) ? x : -x;
                screen_shake_.random_y =
                    (direction == 0U || direction == 3U) ? -y : y;
                screen_shake_.delta_x =
                    screen_shake_.random_x - previous_x;
                screen_shake_.delta_y =
                    screen_shake_.random_y - previous_y;
            }
        }
        int32_t oscillation =
            screen_shake_.oscillation + screen_shake_.velocity;
        const int32_t amplitude = screen_shake_.amplitude;
        if (oscillation > amplitude || oscillation < -amplitude) {
            screen_shake_.velocity = -screen_shake_.velocity;
        }
        if (oscillation > amplitude) {
            oscillation = amplitude * 2 - oscillation;
        }
        if (oscillation < -amplitude) {
            oscillation = amplitude * -2 - oscillation;
        }
        screen_shake_.oscillation = oscillation;

        int32_t x = 0;
        int32_t y = 0;
        switch (screen_shake_.mode) {
        case 0:
            x = oscillation;
            break;
        case 1:
            y = oscillation;
            break;
        case 2:
            x = oscillation;
            y = oscillation;
            break;
        case 3:
            x = screen_shake_.delta_x * screen_shake_.frame /
                    screen_shake_.frames_per_stage +
                screen_shake_.offset_x;
            y = screen_shake_.delta_y * screen_shake_.frame /
                    screen_shake_.frames_per_stage +
                screen_shake_.offset_y;
            break;
        default:
            break;
        }
        screen_shake_.offset_x = x >> 12;
        screen_shake_.offset_y = y >> 12;
        ++screen_shake_.frame;
        if (screen_shake_.frame >= screen_shake_.frames_per_stage) {
            screen_shake_.frame = 0;
            ++screen_shake_.stage;
            screen_shake_.amplitude =
                ((100 - screen_shake_.decay_percent) *
                 screen_shake_.amplitude) /
                100;
            screen_shake_.active =
                screen_shake_.stage < screen_shake_.stages;
        }
        if (!screen_shake_.active) {
            screen_shake_.offset_x = 0;
            screen_shake_.offset_y = 0;
        }
    }
    [[nodiscard]] const ScreenShakeState& screen_shake() const {
        return screen_shake_;
    }
    [[nodiscard]] int32_t screen_shake_offset_x() const {
        return screen_shake_.active ? screen_shake_.offset_x : 0;
    }
    [[nodiscard]] int32_t screen_shake_offset_y() const {
        return screen_shake_.active ? screen_shake_.offset_y : 0;
    }
    struct BitmapScale {
        uint32_t destination = 0;
        uint32_t source = 0;
        uint32_t scale_x = 0;
        uint32_t scale_y = 0;
        uint32_t interpolation = 0;
    };
    uint32_t scale_bitmap(uint32_t destination, uint32_t source,
                          uint32_t scale_x, uint32_t scale_y,
                          uint32_t interpolation) {
        auto src = graphics_surfaces_.find(source);
        if (src == graphics_surfaces_.end()) {
            return 2;
        }
        if (src->second.format != 1 && src->second.format != 2) {
            return 3;
        }
        const uint32_t width =
            static_cast<uint32_t>(
                (static_cast<uint64_t>(src->second.width) * scale_x +
                 0x8000U) >>
                16U);
        const uint32_t height =
            static_cast<uint32_t>(
                (static_cast<uint64_t>(src->second.height) * scale_y +
                 0x8000U) >>
                16U);
        if (width == 0 || height == 0) {
            return 4;
        }
        create_graphics_surface(destination, height, width,
                                src->second.format);
        GraphicsSurface& output = graphics_surfaces_.at(destination);
        if (interpolation == 0) {
            if (!materialize_graphics_pixels(src->second) ||
                !materialize_graphics_pixels(output)) {
                return 3;
            }
            if (output.rgba.size() !=
                static_cast<std::size_t>(output.width) *
                    output.height * 4U) {
                output.rgba.assign(
                    static_cast<std::size_t>(output.width) *
                        output.height * 4U,
                    0);
            }
            const int32_t signed_scale_x =
                static_cast<int32_t>(scale_x);
            const int32_t signed_scale_y =
                static_cast<int32_t>(scale_y);
            if (signed_scale_x == 0 || signed_scale_y == 0) {
                return 4;
            }
            const int32_t centered_x =
                static_cast<int32_t>(output.width >> 1U) -
                static_cast<int32_t>(width >> 1U);
            const int32_t centered_y =
                static_cast<int32_t>(output.height >> 1U) -
                static_cast<int32_t>(height >> 1U);
            const int32_t start_x = std::max(centered_x, 0);
            const int32_t start_y = std::max(centered_y, 0);
            const int32_t end_x = std::min<int32_t>(
                static_cast<int32_t>(output.width),
                centered_x + static_cast<int32_t>(width));
            const int32_t end_y = std::min<int32_t>(
                static_cast<int32_t>(output.height),
                centered_y + static_cast<int32_t>(height));
            const uint32_t step_x =
                (src->second.width << 16U) / width;
            const uint32_t step_y =
                (src->second.height << 16U) / height;
            const auto initial_fixed = [](int32_t clipped,
                                          int32_t centered,
                                          int32_t scale,
                                          uint32_t source_size) {
                const double value =
                    (static_cast<double>(clipped - centered) /
                     static_cast<double>(scale)) *
                    static_cast<double>(
                        static_cast<uint64_t>(source_size) << 16U);
                return static_cast<uint32_t>(
                    static_cast<int32_t>(std::floor(value)));
            };
            uint32_t source_x_fixed =
                initial_fixed(start_x, centered_x,
                              signed_scale_x, src->second.width);
            uint32_t source_y_fixed =
                initial_fixed(start_y, centered_y,
                              signed_scale_y, src->second.height);
            for (int32_t target_y = start_y; target_y < end_y;
                 ++target_y) {
                const uint32_t source_y = source_y_fixed >> 16U;
                uint32_t x_fixed = source_x_fixed;
                for (int32_t target_x = start_x; target_x < end_x;
                     ++target_x) {
                    const uint32_t source_x = x_fixed >> 16U;
                    if (source_x < src->second.width &&
                        source_y < src->second.height) {
                        const std::size_t source_offset =
                            static_cast<std::size_t>(source_y) *
                                src->second.stride +
                            static_cast<std::size_t>(source_x) * 4U;
                        const std::size_t destination_offset =
                            static_cast<std::size_t>(target_y) *
                                output.stride +
                            static_cast<std::size_t>(target_x) * 4U;
                        for (uint32_t channel = 0; channel < 4U;
                             ++channel) {
                            output.pixels[
                                destination_offset + channel] =
                                src->second.pixels[
                                    source_offset + channel];
                        }
                        output.rgba[destination_offset] =
                            output.pixels[
                                destination_offset + 2U];
                        output.rgba[destination_offset + 1U] =
                            output.pixels[
                                destination_offset + 1U];
                        output.rgba[destination_offset + 2U] =
                            output.pixels[destination_offset];
                        output.rgba[destination_offset + 3U] =
                            output.format == 1U
                                ? 0xFFU
                                : output.pixels[
                                      destination_offset + 3U];
                    }
                    x_fixed += step_x;
                }
                source_y_fixed += step_y;
            }
        } else if (scale_x >= 0x10000U &&
                   scale_y >= 0x10000U) {
            if (!materialize_graphics_pixels(src->second) ||
                !materialize_graphics_pixels(output)) {
                return 3;
            }
            if (output.rgba.size() !=
                static_cast<std::size_t>(output.width) *
                    output.height * 4U) {
                output.rgba.assign(
                    static_cast<std::size_t>(output.width) *
                        output.height * 4U,
                    0);
            }
            const uint32_t step_x =
                static_cast<uint32_t>(
                    (static_cast<uint64_t>(src->second.width - 1U)
                     << 16U) /
                    width);
            const uint32_t step_y =
                static_cast<uint32_t>(
                    (static_cast<uint64_t>(src->second.height - 1U)
                     << 16U) /
                    height);
            const auto source_byte = [&src](uint32_t x, uint32_t y,
                                            uint32_t channel) {
                if (x >= src->second.width ||
                    y >= src->second.height) {
                    return uint8_t{0};
                }
                return src->second.pixels[
                    static_cast<std::size_t>(y) *
                        src->second.stride +
                    static_cast<std::size_t>(x) * 4U + channel];
            };
            uint32_t source_y_fixed = 0;
            for (uint32_t target_y = 0; target_y < output.height;
                 ++target_y) {
                const uint32_t source_y =
                    source_y_fixed >> 16U;
                const uint32_t weight_y =
                    (source_y_fixed >> 8U) & 0xFFU;
                uint32_t source_x_fixed = 0;
                for (uint32_t target_x = 0;
                     target_x < output.width; ++target_x) {
                    const uint32_t source_x =
                        source_x_fixed >> 16U;
                    const uint32_t weight_x =
                        (source_x_fixed >> 8U) & 0xFFU;
                    const std::size_t destination_offset =
                        static_cast<std::size_t>(target_y) *
                            output.stride +
                        static_cast<std::size_t>(target_x) * 4U;
                    for (uint32_t channel = 0; channel < 4U;
                         ++channel) {
                        const uint32_t top =
                            static_cast<uint32_t>(
                                source_byte(
                                    source_x, source_y, channel)) *
                                (0x100U - weight_x) +
                            static_cast<uint32_t>(
                                source_byte(
                                    source_x + 1U, source_y,
                                    channel)) *
                                weight_x;
                        const uint32_t bottom =
                            static_cast<uint32_t>(
                                source_byte(
                                    source_x, source_y + 1U,
                                    channel)) *
                                (0x100U - weight_x) +
                            static_cast<uint32_t>(
                                source_byte(
                                    source_x + 1U,
                                    source_y + 1U,
                                    channel)) *
                                weight_x;
                        output.pixels[
                            destination_offset + channel] =
                            static_cast<uint8_t>(
                                (top * (0x100U - weight_y) +
                                 bottom * weight_y) >>
                                16U);
                    }
                    output.rgba[destination_offset] =
                        output.pixels[destination_offset + 2U];
                    output.rgba[destination_offset + 1U] =
                        output.pixels[destination_offset + 1U];
                    output.rgba[destination_offset + 2U] =
                        output.pixels[destination_offset];
                    output.rgba[destination_offset + 3U] =
                        output.format == 1U
                            ? 0xFFU
                            : output.pixels[
                                  destination_offset + 3U];
                    source_x_fixed += step_x;
                }
                source_y_fixed += step_y;
            }
        } else if ((scale_x >= 0x8000U &&
                    scale_x < 0x10000U) ||
                   (scale_y >= 0x8000U &&
                    scale_y < 0x10000U)) {
            if (!materialize_graphics_pixels(src->second) ||
                !materialize_graphics_pixels(output)) {
                return 3;
            }
            if (output.rgba.size() !=
                static_cast<std::size_t>(output.width) *
                    output.height * 4U) {
                output.rgba.assign(
                    static_cast<std::size_t>(output.width) *
                        output.height * 4U,
                    0);
            }
            const int32_t signed_scale_x =
                static_cast<int32_t>(scale_x);
            const int32_t signed_scale_y =
                static_cast<int32_t>(scale_y);
            if (signed_scale_x == 0 || signed_scale_y == 0 ||
                (scale_x >> 8U) == 0 || (scale_y >> 8U) == 0) {
                throw BpFault(
                    "bitmap scale middle box divisor is zero");
            }
            const uint32_t box_width_fixed =
                0x10000U / (scale_x >> 8U);
            const uint32_t box_height_fixed =
                0x10000U / (scale_y >> 8U);
            const int32_t centered_x =
                static_cast<int32_t>(output.width >> 1U) -
                static_cast<int32_t>(width >> 1U);
            const int32_t centered_y =
                static_cast<int32_t>(output.height >> 1U) -
                static_cast<int32_t>(height >> 1U);
            const int32_t start_x = std::max(centered_x, 0);
            const int32_t start_y = std::max(centered_y, 0);
            const int32_t end_x = std::min<int32_t>(
                static_cast<int32_t>(output.width),
                centered_x + static_cast<int32_t>(width));
            const int32_t end_y = std::min<int32_t>(
                static_cast<int32_t>(output.height),
                centered_y + static_cast<int32_t>(height));
            const uint32_t step_x =
                static_cast<uint32_t>(
                    static_cast<uint32_t>(
                        src->second.width -
                        (box_width_fixed >> 8U))
                    << 16U) /
                width;
            const uint32_t step_y =
                static_cast<uint32_t>(
                    static_cast<uint32_t>(
                        src->second.height -
                        (box_height_fixed >> 8U))
                    << 16U) /
                height;
            const auto initial_fixed =
                [&](int32_t clipped, int32_t centered,
                    int32_t scale, uint32_t source_size,
                    uint32_t box_fixed) {
                    const double value =
                        (static_cast<double>(clipped - centered) /
                         static_cast<double>(scale)) *
                        static_cast<double>(
                            static_cast<uint64_t>(source_size)
                            << 16U);
                    return static_cast<uint32_t>(
                        static_cast<int32_t>(
                            std::floor(value))) +
                        (box_fixed << 7U);
                };
            const auto boxed_sample =
                [&](uint32_t x_fixed, uint32_t y_fixed)
                    -> std::array<uint8_t, 4> {
                int64_t left =
                    static_cast<int64_t>(x_fixed >> 8U);
                int64_t top =
                    static_cast<int64_t>(y_fixed >> 8U);
                int64_t right = left + box_width_fixed;
                int64_t bottom = top + box_height_fixed;
                left = std::max<int64_t>(left, 0);
                top = std::max<int64_t>(top, 0);
                right = std::min<int64_t>(
                    right,
                    static_cast<int64_t>(
                        src->second.width << 8U));
                bottom = std::min<int64_t>(
                    bottom,
                    static_cast<int64_t>(
                        src->second.height << 8U));
                if (left >= right || top >= bottom) {
                    throw BpFault(
                        "bitmap scale middle box has no source "
                        "coverage");
                }
                const uint64_t total_area =
                    (static_cast<uint64_t>(bottom - top) *
                     static_cast<uint64_t>(right - left)) >>
                    8U;
                if (total_area == 0) {
                    throw BpFault(
                        "bitmap scale middle box area is zero");
                }
                std::array<uint32_t, 4> sums{};
                int64_t y = top;
                while (y < bottom) {
                    const int64_t y_next = std::min<int64_t>(
                        (y + 0x100) & ~int64_t{0xFF}, bottom);
                    int64_t x = left;
                    while (x < right) {
                        const int64_t x_next = std::min<int64_t>(
                            (x + 0x100) & ~int64_t{0xFF}, right);
                        const uint64_t fragment_area =
                            static_cast<uint64_t>(x_next - x) *
                            static_cast<uint64_t>(y_next - y);
                        const uint32_t weight =
                            static_cast<uint32_t>(
                                ((fragment_area * 0x100U) /
                                 total_area) >>
                                8U);
                        const uint32_t source_x =
                            static_cast<uint32_t>(x >> 8U);
                        const uint32_t source_y =
                            static_cast<uint32_t>(y >> 8U);
                        const std::size_t offset =
                            static_cast<std::size_t>(source_y) *
                                src->second.stride +
                            static_cast<std::size_t>(source_x) * 4U;
                        for (uint32_t channel = 0; channel < 4U;
                             ++channel) {
                            sums[channel] +=
                                static_cast<uint32_t>(
                                    src->second.pixels[
                                        offset + channel]) *
                                weight;
                        }
                        x = x_next;
                    }
                    y = y_next;
                }
                std::array<uint8_t, 4> result{};
                for (uint32_t channel = 0; channel < 4U;
                     ++channel) {
                    result[channel] = static_cast<uint8_t>(
                        std::min<uint32_t>(sums[channel] >> 8U,
                                           0xFFU));
                }
                return result;
            };
            uint32_t source_y_fixed =
                initial_fixed(start_y, centered_y,
                              signed_scale_y, src->second.height,
                              box_height_fixed);
            for (int32_t target_y = start_y; target_y < end_y;
                 ++target_y) {
                uint32_t source_x_fixed =
                    initial_fixed(start_x, centered_x,
                                  signed_scale_x,
                                  src->second.width,
                                  box_width_fixed);
                for (int32_t target_x = start_x; target_x < end_x;
                     ++target_x) {
                    const auto pixel =
                        boxed_sample(source_x_fixed,
                                     source_y_fixed);
                    const std::size_t destination_offset =
                        static_cast<std::size_t>(target_y) *
                            output.stride +
                        static_cast<std::size_t>(target_x) * 4U;
                    for (uint32_t channel = 0; channel < 4U;
                         ++channel) {
                        output.pixels[
                            destination_offset + channel] =
                            pixel[channel];
                    }
                    output.rgba[destination_offset] =
                        pixel[2];
                    output.rgba[destination_offset + 1U] =
                        pixel[1];
                    output.rgba[destination_offset + 2U] =
                        pixel[0];
                    output.rgba[destination_offset + 3U] =
                        output.format == 1U ? 0xFFU : pixel[3];
                    source_x_fixed += step_x;
                }
                source_y_fixed += step_y;
            }
        } else if (scale_x < 0x8000U || scale_y < 0x8000U) {
            if (!materialize_graphics_pixels(src->second) ||
                !materialize_graphics_pixels(output)) {
                return 3;
            }
            if (output.rgba.size() !=
                static_cast<std::size_t>(output.width) *
                    output.height * 4U) {
                output.rgba.assign(
                    static_cast<std::size_t>(output.width) *
                        output.height * 4U,
                    0);
            }
            const int32_t signed_scale_x =
                static_cast<int32_t>(scale_x);
            const int32_t signed_scale_y =
                static_cast<int32_t>(scale_y);
            if (signed_scale_x == 0 || signed_scale_y == 0 ||
                (scale_x >> 8U) == 0 || (scale_y >> 8U) == 0) {
                throw BpFault("bitmap scale box divisor is zero");
            }
            const uint32_t box_width_fixed =
                0x10000U / (scale_x >> 8U);
            const uint32_t box_height_fixed =
                0x10000U / (scale_y >> 8U);
            const uint32_t box_width =
                (box_width_fixed + 0xFFU) >> 8U;
            const uint32_t box_height =
                (box_height_fixed + 0xFFU) >> 8U;
            const int32_t centered_x =
                static_cast<int32_t>(output.width >> 1U) -
                static_cast<int32_t>(width >> 1U);
            const int32_t centered_y =
                static_cast<int32_t>(output.height >> 1U) -
                static_cast<int32_t>(height >> 1U);
            const int32_t start_x = std::max(centered_x, 0);
            const int32_t start_y = std::max(centered_y, 0);
            const int32_t end_x = std::min<int32_t>(
                static_cast<int32_t>(output.width),
                centered_x + static_cast<int32_t>(width));
            const int32_t end_y = std::min<int32_t>(
                static_cast<int32_t>(output.height),
                centered_y + static_cast<int32_t>(height));
            const uint32_t step_x =
                static_cast<uint32_t>(
                    static_cast<uint32_t>(
                        src->second.width -
                        (box_width_fixed >> 8U))
                    << 16U) /
                width;
            const uint32_t step_y =
                static_cast<uint32_t>(
                    static_cast<uint32_t>(
                        src->second.height -
                        (box_height_fixed >> 8U))
                    << 16U) /
                height;
            const auto boxed_sample =
                [&](uint32_t x_fixed, uint32_t y_fixed)
                    -> std::array<uint8_t, 4> {
                const int64_t left =
                    static_cast<int64_t>(x_fixed >> 16U);
                const int64_t top =
                    static_cast<int64_t>(y_fixed >> 16U);
                const int64_t right = std::min<int64_t>(
                    left + box_width,
                    src->second.width);
                const int64_t bottom = std::min<int64_t>(
                    top + box_height,
                    src->second.height);
                if (left >= right || top >= bottom) {
                    return {};
                }
                std::array<uint32_t, 4> sums{};
                uint32_t included = 0;
                for (int64_t y = top; y < bottom; ++y) {
                    for (int64_t x = left; x < right; ++x) {
                        const std::size_t offset =
                            static_cast<std::size_t>(y) *
                                src->second.stride +
                            static_cast<std::size_t>(x) * 4U;
                        const uint8_t alpha =
                            src->second.pixels[offset + 3U];
                        if (src->second.format == 2U &&
                            alpha == 0U) {
                            continue;
                        }
                        sums[0] += src->second.pixels[offset];
                        sums[1] +=
                            src->second.pixels[offset + 1U];
                        sums[2] +=
                            src->second.pixels[offset + 2U];
                        sums[3] += alpha;
                        ++included;
                    }
                }
                const uint32_t area =
                    static_cast<uint32_t>(right - left) *
                    static_cast<uint32_t>(bottom - top);
                const uint32_t rgb_divisor =
                    std::max(included, 1U);
                return {
                    static_cast<uint8_t>(
                        sums[0] / rgb_divisor),
                    static_cast<uint8_t>(
                        sums[1] / rgb_divisor),
                    static_cast<uint8_t>(
                        sums[2] / rgb_divisor),
                    static_cast<uint8_t>(
                        sums[3] / std::max(area, 1U))};
            };
            const auto initial_fixed =
                [&](int32_t clipped, int32_t centered,
                    int32_t scale, uint32_t source_size,
                    uint32_t box_fixed) {
                    const double value =
                        (static_cast<double>(clipped - centered) /
                         static_cast<double>(scale)) *
                        static_cast<double>(
                            static_cast<uint64_t>(source_size)
                            << 16U);
                    return static_cast<uint32_t>(
                        static_cast<int32_t>(std::floor(value))) +
                        (box_fixed << 7U);
                };
            uint32_t source_y_fixed =
                initial_fixed(start_y, centered_y,
                              signed_scale_y, src->second.height,
                              box_height_fixed);
            for (int32_t target_y = start_y; target_y < end_y;
                 ++target_y) {
                uint32_t source_x_fixed =
                    initial_fixed(start_x, centered_x,
                                  signed_scale_x,
                                  src->second.width,
                                  box_width_fixed);
                for (int32_t target_x = start_x; target_x < end_x;
                     ++target_x) {
                    const auto pixel =
                        boxed_sample(source_x_fixed, source_y_fixed);
                    const std::size_t destination_offset =
                        static_cast<std::size_t>(target_y) *
                            output.stride +
                        static_cast<std::size_t>(target_x) * 4U;
                    for (uint32_t channel = 0; channel < 4U;
                         ++channel) {
                        output.pixels[
                            destination_offset + channel] =
                            pixel[channel];
                    }
                    output.rgba[destination_offset] =
                        pixel[2];
                    output.rgba[destination_offset + 1U] =
                        pixel[1];
                    output.rgba[destination_offset + 2U] =
                        pixel[0];
                    output.rgba[destination_offset + 3U] =
                        output.format == 1U ? 0xFFU : pixel[3];
                    source_x_fixed += step_x;
                }
                source_y_fixed += step_y;
            }
        }
        if (debug_enabled()) bitmap_scales_.push_back(
            {destination, source, scale_x, scale_y, interpolation});
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::vector<BitmapScale>& bitmap_scales() const {
        return bitmap_scales_;
    }
    struct BitmapFitOperation {
        uint32_t destination = 0;
        uint32_t source = 0;
        uint32_t source_width = 0;
        uint32_t source_height = 0;
        uint32_t destination_width = 0;
        uint32_t destination_height = 0;
        uint32_t draw_width = 0;
        uint32_t draw_height = 0;
        int32_t x = 0;
        int32_t y = 0;
        float scale = 0.0F;
    };
    uint32_t fit_bitmap(uint32_t destination, uint32_t source) {
        const auto target = graphics_surfaces_.find(destination);
        if (target == graphics_surfaces_.end()) {
            return 9;
        }
        const auto input = graphics_surfaces_.find(source);
        if (input == graphics_surfaces_.end()) {
            return 10;
        }
        if (target->second.width == 0 ||
            target->second.height == 0 ||
            input->second.width == 0 ||
            input->second.height == 0) {
            return 0;
        }
        const float x_scale =
            static_cast<float>(target->second.width) /
            static_cast<float>(input->second.width);
        const float y_scale =
            static_cast<float>(target->second.height) /
            static_cast<float>(input->second.height);
        const float scale = std::min(x_scale, y_scale);
        const uint32_t width = static_cast<uint32_t>(
            static_cast<float>(input->second.width) * scale);
        const uint32_t height = static_cast<uint32_t>(
            static_cast<float>(input->second.height) * scale);
        BitmapFitOperation operation;
        operation.destination = destination;
        operation.source = source;
        operation.source_width = input->second.width;
        operation.source_height = input->second.height;
        operation.destination_width = target->second.width;
        operation.destination_height = target->second.height;
        operation.draw_width = width;
        operation.draw_height = height;
        operation.x = static_cast<int32_t>(
            (target->second.width - width) / 2U);
        operation.y = static_cast<int32_t>(
            (target->second.height - height) / 2U);
        operation.scale = scale;
        if (debug_enabled()) bitmap_fit_operations_.push_back(operation);
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::vector<BitmapFitOperation>&
    bitmap_fit_operations() const {
        return bitmap_fit_operations_;
    }
    bool compute_scaled_size_descriptor(
        uint32_t output, uint32_t input, uint32_t scale_x,
        uint32_t scale_y, uint32_t base) {
        if (output == 0 || input == 0) {
            return false;
        }
        const uint32_t width = memory_.read_u32(input, base);
        const uint32_t height =
            memory_.read_u32(input + 4U, base);
        const uint32_t factor = memory_.read_u32(input + 8U, base);
        const auto adjust = [factor](uint32_t scale) -> uint32_t {
            if (factor == 0 || scale == 0) {
                return 0x10000U;
            }
            const int64_t scaled =
                static_cast<int64_t>(scale) << 16U;
            if (static_cast<int32_t>(factor) < 0) {
                return static_cast<uint32_t>(
                    (scaled - static_cast<int64_t>(factor)) /
                    static_cast<int64_t>(scale));
            }
            const int64_t denominator =
                static_cast<int64_t>(factor) + scaled;
            if (denominator == 0) {
                return 0x10000U;
            }
            return static_cast<uint32_t>(scaled / denominator);
        };
        const uint32_t adjusted_x = adjust(scale_x);
        const uint32_t adjusted_y = adjust(scale_y);
        const uint32_t result_x = static_cast<uint32_t>(
            (static_cast<int64_t>(static_cast<int32_t>(width)) *
                 static_cast<int32_t>(adjusted_x)) >>
            16U);
        const uint32_t result_y = static_cast<uint32_t>(
            (static_cast<int64_t>(static_cast<int32_t>(height)) *
                 static_cast<int32_t>(adjusted_y)) >>
            16U);
        memory_.write_u32(output, base, result_x);
        memory_.write_u32(output + 4U, base, result_y);
        return true;
    }
    struct ToneCurveState {
        uint32_t identifier = 0;
        std::array<int32_t, 6> descriptor{};
        std::array<std::array<uint32_t, 256>, 3> channels{};
        std::array<uint32_t, 256> packed{};
    };
    uint32_t update_tone_curve(uint32_t identifier,
                               uint32_t descriptor, uint32_t base) {
        if (descriptor == 0) {
            return tone_curves_.erase(identifier) == 0
                       ? 0x8000000FU
                       : 0U;
        }
        std::array<int32_t, 6> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            values[index] = static_cast<int32_t>(memory_.read_u32(
                descriptor + static_cast<uint32_t>(index * 4U),
                base));
        }
        for (std::size_t channel = 0; channel < 3; ++channel) {
            if (values[channel * 2U] < 1 ||
                values[channel * 2U] > 254 ||
                values[channel * 2U + 1U] < 0 ||
                values[channel * 2U + 1U] > 255) {
                return 0x16;
            }
        }
        ToneCurveState state;
        state.identifier = identifier;
        state.descriptor = values;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const double control_x =
                values[4U - channel * 2U];
            const double control_y =
                values[5U - channel * 2U];
            const std::array<double, 3> x{0.0, control_x, 255.0};
            const std::array<double, 3> y{0.0, control_y, 255.0};
            const double h0 = x[1] - x[0];
            const double h1 = x[2] - x[1];
            double middle = 0.0;
            if (h0 != 0.0 && h1 != 0.0) {
                middle =
                    6.0 * ((y[2] - y[1]) / h1 -
                           (y[1] - y[0]) / h0) /
                    (2.0 * (h0 + h1));
            }
            const std::array<double, 3> second{0.0, middle, 0.0};
            for (uint32_t sample = 0; sample < 256U; ++sample) {
                const double input = sample;
                const std::size_t segment =
                    h0 == 0.0 || input <= x[1] ? 0U : 1U;
                const double left = x[segment];
                const double right = x[segment + 1U];
                const double span = right - left;
                double value = 0.0;
                if (span != 0.0) {
                    const double a = (right - input) / span;
                    const double b = (input - left) / span;
                    value =
                        second[segment] * (right - input) *
                            (right - input) * (right - input) /
                            (6.0 * span) +
                        second[segment + 1U] * (input - left) *
                            (input - left) * (input - left) /
                            (6.0 * span) +
                        (y[segment] -
                         second[segment] * span * span / 6.0) *
                            a +
                        (y[segment + 1U] -
                         second[segment + 1U] * span * span / 6.0) *
                            b;
                }
                const double rounded =
                    value >= 0.0 ? value + 0.5 : value - 0.5;
                const int32_t integer = static_cast<int32_t>(rounded);
                state.channels[channel][sample] =
                    static_cast<uint32_t>(std::clamp(integer, 0, 255));
            }
        }
        for (std::size_t sample = 0; sample < state.packed.size();
             ++sample) {
            state.packed[sample] =
                state.channels[0][sample] |
                (state.channels[1][sample] << 8U) |
                (state.channels[2][sample] << 16U);
        }
        tone_curves_[identifier] = std::move(state);
        return 0;
    }
    [[nodiscard]] const std::map<uint32_t, ToneCurveState>&
    tone_curves() const {
        return tone_curves_;
    }
    bool clone_bitmap(uint32_t destination, uint32_t source) {
        const auto src = graphics_surfaces_.find(source);
        if (src == graphics_surfaces_.end()) {
            return false;
        }
        install_allocated_graphics_surface(destination, src->second);
        display_dirty_ = true;
        return true;
    }
    struct BitmapRegion {
        uint32_t destination = 0;
        uint32_t source = 0;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    uint32_t create_bitmap_region(uint32_t destination, uint32_t source,
                                  int32_t x, int32_t y, uint32_t width,
                                  uint32_t height) {
        auto src = graphics_surfaces_.find(source);
        if (src == graphics_surfaces_.end()) {
            return 2;
        }
        if (width == 0 || height == 0) {
            return 3;
        }
        create_graphics_surface(destination, height, width,
                                src->second.format);
        GraphicsSurface& output = graphics_surfaces_.at(destination);
        if (!materialize_graphics_pixels(src->second) ||
            !materialize_graphics_pixels(output)) {
            return 1;
        }
        if (output.rgba.size() !=
            static_cast<std::size_t>(output.width) *
                output.height * 4U) {
            output.rgba.assign(
                static_cast<std::size_t>(output.width) *
                    output.height * 4U,
                0);
        }
        const uint32_t bytes_per_pixel =
            src->second.bytes_per_pixel;
        for (uint32_t target_y = 0; target_y < output.height;
             ++target_y) {
            const int64_t source_y =
                static_cast<int64_t>(y) + target_y;
            if (source_y < 0 ||
                source_y >= static_cast<int64_t>(
                                  src->second.height)) {
                continue;
            }
            for (uint32_t target_x = 0; target_x < output.width;
                 ++target_x) {
                const int64_t source_x =
                    static_cast<int64_t>(x) + target_x;
                if (source_x < 0 ||
                    source_x >= static_cast<int64_t>(
                                      src->second.width)) {
                    continue;
                }
                const std::size_t source_offset =
                    static_cast<std::size_t>(source_y) *
                        src->second.stride +
                    static_cast<std::size_t>(source_x) *
                        bytes_per_pixel;
                const std::size_t destination_offset =
                    static_cast<std::size_t>(target_y) *
                        output.stride +
                    static_cast<std::size_t>(target_x) *
                        output.bytes_per_pixel;
                for (uint32_t byte = 0; byte < bytes_per_pixel;
                     ++byte) {
                    output.pixels[destination_offset + byte] =
                        src->second.pixels[source_offset + byte];
                }
                if (output.bytes_per_pixel == 4U) {
                    output.rgba[destination_offset] =
                        output.pixels[destination_offset + 2U];
                    output.rgba[destination_offset + 1U] =
                        output.pixels[destination_offset + 1U];
                    output.rgba[destination_offset + 2U] =
                        output.pixels[destination_offset];
                    output.rgba[destination_offset + 3U] =
                        output.format == 1U
                            ? 0xFFU
                            : output.pixels[destination_offset + 3U];
                }
            }
        }
        if (debug_enabled()) bitmap_regions_.push_back(
            {destination, source, x, y, width, height});
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::vector<BitmapRegion>& bitmap_regions() const {
        return bitmap_regions_;
    }
    void record_bitmap_load_error(uint32_t type, std::string message) {
        bitmap_load_errors_[type] = std::move(message);
    }
    [[nodiscard]] const std::map<uint32_t, std::string>&
    bitmap_load_errors() const {
        return bitmap_load_errors_;
    }
    void set_surface_command(uint32_t type, uint32_t value);
    [[nodiscard]] const std::map<uint32_t, uint32_t>&
    graphics_surface_commands() const {
        return graphics_surface_commands_;
    }
    void set_input_query_table(std::string name, std::vector<uint8_t> bits) {
        input_query_tables_[std::move(name)] = std::move(bits);
    }
    [[nodiscard]] const std::map<std::string, std::vector<uint8_t>>&
    input_query_tables() const {
        return input_query_tables_;
    }
    void set_last_input_query_name(std::string name) {
        last_input_query_name_ = std::move(name);
    }
    [[nodiscard]] const std::string& last_input_query_name() const {
        return last_input_query_name_;
    }
    void set_input_descriptor(uint32_t descriptor, uint32_t raw) {
        const uint32_t previous = input_descriptor(descriptor);
        input_descriptor_state_[descriptor] = raw;
        // Original WM_KEYDOWN: 004A6D14 / 140101866 clears the down
        // latch before registering another press when record+0x14 is set.
        const bool retrigger = descriptor < input_descriptor_retrigger_.size() &&
            input_descriptor_retrigger_[descriptor] != 0 &&
            (raw & previous & 0x8000U) != 0;
        if (retrigger) {
            ++input_descriptor_event_values_[descriptor];
        }
        if ((raw & 0x8000U) != 0 &&
            ((previous & 0x8000U) == 0 || retrigger)) {
            input_descriptor_active_[descriptor] = true;
            input_descriptor_consumed_[descriptor] = false;
        }
    }
    // WM_KEYDOWN's latch is separate from GetAsyncKeyState's held bit.
    // PE32 00479230 / PE64 1400C5CE0 return true only for a new latch;
    // WM_KEYDOWN optionally clears it first through Sys1:10's record+14h.
    bool notify_keyboard_key_down(uint32_t descriptor, bool honor_retrigger = true) {
        if (descriptor >= 256U) {
            throw Error("keyboard descriptor exceeds 255");
        }
        if (honor_retrigger && input_descriptor_retrigger_[descriptor] != 0) {
            notify_keyboard_key_up(descriptor);
        }
        if (input_descriptor_active_[descriptor]) {
            return false;
        }
        queue_input_descriptor_event(descriptor, 1);
        // 00479DF0 / 1400C5180 precedes the type-3 record on this path.
        ++sys0_counter_542e90_;
        enqueue_system_event_record(3, descriptor);
        return true;
    }
    void notify_keyboard_key_up(uint32_t descriptor) {
        // Original 004792B0 / 1400C5D60 retain queued counts/FIFO records.
        if (descriptor >= 256U) {
            throw Error("keyboard descriptor exceeds 255");
        }
        input_descriptor_active_[descriptor] = false;
        input_descriptor_consumed_[descriptor] = false;
    }
    void set_input_descriptor_raw_state(uint32_t descriptor, uint32_t raw) {
        // Host sampling must not create another message edge or retrigger.
        input_descriptor_state_[descriptor] = raw;
    }
    void set_keyboard_state_provider(KeyboardStateProvider provider) {
        keyboard_state_provider_ = std::move(provider);
    }
    [[nodiscard]] std::array<uint8_t, 256> keyboard_state() const {
        if (keyboard_state_provider_) return keyboard_state_provider_();
        std::array<uint8_t, 256> states{};
        for (uint32_t key = 0; key < states.size(); ++key) {
            if ((input_descriptor(key) & 0x8000U) != 0) states[key] = 0x80;
        }
        return states;
    }
    // One accepted WM_MOUSEWHEEL message, after host direction normalization.
    // Returns no held key: 0E/0F are immediate press/release pulses.
    void notify_mouse_wheel(bool down);
    void notify_mouse_horizontal_wheel(bool right);
    uint32_t exchange_input_descriptor_retrigger(uint32_t descriptor,
                                                  uint32_t value) {
        if (descriptor >= input_descriptor_retrigger_.size()) {
            throw Error("Sys1:0x10 descriptor exceeds 255");
        }
        return std::exchange(input_descriptor_retrigger_[descriptor], value);
    }
    [[nodiscard]] uint32_t input_descriptor(uint32_t descriptor) const {
        const auto found = input_descriptor_state_.find(descriptor);
        return found == input_descriptor_state_.end() ? 0U
                                                       : found->second;
    }
    [[nodiscard]] uint32_t descriptor_state_for_mask(
        uint32_t mask) const {
        static const std::array<std::vector<uint32_t>, 24> tables = {{
            {1}, {2}, {4}, {5}, {6}, {14}, {15}, {13},
            {32}, {38}, {40}, {37}, {39}, {49, 97}, {50, 98},
            {51, 99}, {52, 100}, {53, 101}, {54, 102},
            {55, 103}, {56, 104}, {57, 105}, {48, 96}, {9},
        }};
        static constexpr std::array<uint32_t, 24> masks = {
            0x00000001U, 0x00000002U, 0x00000004U, 0x00000010U,
            0x00000020U, 0x00000040U, 0x00000080U, 0x00000100U,
            0x00000200U, 0x00001000U, 0x00002000U, 0x00004000U,
            0x00008000U, 0x00010000U, 0x00020000U, 0x00040000U,
            0x00080000U, 0x00100000U, 0x00200000U, 0x00400000U,
            0x00800000U, 0x01000000U, 0x02000000U, 0x40000000U,
        };
        uint32_t result = 0;
        for (std::size_t index = 0; index < masks.size(); ++index) {
            if ((mask & masks[index]) == 0) {
                continue;
            }
            for (const uint32_t descriptor : tables[index]) {
                result = input_descriptor(descriptor);
            }
        }
        return result;
    }
    void clear_input_state() {
        input_descriptor_state_.clear();
        input_descriptor_event_values_.clear();
        input_descriptor_active_.clear();
        input_descriptor_consumed_.clear();
    }
    void queue_input_descriptor_event(uint32_t descriptor,
                                      uint32_t value) {
        input_descriptor_event_values_[descriptor] += value;
        input_descriptor_active_[descriptor] = true;
        input_descriptor_consumed_[descriptor] = false;
    }
    uint32_t input_descriptor_result(uint32_t descriptor) {
        if (descriptor >= 0x100U) {
            throw BpFault("input descriptor must be below 256");
        }
        const uint32_t raw = input_descriptor(descriptor);
        if (input_descriptor_active_[descriptor] &&
            (raw & 0x8000U) == 0) {
            input_descriptor_active_[descriptor] = false;
            input_descriptor_consumed_[descriptor] = false;
        }
        if (descriptor >= 0xC1U && descriptor <= 0xD7U) {
            static const std::map<uint32_t, std::vector<uint32_t>> groups = {
                {0xC1, {13}},
                {0xC2, {32}},
                {0xC3, {38, 14}},
                {0xC4, {40, 15}},
            };
            uint32_t high = 0;
            uint32_t total = 0;
            const auto found = groups.find(descriptor);
            if (found != groups.end()) {
                for (const uint32_t child : found->second) {
                    const uint32_t value =
                        input_descriptor_result(child);
                    high |= value & 0x80000000U;
                    total += value & 0x7FFFFFFFU;
                }
            }
            return high | total;
        }
        uint32_t result = input_descriptor_event_values_[descriptor];
        if (input_descriptor_active_[descriptor] &&
            !input_descriptor_consumed_[descriptor]) {
            result |= 0x80000000U;
            input_descriptor_consumed_[descriptor] = true;
        }
        input_descriptor_event_values_[descriptor] = 0;
        return result;
    }
    [[nodiscard]] uint32_t poll_fixed_input_masks() {
        static const std::vector<std::vector<uint32_t>> tables = {
            {14}, {15}, {13}, {32}, {38}, {40}, {37}, {39},
            {49, 97}, {50, 98}, {51, 99}, {52, 100}, {53, 101},
            {54, 102}, {55, 103}, {56, 104}, {57, 105},
            {48, 96}, {9},
        };
        // 004795C8 indexes EBP-50h; EBP-54h is the pointer-list sentinel.
        static constexpr std::array<uint32_t, 19> masks = {
            0x00000040U, 0x00000080U, 0x00000100U, 0x00000200U,
            0x00001000U, 0x00002000U, 0x00004000U, 0x00008000U,
            0x00010000U, 0x00020000U, 0x00040000U, 0x00080000U,
            0x00100000U, 0x00200000U, 0x00400000U, 0x00800000U,
            0x01000000U, 0x02000000U, 0x40000000U,
        };
        uint32_t result = 0;
        for (std::size_t index = 0; index < tables.size(); ++index) {
            const auto configured = input_classes_.find(masks[index]);
            const auto& descriptors = configured == input_classes_.end()
                ? tables[index] : configured->second;
            for (const uint32_t descriptor : descriptors) {
                if (input_descriptor_result(descriptor) != 0) {
                    result |= masks[index];
                    break;
                }
            }
        }
        return result;
    }
    void arm_input_capture(uint32_t key) {
        // 00478D60: first capture chain contains a real unbounded mouse
        // rectangle (PE 0052A52C), independent of keyboard priority tokens.
        add_input_region({key,INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX,0});
        input_capture_first_.insert(std::lower_bound(input_capture_first_.begin(),input_capture_first_.end(),key,std::greater<uint32_t>()),key);
        input_capture_second_.insert(std::lower_bound(input_capture_second_.begin(),input_capture_second_.end(),key,std::greater<uint32_t>()),key);
    }
    void disarm_input_capture(uint32_t key) {
        // 00478DE0/00478E20/00478E30 remove only the first matching key.
        const auto erase_one=[&](auto& list){auto n=std::find(list.begin(),list.end(),key);if(n!=list.end())list.erase(n);};
        erase_one(input_capture_first_);erase_one(input_capture_second_);
        const auto region=std::find_if(input_regions_.begin(),input_regions_.end(),
            [key](const InputRegion& n){return n.priority==key;});
        if(region!=input_regions_.end())input_regions_.erase(region);
    }
    [[nodiscard]] bool input_capture_gate(uint32_t key) const {
        return sys0_input_global_52a5ac_ != 0 &&
               (input_capture_second_.empty() || *input_capture_second_.begin() <= key);
    }
    void set_input_edge_mode(bool value) {
        input_edge_mode_5446a0_ = value;
    }
    [[nodiscard]] bool input_edge_mode() const {
        return input_edge_mode_5446a0_;
    }
    void set_input_return_gate(bool value) {
        input_return_gate_542ca8_ = value;
    }
    [[nodiscard]] bool input_return_gate() const {
        return input_return_gate_542ca8_;
    }
    void set_input_regions_active(bool value) {
        input_regions_active_ = value;
    }
    [[nodiscard]] bool input_regions_active() const {
        return input_regions_active_;
    }
    struct InputRegion {
        uint32_t priority = 0;
        int32_t left = 0;
        int32_t top = 0;
        int32_t right = 0;
        int32_t bottom = 0;
        uint32_t object = 0;
    };
    void add_input_region(InputRegion region) {
        const auto position = std::find_if(
            input_regions_.begin(), input_regions_.end(),
            [&region](const InputRegion& current) {
                return current.priority <= region.priority;
            });
        input_regions_.insert(position, region);
    }
    bool remove_input_region(uint32_t object) {
        const auto found = std::find_if(
            input_regions_.begin(), input_regions_.end(),
            [object](const InputRegion& region) {
                return region.object == object;
            });
        if (found == input_regions_.end()) {
            return false;
        }
        input_regions_.erase(found);
        return true;
    }
    [[nodiscard]] const std::vector<InputRegion>& input_regions() const {
        return input_regions_;
    }
    [[nodiscard]] bool input_region_hit(uint32_t priority) const {
        const auto [x, y] = pointer_position();
        return input_region_hit_at(priority, x, y);
    }
    [[nodiscard]] bool input_region_hit_at(uint32_t priority, int32_t x, int32_t y) const {
        if (!input_regions_active_) {
            return false;
        }
        for (const InputRegion& region : input_regions_) {
            if (region.priority < priority) {
                return false;
            }
            const bool inside =
                region.left <= x && x <= region.right &&
                region.top <= y && y <= region.bottom;
            bool object_hit = inside;
            if (region.object != 0) {
                const auto object =
                    display_object_states_.find(region.object);
                object_hit =
                    object != display_object_states_.end() &&
                    object->second.available && inside &&
                    input_object_mask_hit(region.object,
                        x - region.left, y - region.top,
                        object->second.width, object->second.height);
            }
            if (!object_hit) {
                continue;
            }
            if (region.priority > priority) {
                return false;
            }
            return true;
        }
        return false;
    }
    [[nodiscard]] uint32_t input_modifier_word() const {
        uint32_t modifiers = 0;
        const auto add = [this, &modifiers](
                             uint32_t descriptor, uint32_t mask) {
            if ((input_descriptor(descriptor) & 0x8000U) != 0) {
                modifiers |= mask;
            }
        };
        add(0x11, 0x0008U);
        add(0x01, 0x0001U);
        add(0x04, 0x0010U);
        add(0x02, 0x0002U);
        add(0x10, 0x0004U);
        add(0x05, 0x0020U);
        add(0x06, 0x0040U);
        return modifiers;
    }
    uint32_t synthetic_click(uint32_t button) {
        if (button != 1U && button != 2U && button != 4U &&
            button != 5U && button != 6U) {
            return 0;
        }
        const auto [x, y] = pointer_position();
        last_synthetic_click_modifiers_ = input_modifier_word();
        set_input_descriptor(button, 0x8000U);
        queue_input_descriptor_event(button, 1U);
        (void)queue_icon_input_at(x, y, true);
        set_input_descriptor(button, 0U);
        (void)queue_icon_input_at(x, y, false);
        return 1;
    }
    [[nodiscard]] uint32_t last_synthetic_click_modifiers() const {
        return last_synthetic_click_modifiers_;
    }
    bool consume_active_input_event() {
        bool any_down = false;
        uint32_t flag = sys0_input_global_5446a4_;
        // 00479600 reads DAT_0052A310 (Sys0:1B class 80000000),
        // not Sys0:62's fullscreen shortcut list DAT_00542BF0.
        static const std::vector<uint32_t> default_skip_keys = {0x11};
        const auto configured = input_classes_.find(0x80000000U);
        const auto& descriptors = configured == input_classes_.end()
            ? default_skip_keys : configured->second;
        for (const uint32_t descriptor : descriptors) {
            const uint32_t raw = input_descriptor(descriptor);
            if ((raw & 0x8000U) != 0) {
                any_down = true;
            }
            if (!input_edge_mode_5446a0_) {
                if ((raw & 0x8000U) != 0 &&
                    input_return_gate_542ca8_) {
                    return sys0_input_global_52a5f8_ != 0;
                }
                continue;
            }
            if (input_descriptor_result(descriptor) != 0 &&
                input_return_gate_542ca8_) {
                flag = 1;
            }
        }
        if (input_edge_mode_5446a0_ && !any_down) {
            input_edge_mode_5446a0_ = false;
        }
        return flag != 0 && sys0_input_global_52a5f8_ != 0;
    }
    uint32_t poll_input_capture(uint32_t key) {
        uint32_t result = 0;
        if (input_capture_gate(key)) {
            result = poll_fixed_input_masks();
            if (consume_active_input_event()) {
                result |= 0x80000000U;
            }
        }
        if (input_region_hit(key)) {
            static constexpr std::array<uint32_t, 5> descriptors = {
                1, 2, 4, 5, 6};
            static constexpr std::array<uint32_t, 5> masks = {
                1, 2, 4, 0x10, 0x20};
            for (std::size_t index = 0; index < descriptors.size(); ++index) {
                if (input_descriptor_result(descriptors[index]) != 0) {
                    result |= masks[index];
                }
            }
        }
        return result;
    }
    void set_input_value_542e8c(uint32_t value) {
        input_value_542e8c_ = value;
    }
    [[nodiscard]] uint32_t input_value_542e8c() const {
        return input_value_542e8c_;
    }
    void set_sys1_value_58d2e0(uint32_t value) {
        sys1_value_58d2e0_ = value;
    }
    [[nodiscard]] uint32_t sys1_value_58d2e0() const {
        return sys1_value_58d2e0_;
    }
    void set_sys1_value_540f00(uint32_t value) {
        sys1_value_540f00_ = value;
    }
    [[nodiscard]] uint32_t sys1_value_540f00() const {
        return sys1_value_540f00_;
    }
    void set_hotkeys_enabled(bool enabled) {
        hotkeys_enabled_ = enabled;
    }
    [[nodiscard]] bool hotkeys_enabled() const {
        return hotkeys_enabled_;
    }
    void set_input_class(uint32_t mask, std::vector<uint32_t> descriptors) {
        input_classes_[mask] = std::move(descriptors);
    }
    bool set_sys1_input_table(uint32_t index, uint32_t value) {
        if (index >= sys1_input_table_5409a0_.size()) {
            return false;
        }
        sys1_input_table_5409a0_[index] = value;
        return true;
    }
    [[nodiscard]] uint32_t sys1_input_table(uint32_t index) const {
        return index < sys1_input_table_5409a0_.size()
                   ? sys1_input_table_5409a0_[index]
                   : 0U;
    }
    void set_sys1_input_global_529bac(uint32_t value) {
        sys1_input_global_529bac_ = value;
    }
    [[nodiscard]] uint32_t sys1_input_global_529bac() const {
        return sys1_input_global_529bac_;
    }
    [[nodiscard]] const std::map<uint32_t, std::vector<uint32_t>>&
    input_classes() const {
        return input_classes_;
    }
    void set_active_descriptor_set(uint32_t value,
                                   std::vector<uint32_t> descriptors) {
        active_descriptor_value_ = value;
        active_descriptors_ = std::move(descriptors);
    }
    void set_active_descriptor_value(uint32_t value) {
        active_descriptor_value_ = value;
    }
    [[nodiscard]] uint32_t active_descriptor_value() const {
        return active_descriptor_value_;
    }
    [[nodiscard]] const std::vector<uint32_t>& active_descriptors() const {
        return active_descriptors_;
    }
    struct Sys0RecordSet {
        uint32_t max_entries = 0;
        uint32_t record_size = 0;
        uint32_t mode = 1;
        std::deque<std::vector<uint8_t>> records;
    };
    uint32_t create_sys0_record_set(uint32_t max_entries,
                                    uint32_t record_size) {
        if (max_entries == 0 || record_size == 0) {
            return 0;
        }
        const uint32_t id = ++next_sys0_record_set_id_;
        sys0_record_sets_[id] =
            Sys0RecordSet{max_entries, record_size, 1, {}};
        return id;
    }
    uint32_t remove_sys0_record_set(uint32_t id) {
        return sys0_record_sets_.erase(id) != 0 ? 0U : 1U;
    }
    uint32_t sys0_record_count(uint32_t id, uint32_t* count) const {
        const auto found = sys0_record_sets_.find(id);
        if (found == sys0_record_sets_.end()) {
            return 1;
        }
        if (count != nullptr) {
            *count = static_cast<uint32_t>(found->second.records.size());
        }
        return 0;
    }
    uint32_t append_sys0_record(uint32_t id,
                                std::span<const uint8_t> data) {
        const auto found = sys0_record_sets_.find(id);
        if (found == sys0_record_sets_.end()) {
            return 1;
        }
        Sys0RecordSet& set = found->second;
        if (data.size() < set.record_size) {
            return 1;
        }
        std::span<const uint8_t> record =
            data.first(set.record_size);
        std::vector<uint8_t> stored =
            set.mode == 1 ? encode_zero_runs(record)
                          : std::vector<uint8_t>(record.begin(),
                                                 record.end());
        set.records.push_front(std::move(stored));
        while (set.records.size() > set.max_entries) {
            set.records.pop_back();
        }
        return 0;
    }
    uint32_t read_sys0_record(uint32_t id, uint32_t index,
                              std::vector<uint8_t>& output) const {
        const auto found = sys0_record_sets_.find(id);
        if (found == sys0_record_sets_.end()) {
            return 1;
        }
        const Sys0RecordSet& set = found->second;
        if (index >= set.records.size()) {
            return 2;
        }
        if (set.mode == 1) {
            output = decode_zero_runs(
                set.records[index], set.record_size);
            return output.size() == set.record_size ? 0U : 2U;
        }
        output = set.records[index];
        return 0;
    }
    uint32_t truncate_sys0_records(uint32_t id, uint32_t index,
                                   uint32_t count) {
        const auto found = sys0_record_sets_.find(id);
        if (found == sys0_record_sets_.end()) {
            return 1;
        }
        Sys0RecordSet& set = found->second;
        if (index >= set.records.size()) {
            return 2;
        }
        const std::size_t available = set.records.size() - index;
        const std::size_t remove_count =
            std::min<std::size_t>(count, available);
        set.records.erase(
            set.records.begin() + static_cast<std::ptrdiff_t>(index),
            set.records.begin() +
                static_cast<std::ptrdiff_t>(index + remove_count));
        return 0;
    }
    uint32_t set_sys0_record_mode(uint32_t id, uint32_t mode) {
        const auto found = sys0_record_sets_.find(id);
        if (found == sys0_record_sets_.end()) {
            return 1;
        }
        if (mode > 1) {
            return 2;
        }
        found->second.mode = mode;
        return 0;
    }
    [[nodiscard]] const std::map<uint32_t, Sys0RecordSet>&
    sys0_record_sets() const {
        return sys0_record_sets_;
    }
    // PE64 Ext2 1400EFF00: two independent pools, low-byte slot lookup.
    uint32_t ext2_buffer_operation(uint8_t sub, Thread& thread);
    uint32_t ext2_sort_records(Thread& thread);
    struct Sys1BufferManager {
        std::vector<std::optional<std::vector<uint8_t>>> slots;
    };
    uint32_t create_sys1_buffer_manager(uint32_t capacity,
                                        uint32_t& id) {
        if (capacity <= 1U) {
            return 0x80000001U;
        }
        id = ++next_sys1_buffer_manager_id_;
        sys1_buffer_managers_[id].slots.resize(capacity);
        return 0;
    }
    uint32_t destroy_sys1_buffer_manager(uint32_t id) {
        return sys1_buffer_managers_.erase(id) != 0
                   ? 0U
                   : 0x80000002U;
    }
    uint32_t store_sys1_buffer(uint32_t id, uint32_t index,
                               bool automatic, std::span<const uint8_t> data,
                               uint32_t& chosen_index) {
        const auto found = sys1_buffer_managers_.find(id);
        if (found == sys1_buffer_managers_.end()) {
            return 0x80000002U;
        }
        if (data.empty()) {
            return 0x80000001U;
        }
        auto& slots = found->second.slots;
        if (automatic) {
            const auto free = std::find_if(
                slots.begin(), slots.end(),
                [](const auto& slot) { return !slot.has_value(); });
            chosen_index = free == slots.end()
                               ? static_cast<uint32_t>(slots.size())
                               : static_cast<uint32_t>(
                                     free - slots.begin());
        } else {
            chosen_index = index;
        }
        if (chosen_index >= slots.size()) {
            std::size_t capacity = slots.size();
            do {
                if (capacity >
                    std::numeric_limits<std::size_t>::max() / 2U) {
                    return 0x80000003U;
                }
                capacity *= 2U;
            } while (capacity <= chosen_index);
            slots.resize(capacity);
        }
        slots[chosen_index] =
            std::vector<uint8_t>(data.begin(), data.end());
        return 0;
    }
    uint32_t release_sys1_buffer(uint32_t id, uint32_t index) {
        const auto found = sys1_buffer_managers_.find(id);
        if (found == sys1_buffer_managers_.end() ||
            index >= found->second.slots.size() ||
            !found->second.slots[index].has_value()) {
            return 0x80000002U;
        }
        found->second.slots[index].reset();
        return 0;
    }
    uint32_t read_sys1_buffer(uint32_t id, uint32_t index,
                              std::vector<uint8_t>& data) const {
        const auto found = sys1_buffer_managers_.find(id);
        if (found == sys1_buffer_managers_.end() ||
            index >= found->second.slots.size() ||
            !found->second.slots[index].has_value()) {
            return 0x80000002U;
        }
        data = *found->second.slots[index];
        return 0;
    }
    [[nodiscard]] std::vector<std::pair<uint32_t, uint32_t>>
    enumerate_sys1_buffers(uint32_t id) const {
        std::vector<std::pair<uint32_t, uint32_t>> result;
        const auto found = sys1_buffer_managers_.find(id);
        if (found == sys1_buffer_managers_.end()) {
            return result;
        }
        for (std::size_t index = 0;
             index < found->second.slots.size(); ++index) {
            const auto& slot = found->second.slots[index];
            if (slot.has_value()) {
                result.emplace_back(
                    static_cast<uint32_t>(index),
                    static_cast<uint32_t>(slot->size()));
            }
        }
        return result;
    }
    [[nodiscard]] const std::map<uint32_t, Sys1BufferManager>&
    sys1_buffer_managers() const {
        return sys1_buffer_managers_;
    }
    void clear_sys1_string_table() {
        sys1_string_table_.clear();
    }
    bool load_sys1_string_table(std::span<const uint8_t> data) {
        std::size_t offset = 0;
        const auto read_u32 = [&data, &offset](uint32_t& value) {
            if (offset + 4U > data.size()) {
                return false;
            }
            value = static_cast<uint32_t>(data[offset]) |
                    (static_cast<uint32_t>(data[offset + 1]) << 8U) |
                    (static_cast<uint32_t>(data[offset + 2]) << 16U) |
                    (static_cast<uint32_t>(data[offset + 3]) << 24U);
            offset += 4U;
            return true;
        };
        const auto read_string = [&data, &offset](
                                     std::string& value) {
            const std::size_t start = offset;
            while (offset < data.size() && data[offset] != 0) {
                ++offset;
            }
            if (offset >= data.size()) {
                return false;
            }
            value.assign(
                reinterpret_cast<const char*>(data.data() + start),
                offset - start);
            ++offset;
            return true;
        };

        uint32_t top_count = 0;
        if (!read_u32(top_count)) {
            return false;
        }
        for (uint32_t top_index = 0; top_index < top_count; ++top_index) {
            std::string top_key;
            uint32_t child_count = 0;
            if (!read_string(top_key) || !read_u32(child_count)) {
                return false;
            }
            std::map<std::string, std::string> children;
            for (uint32_t child_index = 0;
                 child_index < child_count; ++child_index) {
                std::string child;
                if (!read_string(child)) {
                    return false;
                }
                children[child] = child;
            }
            sys1_string_table_[top_key] = std::move(children);
        }
        return offset == data.size();
    }
    [[nodiscard]] std::optional<std::string>
    lookup_sys1_string(std::string_view top_key,
                       std::string_view child_key) const {
        const auto top = sys1_string_table_.find(std::string(top_key));
        if (top == sys1_string_table_.end()) {
            return std::nullopt;
        }
        const auto child = top->second.find(std::string(child_key));
        return child == top->second.end()
                   ? std::nullopt
                   : std::optional<std::string>(child->second);
    }
    [[nodiscard]] const std::map<
        std::string, std::map<std::string, std::string>>&
    sys1_string_table() const {
        return sys1_string_table_;
    }
    void set_audio_channel(uint32_t channel, uint32_t value) {
        audio_channels_.at(channel) = value;
        if (!audio_channels_muted_) sound_.set_bgm_primary_volume(channel, value);
    }
    [[nodiscard]] uint32_t audio_channel(uint32_t channel) const {
        return audio_channels_.at(channel);
    }
    void set_audio_channel_9(uint32_t channel, uint32_t value) {
        audio_channels_9_.at(channel) = value;
        if (!audio_channels_muted_) sound_.set_effect_primary_volume(channel, value);
    }
    [[nodiscard]] uint32_t audio_channel_9(uint32_t channel) const {
        return audio_channels_9_.at(channel);
    }
    // Original 0049F7B0/0049F800: minimize mutes the live +0C gain;
    // script changes still update the tables and restore reapplies them.
    void set_audio_channels_muted(bool muted) {
        if (audio_channels_muted_ == muted) return;
        audio_channels_muted_ = muted;
        for (uint32_t i = 0; i < audio_channels_.size(); ++i)
            sound_.set_bgm_primary_volume(i, muted ? 0U : audio_channels_[i]);
        for (uint32_t i = 0; i < audio_channels_9_.size(); ++i)
            sound_.set_effect_primary_volume(i, muted ? 0U : audio_channels_9_[i]);
    }
    [[nodiscard]] bool audio_channels_muted() const { return audio_channels_muted_; }
    struct SoundEffectRequest {
        uint32_t channel = 0;
        uint32_t volume = 0;
        uint32_t pan = 0;
    };
    uint32_t request_sound_effect(uint32_t channel, uint32_t volume,
                                  uint32_t pan) {
        if (pan > 0x80U || volume > 0x80U || channel >= 0x80U) {
            throw BpFault("invalid sound-effect arguments");
        }
        const uint32_t status = sound_.check_effect(channel);
        if (status == SoundSubsystem::kOk) {
            sound_.start_effect_playback(channel, volume, pan);
            sound_effect_requests_.push_back({channel, volume, pan});
        }
        if (status == SoundSubsystem::kOk ||
            status == SoundSubsystem::kDeviceUnavailable) {
            return sound_.effect_position(channel);
        }
        return 0;
    }
    [[nodiscard]] const std::vector<SoundEffectRequest>&
    sound_effect_requests() const {
        return sound_effect_requests_;
    }
    std::vector<SoundEffectRequest> take_sound_effect_requests() {
        std::vector<SoundEffectRequest> requests;
        requests.swap(sound_effect_requests_);
        return requests;
    }
    [[nodiscard]] SoundSubsystem& sound() { return sound_; }
    [[nodiscard]] const SoundSubsystem& sound() const { return sound_; }
    void set_master_volume(uint32_t volume) {
        if (volume == 0) {
            master_volume_ = -10000;
        } else {
            const double level =
                (12800.0 - static_cast<double>(volume) * 100.0) /
                (8.0 / 3.0);
            master_volume_ = -static_cast<int32_t>(level);
        }
        set_global_movie_volume(volume);
    }
    [[nodiscard]] int32_t master_volume() const { return master_volume_; }
    void set_effect_timer(bool active, uint64_t deadline, int32_t duration) {
        effect_timer_active_ = active;
        effect_timer_deadline_ms_ = deadline;
        effect_timer_duration_ms_ = duration;
    }
    [[nodiscard]] bool effect_timer_active() const {
        return effect_timer_active_;
    }
    [[nodiscard]] uint64_t effect_timer_deadline_ms() const {
        return effect_timer_deadline_ms_;
    }
    void set_graphics_pair_97(uint32_t first, uint32_t second) {
        graphics_first_532810_ = first;
        graphics_second_532818_ = second;
    }
    [[nodiscard]] uint32_t graphics_first_532810() const {
        return graphics_first_532810_;
    }
    [[nodiscard]] uint32_t graphics_second_532818() const {
        return graphics_second_532818_;
    }

    // Nine original graphics setting slots selected by Grp1:0x9A keys
    // 0x80000000..0x80000009.
    void set_graphics_setting(uint32_t key, uint32_t value) {
        graphics_settings_[key] = value;
    }
    [[nodiscard]] const std::map<uint32_t, uint32_t>& graphics_settings() const {
        return graphics_settings_;
    }

    // Original global 0x00529B60. Grp1:0x99 stores 0 for zero input,
    // otherwise ceil(65535 / value).
    void set_graphics_reciprocal(uint32_t value) {
        graphics_reciprocal_ =
            value == 0 ? 0U : (value + 0xFFFFU) / value;
    }
    [[nodiscard]] uint32_t graphics_reciprocal() const {
        return graphics_reciprocal_;
    }

    // Original globals 0x00533224/0x00533234/0x00533284. Grp0:0xAF
    // writes the same filtering value into all three renderer slots.
    void set_render_filter(uint32_t value) { render_filter_ = value; }
    [[nodiscard]] uint32_t render_filter() const { return render_filter_; }

    // Original global 0x00542E50. Sys0:0xAF accepts only 0 or 1.
    void set_system_flag_542e50(uint32_t value) {
        system_flag_542e50_ = value;
    }
    [[nodiscard]] uint32_t system_flag_542e50() const {
        return system_flag_542e50_;
    }

    // Original global 0x00589AF0. Ext1:0x1F accepts values 0, 1 and 2.
    void set_ext1_mode_589af0(uint32_t value) { ext1_mode_589af0_ = value; }
    [[nodiscard]] uint32_t ext1_mode_589af0() const {
        return ext1_mode_589af0_;
    }

    uint32_t create_ext1_object();
    uint32_t destroy_ext1_object(uint32_t id);
    uint32_t configure_ext1_object(
        uint32_t id, uint32_t count,
        std::span<const uint32_t> values, uint32_t auxiliary);
    uint32_t query_ext1_object(
        uint32_t id, uint32_t index,
        std::array<uint32_t, 3>& value) const;
    uint32_t create_ext1_window(
        uint32_t width, uint32_t height, uint32_t& id);
    uint32_t destroy_ext1_window(uint32_t id);
    uint32_t set_ext1_window_state(uint32_t id, uint32_t value);
    uint32_t configure_ext1_window(
        uint32_t id, uint32_t first, uint32_t second,
        uint32_t third, uint32_t fourth,
        uint32_t fifth);
    uint32_t configure_ext1_window_blocks(
        uint32_t id, uint32_t count, std::span<const uint32_t> sizes,
        std::span<uint32_t> offsets);
    uint32_t commit_ext1_window(uint32_t id);
    uint32_t set_ext1_window_value(uint32_t id, uint32_t value);
    uint32_t apply_ext1_window_frame(
        uint32_t id, const std::array<uint32_t, 9>& values);
    uint32_t create_ext1_control(
        uint32_t first, uint32_t second, uint32_t& id);
    uint32_t destroy_ext1_control(uint32_t id);
    uint32_t set_ext1_control_values(
        uint32_t id, uint32_t opcode,
        std::span<const uint32_t> values);
    uint32_t set_ext1_control_interval(
        uint32_t id, uint32_t interval);
    uint32_t set_ext1_emitter_values(
        uint32_t id, uint32_t opcode,
        std::span<const uint32_t> values);
    uint32_t load_ext1_bwfe(
        std::string_view prefix, std::string_view path, int32_t delta,
        std::vector<std::array<int32_t, 2>>& entries) const;
    void record_ext1_extended_values(
        uint32_t opcode, std::span<const uint32_t> values);
    enum class FactoryObjectKind : uint8_t {
        Grp0Primary = 0,
        Grp0Secondary = 1,
        Grp1Primary = 2,
        Grp1Secondary = 3,
    };
    struct FactoryObject {
        FactoryObjectKind kind = FactoryObjectKind::Grp0Primary;
        DisplayObjectClass display_class = DisplayObjectClass::CDspObj;
        uint32_t handle = 0;
        uint32_t instance_id = 0;
        uint32_t object_size = 0;
        uint32_t vtable = 0;
        bool display_registered = false;
        int32_t renderer_slot = -1;
    };
    struct LandscapeObjectState {
        struct Part {
            int32_t source_x = 0;
            int32_t source_y = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t field_10 = 0;
            uint32_t format = 0;
            int32_t clipped_x = 0;
            int32_t clipped_y = 0;
            uint32_t clipped_width = 0;
            uint32_t clipped_height = 0;
        };
        struct Image {
            std::array<uint32_t, 34> record{};
            uint32_t count = 0;
            uint32_t selected = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            int32_t x_offset = 0;
            int32_t y_offset = 0;
            int32_t alpha_channel = -1;
            std::vector<uint8_t> alpha;
        };
        struct Cell {
            int32_t image = -1;
            std::array<int32_t, 4> bounds{};
            uint32_t priority = 0;
            std::array<uint32_t, 4> guides_a{
                0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU};
            std::array<uint32_t, 4> guides_b{};
            uint32_t silhouette_type = 0;
            uint32_t silhouette_level = 0;
            uint32_t tail = 0;
            uint16_t value = 0;
        };
        uint32_t cell_size = 0x40;
        uint32_t width = 0x10;
        uint32_t height = 0x10;
        uint32_t line_pitch = 0;
        uint32_t field_158 = 0;
        uint32_t field_15c = 0;
        bool visible = true;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t blend_mode = 0;
        uint32_t effect_level = 0;
        uint32_t priority_bias = 0;
        uint32_t source_bitmap = 0;
        std::vector<Part> parts;
        std::vector<Image> images;
        std::vector<Part> guides;
        uint32_t columns = 0;
        uint32_t rows = 0;
        std::vector<int32_t> grid;
        std::vector<Cell> cells;
        bool map_ready = false;
    };
    std::optional<FactoryObject> create_factory_object(
        FactoryObjectKind kind);
    std::optional<uint32_t> create_grp1_landscape(
        uint32_t cell_size, uint32_t width, uint32_t height,
        uint32_t line_pitch, uint32_t field_158, uint32_t field_15c);
    uint32_t configure_landscape_images(
        uint32_t handle, uint32_t bitmap, uint32_t part_count,
        uint32_t parts_address, uint32_t image_step,
        uint32_t image_count, uint32_t images_address, uint32_t base);
    uint32_t configure_landscape_map(
        uint32_t handle, uint32_t columns, uint32_t rows,
        uint32_t grid_address, uint32_t base);
    uint32_t configure_landscape_guides(
        uint32_t handle, uint32_t bitmap, uint32_t count,
        uint32_t records_address, uint32_t base);
    uint32_t rebuild_landscape_cells(LandscapeObjectState& landscape);
    uint32_t set_landscape_cell_silhouette(
        uint32_t handle, uint32_t column, uint32_t row,
        uint32_t type, uint32_t level, uint32_t tail) {
        const auto landscape = landscape_objects_.find(handle);
        if (landscape == landscape_objects_.end() ||
            object_display_classes_.find(handle) ==
                object_display_classes_.end() ||
            object_display_classes_.at(handle) !=
                DisplayObjectClass::CDspObjLandscape) {
            return 0xFFU;
        }
        if (!landscape->second.map_ready) {
            return 0x0DU;
        }
        if (column >= landscape->second.columns) {
            return 0x0BU;
        }
        if (row >= landscape->second.rows) {
            return 0x0CU;
        }
        if (type >= 3) {
            return 0x12U;
        }
        if (level > 0x100U) {
            return 0x13U;
        }
        LandscapeObjectState::Cell& cell =
            landscape->second.cells[
                static_cast<std::size_t>(row) *
                    landscape->second.columns +
                column];
        cell.silhouette_type = type;
        cell.silhouette_level = level;
        cell.tail = tail;
        display_dirty_ = true;
        return 0;
    }
    uint32_t set_landscape_cell_image(
        uint32_t handle, uint32_t column, uint32_t row,
        int32_t image) {
        const auto landscape = landscape_objects_.find(handle);
        if (landscape == landscape_objects_.end() ||
            object_display_classes_.find(handle) ==
                object_display_classes_.end() ||
            object_display_classes_.at(handle) !=
                DisplayObjectClass::CDspObjLandscape) {
            return 0xFFU;
        }
        if (!landscape->second.map_ready) {
            return 0x0DU;
        }
        if (column >= landscape->second.columns) {
            return 0x0BU;
        }
        if (row >= landscape->second.rows) {
            return 0x0CU;
        }
        if (image < -1 ||
            (image >= 0 &&
             static_cast<std::size_t>(image) >=
                 landscape->second.images.size())) {
            return 0x0AU;
        }
        landscape->second.grid[
            static_cast<std::size_t>(row) *
                landscape->second.columns +
            column] = image;
        return rebuild_landscape_cells(landscape->second);
    }
    uint32_t set_landscape_cell_guide(
        uint32_t handle, uint32_t column, uint32_t row,
        uint32_t layer, uint32_t index, uint32_t value) {
        const auto landscape = landscape_objects_.find(handle);
        if (landscape == landscape_objects_.end() ||
            object_display_classes_.find(handle) ==
                object_display_classes_.end() ||
            object_display_classes_.at(handle) !=
                DisplayObjectClass::CDspObjLandscape) {
            return 0xFFU;
        }
        if (!landscape->second.map_ready) {
            return 0x0DU;
        }
        if (column >= landscape->second.columns) {
            return 0x0BU;
        }
        if (row >= landscape->second.rows) {
            return 0x0CU;
        }
        if (layer >= 4U) {
            return 0x10U;
        }
        if (index >= landscape->second.guides.size()) {
            return 0x11U;
        }
        LandscapeObjectState::Cell& cell =
            landscape->second.cells[
                static_cast<std::size_t>(row) *
                    landscape->second.columns +
                column];
        cell.guides_a[layer] = index;
        cell.guides_b[layer] = value;
        display_dirty_ = true;
        return 0;
    }
    uint32_t copy_landscape_part(
        uint32_t handle, uint32_t destination, uint32_t source) {
        const auto landscape = landscape_objects_.find(handle);
        if (landscape == landscape_objects_.end() ||
            object_display_classes_.find(handle) ==
                object_display_classes_.end() ||
            object_display_classes_.at(handle) !=
                DisplayObjectClass::CDspObjLandscape) {
            return 0xFFU;
        }
        if (landscape->second.images.empty()) {
            return 4;
        }
        if (destination >= landscape->second.parts.size()) {
            return 8;
        }
        if (source >= landscape->second.parts.size()) {
            return 9;
        }
        LandscapeObjectState::Part& target =
            landscape->second.parts[destination];
        const LandscapeObjectState::Part& input =
            landscape->second.parts[source];
        if (target.width != input.width ||
            target.height != input.height ||
            target.field_10 != input.field_10 ||
            target.format != input.format) {
            return 9;
        }
        target.source_x = input.source_x;
        target.source_y = input.source_y;
        target.clipped_x = input.clipped_x;
        target.clipped_y = input.clipped_y;
        target.clipped_width = input.clipped_width;
        target.clipped_height = input.clipped_height;
        display_dirty_ = true;
        return 0;
    }
    uint32_t get_landscape_cell_value(
        uint32_t handle, uint32_t column, uint32_t row,
        uint16_t& value) {
        const auto landscape = landscape_objects_.find(handle);
        if (landscape == landscape_objects_.end() ||
            object_display_classes_.find(handle) ==
                object_display_classes_.end() ||
            object_display_classes_.at(handle) !=
                DisplayObjectClass::CDspObjLandscape) {
            return 0xFFU;
        }
        if (!landscape->second.map_ready) {
            return 0x0DU;
        }
        if (column >= landscape->second.columns) {
            return 0x0BU;
        }
        if (row >= landscape->second.rows) {
            return 0x0CU;
        }
        const LandscapeObjectState::Cell& cell =
            landscape->second.cells[
                static_cast<std::size_t>(row) *
                    landscape->second.columns +
                column];
        if (cell.image < 0) {
            return 0x0EU;
        }
        value = static_cast<uint16_t>(cell.priority >> 16U);
        return 0;
    }
    uint32_t hit_test_landscape(
        uint32_t handle, uint32_t output, uint32_t alternate,
        uint32_t base) {
        const auto landscape = landscape_objects_.find(handle);
        if (landscape == landscape_objects_.end() ||
            object_display_classes_.find(handle) ==
                object_display_classes_.end() ||
            object_display_classes_.at(handle) !=
                DisplayObjectClass::CDspObjLandscape) {
            return 0xFFU;
        }
        if (!landscape->second.map_ready) {
            return 0x0DU;
        }
        int32_t origin_x = 0;
        int32_t origin_y = 0;
        if (const auto position = object_positions_.find(handle);
            position != object_positions_.end()) {
            origin_x = position->second.first;
            origin_y = position->second.second;
        } else if (const auto absolute =
                       object_absolute_positions_.find(handle);
                   absolute != object_absolute_positions_.end()) {
            origin_x = static_cast<int32_t>(absolute->second[0]) >> 16;
            origin_y = static_cast<int32_t>(absolute->second[1]) >> 16;
        }
        const auto [pointer_x, pointer_y] = pointer_position();
        const int32_t local_x = pointer_x - origin_x;
        const int32_t local_y = pointer_y - origin_y;
        (void)alternate;
        const int32_t cell_size =
            static_cast<int32_t>(landscape->second.cell_size);
        const int32_t row_span =
            static_cast<int32_t>(landscape->second.width);
        if (cell_size <= 0 || row_span <= 0) {
            return 0x0EU;
        }
        int32_t first_row =
            (local_y -
             static_cast<int32_t>(landscape->second.line_pitch)) /
                row_span +
            (static_cast<int32_t>(landscape->second.line_pitch) <=
                     local_y
                 ? 1
                 : 0);
        const int32_t lower_row = std::max(first_row, 0);
        int32_t upper_row = std::min(
            first_row +
                static_cast<int32_t>(
                    static_cast<uint64_t>(landscape->second.height) *
                    32U /
                    landscape->second.width),
            static_cast<int32_t>(landscape->second.rows) - 1);
        for (int32_t row = upper_row;
             row >= lower_row; --row) {
            const int32_t base_column = local_x / cell_size;
            const int32_t column =
                (row & 1) == 0
                    ? base_column
                    : base_column -
                          (local_x % cell_size <
                                   static_cast<int32_t>(
                                       landscape->second.cell_size / 2U)
                               ? 1
                               : 0);
            if (column < 0 ||
                column >= static_cast<int32_t>(
                              landscape->second.columns)) {
                continue;
            }
            const LandscapeObjectState::Cell& cell =
                landscape->second.cells[
                    static_cast<std::size_t>(row) *
                        landscape->second.columns +
                    static_cast<std::size_t>(column)];
            if (cell.image < 0 ||
                local_x < cell.bounds[0] ||
                local_y < cell.bounds[1] ||
                local_x > cell.bounds[2] ||
                local_y > cell.bounds[3]) {
                continue;
            }
            const LandscapeObjectState::Image& image =
                landscape->second.images[
                    static_cast<std::size_t>(cell.image)];
            if (image.alpha_channel < 0 ||
                image.alpha.size() !=
                    static_cast<std::size_t>(
                        image.width) * image.height) {
                continue;
            }
            const int64_t image_x =
                local_x - cell.bounds[0];
            const int64_t image_y =
                local_y - cell.bounds[1];
            if (image_x < 0 || image_y < 0 ||
                image_x >= image.width ||
                image_y >= image.height) {
                continue;
            }
            const uint8_t alpha = image.alpha[
                static_cast<std::size_t>(image_y) *
                    image.width +
                static_cast<std::size_t>(image_x)];
            if (alpha != 0) {
                memory_.write_u32(output, base, column);
                memory_.write_u32(output + 4U, base, row);
                return 0;
            }
        }
        return 0x0EU;
    }
    uint32_t copy_landscape_cell_image(
        uint32_t handle, uint32_t destination,
        uint32_t column, uint32_t row) {
        const auto landscape = landscape_objects_.find(handle);
        if (landscape == landscape_objects_.end() ||
            object_display_classes_.find(handle) ==
                object_display_classes_.end() ||
            object_display_classes_.at(handle) !=
                DisplayObjectClass::CDspObjLandscape) {
            return 0xFFU;
        }
        if (!landscape->second.map_ready) {
            return 0x0DU;
        }
        if (column >= landscape->second.columns) {
            return 0x0BU;
        }
        if (row >= landscape->second.rows) {
            return 0x0CU;
        }
        const LandscapeObjectState::Cell& cell =
            landscape->second.cells[
                static_cast<std::size_t>(row) *
                    landscape->second.columns +
                column];
        if (cell.image < 0) {
            return 0x0EU;
        }
        const LandscapeObjectState::Image& image =
            landscape->second.images[
                static_cast<std::size_t>(cell.image)];
        if (image.selected == 0 ||
            image.selected > image.count) {
            return 0x0EU;
        }
        const uint32_t part_index =
            image.record[image.selected];
        if (part_index >= landscape->second.parts.size() ||
            destination >= 0x4000U) {
            return 0x0FU;
        }
        const auto source =
            graphics_surfaces_.find(landscape->second.source_bitmap);
        if (source == graphics_surfaces_.end() ||
            !materialize_graphics_pixels(source->second)) {
            return 0x0FU;
        }
        const LandscapeObjectState::Part& part =
            landscape->second.parts[part_index];
        GraphicsSurface output;
        output.width = part.width;
        output.height = part.height;
        output.format = part.format;
        output.bytes_per_pixel =
            bitmap_bytes_per_pixel(output.format);
        if (output.width == 0 || output.height == 0 ||
            output.bytes_per_pixel == 0) {
            return 0x0FU;
        }
        output.stride = output.width * output.bytes_per_pixel;
        output.pixels.assign(
            static_cast<std::size_t>(output.stride) *
                output.height,
            0);
        const std::size_t row_bytes =
            static_cast<std::size_t>(part.clipped_width) *
            output.bytes_per_pixel;
        for (uint32_t y = 0; y < part.clipped_height; ++y) {
            const std::size_t source_offset =
                static_cast<std::size_t>(
                    part.source_y + part.clipped_y + y) *
                    source->second.stride +
                static_cast<std::size_t>(
                    part.source_x + part.clipped_x) *
                    output.bytes_per_pixel;
            const std::size_t destination_offset =
                static_cast<std::size_t>(
                    part.clipped_y + y) *
                    output.stride +
                static_cast<std::size_t>(part.clipped_x) *
                    output.bytes_per_pixel;
            if (source_offset + row_bytes >
                    source->second.pixels.size() ||
                destination_offset + row_bytes >
                    output.pixels.size()) {
                return 0x0FU;
            }
            std::copy_n(
                source->second.pixels.begin() +
                    static_cast<std::ptrdiff_t>(source_offset),
                row_bytes,
                output.pixels.begin() +
                    static_cast<std::ptrdiff_t>(
                        destination_offset));
        }
        if (output.format == 2U) {
            output.rgba.resize(
                static_cast<std::size_t>(
                    output.width) * output.height * 4U);
            for (std::size_t index = 0;
                 index < output.rgba.size() / 4U; ++index) {
                output.rgba[index * 4U] =
                    output.pixels[index * 4U + 2U];
                output.rgba[index * 4U + 1U] =
                    output.pixels[index * 4U + 1U];
                output.rgba[index * 4U + 2U] =
                    output.pixels[index * 4U];
                output.rgba[index * 4U + 3U] =
                    output.pixels[index * 4U + 3U];
            }
        }
        install_allocated_graphics_surface(destination, std::move(output));
        display_dirty_ = true;
        return 0;
    }
    uint32_t configure_landscape_object(
        uint32_t handle, int32_t x, int32_t y,
        uint32_t blend_mode, uint32_t effect_level,
        uint32_t priority_bias) {
        const auto landscape = landscape_objects_.find(handle);
        if (landscape == landscape_objects_.end() ||
            object_display_classes_.find(handle) ==
                object_display_classes_.end() ||
            object_display_classes_.at(handle) !=
                DisplayObjectClass::CDspObjLandscape) {
            return 0xFFU;
        }
        if (priority_bias >= 0x10000U) {
            return 0;
        }
        landscape->second.priority_bias = priority_bias;
        landscape->second.x = x;
        landscape->second.y = y;
        landscape->second.blend_mode = blend_mode;
        landscape->second.effect_level = effect_level;
        object_positions_[handle] = {x, y};
        display_dirty_ = true;
        return 0;
    }
    bool set_landscape_enabled(uint32_t handle, uint32_t enabled) {
        const auto display_class = object_display_classes_.find(handle);
        if (display_class == object_display_classes_.end() ||
            display_class->second != DisplayObjectClass::CDspObjLandscape ||
            landscape_objects_.count(handle) == 0) {
            return false;
        }
        LandscapeObjectState& landscape = landscape_objects_.at(handle);
        const bool previous_visible = landscape.visible;
        object_enabled_[handle] = enabled;
        landscape.visible =
            enabled != 0 &&
            object_base_draw_enabled(handle) != 0 &&
            object_visibility_threshold(handle) < 0x100U;
        if (landscape.visible != previous_visible) {
            display_dirty_ = true;
        }
        return true;
    }
    bool destroy_factory_object(FactoryObjectKind kind, uint32_t handle);
    bool release_object_handle(uint32_t handle);
    [[nodiscard]] const std::map<uint32_t, LandscapeObjectState>&
    landscape_objects() const {
        return landscape_objects_;
    }
    struct IconImageRecord {
        int32_t source = -1;
        GraphicsSurface image;
    };
    bool configure_icon_image_list(
        uint32_t count, uint32_t ids_address, uint32_t base) {
        icon_invalid_source_ = -1;
        if (count <= 1U) {
            icon_images_.clear();
            return true;
        }
        std::vector<IconImageRecord> images;
        images.reserve(count);
        for (uint32_t index = 0; index < count; ++index) {
            const int32_t id = static_cast<int32_t>(
                memory_.read_u32(
                    ids_address + index * 4U, base));
            IconImageRecord record;
            record.source = id;
            if (id == -1) {
                images.push_back(std::move(record));
                continue;
            }
            const auto source =
                graphics_surfaces_.find(static_cast<uint32_t>(id));
            if (source == graphics_surfaces_.end()) {
                icon_invalid_source_ = id;
                return false;
            }
            GraphicsSurface copy = source->second;
            if (!materialize_graphics_pixels(copy)) {
                icon_invalid_source_ = id;
                return false;
            }
            record.image = std::move(copy);
            images.push_back(std::move(record));
        }
        icon_images_ = std::move(images);
        return true;
    }
    [[nodiscard]] const std::vector<IconImageRecord>&
    icon_images() const {
        return icon_images_;
    }
    [[nodiscard]] int32_t icon_invalid_source() const {
        return icon_invalid_source_;
    }
    uint32_t link_display_object(uint32_t destination,
                                 uint32_t source) {
        if ((destination & 0xFF000000U) != 0x80000000U ||
            display_objects_.count(destination) == 0) {
            return 0xFFU;
        }
        if (destination == source ||
            (source != 0 &&
             ((source & 0xFF000000U) != 0x80000000U ||
              display_objects_.count(source) == 0))) {
            return 0x0BU;
        }
        if (source == 0) {
            if (display_object_links_.erase(destination) == 0) {
                return 0x0EU;
            }
            return 0;
        }
        if (display_object_links_.count(destination) != 0) {
            return 0x0DU;
        }
        display_object_links_[destination] = source;
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const std::map<uint32_t, uint32_t>&
    display_object_links() const {
        return display_object_links_;
    }
    struct CompositeEffectState {
        std::vector<uint32_t> sources;
        uint32_t first_bitmap = 0;
        uint32_t second_bitmap = 0;
        uint32_t mask_bitmap = 0xFFFFFFFFU;
        uint32_t mode = 0;
        uint32_t vertex_count = 0;
        uint32_t vertex_mode = 0;
        uint32_t field_170 = 0;
        uint32_t bitmap = 0;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t value = 0;
        uint32_t field_158 = 0;
        uint32_t field_15c = 0;
        uint32_t field_160 = 0;
    };
    uint32_t configure_composite_effect_sources(
        uint32_t count, uint32_t ids_address, uint32_t base) {
        if (count < 2U || count > 32U) {
            return 1;
        }
        std::vector<uint32_t> sources;
        sources.reserve(count);
        for (uint32_t index = 0; index < count; ++index) {
            const uint32_t bitmap = memory_.read_u32(
                ids_address + index * 4U, base);
            if (graphics_surfaces_.count(bitmap) == 0) {
                return 2;
            }
            sources.push_back(bitmap);
        }
        composite_effect_.sources = std::move(sources);
        display_dirty_ = true;
        return 0;
    }
    uint32_t configure_composite_effect_pair(
        uint32_t first, uint32_t second, uint32_t mask,
        uint32_t field) {
        const auto first_surface = graphics_surfaces_.find(first);
        if (first_surface == graphics_surfaces_.end()) {
            return 1;
        }
        const auto second_surface = graphics_surfaces_.find(second);
        if (second_surface == graphics_surfaces_.end()) {
            return 3;
        }
        if (first_surface->second.format !=
            second_surface->second.format) {
            return 4;
        }
        if (mask != 0xFFFFFFFFU &&
            graphics_surfaces_.count(mask) == 0) {
            return 5;
        }
        composite_effect_.first_bitmap = first;
        composite_effect_.second_bitmap = second;
        composite_effect_.mask_bitmap = mask;
        composite_effect_.field_158 = field;
        display_dirty_ = true;
        return 0;
    }
    uint32_t configure_composite_effect_mode(
        uint32_t bitmap, uint32_t mode) {
        if (graphics_surfaces_.count(bitmap) == 0) {
            return 1;
        }
        if (mode >= 2U) {
            return 3;
        }
        composite_effect_.bitmap = bitmap;
        composite_effect_.mode = mode;
        display_dirty_ = true;
        return 0;
    }
    uint32_t configure_composite_effect_vertices(
        uint32_t bitmap, uint32_t count, uint32_t mode,
        uint32_t field) {
        if (graphics_surfaces_.count(bitmap) == 0) {
            return 3;
        }
        if (mode >= 8U) {
            return 7;
        }
        if (count == 0) {
            return 5;
        }
        composite_effect_.bitmap = bitmap;
        composite_effect_.vertex_count = count;
        composite_effect_.vertex_mode = mode;
        composite_effect_.field_170 = field;
        display_dirty_ = true;
        return 0;
    }
    uint32_t configure_composite_effect_rectangle(
        uint32_t bitmap, int32_t x, int32_t y,
        uint32_t width, uint32_t height) {
        if (graphics_surfaces_.count(bitmap) == 0) {
            return 1;
        }
        if (width <= 1U || height <= 1U) {
            return 2;
        }
        composite_effect_.bitmap = bitmap;
        composite_effect_.x = x;
        composite_effect_.y = y;
        composite_effect_.width = width;
        composite_effect_.height = height;
        display_dirty_ = true;
        return 0;
    }
    uint32_t configure_composite_effect_value(
        uint32_t bitmap, uint32_t value) {
        if (graphics_surfaces_.count(bitmap) == 0) {
            return 1;
        }
        if (value == 0) {
            return 2;
        }
        composite_effect_.bitmap = bitmap;
        composite_effect_.value = value;
        composite_effect_.field_15c = 0;
        composite_effect_.field_160 = 0;
        display_dirty_ = true;
        return 0;
    }
    uint32_t configure_composite_effect_pair_state(
        uint32_t first, uint32_t second, uint32_t first_flag,
        uint32_t second_flag) {
        if (graphics_surfaces_.count(first) == 0) {
            return 1;
        }
        if (second != 0x7000U && second != 0x7001U &&
            graphics_surfaces_.count(second) == 0) {
            return 2;
        }
        if (first_flag >= 2U) {
            return 3;
        }
        if (second_flag >= 2U) {
            return 4;
        }
        composite_effect_.first_bitmap = first;
        composite_effect_.second_bitmap = second;
        composite_effect_.field_15c = first_flag;
        composite_effect_.field_160 = second_flag;
        display_dirty_ = true;
        return 0;
    }
    [[nodiscard]] const CompositeEffectState&
    composite_effect() const {
        return composite_effect_;
    }
    uint32_t check_resource_alternatives(
        uint32_t base_path_address, uint32_t list_address,
        uint32_t output_address, uint32_t base) {
        const std::string base_path =
            base_path_address == 0
                ? std::string()
                : memory_.read_cstring(base_path_address, base);
        const std::string list =
            list_address == 0
                ? std::string()
                : memory_.read_cstring(list_address, base);
        std::size_t begin = 0;
        while (begin <= list.size()) {
            const std::size_t slash = list.find('/', begin);
            const std::size_t end =
                slash == std::string::npos ? list.size() : slash;
            std::size_t token_begin = begin;
            std::size_t token_end = end;
            while (token_begin < token_end &&
                   list[token_begin] == ' ') {
                ++token_begin;
            }
            const std::size_t comma =
                list.find_first_of(",>", token_begin);
            if (comma != std::string::npos &&
                comma < token_end) {
                token_end = comma;
            }
            while (token_end > token_begin &&
                   list[token_end - 1] == ' ') {
                --token_end;
            }
            if (token_end > token_begin) {
                const std::string resource = list.substr(
                    token_begin, token_end - token_begin);
                if (read_binary_source(base_path, resource)) {
                    if (output_address != 0) {
                        memory_.write_cstring(
                            output_address, resource, base);
                    }
                    return 0;
                }
            }
            if (slash == std::string::npos) {
                break;
            }
            begin = slash + 1U;
        }
        return 1;
    }

    bool set_display_constant(uint32_t key, uint32_t value) {
        if (key == 0x01220000U) {
            display_constant_1220000_ = value;
            return true;
        }
        if (key == 0x80111600U) {
            display_constant_80111600_ = value;
            return true;
        }
        return false;
    }
    [[nodiscard]] std::map<uint32_t, uint32_t> display_constants() const {
        return {
            {0x01220000U, display_constant_1220000_},
            {0x80111600U, display_constant_80111600_},
        };
    }

    // Original external-path registries at 0x00541C28/0x00540F18.
    void mount_external_path(std::string prefix, std::string path) {
        external_mounts_[std::move(prefix)] = std::move(path);
    }
    [[nodiscard]] const std::map<std::string, std::string>&
    external_mounts() const {
        return external_mounts_;
    }

    void set_primary_directory(std::string value) {
        primary_directory_ = std::move(value);
    }
    void set_secondary_directory(std::string value) {
        secondary_directory_ = std::move(value);
    }
    [[nodiscard]] const std::string& primary_directory() const {
        return primary_directory_;
    }
    [[nodiscard]] const std::string& secondary_directory() const {
        return secondary_directory_;
    }
    [[nodiscard]] const std::string& current_directory() const {
        return current_directory_;
    }
    void set_current_directory(std::string value) {
        current_directory_ = std::move(value);
    }
    void set_directory_probe_path(std::string value) {
        directory_probe_path_ = std::move(value);
    }
    [[nodiscard]] const std::string& directory_probe_path() const {
        return directory_probe_path_;
    }
    void set_archive_search_state(
        std::string source, std::string archive_name) {
        archive_source_path_529c50_ = std::move(source);
        archive_name_529f60_ = std::move(archive_name);
    }
    [[nodiscard]] const std::string& archive_source_path() const {
        return archive_source_path_529c50_;
    }
    [[nodiscard]] const std::string& archive_name() const {
        return archive_name_529f60_;
    }
    void record_module_request(ModuleRequest request) {
        module_requests_.push_back(std::move(request));
    }
    [[nodiscard]] const std::vector<ModuleRequest>& module_requests() const {
        return module_requests_;
    }
    bool load_module(uint32_t index, std::string prefix,
                     std::string path) {
        if (index >= modules_.size() || path.empty()) {
            return false;
        }
        modules_[index].reset();
        std::optional<std::vector<uint8_t>> bytes;
        if (!prefix.empty() && resource_loader_) {
            try {
                bytes = resource_loader_(prefix, path);
            } catch (const std::exception&) {
                bytes.reset();
            }
        } else {
            bytes = read_virtual_file(path);
            if (!bytes && resource_loader_) {
                try {
                    bytes = resource_loader_("", path);
                } catch (const std::exception&) {
                    bytes.reset();
                }
            }
        }
        if (!bytes || bytes->empty()) {
            record_module_request(
                {index, std::move(prefix), std::move(path), 0, false});
            return false;
        }
        const std::size_t byte_count = bytes->size();
        modules_[index] = LoadedModule{
            path, std::move(*bytes)};
        record_module_request(
            {index, std::move(prefix), std::move(path),
             byte_count, true});
        return true;
    }
    [[nodiscard]] const std::optional<LoadedModule>& module(
        uint32_t index) const {
        static const std::optional<LoadedModule> empty;
        return index < modules_.size() ? modules_[index] : empty;
    }
    bool release_module(uint32_t index) {
        if (index >= modules_.size() || !modules_[index]) {
            return false;
        }
        modules_[index].reset();
        return true;
    }
    uint64_t data_stack_limit() const {
        uint32_t available = thread_.data_capacity;
        for (const auto& allocation : thread_.data_allocations) {
            available = std::min(available, allocation.offset);
        }
        return static_cast<uint64_t>(thread_.data_base) + available;
    }
    void push_physical_return(uint32_t return_ip) {
        if (static_cast<uint64_t>(thread_.base) + 4U >
            data_stack_limit()) {
            throw BpFault("call stack exhausted");
        }
        const uint32_t slot = thread_.base;
        const uint32_t physical_ip = return_ip +
            (has_active_code_resource_ ? active_code_resource_base_ : 0U);
        memory_.write_u32(Memory::kFrameTag | slot, 0, physical_ip);
        std::erase_if(thread_.frame_stack, [slot](const CallFrame& frame) {
            return frame.return_slot == slot;
        });
        thread_.frame_stack.push_back(
            {return_ip, has_active_code_resource_, active_code_resource_base_, slot});
        thread_.base += 4U;
    }
    CallFrame pop_physical_return() {
        thread_.base -= 4U;
        const uint32_t saved_ip =
            memory_.read_u32(Memory::kFrameTag | thread_.base, 0);
        CallFrame result{saved_ip, has_active_code_resource_,
                         active_code_resource_base_, thread_.base};
        for (auto it = thread_.frame_stack.end(); it != thread_.frame_stack.begin();) {
            --it;
            if (it->return_slot == thread_.base) {
                result.has_code_resource = it->has_code_resource;
                result.code_resource_base = it->code_resource_base;
                thread_.frame_stack.erase(it);
                break;
            }
        }
        return result;
    }
    void resume_physical_return(const CallFrame& frame) {
        has_active_code_resource_ = false;
        active_code_resource_base_ = 0;
        memory_.set_active_code({});
        thread_.ip = frame.ip;
        const auto use_resource = [&](uint32_t base) {
            const auto found = resource_segments_.find(base);
            if (found == resource_segments_.end() || frame.ip < base ||
                frame.ip - base >= found->second.code_size) return false;
            has_active_code_resource_ = true;
            active_code_resource_base_ = base;
            thread_.ip = frame.ip - base;
            memory_.set_active_code(
                std::span<const uint8_t>(found->second.bytes).subspan(
                    found->second.code_offset, found->second.code_size), base);
            return true;
        };
        if (!(frame.has_code_resource && use_resource(frame.code_resource_base)) &&
            frame.ip >= memory_.code().size()) {
            const auto upper = resource_segments_.upper_bound(frame.ip);
            if (upper != resource_segments_.begin()) use_resource(std::prev(upper)->first);
        }
        sync_code_fallbacks();
    }
    bool execute_module(uint32_t index) {
        if (index >= modules_.size() || !modules_[index]) {
            return false;
        }
        // PE64 0x1401D3568: header {16,16}, then an EE relative call
        // to the appended module and FF:F8 to retire both program records.
        // The older engine uses its 06/16 push-address/call gate instead.
        static constexpr std::array<uint8_t, 16> legacy_trampoline{
            0x06, 0x10, 0x00, 0x16, 0xFF, 0xF8, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        static constexpr std::array<uint8_t, 16> compact64_trampoline{
            0xEE, 0x10, 0x00, 0xFF, 0xF8, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        const auto& trampoline = compact_64_opcodes_
            ? compact64_trampoline : legacy_trampoline;
        const LoadedModule& selected = *modules_[index];
        if (selected.bytes.size() < 8U) {
            throw BpFault("module payload is too small");
        }
        const auto word = [&selected](std::size_t offset) {
            return static_cast<uint32_t>(selected.bytes[offset]) |
                   (static_cast<uint32_t>(selected.bytes[offset + 1U])
                    << 8U) |
                   (static_cast<uint32_t>(selected.bytes[offset + 2U])
                    << 16U) |
                   (static_cast<uint32_t>(selected.bytes[offset + 3U])
                    << 24U);
        };
        const uint32_t code_offset = word(0);
        const uint32_t code_size = word(4);
        // 0x0044BF30 copies the code span selected by the first two
        // payload dwords. The complete resource remains in the module slot.
        if (static_cast<uint64_t>(code_offset) + code_size >
            selected.bytes.size()) {
            throw BpFault("module code range is outside payload");
        }
        const std::span<const uint8_t> caller_code = current_code();
        std::vector<uint8_t> caller_copy(
            caller_code.begin(), caller_code.end());
        const std::span<const uint8_t> root_code = memory_.code();
        std::vector<uint8_t> root_code_before(
            root_code.begin(), root_code.end());
        const std::size_t trampoline_offset = caller_copy.size();
        push_physical_return(thread_.ip);
        module_frames_.push_back(
            {std::move(root_code_before)});
        std::vector<uint8_t> combined =
            std::move(caller_copy);
        combined.insert(
            combined.end(), trampoline.begin(), trampoline.end());
        combined.insert(
            combined.end(),
            selected.bytes.begin() + code_offset,
            selected.bytes.begin() + code_offset + code_size);
        memory_.set_code(std::move(combined));
        has_active_code_resource_ = false;
        active_code_resource_base_ = 0;
        memory_.set_active_code({});
        sync_code_fallbacks();
        thread_.ip = static_cast<uint32_t>(trampoline_offset);
        return true;
    }
    bool return_mediated_module() {
        if (module_frames_.empty()) {
            return false;
        }
        const ModuleExecutionFrame frame = module_frames_.back();
        module_frames_.pop_back();
        // 004A64B0 unloads two program records with 0044BFD0, then pops
        // ONE saved return IP with 0044C370. The module's ordinary BP
        // return already consumed the trampoline call's return frame.
        const CallFrame caller = pop_physical_return();
        memory_.set_code(frame.root_code_before);
        resume_physical_return(caller);
        return true;
    }
    struct MemoryBoundary {
        uint32_t address = 0;
        uint32_t length = 0;
        std::string label;
    };
    bool register_memory_boundary(uint32_t address, uint32_t length,
                                  std::string label) {
        if (length == 0) {
            return false;
        }
        memory_boundaries_.push_back(
            {address, length, std::move(label)});
        return true;
    }
    [[nodiscard]] const std::vector<MemoryBoundary>&
    memory_boundaries() const {
        return memory_boundaries_;
    }
    struct DebugVariableTable {
        std::vector<uint32_t> values;
        std::vector<uint8_t> flags;
    };
    void register_debug_variable_table(uint8_t table, uint32_t count) {
        DebugVariableTable state;
        state.values.resize(count);
        state.flags.resize(count);
        debug_variable_tables_[table] = std::move(state);
    }
    uint32_t set_debug_variable(uint32_t key, uint32_t value) {
        const uint8_t table = static_cast<uint8_t>(key >> 8U);
        const uint8_t index = static_cast<uint8_t>(key);
        const auto found = debug_variable_tables_.find(table);
        if (found == debug_variable_tables_.end()) {
            return 3;
        }
        if (index >= found->second.values.size()) {
            return 4;
        }
        found->second.values[index] = value;
        return 0;
    }
    uint32_t set_debug_variable_flags(uint32_t key, uint32_t value) {
        const uint8_t table = static_cast<uint8_t>(key >> 8U);
        const uint8_t index = static_cast<uint8_t>(key);
        const auto found = debug_variable_tables_.find(table);
        if (found == debug_variable_tables_.end()) return 3;
        if (index >= found->second.flags.size()) return 4;
        // Original00466490 stores a DWORD. The current snapshot model retains
        // byte flags; reject the unrepresented high bits rather than lose them.
        if (value > 0xFFU) throw BpFault("wide debug-variable metadata flags unsupported");
        found->second.flags[index] = static_cast<uint8_t>(value);
        return 0;
    }
    uint32_t enumerate_debug_variables(uint32_t destination,
                                       uint32_t base) {
        uint32_t count = 0;
        for (const auto& [table_id, table] : debug_variable_tables_) {
            for (std::size_t index = 0; index < table.values.size();
                 ++index) {
                if (table.values[index] == 0 ||
                    (table.flags[index] & 1U) != 0) {
                    continue;
                }
                if (destination != 0) {
                    const uint32_t address =
                        destination + count * 8U;
                    memory_.write_u32(
                        address, base,
                        (static_cast<uint32_t>(table_id) << 8U) |
                            static_cast<uint32_t>(index));
                    memory_.write_u32(address + 4, base,
                                      table.values[index]);
                }
                ++count;
            }
        }
        return count;
    }
    void clear_debug_variable_values() {
        for (auto& [table_id, table] : debug_variable_tables_) {
            (void)table_id;
            std::fill(table.values.begin(), table.values.end(), 0);
        }
    }
    void set_debug_variable_global(uint32_t value) {
        debug_variable_global_ = value;
    }
    [[nodiscard]] uint32_t debug_variable_global() const {
        return debug_variable_global_;
    }
    void set_window_minimized(bool value) {
        window_minimized_ = value;
    }
    [[nodiscard]] bool window_minimized() const {
        return window_minimized_;
    }
    void minimize_window() {
        window_minimized_ = true;
        if (minimize_handler_) {
            minimize_handler_();
        }
    }
    void request_engine_exit() { engine_exit_requested_ = true; }
    [[nodiscard]] bool engine_exit_requested() const { return engine_exit_requested_; }
    void request_window_close() {
        if (close_request_handler_) {
            close_request_handler_();
        }
    }
    void set_drag_accept(uint32_t value) {
        const bool enabled = value != 0;
        drag_accept_enabled_ = enabled;
        if (drag_accept_handler_) {
            drag_accept_handler_(enabled);
        }
        dropped_file_path_.clear();
    }
    [[nodiscard]] bool drag_accept_enabled() const {
        return drag_accept_enabled_;
    }
    bool set_dropped_file_path(std::string_view path) {
        if (path.size() >= 0x30CU) {
            return false;
        }
        dropped_file_path_ = path;
        return true;
    }
    [[nodiscard]] const std::string& dropped_file_path() const {
        return dropped_file_path_;
    }
    uint32_t query_dropped_file_path(uint32_t destination, uint32_t base) {
        if (!drag_accept_enabled_ || dropped_file_path_.empty()) {
            return 0;
        }
        memory_.write_cstring(destination, dropped_file_path_, base);
        return 1;
    }
    void set_pointer_position(int32_t x, int32_t y, bool available = true) {
        pointer_x_ = x;
        pointer_y_ = y;
        pointer_available_ = available;
    }
    [[nodiscard]] std::pair<int32_t, int32_t> pointer_position() const {
        return pointer_available_ ? std::pair<int32_t, int32_t>{
                                        pointer_x_, pointer_y_}
                                  : std::pair<int32_t, int32_t>{0, 0};
    }
    void set_cursor_visible(bool value) { cursor_visible_ = value; }
    [[nodiscard]] bool cursor_visible() const { return cursor_visible_; }
    bool set_custom_cursor(uint32_t image, int32_t x, int32_t y) {
        if (image == 0) {
            custom_cursor_image_ = 0;
            return true;
        }
        if (graphics_surfaces_.count(image) == 0) {
            return false;
        }
        custom_cursor_image_ = image;
        custom_cursor_x_ = x;
        custom_cursor_y_ = y;
        return true;
    }
    void clear_custom_cursor() { custom_cursor_image_ = 0; }
    [[nodiscard]] uint32_t custom_cursor_image() const {
        return custom_cursor_image_;
    }
    [[nodiscard]] int32_t custom_cursor_x() const { return custom_cursor_x_; }
    [[nodiscard]] int32_t custom_cursor_y() const { return custom_cursor_y_; }
    void set_external_path(std::string value) {
        external_path_542cac_ = std::move(value);
    }
    [[nodiscard]] const std::string& external_path() const {
        return external_path_542cac_;
    }
    void set_wallpaper_callback(
        std::function<void(std::string_view, uint32_t, uint32_t)> callback) {
        wallpaper_callback_ = std::move(callback);
    }
    void set_desktop_wallpaper(std::string path, uint32_t style,
                               uint32_t tiled) {
        wallpaper_path_ = std::move(path);
        wallpaper_style_ = style != 0 ? 2U : 0U;
        wallpaper_tiled_ = tiled != 0 ? 1U : 0U;
        if (wallpaper_callback_) {
            wallpaper_callback_(wallpaper_path_, wallpaper_style_,
                                wallpaper_tiled_);
        }
    }
    [[nodiscard]] const std::string& wallpaper_path() const {
        return wallpaper_path_;
    }
    [[nodiscard]] uint32_t wallpaper_style() const {
        return wallpaper_style_;
    }
    [[nodiscard]] uint32_t wallpaper_tiled() const {
        return wallpaper_tiled_;
    }
    bool set_text_encoding_mode(uint32_t mode) {
        if (mode == 0) {
            text_encoding_mode_ = 0;
            text_code_page_ = 932;
            return true;
        }
        if (mode == 1) {
            text_encoding_mode_ = 1;
            text_code_page_ = 65001;
            return true;
        }
        return false;
    }
    [[nodiscard]] uint32_t text_encoding_mode() const {
        return text_encoding_mode_;
    }
    [[nodiscard]] uint32_t text_code_page() const {
        return text_code_page_;
    }
    struct Grp1TextStyle {
        std::string font_name;
        uint32_t field_5328bc = 0;
        uint32_t field_533218 = 0;
        uint32_t field_532838 = 0;
        uint32_t field_53283c = 0;
        uint32_t field_529bf8 = 0;
        uint32_t field_529bfc = 0;
    };
    bool set_grp1_text_style_fields(
        uint32_t font_id, uint32_t field_5328bc,
        uint32_t field_533218, uint32_t field_532838,
        uint32_t field_53283c, uint32_t field_529bf8,
        uint32_t field_529bfc) {
        const std::string name = font_name(font_id);
        if (name.size() > 0xFFU) {
            return false;
        }
        grp1_text_style_.font_name = name;
        grp1_text_style_.field_5328bc = field_5328bc;
        grp1_text_style_.field_533218 = field_533218;
        grp1_text_style_.field_532838 = field_532838;
        grp1_text_style_.field_53283c = field_53283c;
        grp1_text_style_.field_529bf8 = field_529bf8;
        grp1_text_style_.field_529bfc = field_529bfc;
        return true;
    }
    bool set_grp1_text_style(uint32_t font_id, uint32_t field_5328bc,
                             uint32_t field_533218,
                             uint32_t field_532838,
                             uint32_t field_53283c) {
        return set_grp1_text_style_fields(
            font_id, field_5328bc, field_533218, field_532838,
            field_53283c, 0xFFFFFFFFU, 0xFFFFFFFFU);
    }
    [[nodiscard]] const Grp1TextStyle& grp1_text_style() const {
        return grp1_text_style_;
    }
    void clear_rich_text_aliases() {
        rich_text_aliases_.clear();
    }
    void erase_rich_text_alias(const std::string& key) {
        rich_text_aliases_.erase(key);
    }
    void set_rich_text_alias(std::string key, std::string value) {
        rich_text_aliases_[std::move(key)] = std::move(value);
    }
    [[nodiscard]] const std::map<std::string, std::string>&
    rich_text_aliases() const {
        return rich_text_aliases_;
    }
    using TextCacheRecord = std::array<uint8_t, 0x80>;
    bool append_text_cache_record(std::string_view text, uint32_t first,
                                  uint32_t second) {
        if (text_cache_count_ >= text_cache_storage_.size()) {
            return false;
        }
        TextCacheRecord& record = text_cache_storage_[text_cache_count_];
        std::fill_n(record.begin(), 0x60, 0);
        const std::size_t length =
            std::min<std::size_t>(text.size(), 0x5FU);
        std::copy_n(
            reinterpret_cast<const uint8_t*>(text.data()), length,
            record.begin());
        const auto write_word =
            [&record](std::size_t offset, uint32_t value) {
                for (uint32_t index = 0; index < 4; ++index) {
                    record[offset + index] =
                        static_cast<uint8_t>(value >> (index * 8U));
                }
            };
        write_word(0x78, first);
        write_word(0x7C, second);
        ++text_cache_count_;
        return true;
    }
    uint32_t take_text_cache_records(uint32_t destination, uint32_t base) {
        const uint32_t count = text_cache_count_;
        if (destination != 0) {
            for (std::size_t index = 0; index < count; ++index) {
                memory_.write_bytes(
                    destination + static_cast<uint32_t>(index * 0x80U),
                    text_cache_storage_[index], base);
            }
        }
        text_cache_count_ = 0;
        return count;
    }
    [[nodiscard]] uint32_t text_cache_record_count() const {
        return text_cache_count_;
    }
    bool set_clock_wrap_span(uint32_t value) {
        if (value < 50U || value > 60000U) {
            return false;
        }
        clock_wrap_span_ms_ = value;
        return true;
    }
    [[nodiscard]] uint32_t clock_wrap_span_ms() const {
        return clock_wrap_span_ms_;
    }
    void set_mouse_preset(uint32_t index, int32_t x, int32_t y) {
        if (index < mouse_presets_.size()) {
            mouse_presets_[index] = {x, y};
        }
    }
    uint32_t query_mouse_preset(uint32_t index, uint32_t destination,
                                uint32_t base) {
        if (index >= mouse_presets_.size()) {
            return 0;
        }
        memory_.write_u32(destination, base,
                          static_cast<uint32_t>(mouse_presets_[index].first));
        memory_.write_u32(
            destination + 4, base,
            static_cast<uint32_t>(mouse_presets_[index].second));
        return 1;
    }
    struct PointerMotionState {
        int32_t start_x = 0;
        int32_t start_y = 0;
        int32_t delta_x = 0;
        int32_t delta_y = 0;
        uint32_t curve = 0;
        uint32_t frame_index = 0;
        uint32_t frame_count = 0;
        uint32_t duration = 0;
        uint32_t rate = 0;
        uint32_t start_time = 0;
        uint32_t next_frame_time = 0;
        uint32_t stop_outside = 0;
        bool active = false;
    };
    bool start_pointer_motion(int32_t target_x, int32_t target_y,
                              uint32_t curve, uint32_t duration,
                              uint32_t rate, uint32_t stop_outside,
                              uint32_t now_ms) {
        if (window_minimized_) {
            return false;
        }
        const auto [start_x, start_y] = pointer_position();
        const uint32_t raw_frames = (duration * rate) / 1000U;
        const uint32_t frame_count = raw_frames + (raw_frames == 0 ? 1U : 0U);
        pointer_motion_.start_x = start_x;
        pointer_motion_.start_y = start_y;
        pointer_motion_.delta_x =
            static_cast<int32_t>(static_cast<uint32_t>(target_x) -
                                 static_cast<uint32_t>(start_x));
        pointer_motion_.delta_y =
            static_cast<int32_t>(static_cast<uint32_t>(target_y) -
                                 static_cast<uint32_t>(start_y));
        pointer_motion_.curve = curve;
        pointer_motion_.frame_index = 0;
        pointer_motion_.frame_count = frame_count;
        pointer_motion_.duration = duration;
        pointer_motion_.rate = rate;
        pointer_motion_.start_time = now_ms;
        pointer_motion_.next_frame_time =
            now_ms + duration / frame_count;
        pointer_motion_.stop_outside = stop_outside;
        pointer_motion_.active = true;
        return true;
    }
    [[nodiscard]] const PointerMotionState& pointer_motion() const {
        return pointer_motion_;
    }
    struct PointerHistoryRecord {
        std::array<uint32_t, 6> fields{};
    };
    bool configure_pointer_history(uint32_t limit, int32_t interval) {
        if (limit >= 0x201U) {
            return false;
        }
        pointer_history_.clear();
        pointer_history_limit_ = limit;
        pointer_history_interval_ = interval;
        return true;
    }
    void append_pointer_history(
        const std::array<uint32_t, 6>& fields) {
        if (pointer_history_.size() >= pointer_history_limit_) {
            return;
        }
        pointer_history_.push_back({fields});
    }
    uint32_t query_pointer_history(uint32_t points,
                                   uint32_t angles,
                                   uint32_t start,
                                   uint32_t count,
                                   uint32_t base) {
        if (start >= pointer_history_.size()) {
            return 0;
        }
        uint32_t written = 0;
        for (std::size_t index = start;
             index < pointer_history_.size() && written < count;
             ++index, ++written) {
            const auto& current = pointer_history_[index];
            memory_.write_u32(
                points + written * 8U, base, current.fields[0]);
            memory_.write_u32(
                points + written * 8U + 4U, base, current.fields[1]);
            int32_t angle = -1;
            if (index + 1 < pointer_history_.size()) {
                const auto& next = pointer_history_[index + 1];
                const int32_t dx = static_cast<int32_t>(
                    current.fields[0] - next.fields[0]);
                const int32_t dy = static_cast<int32_t>(
                    current.fields[1] - next.fields[1]);
                double degrees =
                    std::atan2(static_cast<double>(dy),
                               static_cast<double>(dx)) *
                    180.0 / std::numbers::pi;
                if (degrees < 0.0) {
                    degrees += 360.0;
                }
                angle = static_cast<int32_t>(degrees * 65536.0);
            }
            memory_.write_u32(
                angles + written * 4U, base,
                static_cast<uint32_t>(angle));
        }
        return written;
    }
    uint32_t enumerate_pointer_history(uint32_t destination,
                                       uint32_t base) {
        for (std::size_t index = 0; index < pointer_history_.size();
             ++index) {
            if (destination == 0) {
                continue;
            }
            for (std::size_t field = 0; field < 6; ++field) {
                memory_.write_u32(
                    destination +
                        static_cast<uint32_t>((index * 6U + field) * 4U),
                    base, pointer_history_[index].fields[field]);
            }
        }
        return static_cast<uint32_t>(pointer_history_.size());
    }
    void set_memory_access_variant(bool enabled) {
        memory_access_variant_ = enabled;
    }
    [[nodiscard]] bool memory_access_variant() const {
        return memory_access_variant_;
    }
    struct CharMapRecord {
        uint32_t bit_length = 0;
        std::vector<uint8_t> bits;
    };
    [[nodiscard]] std::span<const uint8_t> gdb_metadata() const {
        return memory_.frame().first(std::min<std::size_t>(0x400, memory_.frame().size()));
    }
    [[nodiscard]] const std::vector<uint8_t>& gdb_character_data() const {
        return gdb_character_data_;
    }
    void set_gdb_metadata(std::vector<uint8_t> data) {
        data.resize(0x400, 0);
        if (memory_.frame().size() < data.size()) {
            throw BpFault("GDB metadata exceeds runtime buffer");
        }
        std::copy(data.begin(), data.end(), memory_.frame().begin());
    }
    void set_gdb_character_data(std::vector<uint8_t> data) {
        data.resize(0x100000, 0);
        if (data.size() > 0x100000) {
            data.resize(0x100000);
        }
        gdb_character_data_ = std::move(data);
    }
    [[nodiscard]] const std::map<std::string, CharMapRecord>&
    char_maps() const {
        return char_maps_;
    }
    bool load_gdb_payload(
        std::span<const uint8_t> decoded,
        uint32_t metadata_destination, uint32_t metadata_size_destination,
        uint32_t strings_destination, uint32_t strings_size_destination,
        uint32_t base, bool replace_character_maps = false) {
        static constexpr std::string_view magic = "BURIKO GDB 3.00";
        if (decoded.size() < 0x20 ||
            !std::equal(magic.begin(), magic.end(), decoded.begin())) {
            return false;
        }
        const auto read_u32 = [&decoded](std::size_t offset)
            -> std::optional<uint32_t> {
            if (offset > decoded.size() ||
                decoded.size() - offset < 4) {
                return std::nullopt;
            }
            return static_cast<uint32_t>(decoded[offset]) |
                   (static_cast<uint32_t>(decoded[offset + 1]) << 8U) |
                   (static_cast<uint32_t>(decoded[offset + 2]) << 16U) |
                   (static_cast<uint32_t>(decoded[offset + 3]) << 24U);
        };
        const std::optional<uint32_t> total_length = read_u32(0x10);
        if (!total_length || *total_length != decoded.size()) {
            return false;
        }
        const std::optional<uint32_t> metadata_length =
            read_u32(0x1C);
        if (!metadata_length || *metadata_length > 0x400U ||
            0x20U + *metadata_length > decoded.size()) {
            return false;
        }
        std::vector<uint8_t> metadata(0x400, 0);
        std::copy_n(
            decoded.begin() + 0x20, *metadata_length,
            metadata.begin());

        const std::size_t block_offset =
            0x20U + static_cast<std::size_t>(*metadata_length);
        const std::optional<uint32_t> strings_length =
            read_u32(block_offset);
        if (!strings_length || *strings_length > 0x100000U ||
            block_offset + 4U + *strings_length > decoded.size()) {
            return false;
        }
        std::vector<uint8_t> strings(0x100000, 0);
        std::copy_n(
            decoded.begin() +
                static_cast<std::ptrdiff_t>(block_offset + 4U),
            *strings_length, strings.begin());

        const std::size_t count_offset =
            block_offset + 4U + *strings_length;
        const std::optional<uint32_t> string_count =
            read_u32(count_offset);
        if (!string_count) {
            return false;
        }
        std::size_t cursor = count_offset + 4U;
        std::vector<std::string> table;
        table.reserve(*string_count);
        for (uint32_t index = 0; index < *string_count; ++index) {
            std::size_t end = cursor;
            while (end < decoded.size() && decoded[end] != 0) {
                ++end;
            }
            if (end == decoded.size()) {
                return false;
            }
            table.emplace_back(
                reinterpret_cast<const char*>(decoded.data() + cursor),
                end - cursor);
            cursor = end + 1U;
        }

        const std::optional<uint32_t> map_count = read_u32(cursor);
        if (!map_count) {
            return false;
        }
        cursor += 4U;
        struct ParsedMap {
            std::string key;
            uint32_t bit_length = 0;
            std::vector<uint8_t> bits;
        };
        std::vector<ParsedMap> parsed_maps;
        parsed_maps.reserve(*map_count);
        for (uint32_t index = 0; index < *map_count; ++index) {
            std::size_t key_end = cursor;
            while (key_end < decoded.size() && decoded[key_end] != 0) {
                ++key_end;
            }
            if (key_end == decoded.size()) {
                return false;
            }
            const std::string key(
                reinterpret_cast<const char*>(decoded.data() + cursor),
                key_end - cursor);
            cursor = key_end + 1U;
            const std::optional<uint32_t> bit_length = read_u32(cursor);
            if (!bit_length) {
                return false;
            }
            cursor += 4U;
            const std::size_t byte_length =
                (static_cast<std::size_t>(*bit_length) + 7U) / 8U;
            if (byte_length > decoded.size() - cursor) {
                return false;
            }
            ParsedMap parsed;
            parsed.key = key;
            parsed.bit_length = *bit_length;
            parsed.bits.assign(
                decoded.begin() +
                    static_cast<std::ptrdiff_t>(cursor),
                decoded.begin() +
                    static_cast<std::ptrdiff_t>(
                        cursor + byte_length));
            parsed_maps.push_back(std::move(parsed));
            cursor += byte_length;
        }

        if (replace_character_maps) {
            char_maps_.clear();
        }
        std::vector<std::string> map_order;
        map_order.reserve(char_maps_.size() + parsed_maps.size());
        for (const std::string& key : char_map_order_) {
            if (char_maps_.contains(key) &&
                std::find(map_order.begin(), map_order.end(), key) ==
                    map_order.end()) {
                map_order.push_back(key);
            }
        }
        for (const auto& [key, record] : char_maps_) {
            (void)record;
            if (std::find(map_order.begin(), map_order.end(), key) ==
                map_order.end()) {
                map_order.push_back(key);
            }
        }
        for (std::size_t index = parsed_maps.size(); index-- > 0;) {
            const ParsedMap& parsed = parsed_maps[index];
            const auto existing = char_maps_.find(parsed.key);
            if (existing == char_maps_.end()) {
                CharMapRecord record;
                record.bit_length = parsed.bit_length;
                record.bits = parsed.bits;
                char_maps_[parsed.key] = std::move(record);
                map_order.insert(map_order.begin(), parsed.key);
            } else {
                CharMapRecord& record = existing->second;
                const std::size_t previous_bytes = record.bits.size();
                record.bit_length =
                    std::max(record.bit_length, parsed.bit_length);
                record.bits.resize(
                    (static_cast<std::size_t>(record.bit_length) + 7U) /
                        8U,
                    0);
                const std::size_t overlap =
                    std::min(parsed.bits.size(), previous_bytes);
                for (std::size_t byte = 0; byte < overlap; ++byte) {
                    record.bits[byte] = static_cast<uint8_t>(
                        record.bits[byte] | parsed.bits[byte]);
                }
                const auto order =
                    std::find(map_order.begin(), map_order.end(), parsed.key);
                if (order != map_order.end()) {
                    map_order.erase(order);
                }
                map_order.insert(map_order.begin(), parsed.key);
            }
        }

        string_namespaces_[0x80000000U] = std::move(table);
        if (*metadata_length > memory_.frame().size()) {
            throw BpFault("GDB metadata exceeds runtime buffer");
        }
        std::copy_n(metadata.begin(), *metadata_length, memory_.frame().begin());
        gdb_character_data_ = strings;
        char_map_order_ = std::move(map_order);
        if (metadata_destination != 0) {
            memory_.write_bytes(
                metadata_destination, metadata, base);
        }
        if (metadata_size_destination != 0) {
            memory_.write_u32(
                metadata_size_destination, base, 0x400);
        }
        if (strings_destination != 0) {
            memory_.write_bytes(
                strings_destination, strings, base);
        }
        if (strings_size_destination != 0) {
            memory_.write_u32(
                strings_size_destination, base, 0x100000);
        }
        return true;
    }
    std::vector<uint8_t> build_gdb_payload() const {
        const uint32_t window_left =
            static_cast<uint32_t>(window_x_);
        const uint32_t window_top =
            static_cast<uint32_t>(window_y_);
        std::vector<uint8_t> metadata(0x400, 0);
        std::copy_n(memory_.frame().begin(),
                    std::min(memory_.frame().size(), metadata.size()), metadata.begin());
        std::vector<uint8_t> character_data(0x100000, 0);
        std::copy_n(
            gdb_character_data_.begin(),
            std::min(
                gdb_character_data_.size(), character_data.size()),
            character_data.begin());

        std::vector<std::string> strings;
        if (const auto global =
                string_namespaces_.find(0x80000000U);
            global != string_namespaces_.end()) {
            strings = global->second;
        }
        std::vector<uint8_t> string_bytes;
        for (const std::string& value : strings) {
            string_bytes.insert(
                string_bytes.end(), value.begin(), value.end());
            string_bytes.push_back(0);
        }
        std::vector<uint8_t> map_bytes;
        const auto append_u32 = [](std::vector<uint8_t>& output,
                                   uint32_t value) {
            output.push_back(static_cast<uint8_t>(value));
            output.push_back(static_cast<uint8_t>(value >> 8U));
            output.push_back(static_cast<uint8_t>(value >> 16U));
            output.push_back(static_cast<uint8_t>(value >> 24U));
        };
        append_u32(
            map_bytes, static_cast<uint32_t>(char_maps_.size()));
        std::vector<std::string> ordered_keys;
        ordered_keys.reserve(char_maps_.size());
        for (const std::string& key : char_map_order_) {
            if (char_maps_.contains(key) &&
                std::find(
                    ordered_keys.begin(), ordered_keys.end(), key) ==
                    ordered_keys.end()) {
                ordered_keys.push_back(key);
            }
        }
        for (const auto& [key, record] : char_maps_) {
            (void)record;
            if (std::find(
                    ordered_keys.begin(), ordered_keys.end(), key) ==
                ordered_keys.end()) {
                ordered_keys.push_back(key);
            }
        }
        for (const std::string& key : ordered_keys) {
            const CharMapRecord& record = char_maps_.at(key);
            map_bytes.insert(
                map_bytes.end(), key.begin(), key.end());
            map_bytes.push_back(0);
            append_u32(map_bytes, record.bit_length);
            map_bytes.insert(
                map_bytes.end(), record.bits.begin(), record.bits.end());
        }
        const std::size_t total_size =
            0x100428U + string_bytes.size() + map_bytes.size();
        if (total_size > std::numeric_limits<uint32_t>::max()) {
            throw BpFault("GDB payload exceeds the 32-bit size limit");
        }
        std::vector<uint8_t> output(total_size, 0);
        static constexpr std::string_view magic = "BURIKO GDB 3.00";
        std::copy(magic.begin(), magic.end(), output.begin());
        const auto write_u32 = [&output](
                                   std::size_t offset, uint32_t value) {
            output[offset] = static_cast<uint8_t>(value);
            output[offset + 1] = static_cast<uint8_t>(value >> 8U);
            output[offset + 2] = static_cast<uint8_t>(value >> 16U);
            output[offset + 3] = static_cast<uint8_t>(value >> 24U);
        };
        write_u32(0x10, static_cast<uint32_t>(total_size));
        write_u32(0x14, window_left);
        write_u32(0x18, window_top);
        write_u32(0x1C, static_cast<uint32_t>(metadata.size()));
        std::copy(metadata.begin(), metadata.end(), output.begin() + 0x20);
        write_u32(0x420, static_cast<uint32_t>(character_data.size()));
        std::copy(
            character_data.begin(), character_data.end(),
            output.begin() + 0x424);
        write_u32(
            0x100424, static_cast<uint32_t>(strings.size()));
        std::copy(
            string_bytes.begin(), string_bytes.end(),
            output.begin() + 0x100428);
        std::copy(
            map_bytes.begin(), map_bytes.end(),
            output.begin() +
                static_cast<std::ptrdiff_t>(
                    0x100428U + string_bytes.size()));
        return output;
    }
    void set_gdb_database(std::vector<uint8_t> data) {
        gdb_database_ = std::move(data);
    }
    [[nodiscard]] std::span<const uint8_t> gdb_database() const {
        return gdb_database_;
    }
    [[nodiscard]] std::span<const uint8_t> active_code() const {
        return current_code();
    }
    [[nodiscard]] bool has_active_code_resource() const {
        return has_active_code_resource_;
    }
    [[nodiscard]] uint32_t active_code_resource_base() const {
        return active_code_resource_base_;
    }
    void set_resource_loader(ResourceLoader loader) {
        resource_loader_ = std::move(loader);
    }
    using ResourceLoadTrace = std::function<void(
        std::string_view, std::string_view, uint32_t, int32_t,
        uint32_t)>;
    void set_resource_load_trace_handler(ResourceLoadTrace trace) {
        resource_load_trace_ = std::move(trace);
    }
    uint32_t load_resource(std::string_view prefix, std::string_view path) {
        if (!resource_loader_) {
            throw BpFault("resource loader is not configured");
        }
        std::vector<uint8_t> data = resource_loader_(prefix, path);
        uint32_t code_size = 0;
        bool has_code_segment = false;
        if (data.size() >= 8) {
            const auto word = [&data](std::size_t offset) {
                return static_cast<uint32_t>(data[offset]) |
                       (static_cast<uint32_t>(data[offset + 1]) << 8U) |
                       (static_cast<uint32_t>(data[offset + 2]) << 16U) |
                       (static_cast<uint32_t>(data[offset + 3]) << 24U);
            };
            const uint32_t code_offset = word(0);
            code_size = word(4);
            if (code_offset <= data.size() &&
                static_cast<uint64_t>(code_offset) + code_size <=
                    data.size()) {
                has_code_segment = true;
            }
        }
        const uint32_t id = allocate_resource_address(
            has_code_segment ? code_size : data.size());
        if (has_code_segment) {
            const uint32_t code_offset = static_cast<uint32_t>(data[0]) |
                (static_cast<uint32_t>(data[1]) << 8U) |
                (static_cast<uint32_t>(data[2]) << 16U) |
                (static_cast<uint32_t>(data[3]) << 24U);
            resource_segments_[id] =
                {data, code_offset, code_size,
                 std::string(prefix) + ":" + std::string(path)};
        }
        resource_cache_[std::to_string(id) + ":" + std::string(prefix) +
                        ":" + std::string(path)] = std::move(data);
        resource_orders_by_thread_[thread_.id].push_back(id);
        return id;
    }
    std::vector<uint8_t> read_resource(std::string_view prefix,
                                       std::string_view path) const {
        if (!resource_loader_) {
            throw BpFault("resource loader is not configured");
        }
        return resource_loader_(prefix, path);
    }
    [[nodiscard]] const std::map<std::string, std::vector<uint8_t>>&
    resource_cache() const {
        return resource_cache_;
    }
    [[nodiscard]] const std::map<uint32_t, ResourceSegment>&
    resource_segments() const {
        return resource_segments_;
    }
    uint32_t release_last_resource() {
        // Original 0044BF30/0044BFD0 maintain the loaded-record chain at
        // CThread +30, never a shared chain spanning scheduler workers.
        // Segment keys are host-virtual arena addresses; only the loading
        // thread may retire its corresponding loaded program record.
        const auto owner = resource_orders_by_thread_.find(thread_.id);
        if (owner == resource_orders_by_thread_.end() || owner->second.empty()) {
            return 0x80000001U;
        }
        auto& order = owner->second;
        const uint32_t id = order.back();
        order.pop_back();
        retire_resource_record(id);
        return static_cast<uint32_t>(order.size());
    }

    [[nodiscard]] const std::string& last_error() const { return last_error_; }
    void set_captured_error(std::string error) {
        captured_error_ = std::move(error);
    }
    void clear_captured_error() { captured_error_.reset(); }
    [[nodiscard]] const std::optional<std::string>& captured_error() const {
        return captured_error_;
    }
    [[nodiscard]] const std::vector<std::string>& output_lines() const {
        return output_lines_;
    }
    void record_output_line(std::string line) {
        output_lines_.push_back(std::move(line));
    }

private:
    uint32_t allocate_resource_address(std::size_t bytes);
    void collect_retired_resource_records();
    bool resource_record_is_referenced(uint32_t id) const;
    void retire_resource_record(uint32_t id);
    void release_thread_resources(uint32_t id);
    // Host diagnostics only: not engine state and deliberately not serialized.
    void trace_bs_packet(uint32_t opcode);
    bool fail_display_draw(uint32_t destination, uint32_t object,
                           std::string_view renderer, std::string_view reason,
                           uint32_t branch = 0);
    std::set<std::string> display_warning_keys_;
    std::optional<DisplayBitmapDraw> display_warning_context_;
    uint64_t display_failure_serial_ = 0;
    void refresh_window_display_geometry(uint32_t object);
    bool draw_window_plane(uint32_t window, int32_t x, int32_t y,
                            uint32_t bitmap, uint32_t mode, uint32_t opacity, bool overlay);
    bool compose_window_at(uint32_t destination, uint32_t window, int32_t x, int32_t y);
    mutable GraphicsSurface display_frame_;
    bool display_frame_gpu_cached_=false;
    DisplayFrameReadbackProvider display_frame_readback_provider_;
    DisplayCallbackTrace display_callback_trace_;
    bool compose_single_bitmap_sprite_at(uint32_t destination, uint32_t handle,
                                         int32_t x, int32_t y);
    using Handler = StepResult (VM::*)();

    ControlObject* lookup_control(uint32_t handle) {
        if ((handle & 0xFF000000U) != 0xF0000000U) {
            return nullptr;
        }
        const uint32_t slot = handle & 0x00FFFFFFU;
        if (slot >= control_objects_.size() ||
            !control_objects_[slot].has_value()) {
            return nullptr;
        }
        return &*control_objects_[slot];
    }
    const ControlObject* lookup_control(uint32_t handle) const {
        if ((handle & 0xFF000000U) != 0xF0000000U) {
            return nullptr;
        }
        const uint32_t slot = handle & 0x00FFFFFFU;
        if (slot >= control_objects_.size() ||
            !control_objects_[slot].has_value()) {
            return nullptr;
        }
        return &*control_objects_[slot];
    }
    [[nodiscard]] bool effector_exists(uint32_t handle) const {
        if ((handle & 0xFF000000U) != 0x91000000U) {
            return false;
        }
        const FactoryObjectRegistry& registry =
            factory_objects_.at(
                static_cast<std::size_t>(
                    FactoryObjectKind::Grp1Primary));
        const uint32_t slot = handle & 0x00FFFFFFU;
        return slot < registry.slot_count &&
               registry.instances[slot].has_value();
    }

    int32_t screen_shake_random(int32_t maximum) {
        if (maximum <= 0) {
            return 0;
        }
        const int32_t divisor = maximum + 1;
        int32_t sum = 0;
        for (uint32_t index = 0; index < 8; ++index) {
            const uint32_t value =
                (random_next() << 15U) | random_next();
            sum += static_cast<int32_t>(value % divisor);
        }
        return sum / 8;
    }

    struct Rect {
        int32_t left = 0;
        int32_t top = 0;
        int32_t right = 0;
        int32_t bottom = 0;
    };
    static bool intersect_rect(Rect& destination, const Rect& other) {
        destination.left = std::max(destination.left, other.left);
        destination.top = std::max(destination.top, other.top);
        destination.right = std::min(destination.right, other.right);
        destination.bottom = std::min(destination.bottom, other.bottom);
        return destination.left <= destination.right &&
               destination.top <= destination.bottom;
    }
    static bool contains_rect(const Rect& outer, const Rect& inner) {
        return outer.left <= inner.left && inner.right <= outer.right &&
               outer.top <= inner.top && inner.bottom <= outer.bottom;
    }

    static constexpr std::array<Handler, 256> make_dispatch();

    [[nodiscard]] std::span<const uint8_t> current_code() const;
    int evaluate_wait_procedure(Thread& thread);
    static std::string stream_path_key(std::string path);
    static std::string clean_host_metadata_string(std::string_view text);
    uint8_t read_code8();
    uint16_t read_code16();
    uint32_t read_code32();
    uint16_t read_imm16();
    uint32_t read_imm32();
    void jump(uint32_t target);

    StepResult op_pushb();
    StepResult op_pushw();
    StepResult op_pushd();
    StepResult op_base_offset();
    StepResult op_code_pointer();
    StepResult op_code_offset();
    StepResult op_jump();
    StepResult op_load_base();
    StepResult op_store_base();
    StepResult op_compact();
    StepResult op_load();
    StepResult op_move();
    StepResult op_movem();
    StepResult op_copy_inline();
    StepResult op_copy_stack();
    StepResult op_add();
    StepResult op_sub();
    StepResult op_mul();
    StepResult op_div();
    StepResult op_mod();
    StepResult op_and();
    StepResult op_or();
    StepResult op_xor();
    StepResult op_not();
    StepResult op_shl();
    StepResult op_shr();
    StepResult op_sar();
    StepResult op_eq();
    StepResult op_neq();
    StepResult op_lt();
    StepResult op_leq();
    StepResult op_gt();
    StepResult op_geq();
    StepResult op_bool_and();
    StepResult op_bool_or();
    StepResult op_iszero();
    StepResult op_ternary();
    StepResult op_muldiv();
    StepResult op_atan2();
    StepResult op_vec3_length();
    StepResult op_rotate3();
    StepResult op_pow16();
    StepResult op_div16();
    StepResult op_sin();
    StepResult op_cos();
    StepResult op_qword_add();
    StepResult op_qword_sub();
    StepResult op_qword_mul();
    StepResult op_qword_div();
    StepResult op_qword_mod();
    StepResult op_mul16();
    StepResult op_vec4_add();
    StepResult op_vec4_sub();
    StepResult op_vec4_mul16();
    StepResult op_vec4_div16();
    StepResult op_vec4_round();
    StepResult op_vec4_scale();
    StepResult op_vec4_scale_recip();
    StepResult op_memcpy();
    StepResult op_memclr();
    StepResult op_memset();
    StepResult op_memrepeat();
    StepResult op_memory_equal();
    StepResult op_find_block();
    StepResult op_find_substring();
    StepResult op_str_replace();
    StepResult op_strcat();
    StepResult op_str_wrap();
    StepResult op_getchar();
    std::string format_opcode_values(
        std::string_view format,
        const std::function<uint32_t()>& take_argument);
    StepResult op_sprintf();
    StepResult op_set_opcode_variant();
    StepResult op_query_object_count();
    StepResult op_confirm();
    StepResult op_show_number();
    StepResult op_dump_memory();
    StepResult op_draw_text_workspace();
    StepResult op_set_clipboard_text();
    StepResult op_show_check();
    StepResult op_select_font();
    StepResult op_print_string();
    StepResult op_native();
    StepResult op_strlen();
    StepResult op_streq();
    StepResult op_strcpy();
    StepResult op_tolower();
    StepResult op_malloc();
    StepResult op_free();
    StepResult op_add_mem_boundary();
    StepResult op_call();
    StepResult op_return();
    StepResult op_jump_if();
    StepResult op_halt();
    StepResult op_yield();
    StepResult op_unimplemented();
    bool run_scheduled_children();
    void remove_thread_subtree(uint32_t id);
    void sync_code_fallbacks();
    [[nodiscard]] uint32_t next_thread_in_tree(uint32_t id) const;
    [[nodiscard]] const std::vector<uint32_t>& children_of(
        uint32_t id) const;
    [[nodiscard]] uint32_t parent_of(uint32_t id) const;
    Thread* find_thread_object(uint32_t id) {
        if (id == 1) {
            return &root_container_thread_;
        }
        if (id == thread_.id) {
            return &thread_;
        }
        if (scheduler_saved_thread_ &&
            scheduler_saved_thread_->id == id) {
            return &*scheduler_saved_thread_;
        }
        const auto found = program_threads_.find(id);
        return found == program_threads_.end() ? nullptr
                                               : &found->second.thread;
    }
    [[nodiscard]] const Thread* find_thread_object(uint32_t id) const {
        if (id == 1) {
            return &root_container_thread_;
        }
        if (id == thread_.id) {
            return &thread_;
        }
        if (scheduler_saved_thread_ &&
            scheduler_saved_thread_->id == id) {
            return &*scheduler_saved_thread_;
        }
        const auto found = program_threads_.find(id);
        return found == program_threads_.end() ? nullptr
                                               : &found->second.thread;
    }
    Thread& current_execution_thread() {
        Thread* current = find_thread_object(thread_id_);
        if (current == nullptr) {
            throw BpFault("current execution thread is missing");
        }
        return *current;
    }
    [[nodiscard]] WindowModeRequest make_window_mode_request(
        uint32_t fullscreen, uint32_t flag, uint32_t index) const {
        const uint32_t mode_width = window_width(index);
        const uint32_t mode_height = window_height(index);
        const bool use_table = window_use_mode_size_ ||
            client_width_ == 0 || client_height_ == 0;
        return {
            fullscreen,
            flag,
            index,
            mode_width,
            mode_height,
            use_table ? mode_width : client_width_,
            use_table ? mode_height : client_height_,
            window_use_mode_size_,
        };
    }
    static std::vector<uint8_t> encode_zero_runs(
        std::span<const uint8_t> data);
    static std::vector<uint8_t> decode_zero_runs(
        std::span<const uint8_t> data, std::size_t expected_size);

    Memory memory_;
    Thread thread_;
    std::vector<uint8_t> main_code_;
    uint32_t active_child_handle_ = 0;
    uint32_t parallel_worker_count_ = 1;
    uint32_t current_opcode_ip_ = 0;
    uint32_t main_slice_dispatch_count_ = 0;
    uint8_t current_native_sub_ = 0;
    bool has_active_code_resource_ = false;
    uint32_t active_code_resource_base_ = 0;
    bool engine_exit_requested_ = false;
    uint32_t system_close_value_ = 0;
    std::optional<std::string> shutdown_path_542cb0_;
    std::string shutdown_path_542c38_;
    std::optional<std::string> shutdown_path_542cb4_;
    uint32_t launcher_mode_ = 0;
    uint32_t sys0_52_value_ = 0;
    uint32_t sys0_state_542c54_ = 0;
    uint32_t sys0_state_542c58_ = 0;
    uint32_t sys0_state_54092c_ = 0;
    uint32_t sys0_counter_542e90_ = 0;
    std::array<uint32_t, 16> sys0_state_block_542c60_{};
    uint32_t sys0_input_global_52a5f8_ = 0;
    uint32_t sys0_input_global_52a5ac_ = 1; // Original initialized input-enable global.
    uint32_t sys0_input_global_5446a4_ = 0;
    std::array<uint32_t, 600> sys0_ring_544bf0_{};
    uint32_t sys0_ring_head_545550_ = 0;
    uint32_t sys0_state_540948_ = 0;
    uint32_t sys0_control_544be8_ = 0;
    uint32_t sys0_control_529bb0_ = 0;
    std::array<uint32_t, 256> sys0_table_542bd0_{};
    uint32_t sys0_process_count_542e00_ = 0;
    uint32_t sys0_input_state_589a30_ = 0;
    uint32_t thread_id_ = 1;
    uint32_t exclusive_thread_id_ = 0;
    Thread root_container_thread_;
    std::optional<Thread> scheduler_saved_thread_;
    uint32_t switch_target_id_ = 0;
    std::vector<uint32_t> root_children_;
    std::array<uint32_t, 16> window_widths_{
        320, 640, 800, 1024, 1024, 1280, 1280, 1920,
        1920, 2560, 2560, 3840, 3840, 1600, 1366, 960};
    std::array<uint32_t, 16> window_heights_{
        240, 480, 600, 768, 576, 960, 720, 1440,
        1080, 1920, 1440, 2880, 2160, 900, 768, 540};
    uint32_t window_fullscreen_ = 0;
    uint32_t window_mode_flag_ = 0;
    uint32_t window_mode_index_ = 2;
    uint32_t client_width_ = 0;
    uint32_t client_height_ = 0;
    bool window_use_mode_size_ = false;
    bool window_mode_in_progress_ = false;
    bool window_display_dirty_ = false;
    uint32_t raster_wait_enabled_ = 0;
    uint64_t window_mode_revision_ = 0;
    uint32_t sizing_limit_width_ = 0;
    uint32_t sizing_limit_height_ = 0;
    std::function<void(uint32_t,uint32_t)> window_sizing_limit_handler_;
    uint32_t window_resizable_ = 0;
    std::function<void(bool)> window_resizable_handler_;
    std::string window_title_;
    int32_t window_x_ = 0;
    int32_t window_y_ = 0;
    uint32_t sys1_62_value_ = 0;
    uint32_t display_mode_ = 0;
    uint32_t display_available_width_ = 0;
    uint32_t display_available_height_ = 0;
    bool shader_capability_ = false;
    uint32_t upscale_mode_ = 0;
    uint32_t display_pair_first_ = 0;
    uint32_t display_pair_second_ = 0;
    uint32_t render_control_first_ = 1;
    uint32_t render_control_second_ = 0;
    uint32_t grp0_render_global_529c38_ = 0;
    uint32_t grp0_render_global_529ba8_ = 0;
    uint32_t grp0_render_global_530ff4_ = 0;
    uint32_t grp0_render_global_530648_ = 0;
    uint32_t grp0_render_global_53282c_ = 0;
    uint32_t grp0_render_531004_ = 0;
    uint32_t grp0_render_531008_ = 0;
    uint32_t grp0_render_529c08_ = 0;
    uint32_t grp0_render_529c0c_ = 0;
    uint32_t grp0_render_53100c_ = 0;
    uint32_t grp0_render_532820_ = 0;
    uint32_t grp0_render_532824_ = 0;
    uint32_t grp0_render_53322c_ = 0;
    uint32_t grp0_render_533230_ = 0;
    uint32_t grp0_render_533228_ = 0;
    uint32_t grp0_render_533238_ = 0;
    uint32_t grp0_render_global_529be0_ = 0;
    uint32_t grp2_text_render_global_533208_ = 0;
    uint32_t grp2_text_render_global_53320c_ = 0;
    uint32_t grp2_text_render_global_533214_ = 0;
    uint32_t grp0_render_global_529bbc_ = 0;
    uint32_t grp0_render_global_529c10_ = 0;
    uint32_t grp0_render_global_529c3c_ = 0;
    uint32_t grp0_render_global_532828_ = 0;
    uint32_t grp0_render_global_53281c_ = 0;
    uint32_t grp0_render_global_529bc0_ = 0;
    uint32_t grp0_render_global_529bc4_ = 0;
    uint32_t grp0_render_global_529bc8_ = 0;
    uint32_t grp0_render_global_529bcc_ = 0;
    uint32_t grp2_render_global_529be4_ = 0;
    uint32_t grp1_render_global_540a68_ = 0;
    uint32_t grp1_render_global_52a658_ = 0;
    std::map<uint32_t, std::array<uint32_t, 24>> grp1_bf_records_;
    uint32_t grp1_text_mode_530ea0_ = 0;
    bool display_dirty_ = false;
    bool redraw_pending_ = false;
    uint32_t redraw_full_ = 0;
    uint32_t ext2_binary_cursor_ = 0;
    uint32_t ext2_string_cursor_ = 0;
    uint32_t random_state_ = 1;
    uint32_t frame_delay_ms_ = 0;
    uint32_t display_fps_ = 60;
    FrameProfilerState frame_profiler_;
    int32_t default_procedure_duration_ = 0;
    int32_t procedure_elapsed_ = 0;
    uint32_t image_cache_capacity_ = 0;
    uint32_t image_cache_threshold_ = 0;
    uint32_t image_cache_used_ = 0;
    std::vector<ImageCacheRecord> image_cache_records_;
    ImageCacheClock image_cache_clock_;
    std::map<uint32_t, SourceResourceState> source_resources_;
    std::map<uint32_t, GlobalResourceRecord> global_resource_records_;
    uint32_t next_global_resource_id_ = 0;
    uint32_t text_flag_530ea4_ = 0;
    std::string default_font_name_;
    std::map<std::string, std::string> font_substitutions_;
    std::map<std::string, std::string> font_aliases_;
    std::map<std::string,std::array<uint32_t,5>> scaled_font_tuples_;
    std::map<std::string, uint32_t> font_ids_;
    std::vector<FontCacheRegistration> font_cache_registrations_;
    std::map<std::string, uint32_t> font_charsets_;
    uint32_t next_font_id_ = 0;
    std::map<std::string, std::array<uint32_t, 4>> font_parameters_;
    std::map<std::tuple<std::string, uint32_t, uint32_t, uint32_t>, uint32_t>
        font_cache_entries_;
    uint32_t text_quality_level_ = 0;
    uint32_t text_quality_scale_ = 2;
    uint32_t text_quality_glyph_scale_ = 2;
    uint32_t text_quality_shift_ = 1;
    std::array<uint32_t, 6> text_metrics_{25,150,0,40,0,0};
    uint32_t text_flag_53341c_ = 0;
    uint32_t cursor_index_ = 0;
    uint32_t save_checksum_enabled_ = 0;
    std::vector<std::string> search_paths_;
    uint32_t search_paths_enabled_ = 0;
    uint32_t graphics_flag_532814_ = 0;
    uint32_t graphics_flag_529bd8_ = 0;
    uint32_t graphics_flag_529bd0_ = 0;
    uint32_t graphics_count_529bdc_ = 0;
    uint32_t graphics_count_529bb4_ = 0;
    uint32_t graphics_flag_529bb8_ = 0;
    uint32_t graphics_first_532810_ = 0;
    uint32_t graphics_second_532818_ = 0;
    uint32_t global_589ec4_ = 0;
    uint32_t text_character_mode_589d54_ = 0;
    std::set<uint32_t> display_objects_{0};
    uint32_t next_display_object_ = 0;
    std::map<uint32_t, SpriteState> sprites_;
    std::map<uint32_t, SpriteModeConfiguration>
        sprite_mode_configurations_;
    std::map<uint32_t, FilterState> filter_states_;
    std::map<uint32_t, MapRasterState> map_rasters_;
    uint32_t graphics_group_counter_ = 0;
    std::map<uint32_t, GraphicsGroupState> graphics_groups_;
    uint32_t graphics_group_property_key_ = 0;
    uint32_t graphics_group_property_value_ = 0;
    std::map<uint32_t, GraphicsSurface> graphics_surfaces_;
    CompressedBgJpegEncoder compressed_bg_jpeg_encoder_;
    std::array<WaveTableState, 8> wave_tables_{};
    std::map<uint32_t, std::pair<uint32_t, uint32_t>>
        bitmap_aux_values_;
    std::map<uint32_t, std::vector<uint32_t>>
        render_cache_records_;
    PrimarySurfaceCaptureProvider primary_surface_capture_provider_;
    WindowCaptureProvider window_capture_provider_;
    uint32_t current_capture_format_ = 2;
    std::map<uint32_t, std::string> bitmap_load_errors_;
    std::map<std::pair<std::string, std::string>,
             std::vector<uint8_t>> bitmap_resource_cache_;
    std::map<uint32_t, MovieSlotState> movie_slots_;
    std::optional<MovieSlotState> global_movie_;
    uint32_t global_movie_handle_ = 0;
    uint32_t next_global_movie_id_ = 0;
    std::vector<BitmapBlit> bitmap_blits_;
    std::vector<BitmapKernelOperation> bitmap_kernel_operations_;
    std::vector<BitmapWarpOperation> bitmap_warp_operations_;
    std::vector<BitmapCompositeOperation> bitmap_composite_operations_;
    ExternalImageLoadProvider external_image_load_provider_;
    ExternalImageSaveProvider external_image_save_provider_;
    TextDrawProvider text_draw_provider_;
    std::vector<TextDrawRequest> text_draw_operations_;
    std::vector<TextProcedureState> text_procedure_states_;
    std::vector<RichTextProcedureState> rich_text_procedure_states_;
    std::vector<DirectTextDrawState> direct_text_draw_states_;
    WindowGlyphProviderFactory window_glyph_provider_factory_;
    std::vector<TextMeasureState> text_measure_states_;
    uint32_t next_text_measure_id_ = 0;
    std::vector<RichTextObjectState> rich_text_object_states_;
    uint32_t next_rich_text_object_id_ = 0;
    std::vector<ImageEffectOperation> image_effect_operations_;
    std::vector<TextListProcessorState> text_list_processors_;
    TextStyleProvider text_style_provider_;
    TextLayoutState text_layout_;
    GlyphLayoutProvider glyph_layout_provider_;
    std::vector<GlyphPosition> glyph_positions_;
    std::vector<DirectBitmapDraw> direct_bitmap_draws_;
    ScreenShakeState screen_shake_;
    std::vector<BitmapScale> bitmap_scales_;
    std::vector<BitmapFitOperation> bitmap_fit_operations_;
    std::map<uint32_t, ToneCurveState> tone_curves_;
    std::vector<BitmapRegion> bitmap_regions_;
    std::map<uint32_t, WindowObject> window_objects_;
    std::map<uint32_t, std::vector<WindowBitmapCommand>>
        window_bitmap_commands_;
    std::map<uint32_t, SelectionIconProcessor>
        selection_icon_processors_;
    uint32_t next_window_object_id_ = 0;
    std::map<uint32_t, uint32_t> object_base_values_;
    std::map<uint32_t, uint32_t> object_enabled_;
    std::map<uint32_t, uint32_t> object_base_draw_enabled_;
    std::map<uint32_t, uint32_t> object_child_propagation_;
    std::map<uint32_t, uint32_t> object_effect_level_;
    std::map<uint32_t, uint32_t> object_visibility_threshold_;
    std::map<uint32_t, uint32_t> object_feature_mask_;
    std::map<uint32_t, uint32_t> object_scaled_level_;
    std::map<uint32_t, std::pair<int32_t, int32_t>> object_pair_44_48_;
    std::map<uint32_t, std::pair<int32_t, int32_t>> object_pair_3c_40_;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> object_raw_fields_;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> object_indexed_fields_;
    std::map<uint32_t, std::vector<std::vector<uint32_t>>>
        object_property_arrays_;
    std::map<uint32_t, DisplayObjectState> display_object_states_;
    std::vector<DisplayBitmapDraw> display_bitmap_draws_;
    uint32_t display_priority_cutoff_ = 0;
    uint32_t bitmap_token_reuse_ = 0;
    uint32_t next_bitmap_token_ = 0;
    uint32_t background_render_enabled_ = 0;
    std::array<uint32_t, 4> background_bitmap_tokens_{0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU};
    uint32_t background_type_ = 1;
    std::array<int32_t, 4> background_bitmaps_{-1, -1, -1, -1};
    std::array<uint32_t, 4> background_bitmap_widths_{};
    uint32_t background_effect_level_ = 0;
    std::array<int32_t, 2> background_center_{0, 0};
    std::array<BackgroundEffectSlot, 8> background_effect_slots_{};
    uint32_t background_effect_active_slot_ = 0;
    std::array<int32_t, 2> background_position_{0, 0};
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> auxiliary_display_producers_;
    uint64_t next_display_sequence_ = 0;
    std::vector<uint32_t> active_display_candidates_;
    std::vector<ActiveDisplayNode> active_display_objects_;
    std::vector<TouchWindowNode> touch_windows_;
    uint32_t next_touch_window_id_ = 0;
    uint32_t active_display_id_ = 0;
    uint32_t next_display_object_id_ = 0x40000000U;
    std::map<uint32_t, IconInputLayout> icon_layouts_;
    std::set<uint32_t> group_objects_;
    uint32_t next_icon_input_processor_id_ = 0;
    std::map<uint32_t, IconInputProcessor> icon_input_processors_;
    IconInputTrace icon_input_trace_;
    StepTrace step_trace_;
    std::vector<ExtendedIconInputLayout> icon_input_layout_history_;
    std::vector<DisplayProcedure> display_procedures_;
    std::vector<ControlDisplayProcedureState> control_display_procedures_;
    std::vector<FramedControlProcedureState> framed_control_procedures_;
    std::vector<ShakeDisplayProcedureState> shake_display_procedures_;
    std::map<uint32_t, std::vector<uint32_t>> group_children_;
    std::map<uint32_t, uint32_t> object_owners_;
    uint32_t next_procedure_id_ = 0;
    std::map<uint32_t, WaitWindowProcedure> wait_window_procedures_;
    std::map<uint32_t, MovieProcedure> movie_procedures_;
    std::optional<MovieSlotState> mf_movie_;
    uint32_t mf_movie_handle_ = 0;
    uint32_t mf_movie_state_ = 0;
    MfMovieOpenProvider mf_movie_open_provider_;
    std::function<bool()> mf_movie_stop_provider_;
    std::map<uint32_t, RegisterBmpProcedure> register_bmp_procedures_;
    std::map<uint32_t, BurikoMovieProcedure>
        buriko_movie_procedures_;
    std::map<uint32_t, HeaderMovieProcedure> header_movie_procedures_;
    std::map<std::string, uint32_t> movie_cache_;
    std::map<uint32_t, BinaryLoadProcedure> binary_load_procedures_;
    std::map<uint32_t, BmvFrameProcedure> bmv_frame_procedures_;
    std::map<uint32_t, CodecProcedure> codec_procedures_;
    std::map<uint32_t, GroupState> group_states_;
    std::map<uint32_t, std::pair<int32_t, int32_t>> object_positions_;
    std::map<uint32_t, std::array<uint32_t, 3>>
        object_absolute_positions_;
    std::map<uint32_t, uint32_t> object_absolute_position_dispatch_;
    std::map<uint32_t, ObjectAttachment> object_attachments_;
    std::map<uint32_t, DisplayObjectClass> object_display_classes_;
    std::vector<uint32_t> active_control_candidates_;
    std::array<std::optional<ControlObject>, 32> control_objects_{};
    uint32_t next_control_object_id_ = 0;
    uint32_t last_created_control_handle_ = 0;
    std::map<uint32_t, EffectorEffectState> effector_effects_;
    std::map<uint32_t, LandscapeObjectState> landscape_objects_;
    std::vector<IconImageRecord> icon_images_;
    int32_t icon_invalid_source_ = -1;
    CompositeEffectState composite_effect_;
    std::map<uint32_t, uint32_t> display_object_links_;
    std::map<std::pair<uint32_t, uint32_t>,
             std::pair<int32_t, int32_t>> group_child_offsets_;
    std::map<uint32_t, SpriteMaskConfig> sprite_mask_configs_;
    std::map<uint32_t, ObjectMaskState> object_masks_;
    std::map<uint32_t, uint32_t> graphics_surface_commands_;
    std::map<std::string, std::vector<uint8_t>> input_query_tables_{
        {"", std::vector<uint8_t>(512, 0)}};
    std::string last_input_query_name_;
    std::array<uint32_t, 256> input_descriptor_retrigger_{};
    std::map<uint32_t, uint32_t> input_descriptor_state_;
    std::map<uint32_t, uint32_t> input_descriptor_event_values_;
    std::map<uint32_t, bool> input_descriptor_active_;
    std::map<uint32_t, bool> input_descriptor_consumed_;
    std::vector<InputRegion> input_regions_;
    uint32_t last_synthetic_click_modifiers_ = 0;
    std::vector<uint32_t> input_capture_first_;
    std::vector<uint32_t> input_capture_second_;
    std::map<uint32_t, uint32_t> icon_input_capture_owners_;
    bool input_edge_mode_5446a0_ = false;
    bool input_return_gate_542ca8_ = false;
    bool input_regions_active_ = false;
    uint32_t input_value_542e8c_ = 0;
    uint32_t sys1_value_58d2e0_ = 0;
    uint32_t sys1_value_540f00_ = 0;
    bool hotkeys_enabled_ = false;
    std::map<uint32_t, std::vector<uint32_t>> input_classes_;
    std::array<uint32_t, 36> sys1_input_table_5409a0_{};
    uint32_t sys1_input_global_529bac_ = 0;
    uint32_t active_descriptor_value_ = 0;
    std::vector<uint32_t> active_descriptors_;
    uint32_t next_sys0_record_set_id_ = 0;
    std::map<uint32_t, Sys0RecordSet> sys0_record_sets_;
    uint32_t next_sys1_buffer_manager_id_ = 0;
    std::map<uint32_t, Sys1BufferManager> sys1_buffer_managers_;
    std::map<std::string, std::map<std::string, std::string>>
        sys1_string_table_;
    bool audio_channels_muted_ = false;
    std::array<uint32_t, 16> audio_channels_{};
    std::array<uint32_t, 128> audio_channels_9_{};
    std::vector<SoundEffectRequest> sound_effect_requests_;
    SoundSubsystem sound_;
    int32_t master_volume_ = -10000;
    bool effect_timer_active_ = false;
    uint64_t effect_timer_deadline_ms_ = 0;
    int32_t effect_timer_duration_ms_ = 0;
    std::map<uint32_t, uint32_t> graphics_settings_;
    uint32_t graphics_reciprocal_ = 0;
    uint32_t render_filter_ = 0;
    uint32_t system_flag_542e50_ = 0;
    uint32_t ext1_mode_589af0_ = 0;
    struct Ext1Object {
        uint32_t auxiliary = 0;
        std::vector<std::array<uint32_t, 3>> entries;
    };
    uint32_t next_ext1_object_id_ = 0;
    std::map<uint32_t, Ext1Object> ext1_objects_;
    struct Ext1WindowObject {
        bool active = true;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t state = 1;
        std::array<uint32_t, 5> configuration{};
        uint32_t committed_value = 0;
        uint32_t value_164 = 0;
        uint32_t half_width = 0;
        uint32_t half_height = 0;
        std::array<uint32_t, 9> frame{};
        std::array<std::array<uint32_t, 16>, 32> emitter_values{};
        std::array<uint8_t, 32> emitter_value_count{};
        std::vector<uint32_t> block_sizes;
        std::vector<uint32_t> block_offsets;
    };
    std::array<std::optional<Ext1WindowObject>, 8> ext1_windows_;
    uint32_t ext1_window_count_ = 0;
    struct Ext1ControlObject {
        bool active = true;
        std::array<uint32_t, 2> creation{};
        std::array<std::array<uint32_t, 8>, 16> values{};
        std::array<uint8_t, 16> value_count{};
    };
    std::array<std::optional<Ext1ControlObject>, 8> ext1_controls_;
    uint32_t ext1_control_count_ = 0;
    uint32_t ext1_control_interval_ = 0;
    std::array<std::array<uint32_t, 18>, 4> ext1_extended_values_{};
    std::array<uint8_t, 4> ext1_extended_value_count_{};
    struct FactoryObjectRegistry {
        uint32_t base = 0;
        uint32_t active = 0;
        uint32_t next_instance = 0;
        uint8_t slot_count = 8;
        std::array<std::optional<uint32_t>, 8> instances{};
    };
    std::array<FactoryObjectRegistry, 4> factory_objects_{{
        {0x90000000U, 0, 0, 8, {}},
        {0xA0000000U, 0, 0, 8, {}},
        {0x91000000U, 0, 0, 8, {}},
        {0xA1000000U, 0, 0, 4, {}},
    }};
    uint32_t display_constant_1220000_ = 0;
    uint32_t display_constant_80111600_ = 0;
    std::map<std::string, std::string> external_mounts_;
    std::string primary_directory_;
    std::string secondary_directory_;
    std::string current_directory_;
    std::string directory_probe_path_;
    std::string archive_source_path_529c50_;
    std::string archive_name_529f60_;
    std::vector<ModuleRequest> module_requests_;
    std::array<std::optional<LoadedModule>, 0xF0> modules_{};
    struct ModuleExecutionFrame {
        std::vector<uint8_t> root_code_before;
    };
    std::vector<ModuleExecutionFrame> module_frames_;
    std::vector<MemoryBoundary> memory_boundaries_;
    std::map<uint8_t, DebugVariableTable> debug_variable_tables_;
    uint32_t debug_variable_global_ = 0;
    bool window_minimized_ = false;
    bool cursor_visible_ = true;
    uint32_t custom_cursor_image_ = 0;
    int32_t custom_cursor_x_ = 0;
    int32_t custom_cursor_y_ = 0;
    bool drag_accept_enabled_ = false;
    std::string dropped_file_path_;
    bool pointer_available_ = false;
    int32_t pointer_x_ = 0;
    int32_t pointer_y_ = 0;
    int32_t display_transform_origin_x_ = 0;
    int32_t display_transform_origin_y_ = 0;
    PointerMotionState pointer_motion_;
    std::string external_path_542cac_;
    std::string wallpaper_path_;
    uint32_t wallpaper_style_ = 0;
    uint32_t wallpaper_tiled_ = 0;
    std::function<void(std::string_view, uint32_t, uint32_t)>
        wallpaper_callback_;
    uint32_t text_encoding_mode_ = 0;
    uint32_t text_code_page_ = 932;
    std::map<std::string, std::string> rich_text_aliases_;
    std::array<TextCacheRecord, 16> text_cache_storage_{};
    uint32_t text_cache_count_ = 0;
    Grp1TextStyle grp1_text_style_;
    uint32_t clock_wrap_span_ms_ = 500;
    std::array<std::pair<int32_t, int32_t>, 5> mouse_presets_ = {{
        {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1},
    }};
    std::vector<PointerHistoryRecord> pointer_history_;
    uint32_t pointer_history_limit_ = 0;
    int32_t pointer_history_interval_ = 0;
    bool memory_access_variant_ = false;
    std::vector<uint8_t> gdb_database_;
    std::vector<uint8_t> gdb_character_data_ =
        std::vector<uint8_t>(0x100000, 0);
    std::map<std::string, CharMapRecord> char_maps_;
    std::vector<std::string> char_map_order_;
    D0Subsystem d0_;
    HostTextEdit host_text_edit_;
    HostChildWindows host_child_windows_;
    std::vector<ChildWindowBitmapCommand>
        child_window_bitmap_commands_;
    ChildWindowTextProvider child_window_text_provider_;
    std::vector<ChildWindowTextCommand>
        child_window_text_commands_;
    HostDialogs host_dialogs_;
    ResourceIntegrityProvider resource_integrity_provider_;
    std::map<uint32_t, ResourceIntegrityData> resource_integrity_procedures_;
    MediaSourceProvider media_source_provider_;
    std::string media_source_message_;
    std::string media_source_caption_;
    uint32_t ui_language_ = 0;
    UiLanguageProvider ui_language_provider_;
    ResourceLoader resource_loader_;
    ResourceLoadTrace resource_load_trace_;
    FileEnumerator file_enumerator_;
    ArchiveEntryEnumerator archive_entry_enumerator_;
    ResourceListHandler resource_list_handler_;
    DirectoryEnumerator directory_enumerator_;
    DriveTypeEnumerator drive_type_enumerator_;
    SpecialFolderProvider special_folder_provider_;
    FontEnumerator font_enumerator_;
    HostSystemMetadataProvider host_system_metadata_provider_;
    VolumeLabelProvider volume_label_provider_;
    WindowWorkAreaEnumerator window_work_area_enumerator_;
    CompositeArchiveRegistrar composite_archive_registrar_;
    FileReader file_reader_;
    FileWriter file_writer_;
    FileStreamTrace file_stream_trace_;
    FileSizeResolver file_size_resolver_;
    FileExistsResolver file_exists_resolver_;
    ExternalPathOpener external_path_opener_;
    ProcessLauncher process_launcher_;
    ExtendedProcessLauncher extended_process_launcher_;
    PluginLoadHandler plugin_load_handler_;
    PluginUnloadHandler plugin_unload_handler_;
    PluginInvokeHandler plugin_invoke_handler_;
    uint32_t next_plugin_id_ = 0;
    std::map<uint32_t, std::string> plugins_;
    ClipboardSetter clipboard_setter_;
    MessageBoxHandler message_box_handler_;
    Ext0DialogHandler ext0_dialog_handler_;
    FolderPicker folder_picker_;
    FileDialogHandler file_dialog_handler_;
    SavedRecordHandler saved_record_handler_;
    RequiredResourceHandler required_resource_handler_;
    std::string game_identifier_;
    bool compact_64_opcodes_ = false;
    uint32_t effector_mode_limit_ = 1;
    RegistryValueProvider registry_value_provider_;
    RegistryDeleteHandler registry_delete_handler_;
    DirectInputStateProvider direct_input_state_provider_;
    KeyboardStateProvider keyboard_state_provider_;
    ShellLinkHandler shell_link_handler_;
    LegacyDialogHandler legacy_dialog_handler_;
    LegacyConfigDialogHandler legacy_config_dialog_handler_;
    PathCleanupHandler path_cleanup_handler_;
    MinimizeHandler minimize_handler_;
    CloseRequestHandler close_request_handler_;
    DragAcceptHandler drag_accept_handler_;
    WindowRepositionHandler window_reposition_handler_;
    WindowModeApplier window_mode_applier_;
    TouchWindowHandler touch_window_handler_;
    NamedMutexHooks named_mutex_hooks_;
    std::map<uint32_t, NamedMutexState> named_mutexes_;
    uint32_t named_mutex_next_id_ = 0;
    std::mutex lock_registry_mutex_;
    std::condition_variable lock_registry_event_;
    std::map<uint32_t, LockRecord> lock_records_;
    uint32_t next_lock_record_id_ = 1;
    std::map<std::string, std::vector<std::string>> composite_archives_;
    std::vector<std::string> composite_archive_ids_;
    std::map<uint32_t, StreamState> streams_;
    std::map<std::string, std::vector<uint8_t>> virtual_files_;
    uint32_t next_stream_id_ = 1;
    std::map<uint32_t, std::vector<std::string>> string_namespaces_;
    std::map<std::string, FlagTable> flag_tables_;
    std::deque<ProcessRecord> process_records_;
    uint32_t process_record_limit_ = 0;
    std::map<uint32_t, ProgramThreadState> program_threads_;
    std::deque<std::array<uint32_t, 3>> system_records_;
    uint64_t system_records_enqueued_ = 0;
    uint64_t system_records_dequeued_ = 0;
    uint32_t next_thread_handle_ = 1;
    std::map<std::string, std::vector<uint8_t>> resource_cache_;
    uint32_t next_resource_offset_ = 0;
    std::map<uint32_t, ResourceSegment> resource_segments_;
    std::map<uint32_t, std::vector<uint32_t>> resource_orders_by_thread_;
    std::string last_error_;
    std::optional<std::string> captured_error_;
    std::vector<std::string> output_lines_;
    NativeRegistry natives_;
};

inline std::optional<DecodedBitmap> VM::decode_bitmap_payload(
    std::span<const uint8_t> encoded, std::string* error, std::string_view resource) {
    const auto fail = [&](std::string_view reason) -> std::optional<DecodedBitmap> {
        const std::string message(reason);
        if (error) *error = message;
        warn_decode_failure("bitmap", message, encoded, resource);
        return std::nullopt;
    };
    if (encoded.size() >= 13U && std::string_view(
            reinterpret_cast<const char*>(encoded.data()),13U) == "CompressedBG_") {
        const auto decoded = decode_compressed_bg(encoded, error, resource);
        if (!decoded) return std::nullopt; // Already diagnosed by container dispatch.
        if (decoded->bits_per_pixel != 24U && decoded->bits_per_pixel != 32U)
            return fail("RGBA bitmap presentation is not implemented for this bit depth");
        DecodedBitmap result;
        result.width = decoded->width; result.height = decoded->height;
        result.format = decoded->bits_per_pixel == 32U ? 2U : 1U;
        const size_t count = static_cast<size_t>(result.width) * result.height;
        const size_t stride = decoded->bits_per_pixel == 24U && decoded->format != 7U ? 3U : 4U;
        if (decoded->pixels.size() < count * stride) return fail("decoded bitmap pixel capacity is invalid");
        result.rgba.resize(count * 4U);
        for (size_t pixel = 0; pixel < count; ++pixel) {
            const size_t from = pixel * stride, to = pixel * 4U;
            result.rgba[to] = decoded->pixels[from+2U];
            result.rgba[to+1U] = decoded->pixels[from+1U];
            result.rgba[to+2U] = decoded->pixels[from];
            result.rgba[to+3U] = result.format == 1U ? 255U : decoded->pixels[from+3U];
        }
        return result;
    }
    auto decoded = decode_bgi_bitmap(encoded);
    if (decoded) return decoded;
    if (encoded.size() < 16U) return fail("unrecognized or truncated bitmap header");
    const auto word = [&](size_t p) { return uint32_t(encoded[p]) | uint32_t(encoded[p+1]) << 8U; };
    if (word(6) != 0U) return fail("raw bitmap compression mode is not implemented");
    if (word(4) != 24U && word(4) != 32U) return fail("raw RGBA bitmap bit depth is not implemented");
    return fail("invalid dimensions or truncated raw bitmap pixels");
}

} // namespace bgiplayer
