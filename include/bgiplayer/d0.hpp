#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bgiplayer {

class D0Subsystem {
    friend struct SnapshotCodec;
public:
    D0Subsystem() = default;
    ~D0Subsystem();
    D0Subsystem(const D0Subsystem&) = delete;
    D0Subsystem& operator=(const D0Subsystem&) = delete;
    D0Subsystem(D0Subsystem&&) = delete;
    D0Subsystem& operator=(D0Subsystem&&) = delete;

    static constexpr uint32_t kStatusOk = 0;
    static constexpr uint32_t kStatusUnknownObject = 1;
    static constexpr uint32_t kStatusInvalidArgument = 2;
    static constexpr uint32_t kStatusNotInitialized = 3;
    static constexpr uint32_t kStatusUnknownField = 4;
    static constexpr uint32_t kStatusOutOfRange = 5;
    static constexpr uint32_t kStatusInvalidNegativeY = 11;
    static constexpr uint32_t kStatusInvalidNegativeX = 10;
    static constexpr std::size_t kWorkerRecordDwords = 0x20DU;
    static constexpr std::size_t kWorkerRecordBytes =
        kWorkerRecordDwords * sizeof(uint32_t);

    struct SpaceRecordCreation {
        uint32_t field_14 = 0;
        uint32_t enabled = 0;
        int32_t field_c_q16 = 0;
        int32_t field_8_q16 = 0;
        int32_t threshold_q16 = 0;
        int32_t radius_x_q16 = 0;
        int32_t radius_y_q16 = 0;
        int32_t radius_z_q16 = 0;
        int32_t position_x_q16 = 0;
        int32_t position_y_q16 = 0;
        int32_t position_z_q16 = 0;
    };

    uint32_t create_field(uint32_t config, uint32_t& id);
    uint32_t destroy_field(uint32_t id);
    uint32_t reset_field(uint32_t id, uint32_t width, uint32_t height,
                         std::span<const uint8_t> cells);
    uint32_t create_snapshot(uint32_t id, std::span<const uint8_t> samples,
                             uint32_t& snapshot_id);
    std::optional<std::pair<uint32_t, uint32_t>> field_size(
        uint32_t id) const;

    uint32_t create_field_object(uint32_t field_id, uint32_t& object_id);
    uint32_t destroy_field_object(uint32_t field_id, uint32_t object_id);
    uint32_t release_field_object_resource(uint32_t field_id,
                                           uint32_t object_id);
    uint32_t set_field_object_origin(uint32_t field_id, uint32_t object_id,
                                     int32_t x, int32_t y);
    uint32_t set_field_object_size(uint32_t field_id, uint32_t object_id,
                                   int32_t width, int32_t height);
    uint32_t set_field_object_values(uint32_t field_id, uint32_t object_id,
                                     uint32_t first, uint32_t second);
    uint32_t set_field_object_snapshot(uint32_t field_id, uint32_t object_id,
                                       uint32_t snapshot_id);
    uint32_t set_field_object_style(uint32_t field_id, uint32_t object_id,
                                    uint32_t style);
    uint32_t direction_from_points(uint32_t field_id, int32_t first_x,
                                   int32_t first_y, int32_t second_x,
                                   int32_t second_y, uint32_t& direction);
    uint32_t relative_angle_from_object(uint32_t field_id, uint32_t object_id,
                                        int32_t first, int32_t second,
                                        uint32_t& angle);

