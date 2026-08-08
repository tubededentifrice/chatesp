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

class ChatTextRecorder final : public ChatTextSink {
public:
    std::array<FixedText<Limits::max_answer_bytes>, 4> updates{};
    std::size_t size = 0;
    TestCancellation *cancellation = nullptr;

    Error write_chat_text(const char *text, std::size_t text_size) override {
        if (size >= updates.size() ||
            !updates[size].assign(text, text_size)) {
            return Error::limit_exceeded;
        }
        ++size;
        if (cancellation != nullptr) {
            cancellation->value = true;
        }
        return Error::none;
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
    int calls = 0;

    Error search(
        const char *query, std::size_t size, ImageResults &results,
        CancellationToken &) override {
        ++calls;
        if (size != 5 || std::memcmp(query, "Dubai", 5) != 0) {
            return Error::invalid_argument;
        }
        results.size = 1;
        results.items[0].id.assign("img0");
        results.items[0].title.assign("A result");
        results.items[0].page_url.assign("https://example.test/page");
        results.items[0].thumbnail_url.assign("https://example.test/thumb.jpg");
        results.items[0].image_url.assign("https://example.test/full.jpg");
        return Error::none;
    }
};

class TestDeviceControl final : public DeviceControlProvider {
public:
    Error status(DeviceStatus &output) override {
        ++status_calls;
        if (failure != Error::none) {
            return failure;
        }
        output = status_value;
        return Error::none;
    }

    Error set_brightness(
        std::uint8_t percent, bool &persisted) override {
        ++brightness_calls;
        last_brightness = percent;
        persisted = persist_changes;
        return failure;
    }

    Error set_volume(
        std::uint8_t percent, bool &persisted) override {
        ++volume_calls;
        last_volume = percent;
        persisted = persist_changes;
        return failure;
    }

    Error schedule_power_off(PowerOffMode &mode) override {
        ++power_off_calls;
        mode = power_mode;
        return failure;
    }

    DeviceStatus status_value{};
    PowerOffMode power_mode = PowerOffMode::system_off;
    Error failure = Error::none;
    int status_calls = 0;
    int brightness_calls = 0;
    int volume_calls = 0;
    int power_off_calls = 0;
    std::uint8_t last_brightness = 0;
    std::uint8_t last_volume = 0;
    bool persist_changes = true;
};

class ImageSelectionChat final : public ChatProvider {
public:
    int calls = 0;

