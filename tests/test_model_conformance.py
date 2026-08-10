from __future__ import annotations

import io
import json
import unittest
import urllib.error
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

from tools.model_conformance import (
    FIXED_UTC_MINUTE,
    IMAGE_PROMPT,
    IMAGE_RESULT_IDS,
    PLOT_PROMPT,
    ConformanceError,
    ToolCall,
    build_route_request,
    call_openrouter,
    load_route_contract,
    main,
    parse_sse,
    run_live,
    valid_image_query,
    valid_image_selection,
    valid_plot,
    plot_validation_error,
)


ROOT = Path(__file__).resolve().parents[1]
VALID_PLOT = {
    "x": [-1, -0.5, 0, 0.5, 1],
    "y": [-1, -2, None, 2, 1],
    "title": "1/x",
}


def sse_event(value: dict[str, object] | str) -> bytes:
    data = value if isinstance(value, str) else json.dumps(value)
    return f"data: {data}\n\n".encode()


def split_tool_sse(name: str, arguments: str, call_id: str = "call-1") -> list[bytes]:
    midpoint = len(arguments) // 2
    return [
        sse_event(
            {
                "choices": [
                    {
                        "delta": {
                            "tool_calls": [
                                {
                                    "index": 0,
                                    "id": call_id,
                                    "function": {
                                        "name": name[: len(name) // 2],
                                        "arguments": arguments[:midpoint],
                                    },
                                }
                            ]
                        },
                        "finish_reason": None,
                    }
                ]
            }
        ),
        sse_event(
            {
                "choices": [
                    {
                        "delta": {
                            "tool_calls": [
                                {
                                    "index": 0,
                                    "function": {
                                        "name": name[len(name) // 2 :],
                                        "arguments": arguments[midpoint:],
                                    },
                                }
                            ]
                        },
                        "finish_reason": None,
                    }
                ]
            }
        ),
        sse_event(
            {
                "choices": [
                    {"delta": {}, "finish_reason": "tool_calls"}
                ]
            }
        ),
        sse_event("[DONE]"),
    ]


class FakeResponse:
    def __init__(
        self,
        lines: list[bytes],
        content_type: str = "text/event-stream; charset=utf-8",
    ) -> None:
        self.lines = lines
        self.headers = {"Content-Type": content_type}

    def __enter__(self) -> FakeResponse:
        return self

    def __exit__(self, *_args: object) -> None:
        return None

    def __iter__(self):
        return iter(self.lines)


class ModelConformanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = load_route_contract(ROOT)

    def test_contract_comes_from_current_firmware_sources(self) -> None:
        self.assertEqual("~deepseek/deepseek-v4-flash-latest", self.contract.model)
        self.assertIn("Route one ChatESP voice request.", self.contract.prompt)
        self.assertEqual(
            [
                "search_web",
                "search_images",
                "run_python",
                "plot_line",
                "get_device_status",
                "set_brightness",
                "set_volume",
                "power_off",
                "restart_device",
                "remember_memory",
                "forget_memory",
                "clear_memories",
                "compact_memories",
            ],
            [tool.name for tool in self.contract.tools],
        )
        plot_tool = next(
            tool for tool in self.contract.tools if tool.name == "plot_line"
        )
        self.assertEqual(["plot"], plot_tool.parameters["required"])
        self.assertEqual(
            24,
            plot_tool.parameters["properties"]["plot"]["properties"]["x"][
                "maxItems"
            ],
        )
        self.assertEqual(192, self.contract.route_output_tokens)
        self.assertEqual(128, self.contract.tools[-1].parameters["properties"]["memories"]["items"]["properties"]["fact"]["maxLength"])

    def test_request_uses_streaming_firmware_route_shape_and_model_override(self) -> None:
        payload = build_route_request(
            self.contract,
            [{"role": "user", "content": PLOT_PROMPT}],
            "provider/model-variant",
        )

        self.assertTrue(payload["stream"])
        self.assertEqual("required", payload["tool_choice"])
        self.assertFalse(payload["parallel_tool_calls"])
        self.assertEqual(192, payload["max_tokens"])
        self.assertEqual("provider/model-variant", payload["model"])
        self.assertIn(FIXED_UTC_MINUTE, payload["messages"][0]["content"])
        self.assertEqual(PLOT_PROMPT, payload["messages"][1]["content"])

    def test_sse_parser_reassembles_split_tool_deltas(self) -> None:
        arguments = json.dumps({"plot": VALID_PLOT})

        call = parse_sse(split_tool_sse("plot_line", arguments))

        self.assertEqual("call-1", call.call_id)
        self.assertEqual("plot_line", call.name)
        self.assertEqual(arguments, call.arguments)

    def test_sse_parser_rejects_an_incomplete_private_response(self) -> None:
        private_text = "private answer content"
        with self.assertRaisesRegex(ConformanceError, "incomplete_sse") as raised:
            parse_sse(
                [
                    sse_event(
                        {
                            "choices": [
                                {
                                    "delta": {"content": private_text},
                                    "finish_reason": "tool_calls",
                                }
                            ]
                        }
                    ),
                    sse_event("[DONE]"),
                ]
            )
        self.assertNotIn(private_text, str(raised.exception))

    def test_mocked_http_call_streams_and_keeps_key_out_of_errors(self) -> None:
        private_key = "router-token-private-value"
        arguments = json.dumps({"query": "red apple"})
        captured: dict[str, object] = {}

        def opener(request, timeout):
            captured["request"] = request
            captured["timeout"] = timeout
            return FakeResponse(split_tool_sse("search_images", arguments))

        call = call_openrouter(
            "https://service.example/v1",
            private_key,
            {"stream": True},
            12.0,
            opener,
        )

        self.assertEqual("search_images", call.name)
        self.assertEqual(12.0, captured["timeout"])
        self.assertEqual(
            f"Bearer {private_key}", captured["request"].get_header("Authorization")
        )

        def failed_opener(_request, timeout):
            del timeout
            raise urllib.error.URLError(private_key)

        with self.assertRaises(ConformanceError) as raised:
            call_openrouter(
                "https://service.example/v1",
                private_key,
                {"stream": True},
                12.0,
                failed_opener,
            )
        self.assertEqual("network_error", str(raised.exception))
        self.assertNotIn(private_key, str(raised.exception))

        with self.assertRaisesRegex(ConformanceError, "unsupported_media"):
            call_openrouter(
                "https://service.example/v1",
                private_key,
                {"stream": True},
                12.0,
                lambda _request, timeout: FakeResponse(
                    split_tool_sse("search_images", arguments),
                    "application/json",
                ),
            )

    def test_plot_argument_invariants(self) -> None:
        self.assertTrue(valid_plot(json.dumps({"plot": VALID_PLOT})))
        self.assertTrue(
            valid_plot(json.dumps({"plot": {"x": [-1, 0, 1], "y": [-1, None, 1]}}))
        )
        self.assertTrue(
            valid_plot(
                json.dumps(
                    {
                        "plot": {
                            "x": [-1, 0, 0.6, 1],
                            "y": [-1, None, 1.667, 1],
                        }
                    }
                )
            )
        )
        bad_cases = (
            {"plot": VALID_PLOT, "extra": True},
            {"code": "import plot\nplot.line([-1,0,1],[-1,None,1])"},
            {"plot": {"x": [-1, 0, 1], "y": [-1, 0, 1]}},
            {"plot": {"x": [-1, -0.5, 0.5, 1], "y": [-1, -2, 2, 1]}},
            {"plot": {"x": [-1, 0, 1], "y": [-1, None, 2]}},
            {"plot": {"x": [-1, 1, 0], "y": [-1, 1, None]}},
            {"plot": {"x": [-1, 0, 1], "y": [-1, None]}},
            {"plot": {"x": [-1, 0, 1], "y": [-1, None, 1], "extra": 1}},
            {"plot": {"x": [-1, 0, 1], "y": [-1, None, 1], "title": None}},
            {"plot": {"x": [-1, 0, 1], "y": [-1, None, 1], "title": "a\0b"}},
            {"plot": {"x": [-1, 0, 1], "y": [-1, None, 1], "title": "a\nb"}},
            {"plot": {"x": [-1, 0, 1], "y": [-1, None, 1], "title": "café"}},
            {"plot": {"x": [-1, 0, 1], "y": [-1, None, 1], "title": "x" * 49}},
            {
                "plot": {
                    "x": [-1, -0.5, 0.5, 0.75, 1],
                    "y": [-1, -2, 2, None, 1],
                }
            },
            {"plot": {"x": [-1, float("nan"), 1], "y": [-1, None, 1]}},
            {
                "plot": {
                    "x": [-1, *([0] * 23), 1],
                    "y": [-1, *([None] * 23), 1],
                }
            },
        )
        for value in bad_cases:
            with self.subTest(value=value):
                self.assertFalse(valid_plot(json.dumps(value)))
        self.assertEqual(
            "plot_gap",
            plot_validation_error(
                json.dumps(
                    {
                        "plot": {
                            "x": [-1, -0.5, 0.5, 0.75, 1],
                            "y": [-1, -2, 2, None, 1],
                        }
                    }
                )
            ),
        )

    def test_image_argument_invariants(self) -> None:
        self.assertTrue(valid_image_query('{"query":"red apple"}'))
        self.assertTrue(valid_image_query('{"query":"photo de pomme rouge"}'))
        self.assertFalse(valid_image_query('{"select":"image-1"}'))
        self.assertFalse(valid_image_query('{"query":"blue bicycle"}'))
        self.assertFalse(valid_image_query('{"query":"x","extra":1}'))
        self.assertTrue(valid_image_selection('{"select":"image-2"}', IMAGE_RESULT_IDS))
        self.assertFalse(valid_image_selection('{"select":"https://x"}', IMAGE_RESULT_IDS))
        self.assertFalse(valid_image_selection('{"query":"red apple"}', IMAGE_RESULT_IDS))

    def test_one_mocked_trial_runs_query_then_current_id_selection(self) -> None:
        payloads: list[dict[str, object]] = []
        calls = iter(
            (
                ToolCall("plot-1", "plot_line", json.dumps({"plot": VALID_PLOT})),
                ToolCall("image-1", "search_images", '{"query":"red apple"}'),
                ToolCall("select-1", "search_images", '{"select":"image-2"}'),
            )
        )

        def caller(_endpoint, _key, payload, _timeout):
            payloads.append(payload)
            return next(calls)

        results = run_live(
            self.contract,
            "https://service.example/v1",
            "router-token-placeholder",
            self.contract.model,
            10.0,
            1,
            caller,
        )

        self.assertEqual(3, len(results))
        self.assertTrue(all(result.passed for result in results))
        self.assertEqual([1, 1, 1], [result.trial for result in results])
        self.assertEqual(PLOT_PROMPT, payloads[0]["messages"][1]["content"])
        self.assertEqual(IMAGE_PROMPT, payloads[1]["messages"][1]["content"])
        selection_messages = payloads[2]["messages"]
        self.assertEqual("image-1", selection_messages[2]["tool_calls"][0]["id"])
        self.assertEqual("search_images", selection_messages[3]["name"])

    def test_summary_replaces_an_untrusted_tool_name(self) -> None:
        private_name = "private model output"
        calls = iter(
            (
                ToolCall("plot-1", private_name, "{}"),
                ToolCall("image-1", "search_images", '{"query":"red apple"}'),
                ToolCall("select-1", "search_images", '{"select":"image-1"}'),
            )
        )

        results = run_live(
            self.contract,
            "https://service.example/v1",
            "router-token-placeholder",
            self.contract.model,
            10.0,
            1,
            lambda *_arguments: next(calls),
        )

        summary = json.dumps([result.summary() for result in results])
        self.assertEqual("unknown", results[0].observed_tool)
        self.assertNotIn(private_name, summary)

    def test_cli_summary_does_not_print_private_text(self) -> None:
        private_key = "router-token-private-value"
        private_answer = "private answer content"
        results = [
            type(
                "Result",
                (),
                {
                    "passed": False,
                    "summary": lambda self: {
                        "trial": 1,
                        "case": "plot",
                        "expected_tool": "run_python",
                        "observed_tool": None,
                        "arguments_valid": False,
                        "passed": False,
                        "error": "model_failed",
                    },
                },
            )()
        ]
        output = io.StringIO()
        with (
            patch(
                "tools.model_conformance._read_settings",
                return_value={
                    "CHAT_ENDPOINT": "https://service.example/v1",
                    "OPENROUTER_API_KEY": private_key,
                },
            ),
            patch("tools.model_conformance.run_live", return_value=results),
            redirect_stdout(output),
        ):
            exit_code = main(["--trials", "1"])

        self.assertEqual(1, exit_code)
        self.assertNotIn(private_key, output.getvalue())
        self.assertNotIn(private_answer, output.getvalue())
        self.assertNotIn(PLOT_PROMPT, output.getvalue())
        self.assertNotIn(IMAGE_PROMPT, output.getvalue())

    def test_trials_are_bounded(self) -> None:
        output = io.StringIO()
        with redirect_stdout(output):
            exit_code = main(["--trials", "21"])
        self.assertEqual(1, exit_code)
        self.assertEqual(
            {"passed": False, "error": "invalid_option"},
            json.loads(output.getvalue()),
        )


if __name__ == "__main__":
    unittest.main()