    uint32_t create_space_manager(uint32_t& id);
    uint32_t destroy_space_manager(uint32_t id);
    uint32_t create_worker(uint32_t& id);
    uint32_t destroy_worker(uint32_t id);
    uint32_t set_worker_capacity(uint32_t id, int32_t value);
    uint32_t query_worker_result(uint32_t id, uint32_t& result);
    uint32_t configure_worker_records(
        uint32_t id, uint32_t source, uint32_t count,
        std::span<const uint32_t> records, bool asynchronous);
    uint32_t set_worker_source(uint32_t id, uint32_t source);
    uint32_t submit_worker_geometry(
        uint32_t id, std::string first, std::string second,
        uint32_t first_value, uint32_t second_value);
    uint32_t render_worker_record(
        uint32_t id, uint32_t source, uint32_t count,
        std::span<const uint32_t> record, uint32_t flags,
        bool asynchronous);
    uint32_t query_worker_record(
        uint32_t id, uint32_t index,
        std::span<uint32_t> output);
    uint32_t create_space_record(uint32_t manager_id, uint32_t index);
    uint32_t create_space_record(
        uint32_t manager_id, uint32_t index,
        const SpaceRecordCreation& creation);
    uint32_t destroy_space_record(uint32_t manager_id, uint32_t index);
    uint32_t set_space_record_property(uint32_t manager_id, uint32_t index,
                                       uint32_t property, uint32_t value);
    uint32_t get_space_record_property(uint32_t manager_id, uint32_t index,
                                       uint32_t property, uint32_t& value);
    uint32_t get_space_record_position(uint32_t manager_id, uint32_t index,
                                       std::array<uint32_t, 3>& position);
    uint32_t get_space_record_radius(uint32_t manager_id, uint32_t index,
                                     std::array<uint32_t, 3>& radius);
    uint32_t get_space_record_scaled_radius(
        uint32_t manager_id, uint32_t index,
        std::array<uint32_t, 3>& radius);
    uint32_t set_space_record_relation(uint32_t manager_id, uint32_t index,
                                       uint32_t layer, uint32_t target);
    uint32_t get_space_record_relation(uint32_t manager_id, uint32_t index,
                                       uint32_t layer, uint32_t& target);
    uint32_t enumerate_space_record_relations(
        uint32_t manager_id, uint32_t index, uint32_t layer,
        std::vector<uint32_t>& targets);
    uint32_t set_space_record_position(
        uint32_t manager_id, uint32_t index,
        const std::array<uint32_t, 4>& position);
    uint32_t set_space_record_position_fixed(
        uint32_t manager_id, uint32_t index, int32_t x, int32_t y,
        int32_t z);
    uint32_t set_space_record_radius(
        uint32_t manager_id, uint32_t index, int32_t x, int32_t y,
        int32_t z);
    uint32_t space_vector_to_record(
        uint32_t manager_id, uint32_t source, uint32_t target,
        std::array<uint32_t, 5>& result);
    uint32_t direction_to_world_point(
        uint32_t manager_id, uint32_t index, int32_t x, int32_t y,
        int32_t z, std::array<uint32_t, 5>& result);
    uint32_t query_nearby_space_records(
        uint32_t manager_id, uint32_t index, uint32_t filter,
        std::vector<uint32_t>& records);
    uint32_t query_nearby_space_records_at(
        uint32_t manager_id, uint32_t skip, uint32_t filter,
        const std::array<float, 3>& center, float radius,
        std::vector<uint32_t>& records);
    uint32_t query_space_records_between(
        uint32_t manager_id, uint32_t index,
        const std::array<float, 3>& endpoint, uint32_t filter,
        bool exact, std::vector<uint32_t>& records);
    uint32_t query_space_route(
        uint32_t manager_id, uint32_t source, uint32_t target,
        float max_distance, float cell_size, uint32_t filter, bool exact,
        uint32_t max_subdivision, bool async,
        std::vector<std::array<uint32_t, 4>>& points);
    uint32_t query_space_field_peak(
        uint32_t manager_id, int32_t cell_size, uint32_t width,
        uint32_t height, uint32_t sample_count, uint32_t skip,
        uint32_t filter, std::array<uint32_t, 4>& result);
    uint32_t build_field_object_mesh(
        uint32_t field_id, uint32_t object_id, uint32_t limit,
        uint32_t flags, uint32_t mode, uint32_t overlay);
    uint32_t query_field_object_page(
        uint32_t field_id, uint32_t object_id, int32_t x, int32_t y,
        uint32_t& count, std::vector<uint32_t>& directions);
    uint32_t copy_field_object_mesh(
        uint32_t field_id, uint32_t object_id,
        std::vector<std::array<uint32_t, 6>>& pages);
    uint32_t enumerate_field_object_pages(
        uint32_t field_id, uint32_t object_id, bool sorted,
        std::vector<std::array<int32_t, 2>>& cells);
    uint32_t query_field_object_neighbors(
        uint32_t field_id, uint32_t object_id, int32_t x, int32_t y,
        int32_t max_height_delta,
        std::array<int32_t, 6>& values,
        std::array<int32_t, 12>& cells);
    uint32_t query_field_object_neighbors(
        uint32_t field_id, int32_t x, int32_t y,
        int32_t max_height_delta,
        std::array<int32_t, 6>& values,
        std::array<int32_t, 12>& cells);
    uint32_t query_space_record_directions(
        uint32_t manager_id, uint32_t index, uint32_t filter,
        std::vector<std::array<uint32_t, 5>>& records);

private:
    struct MeshPage {
        std::array<uint32_t, 6> words{};
    };

