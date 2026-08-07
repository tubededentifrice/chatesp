#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <unity.h>

#include "chatesp/agent_loop.hpp"
#include "chatesp/brave_protocol.hpp"
#include "chatesp/openrouter_protocol.hpp"
#include "chatesp/system_prompt.hpp"

using namespace chatesp::agent;

namespace {

class TestCancellation final : public CancellationToken {
public:
    bool value = false;
    [[nodiscard]] bool cancelled() const override { return value; }
};

class ProgressRecorder final : public AgentProgressObserver {
public:
    std::array<AgentProgressEvent, 8> events{};
    std::size_t size = 0;
    TestCancellation *cancellation = nullptr;
    AgentProgressEvent cancel_at = AgentProgressEvent::speech_start;
    bool cancel_enabled = false;

    void on_agent_progress(AgentProgressEvent event) override {
        if (size < events.size()) {
            events[size++] = event;
        }
        if (cancel_enabled && event == cancel_at && cancellation != nullptr) {
            cancellation->value = true;
        }
    }
};

class EchoTool final : public Tool {
public:
    int calls = 0;
    bool fail = false;

    [[nodiscard]] const char *name() const override { return "search_web"; }
    [[nodiscard]] const char *description() const override {
        return "Search test data.";
    }
    [[nodiscard]] const char *parameters_schema() const override {
        return "{\"type\":\"object\"}";
    }
    Error execute(
        const char *, std::size_t,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &) override {
        ++calls;
        if (fail) {
            return Error::tool_failed;
        }
        return result.assign("{\"ok\":true}") ? Error::none
                                                : Error::limit_exceeded;
    }
};

class SequenceChat final : public ChatProvider {
public:
    int tool_turns = 0;
    int calls = 0;
    bool cancel_during_call = false;

    Error complete(
        const ConversationHistory &, ChatTurn &turn,
        CancellationToken &cancellation) override {
        ++calls;
        if (cancel_during_call) {
            static_cast<TestCancellation &>(cancellation).value = true;
            return Error::none;
        }
        if (calls <= tool_turns) {
            turn.kind = ChatTurnKind::tool_call;
            turn.tool_call.id.assign("call_1");
            turn.tool_call.name.assign("search_web");
            turn.tool_call.arguments.assign("{\"query\":\"test\"}");
            return Error::none;
        }
        turn.kind = ChatTurnKind::answer;
        turn.answer.assign("A short answer.");
        return Error::none;
    }
};

class TestWebSearch final : public WebSearchProvider {
public:
    Error search(
        const char *query, std::size_t size, WebResults &results,
        CancellationToken &) override {
        if (size != 5 || std::memcmp(query, "Dubai", 5) != 0) {
            return Error::invalid_argument;
        }
        results.size = 1;
        results.items[0].title.assign("Result");
        results.items[0].url.assign("https://example.test/a");
        results.items[0].snippet.assign("Current fact.");
        return Error::none;
    }
};

class TestImageSearch final : public ImageSearchProvider {
public:
    Error search(
        const char *, std::size_t, ImageResults &results,
        CancellationToken &) override {
        results.size = 1;
        results.items[0].id.assign("img0");
        results.items[0].title.assign("A result");
        results.items[0].page_url.assign("https://example.test/page");
        results.items[0].thumbnail_url.assign("https://example.test/thumb.jpg");
        results.items[0].image_url.assign("https://example.test/full.jpg");
        return Error::none;
    }
};

void assert_error(Error expected, Error actual) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual));
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_fixed_text_stops_at_capacity() {
    FixedText<4> text;
    TEST_ASSERT_TRUE(text.append("1234"));
    TEST_ASSERT_FALSE(text.push_back('5'));
    TEST_ASSERT_EQUAL_UINT32(4, text.size());
    TEST_ASSERT_EQUAL_STRING("1234", text.c_str());
}

void test_prompt_is_short_and_voice_focused() {
    TEST_ASSERT_NOT_NULL(std::strstr(system_prompt, "one to three short"));
    TEST_ASSERT_NOT_NULL(std::strstr(system_prompt, "Write for speech"));
    TEST_ASSERT_NULL(std::strstr(system_prompt, "chain of thought"));
}

void test_history_rejects_empty_and_overlong_text() {
    ConversationHistory history;
    assert_error(Error::invalid_argument,
                 history.append_text(MessageRole::user, "", 0));
    char large[Limits::max_message_bytes + 2]{};
    assert_error(Error::invalid_argument,
                 history.append_text(
                     MessageRole::user, large, sizeof(large) - 1));
}

