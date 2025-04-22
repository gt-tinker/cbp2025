#ifndef _PREDICTOR_H_
#define _PREDICTOR_H_

#include <stdlib.h>

#define PT_ENTRIES 1024
#define PT_ENTRIES_MASK (PT_ENTRIES - 1)
#define PT_ENTRIES_LOG2 10

#define LH_ENTRIES 512
#define LH_ENTRIES_MASK (LH_ENTRIES - 1)
#define LH_ENTRIES_LOG2 9

#define LT_ENTRIES 1024
#define LT_ENTRIES_MASK (LT_ENTRIES - 1)
#define LT_ENTRIES_LOG2 8

#define BACKEDGE_WINDOW 1024 // every time the window is encountered, counts are reset to 0. if the count was already 0, the entry is invalidated
#define BACKEDGE_ENTRIES 4

#define MAX_HIST_LENGTH 256
#define HIST_BUFFER_LENGTH (MAX_HIST_LENGTH / 64)
#define HIST_LENGTH 16

#define PC_SHIFT 3

struct compressed_hist {
    uint64_t hist_buffer[HIST_BUFFER_LENGTH];
    uint64_t compressed;
    uint64_t compressed_length;
    compressed_hist() : hist_buffer{0}, compressed(0), compressed_length(0) {}
    void init(uint64_t length) {
        compressed_length = length;
        for (int i = 0; i < HIST_BUFFER_LENGTH; ++i) {
            hist_buffer[i] = 0;
        }
        compressed = 0;
    }
    void update(bool taken) {
        // get the index of the uppermost bit of current history
        uint64_t to_remove = (hist_buffer[HIST_BUFFER_LENGTH - 1] >> 63) & 0x1;
        // get the highest bit of compressed history
        uint64_t to_cycle = (compressed >> (compressed_length - 1)) & 0x1;
        uint64_t to_add = (taken ? 0x1 : 0x0);
        // shift the compressed history
        compressed = (compressed << 1) | to_cycle;
        // mask the compressed history
        compressed &= ((1ULL << compressed_length) - 1);
        // xor out the oldest bit
        compressed ^= to_remove;
        // xor in the newest bit
        compressed ^= to_add;
        // shift the entire history buffer
        for (int i = HIST_BUFFER_LENGTH - 1; i > 0; --i) {
            hist_buffer[i] = (hist_buffer[i] << 1) | (hist_buffer[i - 1] >> 63);
        }
        hist_buffer[0] = (hist_buffer[0] << 1) | to_add;
    }
};


struct SampleHist
{
    uint64_t ghist;
    bool tage_pred;
    bool tage_use_loop;
    bool tage_use_sc;
    bool pred_track;
    bool pred_made;
    
    // compressed histories
    compressed_hist hist[BACKEDGE_ENTRIES];

    uint64_t local_hist[LH_ENTRIES];

    SampleHist()
    {
        ghist = 0;
        for (int i = 0; i < BACKEDGE_ENTRIES; ++i) {
            hist[i].init(HIST_LENGTH);
        }
        for (int i = 0; i < LH_ENTRIES; ++i) {
            local_hist[i] = 0;
        }
    }
};

struct Counter
{
    uint8_t value;
    uint8_t nbits;
    Counter() : Counter(2) {}
    Counter(uint8_t nbits) : nbits(nbits) {
        value = (1 << (nbits - 1));
    }
    void update(bool correct) {
        if (value != 0 && !correct) {
            value--;
        } else if (value != (1 << nbits) - 1 && correct) {
            value++;
        }
    }
    bool predict() const {
        return value >= (1 << (nbits - 1));
    }
};

struct pred_tracker_entry {
    Counter tage_tracker;
    Counter backedge_tracker_d1;
    Counter backedge_tracker_d2;
    Counter altpred;
    pred_tracker_entry() : tage_tracker(3), backedge_tracker_d1(3), backedge_tracker_d2(3), altpred(2){
        // tage_tracker.value = 7;
        // backedge_tracker_d1.value = 0;
        // backedge_tracker_d2.value = 0;
    }
};