    Error complete(
        const ConversationHistory &, ChatTurn &turn,
        CancellationToken &) override {
        ++calls;
        if (calls == 1) {
            turn.kind = ChatTurnKind::tool_call;
            turn.tool_call.id.assign("call_1");
            turn.tool_call.name.assign("search_images");
            turn.tool_call.arguments.assign("{\"query\":\"Dubai\"}");
            return Error::none;
        }
        if (calls == 2) {
            turn.kind = ChatTurnKind::tool_call;
            turn.tool_call.id.assign("call_2");
            turn.tool_call.name.assign("search_images");
            turn.tool_call.arguments.assign("{\"select\":\"img0\"}");
            return Error::none;
        }
        turn.kind = ChatTurnKind::answer;
        turn.answer.assign("Here is the image.");
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
    TEST_ASSERT_NOT_NULL(std::strstr(system_prompt, "select one current"));
    TEST_ASSERT_NOT_NULL(std::strstr(system_prompt, "Never select a URL"));
    TEST_ASSERT_NOT_NULL(
        std::strstr(system_prompt, "same language as the user's question"));
    TEST_ASSERT_NOT_NULL(
        std::strstr(answer_prompt, "same language as the user's question"));
    TEST_ASSERT_NOT_NULL(
        std::strstr(system_prompt, "Never claim that search is unsupported"));
    TEST_ASSERT_NOT_NULL(std::strstr(routing_prompt, "get_device_status"));
    TEST_ASSERT_NOT_NULL(std::strstr(routing_prompt, "explicitly asks"));
    TEST_ASSERT_NOT_NULL(std::strstr(answer_prompt, "power-off is scheduled"));
    TEST_ASSERT_NOT_NULL(std::strstr(answer_prompt, "bottom PWR-button"));
    TEST_ASSERT_NULL(std::strstr(system_prompt, "chain of thought"));
    TEST_ASSERT_NOT_NULL(
        std::strstr(system_prompt, "ask one short clarifying question"));
    TEST_ASSERT_NOT_NULL(
        std::strstr(answer_prompt, "ask one short clarifying question"));
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

void test_agent_loop_stops_after_three_tool_rounds() {
    EchoTool tool;
    ToolRegistry registry;
    registry.add(tool);
    SequenceChat chat;
    chat.tool_turns = 4;
    AgentLoop loop(chat, registry);
    FixedText<Limits::max_answer_bytes> answer;
    TestCancellation cancellation;
    assert_error(
        Error::limit_exceeded,
        loop.run("Question", 8, answer, cancellation));
    TEST_ASSERT_EQUAL_INT(3, tool.calls);
    TEST_ASSERT_EQUAL_INT(4, chat.calls);
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
        "{\"error\":\"requested_data_temporarily_unavailable\"}",
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

    const std::array<AgentProgressEvent, 8> expected = {
        AgentProgressEvent::transcription_complete,
        AgentProgressEvent::model_start,
        AgentProgressEvent::tool_start,
        AgentProgressEvent::model_start,
        AgentProgressEvent::answer_start,
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

void test_agent_loop_nonstream_provider_publishes_only_final_answer() {
    EchoTool tool;
    ToolRegistry registry;
    registry.add(tool);
    SequenceChat chat;
    chat.tool_turns = 1;
    ChatTextRecorder text;
    AgentLoop loop(chat, registry, nullptr, &text);
    FixedText<Limits::max_answer_bytes> answer;
    TestCancellation cancellation;

    assert_error(
        Error::none, loop.run("Question", 8, answer, cancellation));
    TEST_ASSERT_EQUAL_UINT32(1, text.size);
    TEST_ASSERT_EQUAL_STRING("A short answer.", text.updates[0].c_str());
    TEST_ASSERT_EQUAL_STRING("A short answer.", answer.c_str());
}

void test_agent_loop_rolls_back_when_text_callback_cancels() {
    EchoTool tool;
    ToolRegistry registry;
    registry.add(tool);
    SequenceChat chat;
    ChatTextRecorder text;
    TestCancellation cancellation;
    text.cancellation = &cancellation;
    AgentLoop loop(chat, registry, nullptr, &text);
    FixedText<Limits::max_answer_bytes> answer;

    assert_error(
        Error::cancelled, loop.run("Question", 8, answer, cancellation));
    TEST_ASSERT_EQUAL_UINT32(1, text.size);
    TEST_ASSERT_EQUAL_UINT32(0, loop.history().size());
    TEST_ASSERT_TRUE(answer.empty());
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
    TEST_ASSERT_EQUAL_INT(1, image_provider.calls);
    TEST_ASSERT_EQUAL_UINT32(1, image_tool.last_results().size);
    TEST_ASSERT_NOT_NULL(std::strstr(result.c_str(), "thumbnail_url"));
    image_tool.clear_results();
    TEST_ASSERT_EQUAL_UINT32(0, image_tool.last_results().size);
}

void test_device_status_tool_returns_bounded_status() {
    TestDeviceControl provider;
    provider.status_value = {
        42, 17, 88, true, true, PowerOffMode::development_sleep};
    GetDeviceStatusTool tool(provider);
    TestCancellation cancellation;
    FixedText<Limits::max_tool_result_bytes> result;

    assert_error(
        Error::none, tool.execute("{}", 2, result, cancellation));
    TEST_ASSERT_EQUAL_INT(1, provider.status_calls);
    TEST_ASSERT_EQUAL_STRING(
        "{\"brightness_percent\":42,\"volume_percent\":17,"
        "\"battery_percent\":88,\"settings_persistent\":true,"
        "\"power_off_mode\":\"development_sleep\"}",
        result.c_str());

    provider.status_value.battery_available = false;
    result.clear();
    assert_error(
        Error::none, tool.execute(" { } ", 5, result, cancellation));
    TEST_ASSERT_NOT_NULL(std::strstr(result.c_str(), "\"battery_percent\":null"));
    assert_error(
        Error::invalid_argument,
        tool.execute("{\"extra\":1}", 11, result, cancellation));
}

void test_device_setting_tools_validate_and_report_persistence() {
    TestDeviceControl provider;
    TestCancellation cancellation;
    FixedText<Limits::max_tool_result_bytes> result;
    SetBrightnessTool brightness(provider);
    SetVolumeTool volume(provider);

    assert_error(
        Error::none,
        brightness.execute("{\"percent\":5}", 13, result, cancellation));
    TEST_ASSERT_EQUAL_UINT8(5, provider.last_brightness);
    TEST_ASSERT_EQUAL_STRING(
        "{\"brightness_percent\":5,\"persisted\":true}", result.c_str());
    result.clear();
    assert_error(
        Error::none,
        brightness.execute("{\"percent\":100}", 15, result, cancellation));
    TEST_ASSERT_EQUAL_UINT8(100, provider.last_brightness);
    TEST_ASSERT_EQUAL_STRING(
        "{\"brightness_percent\":100,\"persisted\":true}", result.c_str());
    result.clear();
    provider.persist_changes = false;
    assert_error(
        Error::none,
        volume.execute("{\"percent\":0}", 13, result, cancellation));
    TEST_ASSERT_EQUAL_UINT8(0, provider.last_volume);
    TEST_ASSERT_EQUAL_STRING(
        "{\"volume_percent\":0,\"persisted\":false}", result.c_str());
    result.clear();
    assert_error(
        Error::none,
        volume.execute("{\"percent\":100}", 15, result, cancellation));
    TEST_ASSERT_EQUAL_UINT8(100, provider.last_volume);
    TEST_ASSERT_EQUAL_STRING(
        "{\"volume_percent\":100,\"persisted\":false}", result.c_str());

    assert_error(
        Error::invalid_argument,
        brightness.execute("{\"percent\":4}", 13, result, cancellation));
    assert_error(
        Error::invalid_argument,
        brightness.execute("{\"percent\":101}", 15, result, cancellation));
    assert_error(
        Error::invalid_argument,
        volume.execute("{\"percent\":101}", 15, result, cancellation));
    assert_error(
        Error::invalid_argument,
        volume.execute("{\"percent\":50,\"extra\":1}", 25, result,
                       cancellation));
    assert_error(
        Error::invalid_argument,
        volume.execute("{\"percent\":\"50\"}", 16, result, cancellation));
    TEST_ASSERT_EQUAL_INT(2, provider.brightness_calls);
    TEST_ASSERT_EQUAL_INT(2, provider.volume_calls);
}

void test_power_off_tool_is_strict_and_cancellable() {
    TestDeviceControl provider;
    provider.power_mode = PowerOffMode::development_sleep;
    PowerOffTool tool(provider);
    TestCancellation cancellation;
    FixedText<Limits::max_tool_result_bytes> result;

    assert_error(
        Error::none, tool.execute("{}", 2, result, cancellation));
    TEST_ASSERT_EQUAL_INT(1, provider.power_off_calls);
    TEST_ASSERT_EQUAL_STRING(
        "{\"scheduled\":true,\"mode\":\"development_sleep\"}",
        result.c_str());
    assert_error(
        Error::invalid_argument,
        tool.execute("{\"now\":true}", 12, result, cancellation));
    cancellation.value = true;
    assert_error(
        Error::cancelled, tool.execute("{}", 2, result, cancellation));
    TEST_ASSERT_EQUAL_INT(1, provider.power_off_calls);
}

void test_registry_holds_search_and_device_tools() {
    TestWebSearch web_provider;
    TestImageSearch image_provider;
    TestDeviceControl device_provider;
    SearchWebTool web(web_provider);
    SearchImagesTool images(image_provider);
    GetDeviceStatusTool status(device_provider);
    SetBrightnessTool brightness(device_provider);
    SetVolumeTool volume(device_provider);
    PowerOffTool power_off(device_provider);
    ToolRegistry registry;

    assert_error(Error::none, registry.add(web));
    assert_error(Error::none, registry.add(images));
    assert_error(Error::none, registry.add(status));
    assert_error(Error::none, registry.add(brightness));
    assert_error(Error::none, registry.add(volume));
    assert_error(Error::none, registry.add(power_off));
    TEST_ASSERT_EQUAL_UINT32(6, registry.size());
    TEST_ASSERT_EQUAL_UINT32(6, Limits::max_tool_count);
}

void test_image_tool_selects_only_a_current_result_id_once() {
    TestCancellation cancellation;
    TestImageSearch provider;
    SearchImagesTool tool(provider);
    FixedText<Limits::max_tool_result_bytes> result;
    const char query[] = "{\"query\":\"Dubai\"}";
    assert_error(
        Error::none,
        tool.execute(query, sizeof(query) - 1, result, cancellation));

    result.clear();
    const char url_selection[] =
        "{\"select\":\"https://example.test/full.jpg\"}";
    assert_error(
        Error::invalid_argument,
        tool.execute(
            url_selection, sizeof(url_selection) - 1, result, cancellation));
    const char both[] = "{\"query\":\"Dubai\",\"select\":\"img0\"}";
    assert_error(
        Error::invalid_argument,
        tool.execute(both, sizeof(both) - 1, result, cancellation));

    const char selection[] = "{\"select\":\"img0\"}";
    while (result.push_back('x')) {
    }
    assert_error(
        Error::limit_exceeded,
        tool.execute(
            selection, sizeof(selection) - 1, result, cancellation));
    ImageResult selected;
    TEST_ASSERT_FALSE(tool.take_selected(selected));

    result.clear();
    assert_error(
        Error::none,
        tool.execute(
            selection, sizeof(selection) - 1, result, cancellation));
    TEST_ASSERT_EQUAL_STRING("{\"selected\":\"img0\"}", result.c_str());
    TEST_ASSERT_NULL(std::strstr(result.c_str(), "https://"));

    TEST_ASSERT_TRUE(tool.take_selected(selected));
    TEST_ASSERT_EQUAL_STRING("img0", selected.id.c_str());
    TEST_ASSERT_EQUAL_STRING(
        "https://example.test/full.jpg", selected.image_url.c_str());
    TEST_ASSERT_FALSE(tool.take_selected(selected));
    TEST_ASSERT_TRUE(selected.id.empty());
}

void test_image_tool_clears_selection_on_search_and_clear() {
    TestCancellation cancellation;
    TestImageSearch provider;
    SearchImagesTool tool(provider);
    FixedText<Limits::max_tool_result_bytes> result;
    const char query[] = "{\"query\":\"Dubai\"}";
    const char selection[] = "{\"select\":\"img0\"}";
    ImageResult selected;

    assert_error(
        Error::none,
        tool.execute(query, sizeof(query) - 1, result, cancellation));
    result.clear();
    assert_error(
        Error::none,
        tool.execute(
            selection, sizeof(selection) - 1, result, cancellation));
    result.clear();
    assert_error(
        Error::none,
        tool.execute(query, sizeof(query) - 1, result, cancellation));
    TEST_ASSERT_FALSE(tool.take_selected(selected));

    result.clear();
    assert_error(
        Error::none,
        tool.execute(
            selection, sizeof(selection) - 1, result, cancellation));
    tool.clear_results();
    TEST_ASSERT_FALSE(tool.take_selected(selected));
    TEST_ASSERT_EQUAL_UINT32(0, tool.last_results().size);
}

void test_image_tool_can_use_one_fallback_after_a_search() {
    TestCancellation cancellation;
    TestImageSearch provider;
    SearchImagesTool tool(provider);
    FixedText<Limits::max_tool_result_bytes> result;
    const char query[] = "{\"query\":\"Dubai\"}";
    ImageResult selected;

    assert_error(
        Error::none,
        tool.execute(query, sizeof(query) - 1, result, cancellation));
    TEST_ASSERT_TRUE(tool.take_selected_or_first(selected));
    TEST_ASSERT_EQUAL_STRING("img0", selected.id.c_str());
    TEST_ASSERT_FALSE(tool.take_selected_or_first(selected));
    TEST_ASSERT_TRUE(selected.id.empty());
}

void test_agent_loop_searches_selects_and_then_answers() {
    TestImageSearch provider;
    SearchImagesTool tool(provider);
    ToolRegistry registry;
    assert_error(Error::none, registry.add(tool));
    ImageSelectionChat chat;
    AgentLoop loop(chat, registry);
    FixedText<Limits::max_answer_bytes> answer;
    TestCancellation cancellation;

    assert_error(
        Error::none, loop.run("Show Dubai", 10, answer, cancellation));
    TEST_ASSERT_EQUAL_STRING("Here is the image.", answer.c_str());
    TEST_ASSERT_EQUAL_INT(4, chat.calls);
    TEST_ASSERT_EQUAL_INT(1, provider.calls);
    TEST_ASSERT_EQUAL_UINT32(6, loop.history().size());

    ImageResult selected;
    TEST_ASSERT_TRUE(tool.take_selected(selected));
    TEST_ASSERT_EQUAL_STRING("img0", selected.id.c_str());
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

    assert_error(
        Error::none,
        build_openrouter_route_request(
            OpenRouterConfig{}, history, registry, true, body));
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "answer_direct"));
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "search_web"));
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "\"tool_choice\":\"required\""));
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "\"max_tokens\":96"));

    assert_error(
        Error::none,
        build_openrouter_answer_request(
            OpenRouterConfig{}, history, true, body));
    TEST_ASSERT_NULL(std::strstr(body.c_str(), "search_web"));
    TEST_ASSERT_NULL(std::strstr(body.c_str(), "tool_choice"));
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "\"max_tokens\":160"));

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