void test_registry_rejects_duplicate_and_unknown_tool() {
    EchoTool tool;
    ToolRegistry registry;
    assert_error(Error::none, registry.add(tool));
    assert_error(Error::invalid_argument, registry.add(tool));
    ToolInvocation call;
    call.id.assign("call_1");
    call.name.assign("missing");
    call.arguments.assign("{}");
    FixedText<Limits::max_tool_result_bytes> result;
    TestCancellation cancellation;
    assert_error(
        Error::tool_not_found,
        registry.execute(call, result, cancellation));
}

void test_agent_loop_executes_one_tool_and_keeps_history() {
    EchoTool tool;
    ToolRegistry registry;
    assert_error(Error::none, registry.add(tool));
    SequenceChat chat;
    chat.tool_turns = 1;
    AgentLoop loop(chat, registry);
    FixedText<Limits::max_answer_bytes> answer;
    TestCancellation cancellation;
    assert_error(
        Error::none, loop.run("Question", 8, answer, cancellation));
    TEST_ASSERT_EQUAL_STRING("A short answer.", answer.c_str());
    TEST_ASSERT_EQUAL_INT(1, tool.calls);
    TEST_ASSERT_EQUAL_UINT32(4, loop.history().size());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(MessageRole::assistant_tool_call),
        static_cast<int>(loop.history().at(1).role));
}

void test_agent_loop_stops_after_two_tool_rounds() {
    EchoTool tool;
    ToolRegistry registry;
    registry.add(tool);
    SequenceChat chat;
    chat.tool_turns = 3;
    AgentLoop loop(chat, registry);
    FixedText<Limits::max_answer_bytes> answer;
    TestCancellation cancellation;
    assert_error(
        Error::limit_exceeded,
        loop.run("Question", 8, answer, cancellation));
    TEST_ASSERT_EQUAL_INT(2, tool.calls);
    TEST_ASSERT_EQUAL_INT(3, chat.calls);
}

void test_agent_loop_honors_cancellation_before_provider() {
    EchoTool tool;
    ToolRegistry registry;
    registry.add(tool);
    SequenceChat chat;
    AgentLoop loop(chat, registry);
    FixedText<Limits::max_answer_bytes> answer;
    TestCancellation cancellation;
    cancellation.value = true;
    assert_error(
        Error::cancelled, loop.run("Question", 8, answer, cancellation));
    TEST_ASSERT_EQUAL_INT(0, chat.calls);
}

void test_agent_loop_rolls_back_cancelled_turn() {
    EchoTool tool;
    ToolRegistry registry;
    registry.add(tool);
    SequenceChat chat;
    chat.cancel_during_call = true;
    AgentLoop loop(chat, registry);
    FixedText<Limits::max_answer_bytes> answer;
    TestCancellation cancellation;
    assert_error(
        Error::cancelled, loop.run("Question", 8, answer, cancellation));
    TEST_ASSERT_EQUAL_UINT32(0, loop.history().size());
}

void test_agent_loop_gives_model_a_sanitized_tool_error() {
    EchoTool tool;
    tool.fail = true;
    ToolRegistry registry;
    registry.add(tool);
    SequenceChat chat;
    chat.tool_turns = 1;
    AgentLoop loop(chat, registry);
    FixedText<Limits::max_answer_bytes> answer;
    TestCancellation cancellation;
    assert_error(
        Error::none, loop.run("Question", 8, answer, cancellation));
    TEST_ASSERT_EQUAL_STRING(
        "{\"error\":\"Tool is unavailable.\"}",
        loop.history().at(2).content.c_str());
}

void test_agent_loop_reports_bounded_progress_in_order() {
    EchoTool tool;
    ToolRegistry registry;
    registry.add(tool);
    SequenceChat chat;
    chat.tool_turns = 1;
    ProgressRecorder progress;
    AgentLoop loop(chat, registry, &progress);
    FixedText<Limits::max_answer_bytes> answer;
    TestCancellation cancellation;

    assert_error(
        Error::none, loop.run("Question", 8, answer, cancellation));
    loop.report_speech_start();
    loop.report_speech_start();

    const std::array<AgentProgressEvent, 6> expected = {
        AgentProgressEvent::transcription_complete,
        AgentProgressEvent::model_start,
        AgentProgressEvent::tool_start,
        AgentProgressEvent::model_start,
        AgentProgressEvent::answer_ready,
        AgentProgressEvent::speech_start,
    };
    TEST_ASSERT_EQUAL_UINT32(expected.size(), progress.size);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(expected[index]),
            static_cast<int>(progress.events[index]));
    }
}