    struct FieldObject {
        int32_t x = -1;
        int32_t y = -1;
        int32_t width = 0;
        int32_t height = 0;
        uint32_t first_value = 0;
        uint32_t second_value = 0;
        uint32_t snapshot_id = 0;
        uint32_t style = 0;
        std::vector<uint8_t> mesh_cells;
        std::vector<MeshPage> mesh_pages;
        bool mesh_valid = false;
    };

    struct Field {
        uint32_t config = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        bool initialized = false;
        std::vector<uint8_t> cells;
        uint32_t next_snapshot_id = 0x10000000U;
        std::map<uint32_t, std::vector<uint8_t>> snapshots;
        uint32_t next_object_id = 0x20000000U;
        std::map<uint32_t, FieldObject> objects;
    };

    struct SpaceRecord {
        bool active = false;
        std::array<uint32_t, 7> properties{};
        std::array<uint32_t, 4> position{};
        std::array<uint32_t, 4> radius_vector{};
        std::array<uint32_t, 4> scaled_radius_vector{};
        std::array<std::vector<uint32_t>, 2> dependencies{};
    };

    struct SpaceManager {
        bool busy = false;
        std::map<uint32_t, SpaceRecord> records;
    };

    struct Worker {
        enum class Operation {
            None,
            ConfigureRecords,
            RenderRecord,
            BuildGeometry,
        };

        static constexpr std::size_t kRecordDwords =
            D0Subsystem::kWorkerRecordDwords;
        static constexpr std::size_t kRecordBytes =
            D0Subsystem::kWorkerRecordBytes;

        uint32_t id = 0;
        std::mutex critical_section;
        std::condition_variable event;
        bool event_signaled = true;
        bool shutdown = false;
        uint32_t state = 0;
        uint32_t status = 0xFFFF0000U;
        int32_t render_capacity = 0;
        uint32_t source = 0;
        Operation operation = Operation::None;
        uint32_t operation_count = 0;
        uint32_t operation_flags = 0;
        std::string operation_first;
        std::string operation_second;
        std::vector<uint32_t> records;
        std::vector<uint32_t> pending_records;
        std::thread thread;
    };

    void worker_main(Worker& worker);
    uint32_t run_worker_operation(Worker& worker);
    static uint32_t map_worker_status(uint32_t raw);
    void stop_worker(Worker& worker);
    uint32_t query_space_records_between_from(
        uint32_t manager_id, uint32_t index,
        const std::array<float, 4>& start,
        const std::array<float, 4>& finish,
        uint32_t filter, bool exact, std::vector<uint32_t>& records);

    uint32_t next_field_id_ = 1;
    std::map<uint32_t, Field> fields_;
    uint32_t next_space_manager_id_ = 1;
    std::map<uint32_t, SpaceManager> space_managers_;
    uint32_t next_worker_id_ = 1;
    std::map<uint32_t, std::unique_ptr<Worker>> workers_;
};

}  // namespace bgiplayer
