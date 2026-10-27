#include "bgiplayer/vm.hpp"
#include "bgiplayer/debug.hpp"
#include <iostream>

namespace bgiplayer {
void VM::install_mf_movie_wait(Thread& thread, std::string directory,
    std::string resource, uint32_t volume, bool directory_present,
    bool resource_present) {
    const uint32_t token = create_movie_procedure(
        std::move(directory), std::move(resource), volume);
    auto& procedure = movie_procedures_.at(token);
    procedure.source_present = directory_present;
    procedure.extra_present = resource_present;
    thread.snapshot_wait_kind = SnapshotWaitKind::MfMovie;
    thread.snapshot_wait_index = token;
    thread.wait_procedure = [this, token] { return advance_mf_movie_procedure(token); };
    thread.status_flags |= 1U;
    thread.state = ThreadState::Waiting;
}

WaitProcedureResult VM::advance_mf_movie_procedure(uint32_t token) {
    auto& procedure = movie_procedures_.at(token);
    if (procedure.complete) return {1, {procedure.result}};
    if (procedure.state == 0) {
        if (!procedure.extra_present) {
            procedure.result = 0xFFFFFFFDU;
        } else {
            // Original F2580 first destroys any previous MF player. DirectShow
            // is a distinct global object and is not affected by this command.
            (void)stop_mf_movie();
            uint32_t status = 0x80000003U;
            auto resolved = resolve_movie_source({}, procedure.extra);
            if (!resolved && procedure.source_present)
                resolved = resolve_movie_source(procedure.source, procedure.extra);
            if (resolved) {
                mf_movie_ = MovieSlotState{};
                auto& movie = *mf_movie_;
                movie.directory = procedure.source;
                movie.resource = procedure.extra;
                movie.resolved_path = *resolved;
                movie.volume = static_cast<uint32_t>(std::clamp(
                    static_cast<int32_t>(procedure.field_30), 0, 128));
                movie.media_open = true;
                movie.started = true;
                movie.playing = true;
                const auto [width, height] = current_mode_dimensions();
                movie.width = width;
                movie.height = height;
                mf_movie_handle_ = token;
                mf_movie_state_ = 2; // Opened; wait for host Start/first frame.
                if (mf_movie_open_provider_) {
                    status = mf_movie_open_provider_(movie);
                } else {
                    static bool warned = false;
                    if (!warned) {
                        warned = true;
                        std::cerr << "warning: MOVIE_UNSUPPORTED renderer=mf reason=\"host movie playback provider is unavailable\"\n";
                    }
                    status = 0x80000001U;
                }
                if (status != 0) (void)stop_mf_movie();
            }
            if (status == 0) procedure.state = 1;
            else if (status == 0x80000003U) procedure.result = 0xFFFFFFFDU;
            else if (status == 0x80000004U) procedure.result = 0xFFFFFFFCU;
            // 80000001 and unknown open failures retain initialized -1.
            if (debug_trace(DebugTrace::Movie))
                std::cout << "MOVIE_MF_OPEN procedure=0x" << std::hex << token
                          << " status=0x" << status << std::dec
                          << " resource=" << procedure.extra << '\n';
        }
    }
    if (procedure.state == 1) {
        // Original 1400F2750 returns zero only while the host player is in
        // state2. Once asynchronous Start runs, F8 completes; FA/F9 then
        // query and stop the independent player while scripts keep running.
        if (mf_movie_ && mf_movie_state_ == 2U) return {0, {}};
        procedure.result = 0;
    }
    procedure.complete = true;
    return {1, {procedure.result}};
}
}  // namespace bgiplayer