void test_agent_loop_stops_when_progress_observer_sees_cancellation() {
    EchoTool tool;
    ToolRegistry registry;
    registry.add(tool);
    SequenceChat chat;
    ProgressRecorder progress;
    TestCancellation cancellation;
    progress.cancellation = &cancellation;
    progress.cancel_at = AgentProgressEvent::model_start;
    progress.cancel_enabled = true;
    AgentLoop loop(chat, registry, &progress);
    FixedText<Limits::max_answer_bytes> answer;

    assert_error(
        Error::cancelled, loop.run("Question", 8, answer, cancellation));
    TEST_ASSERT_EQUAL_UINT32(2, progress.size);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(AgentProgressEvent::transcription_complete),
        static_cast<int>(progress.events[0]));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(AgentProgressEvent::model_start),
        static_cast<int>(progress.events[1]));
    TEST_ASSERT_EQUAL_INT(0, chat.calls);
    TEST_ASSERT_EQUAL_UINT32(0, loop.history().size());
    TEST_ASSERT_TRUE(answer.empty());

    loop.report_speech_start();
    TEST_ASSERT_EQUAL_UINT32(2, progress.size);
}

void test_agent_loop_cancels_before_a_reported_tool_starts() {
    EchoTool tool;
    ToolRegistry registry;
    registry.add(tool);
    SequenceChat chat;
    chat.tool_turns = 1;
    ProgressRecorder progress;
    TestCancellation cancellation;
    progress.cancellation = &cancellation;
    progress.cancel_at = AgentProgressEvent::tool_start;
    progress.cancel_enabled = true;
    AgentLoop loop(chat, registry, &progress);
    FixedText<Limits::max_answer_bytes> answer;

    assert_error(
        Error::cancelled, loop.run("Question", 8, answer, cancellation));
    TEST_ASSERT_EQUAL_INT(1, chat.calls);
    TEST_ASSERT_EQUAL_INT(0, tool.calls);
    TEST_ASSERT_EQUAL_UINT32(0, loop.history().size());
    TEST_ASSERT_TRUE(answer.empty());
    TEST_ASSERT_EQUAL_UINT32(3, progress.size);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(AgentProgressEvent::tool_start),
        static_cast<int>(progress.events[2]));
}

void test_search_tools_validate_query_and_bound_result() {
    TestCancellation cancellation;
    TestWebSearch web_provider;
    SearchWebTool web_tool(web_provider);
    FixedText<Limits::max_tool_result_bytes> result;
    const char args[] = "{\"query\":\"Dubai\"}";
    assert_error(
        Error::none,
        web_tool.execute(args, sizeof(args) - 1, result, cancellation));
    TEST_ASSERT_NOT_NULL(std::strstr(result.c_str(), "Current fact"));
    assert_error(
        Error::invalid_argument,
        web_tool.execute(
            "{\"query\":\"Dubai\",\"extra\":1}", 27, result,
            cancellation));

    TestImageSearch image_provider;
    SearchImagesTool image_tool(image_provider);
    assert_error(
        Error::none,
        image_tool.execute(args, sizeof(args) - 1, result, cancellation));
    TEST_ASSERT_EQUAL_UINT32(1, image_tool.last_results().size);
    TEST_ASSERT_NOT_NULL(std::strstr(result.c_str(), "thumbnail_url"));
    image_tool.clear_results();
    TEST_ASSERT_EQUAL_UINT32(0, image_tool.last_results().size);
}

void test_openrouter_chat_builder_has_bounded_contract() {
    ConversationHistory history;
    history.append_text(MessageRole::user, "Hello \"watch\"", 13);
    EchoTool tool;
    ToolRegistry registry;
    registry.add(tool);
    ChatRequestBody body;
    assert_error(
        Error::none,
        build_openrouter_chat_request(
            OpenRouterConfig{}, history, registry, true, body));
    TEST_ASSERT_NOT_NULL(
        std::strstr(body.c_str(), "deepseek/deepseek-v4-flash"));
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "\\\"watch\\\""));
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "search_web"));
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "\"max_tokens\":160"));
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "\"stream\":true"));
    TEST_ASSERT_NULL(std::strstr(body.c_str(), "Authorization"));

    OpenRouterConfig invalid;
    invalid.chat_model = "model\r\nbad";
    assert_error(
        Error::invalid_argument,
        build_openrouter_chat_request(
            invalid, history, registry, true, body));
}