struct backedge_tracker_entry {
    uint64_t first; // pc of top of loop
    uint64_t second; // pc of end of loop
    uint8_t call_depth; // filter out calls within loop
    uint16_t uses_in_window; // number of times this backedge was used in the last window
    bool valid;
    backedge_tracker_entry() : first(0), second(0), call_depth(0), uses_in_window(0), valid(false) {}
};

class SampleCondPredictor
{
        
        std::unordered_map<uint64_t/*key*/, SampleHist/*val*/> pred_time_histories;
        pred_tracker_entry pred_tracker[PT_ENTRIES];
        backedge_tracker_entry backedge_tracker[BACKEDGE_ENTRIES];
        Counter local_table[LT_ENTRIES];

        uint64_t guess_right;
        uint64_t guess_wrong;
        uint64_t tage_right;
        uint64_t tage_wrong;
        uint64_t alt_right;
        uint64_t alt_wrong;
        uint64_t total_right;
        uint64_t total_wrong;

    public:
        SampleHist active_hist;
        SampleCondPredictor (void)
        {
        }

        void setup()
        {
            active_hist = SampleHist();
            for (int i = 0; i < BACKEDGE_ENTRIES; ++i) {
                backedge_tracker[i].valid = false;
            }
            for (int i = 0; i < PT_ENTRIES; ++i) {
                pred_tracker[i] = pred_tracker_entry();
            }
            for (int i = 0; i < LT_ENTRIES; ++i) {
                local_table[i] = Counter(2);
            }
            guess_right = 0;
            guess_wrong = 0;
            tage_right = 0;
            tage_wrong = 0;
            alt_right = 0;
            alt_wrong = 0;
            total_right = 0;
            total_wrong = 0;
        }

        void terminate()
        {
            bool useful = total_right > tage_right;
            double usefulness = ((double)total_right / (double)(total_right + total_wrong)) - 
                                 ((double)tage_right / (double)(tage_right + tage_wrong));
            fprintf(stderr, "Sample Predictor: guess_right: %lu, guess_wrong: %lu, tage_right: %lu, tage_wrong: %lu, alt_right: %lu, alt_wrong: %lu, useful: %s, %f\n", guess_right, guess_wrong, tage_right, tage_wrong, alt_right, alt_wrong, useful ? "yes" : "no", usefulness);
            // print out the pred_time_histories
        }

        // sample function to get unique instruction id
        uint64_t get_unique_inst_id(uint64_t seq_no, uint8_t piece) const
        {
            assert(piece < 16);
            return (seq_no << 4) | (piece & 0x000F);
        }

        bool predict (uint64_t seq_no, uint8_t piece, uint64_t PC, const bool tage_pred, const bool tage_use_loop, const bool tage_use_sc)
        {
            active_hist.tage_pred = tage_pred;
            active_hist.tage_use_loop = tage_use_loop;
            active_hist.tage_use_sc = tage_use_sc;
            // checkpoint current hist
            // pred_time_histories.emplace(get_unique_inst_id(seq_no, piece), active_hist);
            const bool pred_taken = predict_using_given_hist(seq_no, piece, PC, active_hist, true/*pred_time_predict*/);
            pred_time_histories.emplace(get_unique_inst_id(seq_no, piece), active_hist);
            // if (tage_use_loop || tage_use_sc) {
            //     return tage_pred;
            // } else {
            //     return pred_taken;
            // }
            return pred_taken;
        }

