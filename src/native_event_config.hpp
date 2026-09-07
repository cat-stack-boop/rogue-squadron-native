#pragma once
#include "ultramodern/ultramodern.hpp"

inline ultramodern::MessageQueueControl rs_message_queue_control() {
    // Native scheduling can deliver completed hardware work in a burst. Retain
    // each completion until the guest queue has room; VI notifications may drop.
    return {.requeue_timer=true,.requeue_sp=true,.requeue_si=true,.requeue_ai=true,
            .requeue_vi=false,.requeue_pi=true,.requeue_dp=true};
}