void test_openrouter_parses_answer_and_tool_call() {
    const char answer_json[] =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"It is 35\\u00b0C.\"},\"finish_reason\":\"stop\"}]}";
    ChatTurn turn;
    assert_error(
        Error::none,
        parse_openrouter_chat_response(
            answer_json, sizeof(answer_json) - 1, turn));
    TEST_ASSERT_EQUAL_STRING("It is 35\xC2\xB0" "C.", turn.answer.c_str());

    const char tool_json[] =
        "{\"choices\":[{\"message\":{\"content\":\"draft\","
        "\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
        "\"function\":{\"name\":\"search_web\","
        "\"arguments\":\"{\\\"query\\\":\\\"weather\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}]}";
    assert_error(
        Error::none,
        parse_openrouter_chat_response(tool_json, sizeof(tool_json) - 1, turn));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ChatTurnKind::tool_call),
        static_cast<int>(turn.kind));
    TEST_ASSERT_EQUAL_STRING("search_web", turn.tool_call.name.c_str());
    TEST_ASSERT_TRUE(turn.answer.empty());
}

void test_openrouter_sse_parser_handles_split_input_and_comments() {
    const char stream[] =
        ": OPENROUTER PROCESSING\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello \"},"
        "\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"watch.\"},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    OpenRouterSseParser parser;
    for (std::size_t index = 0; index < sizeof(stream) - 1; index += 7) {
        const std::size_t remaining = sizeof(stream) - 1 - index;
        const std::size_t chunk = remaining < 7 ? remaining : 7;
        assert_error(Error::none, parser.feed(stream + index, chunk));
    }
    assert_error(Error::none, parser.finish());
    TEST_ASSERT_TRUE(parser.done());
    TEST_ASSERT_EQUAL_STRING("Hello watch.", parser.turn().answer.c_str());
}

void test_openrouter_transcription_and_speech_protocols() {
    MultipartTranscriptionPlan plan;
    assert_error(
        Error::none,
        build_openrouter_transcription_plan(OpenRouterConfig{}, 1'004, plan));
    TEST_ASSERT_EQUAL_UINT64(
        plan.preamble.size() + 1'004 + plan.epilogue.size(),
        plan.content_length);
    TEST_ASSERT_NOT_NULL(std::strstr(plan.preamble.c_str(), "speech.wav"));
    assert_error(
        Error::invalid_argument,
        build_openrouter_transcription_plan(
            OpenRouterConfig{}, Limits::max_audio_file_bytes + 1, plan));

    const char response[] = "{\"text\":\"Bonjour\",\"usage\":{\"seconds\":1}}";
    FixedText<Limits::max_transcript_bytes> transcript;
    assert_error(
        Error::none,
        parse_openrouter_transcription_response(
            response, sizeof(response) - 1, transcript));
    TEST_ASSERT_EQUAL_STRING("Bonjour", transcript.c_str());

    SpeechRequestBody body;
    assert_error(
        Error::none,
        build_openrouter_speech_request(
            OpenRouterConfig{}, "Hello.", 6, body));
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "Transcript:\\nHello."));
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "\"voice\":\"Achird\""));
    assert_error(
        Error::none,
        validate_openrouter_pcm_content_type(
            "audio/pcm;rate=24000;channels=1"));
    assert_error(
        Error::unsupported_media,
        validate_openrouter_pcm_content_type("audio/mpeg"));
}

void test_brave_builders_encode_query_and_set_safe_limits() {
    SearchRequestTarget target;
    const char query[] = "Dubai weather?";
    assert_error(
        Error::none,
        build_brave_web_search_target(
            query, sizeof(query) - 1, BraveSearchOptions{}, target));
    TEST_ASSERT_NOT_NULL(std::strstr(target.c_str(), "Dubai%20weather%3F"));
    TEST_ASSERT_NOT_NULL(std::strstr(target.c_str(), "count=5"));
    TEST_ASSERT_NOT_NULL(std::strstr(target.c_str(), "result_filter=web"));
    TEST_ASSERT_NOT_NULL(std::strstr(target.c_str(), "safesearch=strict"));

    assert_error(
        Error::none,
        build_brave_image_search_target(
            query, sizeof(query) - 1, BraveSearchOptions{}, target));
    TEST_ASSERT_NOT_NULL(std::strstr(target.c_str(), "count=6"));
}