        bool predict_using_given_hist (uint64_t seq_no, uint8_t piece, uint64_t PC, SampleHist& hist_to_use, const bool pred_time_predict)
        {
            bool pred = hist_to_use.tage_pred;
            if ((pred_tracker[(PC>>PC_SHIFT) & PT_ENTRIES_MASK].tage_tracker.predict()) || hist_to_use.tage_use_loop || hist_to_use.tage_use_sc) {
                hist_to_use.pred_track = false;
            } else {
                hist_to_use.pred_track = true;
                // pred = pred_tracker[(PC>>PC_SHIFT) & PT_ENTRIES_MASK].altpred.predict();
                uint64_t lhist = hist_to_use.local_hist[(PC >> PC_SHIFT) & LH_ENTRIES_MASK] & LT_ENTRIES_MASK;
                pred = local_table[lhist].predict();
                hist_to_use.pred_made = pred;
            }
            return pred;
        }

        bool get_tage_pred (uint64_t seq_no, uint8_t piece, uint64_t PC)
        {
            const auto pred_hist_key = get_unique_inst_id(seq_no, piece);
            const auto& pred_time_history = pred_time_histories.at(pred_hist_key);
            return pred_time_history.tage_pred;
        }

        void history_update (uint64_t seq_no, uint8_t piece, uint64_t PC, bool taken, uint64_t nextPC)
        {
            active_hist.ghist = active_hist.ghist << 1;
            active_hist.local_hist[(PC >> PC_SHIFT) & LH_ENTRIES_MASK] = (active_hist.local_hist[(PC >> PC_SHIFT) & LH_ENTRIES_MASK] << 1) | (taken ? 0x1 : 0x0);
            if(taken)
            {
                active_hist.ghist |= 1;
            }
        }

        void update (uint64_t seq_no, uint8_t piece, uint64_t PC, bool resolveDir, bool predDir, uint64_t nextPC)
        {
            const auto pred_hist_key = get_unique_inst_id(seq_no, piece);
            const auto& pred_time_history = pred_time_histories.at(pred_hist_key);
            update(PC, resolveDir, predDir, nextPC, pred_time_history);
            pred_time_histories.erase(pred_hist_key);

            if (nextPC < PC) {
                // fprintf(stderr, "backedge detected!\n");
            }
        }

        void update (uint64_t PC, bool resolveDir, bool pred_taken, uint64_t nextPC, const SampleHist& hist_to_use)
        {
            // stats tracking
            if (hist_to_use.tage_pred != resolveDir) {
                tage_wrong++;
                // pred_tracker[(PC>>PC_SHIFT) & PT_ENTRIES_MASK].tage_tracker.update(false);
                if (hist_to_use.pred_track) {
                    guess_right++;
                    if (hist_to_use.pred_made != resolveDir) {
                        alt_wrong++;
                    } else {
                        alt_right++;
                    }
                } else {
                    guess_wrong++;
                }
            } else {
                tage_right++;
                // fprintf(stderr, "tage was wrong\n");
                // pred_tracker[(PC>>PC_SHIFT) & PT_ENTRIES_MASK].tage_tracker.update(true);
                if (!hist_to_use.pred_track) {
                    guess_right++;
                } else {
                    guess_wrong++;
                    if (hist_to_use.pred_made != resolveDir) {
                        alt_wrong++;
                    } else {
                        alt_right++;
                    }
                }
            }
            if (pred_taken == resolveDir) {
                total_right++;
            } else {
                total_wrong++;
            }


            // update the pred_tracker
            pred_tracker[(PC>>PC_SHIFT) & PT_ENTRIES_MASK].tage_tracker.update(hist_to_use.tage_pred == resolveDir);
            // if (hist_to_use.pred_track) {
                // pred_tracker[(PC>>PC_SHIFT) & PT_ENTRIES_MASK].altpred.update(resolveDir == hist_to_use.pred_made);
            // }
            uint64_t lhist = hist_to_use.local_hist[(PC >> PC_SHIFT) & LH_ENTRIES_MASK] & LT_ENTRIES_MASK;
            local_table[lhist].update(resolveDir);
            active_hist = hist_to_use;
        }
};
// =================
// Predictor End
// =================

#endif
static SampleCondPredictor cond_predictor_impl;
