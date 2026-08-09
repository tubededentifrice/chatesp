#pragma once

#include "chatesp/agent_types.hpp"

namespace chatesp {

inline const char *request_error_message(agent::Error error) {
    switch (error) {
        case agent::Error::none:
            return "THE REQUEST COMPLETED";
        case agent::Error::invalid_argument:
            return "THE DEVICE COULD NOT READ THIS REQUEST";
        case agent::Error::limit_exceeded:
            return "THE REQUEST EXCEEDED A DEVICE DATA LIMIT";
        case agent::Error::tool_round_limit:
            return "THE MODEL USED TOO MANY TOOL STEPS";
        case agent::Error::request_too_large:
            return "THE SERVICE REQUEST WAS TOO LARGE";
        case agent::Error::response_too_large:
            return "THE SERVICE RESPONSE WAS TOO LARGE";
        case agent::Error::malformed_response:
            return "THE SERVICE SENT AN INVALID RESPONSE";
        case agent::Error::cancelled:
            return "THE REQUEST WAS CANCELLED";
        case agent::Error::connect_timeout:
            return "THE SERVICE CONNECTION TIMED OUT";
        case agent::Error::disconnected:
            return "THE SERVICE CONNECTION WAS LOST";
        case agent::Error::first_byte_timeout:
            return "THE SERVICE DID NOT START ITS RESPONSE IN TIME";
        case agent::Error::idle_timeout:
            return "THE SERVICE STOPPED SENDING ITS RESPONSE";
        case agent::Error::total_timeout:
            return "THE SERVICE REQUEST EXCEEDED ITS TIME LIMIT";
        case agent::Error::rate_limited:
            return "THE SERVICE RATE LIMIT WAS REACHED";
        case agent::Error::authentication:
            return "THE SERVICE KEY IS MISSING OR INVALID";
        case agent::Error::payment_required:
            return "THE SERVICE ACCOUNT NEEDS CREDIT";
        case agent::Error::server_error:
            return "THE SERVICE REPORTED AN INTERNAL ERROR";
        case agent::Error::unsupported_media:
            return "THE SERVICE RETURNED AN UNSUPPORTED FORMAT";
        case agent::Error::tool_not_found:
            return "THE REQUESTED TOOL IS NOT AVAILABLE";
        case agent::Error::tool_failed:
            return "THE REQUESTED TOOL COULD NOT FINISH";
        case agent::Error::model_failed:
            return "THE MODEL COULD NOT COMPLETE THE ANSWER";
    }
    return "THE DEVICE REPORTED AN UNKNOWN REQUEST ERROR";
}

inline const char *speech_error_message(agent::Error error) {
    switch (error) {
        case agent::Error::none:
            return "SPEECH COMPLETED";
        case agent::Error::invalid_argument:
            return "THE DEVICE COULD NOT PREPARE THE ANSWER TEXT";
        case agent::Error::limit_exceeded:
            return "SPEECH EXCEEDED A DEVICE DATA LIMIT";
        case agent::Error::tool_round_limit:
            return "SPEECH STOPPED AFTER TOO MANY PREPARATION STEPS";
        case agent::Error::request_too_large:
            return "THE ANSWER TEXT WAS TOO LARGE FOR SPEECH";
        case agent::Error::tool_not_found:
            return "THE SPEECH TOOL IS NOT AVAILABLE";
        case agent::Error::tool_failed:
            return "THE SPEECH TOOL COULD NOT FINISH";
        case agent::Error::response_too_large:
            return "THE SPEECH AUDIO WAS TOO LARGE";
        case agent::Error::malformed_response:
            return "THE SPEECH SERVICE SENT INVALID AUDIO";
        case agent::Error::unsupported_media:
            return "THE SPEECH SERVICE SENT AN UNSUPPORTED AUDIO FORMAT";
        case agent::Error::cancelled:
            return "SPEECH WAS CANCELLED";
        case agent::Error::connect_timeout:
            return "THE SPEECH SERVICE CONNECTION TIMED OUT";
        case agent::Error::disconnected:
            return "SPEECH LOST THE SERVICE CONNECTION";
        case agent::Error::first_byte_timeout:
            return "THE SPEECH SERVICE DID NOT START AUDIO IN TIME";
        case agent::Error::idle_timeout:
            return "THE SPEECH SERVICE STOPPED SENDING AUDIO";
        case agent::Error::total_timeout:
            return "THE SPEECH REQUEST EXCEEDED ITS TIME LIMIT";
        case agent::Error::rate_limited:
            return "THE SPEECH SERVICE RATE LIMIT WAS REACHED";
        case agent::Error::authentication:
            return "THE SPEECH SERVICE KEY IS MISSING OR INVALID";
        case agent::Error::payment_required:
            return "THE SPEECH SERVICE ACCOUNT NEEDS CREDIT";
        case agent::Error::server_error:
            return "THE SPEECH SERVICE REPORTED AN INTERNAL ERROR";
        case agent::Error::model_failed:
            return "THE SPEECH MODEL COULD NOT CREATE AUDIO";
    }
    return "THE DEVICE REPORTED AN UNKNOWN SPEECH ERROR";
}

}  // namespace chatesp