void test_brave_parsers_keep_only_narrow_fields() {
    const char web_json[] =
        "{\"type\":\"search\",\"web\":{\"results\":[{"
        "\"title\":\"Title\",\"url\":\"https://example.test\","
        "\"description\":\"Snippet\",\"age\":\"today\"}]}}";
    WebResults web;
    assert_error(
        Error::none,
        parse_brave_web_response(web_json, sizeof(web_json) - 1, web));
    TEST_ASSERT_EQUAL_UINT32(1, web.size);
    TEST_ASSERT_EQUAL_STRING("Snippet", web.items[0].snippet.c_str());

    const char image_json[] =
        "{\"type\":\"images\",\"results\":[{\"title\":\"Image\","
        "\"url\":\"https://example.test/page\","
        "\"thumbnail\":{\"src\":\"https://example.test/thumb.jpg\"},"
        "\"properties\":{\"url\":\"https://example.test/full.jpg\","
        "\"width\":500,\"height\":400}}],\"extra\":{}}";
    ImageResults images;
    assert_error(
        Error::none,
        parse_brave_image_response(
            image_json, sizeof(image_json) - 1, images));
    TEST_ASSERT_EQUAL_UINT32(1, images.size);
    TEST_ASSERT_EQUAL_STRING("img0", images.items[0].id.c_str());
    TEST_ASSERT_EQUAL_UINT32(500, images.items[0].width);
    TEST_ASSERT_EQUAL_STRING(
        "https://example.test/thumb.jpg",
        images.items[0].thumbnail_url.c_str());
}

void test_retry_policy_never_retries_after_output() {
    const RequestPolicy policy = chat_policy();
    TEST_ASSERT_TRUE(retry_allowed(Error::server_error, 1, false, policy));
    TEST_ASSERT_FALSE(retry_allowed(Error::server_error, 1, true, policy));
    TEST_ASSERT_FALSE(retry_allowed(Error::authentication, 1, false, policy));
    TEST_ASSERT_FALSE(retry_allowed(Error::server_error, 2, false, policy));
}

void test_image_fetch_contract_requires_https_and_hard_caps() {
    ImageFetchRequest request;
    request.url = "https://example.test/image.jpg";
    assert_error(Error::none, validate_image_fetch_request(request));
    request.url = "http://example.test/image.jpg";
    assert_error(
        Error::invalid_argument, validate_image_fetch_request(request));
    request.url = "https://example.test/image.jpg";
    request.max_bytes = Limits::max_image_download_bytes + 1;
    assert_error(
        Error::limit_exceeded, validate_image_fetch_request(request));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_fixed_text_stops_at_capacity);
    RUN_TEST(test_prompt_is_short_and_voice_focused);
    RUN_TEST(test_history_rejects_empty_and_overlong_text);
    RUN_TEST(test_registry_rejects_duplicate_and_unknown_tool);
    RUN_TEST(test_agent_loop_executes_one_tool_and_keeps_history);
    RUN_TEST(test_agent_loop_stops_after_two_tool_rounds);
    RUN_TEST(test_agent_loop_honors_cancellation_before_provider);
    RUN_TEST(test_agent_loop_rolls_back_cancelled_turn);
    RUN_TEST(test_agent_loop_gives_model_a_sanitized_tool_error);
    RUN_TEST(test_agent_loop_reports_bounded_progress_in_order);
    RUN_TEST(test_agent_loop_stops_when_progress_observer_sees_cancellation);
    RUN_TEST(test_agent_loop_cancels_before_a_reported_tool_starts);
    RUN_TEST(test_search_tools_validate_query_and_bound_result);
    RUN_TEST(test_openrouter_chat_builder_has_bounded_contract);
    RUN_TEST(test_openrouter_parses_answer_and_tool_call);
    RUN_TEST(test_openrouter_sse_parser_handles_split_input_and_comments);
    RUN_TEST(test_openrouter_transcription_and_speech_protocols);
    RUN_TEST(test_brave_builders_encode_query_and_set_safe_limits);
    RUN_TEST(test_brave_parsers_keep_only_narrow_fields);
    RUN_TEST(test_retry_policy_never_retries_after_output);
    RUN_TEST(test_image_fetch_contract_requires_https_and_hard_caps);
    return UNITY_END();
}
