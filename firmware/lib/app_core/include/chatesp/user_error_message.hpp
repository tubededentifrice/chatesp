#pragma once

#include "chatesp/agent_types.hpp"

namespace chatesp {

inline const char *request_error_message(agent::Error error) {
    switch (error) {
        case agent::Error::none:
            return "THE REQUEST COMPLETED";
        case agent::Error::invalid_argument:
            return "UNABLE TO PROCESS THE REQUEST";
        case agent::Error::limit_exceeded:
            return "REQUEST LIMIT REACHED";
        case agent::Error::request_too_large:
            return "THE REQUEST WAS TOO LARGE";
        case agent::Error::response_too_large:
            return "THE MODEL RESPONSE WAS TOO LARGE";
        case agent::Error::malformed_response:
            return "THE SERVICE RETURNED INVALID DATA";
        case agent::Error::cancelled:
            return "THE REQUEST WAS CANCELLED";
        case agent::Error::connect_timeout:
        case agent::Error::disconnected:
            return "CHECK THE WI-FI CONNECTION";
        case agent::Error::first_byte_timeout:
        case agent::Error::idle_timeout:
        case agent::Error::total_timeout:
            return "THE REQUEST TOOK TOO LONG";
        case agent::Error::rate_limited:
            return "THE SERVICE IS BUSY";
        case agent::Error::authentication:
            return "CHECK THE SERVICE KEY";
        case agent::Error::payment_required:
            return "THE SERVICE NEEDS CREDIT";
        case agent::Error::server_error:
            return "THE SERVICE FAILED";
        case agent::Error::unsupported_media:
            return "THE SERVICE RETURNED AN UNSUPPORTED FORMAT";
        case agent::Error::tool_not_found:
            return "THE REQUESTED TOOL IS NOT AVAILABLE";
        case agent::Error::tool_failed:
            return "A REQUEST TOOL FAILED";
        case agent::Error::model_failed:
            return "THE MODEL COULD NOT COMPLETE THE ANSWER";
    }
    return "UNABLE TO COMPLETE THE REQUEST";
}

inline const char *speech_error_message(agent::Error error) {
    switch (error) {
        case agent::Error::none:
            return "SPEECH COMPLETED";
        case agent::Error::invalid_argument:
        case agent::Error::limit_exceeded:
        case agent::Error::request_too_large:
        case agent::Error::tool_not_found:
        case agent::Error::tool_failed:
            return "UNABLE TO PREPARE SPEECH";
        case agent::Error::response_too_large:
            return "THE SPEECH AUDIO WAS TOO LARGE";
        case agent::Error::malformed_response:
        case agent::Error::unsupported_media:
            return "THE SPEECH SERVICE RETURNED INVALID AUDIO";
        case agent::Error::cancelled:
            return "SPEECH WAS CANCELLED";
        case agent::Error::connect_timeout:
        case agent::Error::disconnected:
            return "SPEECH LOST THE SERVICE CONNECTION";
        case agent::Error::first_byte_timeout:
        case agent::Error::idle_timeout:
        case agent::Error::total_timeout:
            return "THE SPEECH REQUEST TOOK TOO LONG";
        case agent::Error::rate_limited:
            return "THE SPEECH SERVICE IS BUSY";
        case agent::Error::authentication:
            return "CHECK THE SPEECH SERVICE KEY";
        case agent::Error::payment_required:
            return "THE SPEECH SERVICE NEEDS CREDIT";
        case agent::Error::server_error:
            return "THE SPEECH SERVICE FAILED";
        case agent::Error::model_failed:
            return "UNABLE TO PLAY THE ANSWER";
    }
    return "UNABLE TO PLAY THE ANSWER";
}

}  // namespace chatesp