void test_openrouter_sse_publishes_cumulative_complete_lines() {
    ChatTextRecorder text;
    OpenRouterSseParser parser(&text);
    const char first[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello \"},"
        "\"finish_reason\":null}]}";
    assert_error(Error::none, parser.feed(first, sizeof(first) - 1));
    TEST_ASSERT_EQUAL_UINT32(0, text.size);
    assert_error(Error::none, parser.feed("\n\n", 2));
    TEST_ASSERT_EQUAL_UINT32(1, text.size);
    TEST_ASSERT_EQUAL_STRING("Hello ", text.updates[0].c_str());

    const char final[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"watch.\"},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    assert_error(Error::none, parser.feed(final, sizeof(final) - 1));
    assert_error(Error::none, parser.finish());
    TEST_ASSERT_EQUAL_UINT32(2, text.size);
    TEST_ASSERT_EQUAL_STRING("Hello watch.", text.updates[1].c_str());
}

void test_openrouter_sse_never_publishes_tool_calls() {
    ChatTextRecorder text;
    OpenRouterSseParser parser(&text);
    const char stream[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"draft\","
        "\"tool_calls\":[{\"id\":\"call_1\",\"function\":{"
        "\"name\":\"search_web\","
        "\"arguments\":\"{\\\"query\\\":\\\"now\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";

    assert_error(Error::none, parser.feed(stream, sizeof(stream) - 1));
    assert_error(Error::none, parser.finish());
    TEST_ASSERT_EQUAL_UINT32(0, text.size);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ChatTurnKind::tool_call),
        static_cast<int>(parser.turn().kind));
    TEST_ASSERT_TRUE(parser.turn().answer.empty());
}

void test_openrouter_sse_clears_draft_when_later_event_calls_tool() {
    ChatTextRecorder text;
    OpenRouterSseParser parser(&text);
    const char draft[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"draft\"},"
        "\"finish_reason\":null}]}\n\n";
    const char tool[] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{"
        "\"id\":\"call_1\",\"function\":{\"name\":\"search_web\","
        "\"arguments\":\"{\\\"query\\\":\\\"now\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";

    assert_error(Error::none, parser.feed(draft, sizeof(draft) - 1));
    TEST_ASSERT_EQUAL_UINT32(1, text.size);
    TEST_ASSERT_EQUAL_STRING("draft", text.updates[0].c_str());
    assert_error(Error::none, parser.feed(tool, sizeof(tool) - 1));
    assert_error(Error::none, parser.finish());
    TEST_ASSERT_EQUAL_UINT32(2, text.size);
    TEST_ASSERT_TRUE(text.updates[1].empty());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ChatTurnKind::tool_call),
        static_cast<int>(parser.turn().kind));
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
    TEST_ASSERT_NOT_NULL(
        std::strstr(body.c_str(), "\"model\":\"hexgrad/kokoro-82m\""));
    TEST_ASSERT_NOT_NULL(std::strstr(body.c_str(), "\"input\":\"Hello.\""));
    TEST_ASSERT_NOT_NULL(
        std::strstr(body.c_str(), "\"voice\":\"af_heart\""));
    TEST_ASSERT_NULL(std::strstr(body.c_str(), "Transcript:"));
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

void test_cloud_timeouts_allow_weak_wifi_without_unbounded_waits() {
    const RequestPolicy chat = chat_policy();
    const RequestPolicy transcription = transcription_policy();
    const RequestPolicy speech = speech_policy();
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(20'000, chat.connect_timeout_ms);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(
        20'000, transcription.connect_timeout_ms);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(20'000, speech.connect_timeout_ms);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(75'000, chat.total_timeout_ms);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(
        75'000, transcription.total_timeout_ms);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(90'000, speech.total_timeout_ms);
    TEST_ASSERT_EQUAL_UINT8(2, chat.max_attempts);
    TEST_ASSERT_EQUAL_UINT8(2, transcription.max_attempts);
    TEST_ASSERT_EQUAL_UINT8(2, speech.max_attempts);
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
    RUN_TEST(test_agent_loop_stops_after_three_tool_rounds);
    RUN_TEST(test_agent_loop_honors_cancellation_before_provider);
    RUN_TEST(test_agent_loop_rolls_back_cancelled_turn);
    RUN_TEST(test_agent_loop_gives_model_a_sanitized_tool_error);
    RUN_TEST(test_agent_loop_reports_bounded_progress_in_order);
    RUN_TEST(test_agent_loop_stops_when_progress_observer_sees_cancellation);
    RUN_TEST(test_agent_loop_cancels_before_a_reported_tool_starts);
    RUN_TEST(test_agent_loop_nonstream_provider_publishes_only_final_answer);
    RUN_TEST(test_agent_loop_rolls_back_when_text_callback_cancels);
    RUN_TEST(test_search_tools_validate_query_and_bound_result);
    RUN_TEST(test_device_status_tool_returns_bounded_status);
    RUN_TEST(test_device_setting_tools_validate_and_report_persistence);
    RUN_TEST(test_power_off_tool_is_strict_and_cancellable);
    RUN_TEST(test_registry_holds_search_and_device_tools);
    RUN_TEST(test_image_tool_selects_only_a_current_result_id_once);
    RUN_TEST(test_image_tool_clears_selection_on_search_and_clear);
    RUN_TEST(test_image_tool_can_use_one_fallback_after_a_search);
    RUN_TEST(test_agent_loop_searches_selects_and_then_answers);
    RUN_TEST(test_openrouter_chat_builder_has_bounded_contract);
    RUN_TEST(test_openrouter_parses_answer_and_tool_call);
    RUN_TEST(test_openrouter_sse_parser_handles_split_input_and_comments);
    RUN_TEST(test_openrouter_sse_publishes_cumulative_complete_lines);
    RUN_TEST(test_openrouter_sse_never_publishes_tool_calls);
    RUN_TEST(test_openrouter_sse_clears_draft_when_later_event_calls_tool);
    RUN_TEST(test_openrouter_transcription_and_speech_protocols);
    RUN_TEST(test_brave_builders_encode_query_and_set_safe_limits);
    RUN_TEST(test_brave_parsers_keep_only_narrow_fields);
    RUN_TEST(test_retry_policy_never_retries_after_output);
    RUN_TEST(test_cloud_timeouts_allow_weak_wifi_without_unbounded_waits);
    RUN_TEST(test_image_fetch_contract_requires_https_and_hard_caps);
    return UNITY_END();
}
