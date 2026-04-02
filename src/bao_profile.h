#pragma once

#include <cstdint>

#ifdef BAO_ENABLE_MOVE_PROFILE

struct BaoMoveProfile {
    uint64_t move_calls                = 0;
    uint64_t ok_results                = 0;
    uint64_t infinite_results          = 0;
    uint64_t inner_row_empty_results   = 0;
    uint64_t sow_calls                 = 0;
    uint64_t large_sow_calls           = 0;
    uint64_t capture_resows            = 0;
    uint64_t relay_resows              = 0;
    uint64_t total_sown                = 0;
    uint64_t max_total_sown            = 0;
    uint64_t max_sow_calls_in_move     = 0;
};

extern thread_local BaoMoveProfile* g_bao_move_profile;

inline void bao_profile_record_sow(int count) {
    if (BaoMoveProfile* p = g_bao_move_profile) {
        p->sow_calls++;
        if (count >= 16)
            p->large_sow_calls++;
    }
}

inline void bao_profile_finish_move(bool ok, bool infinite, bool inner_row_empty,
                                    uint64_t sow_calls, uint64_t total_sown,
                                    uint64_t capture_resows,
                                    uint64_t relay_resows) {
    if (BaoMoveProfile* p = g_bao_move_profile) {
        p->move_calls++;
        if (ok) p->ok_results++;
        if (infinite) p->infinite_results++;
        if (inner_row_empty) p->inner_row_empty_results++;
        p->capture_resows += capture_resows;
        p->relay_resows += relay_resows;
        p->total_sown += total_sown;
        if (total_sown > p->max_total_sown)
            p->max_total_sown = total_sown;
        if (sow_calls > p->max_sow_calls_in_move)
            p->max_sow_calls_in_move = sow_calls;
    }
}

#else

struct BaoMoveProfile {};

inline void bao_profile_record_sow(int) {}

inline void bao_profile_finish_move(bool, bool, bool,
                                    uint64_t, uint64_t,
                                    uint64_t, uint64_t) {}

#endif
