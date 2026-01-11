#include "imbalance_taker.hpp"

int ImbalanceTaker::on_state(const MarketState& s) {
    const double I = s.imbalance;

    // decrement holding timer if in position
    if (ticks_left_ > 0) --ticks_left_;

    if (state_ != 0) {
        // Exit when timer expires
        if (ticks_left_ == 0) {
            int out = -state_; // opposite action closes position
            state_ = 0;
            return out;
        }

        // Flip when imbalance strongly reverses
        if (state_ > 0 && I < -thresh_) {
            state_ = -1;
            ticks_left_ = hold_ticks_;
            return -1;
        }

        if (state_ < 0 && I > +thresh_) {
            state_ = +1;
            ticks_left_ = hold_ticks_;
            return +1;
        }

        return 0;
    }

    // Flat: entry signals
    if (I > +thresh_) {
        state_ = +1;
        ticks_left_ = hold_ticks_;
        return +1;
    }

    if (I < -thresh_) {
        state_ = -1;
        ticks_left_ = hold_ticks_;
        return -1;
    }

    return 0;
}
