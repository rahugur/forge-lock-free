// Tests for SummarizingConversation.
#include <catch2/catch_test_macros.hpp>

#include "session/summarizing_conversation.h"

#include "../common/scripted_mock_llm.h"

TEST_CASE("SummarizingConversation: under threshold no summarization", "[summarize]") {
    forge::testing::ScriptedMockLLM llm;

    forge::SummarizingConversation conv(llm, /*max_messages=*/100,
                                         /*summarize_threshold=*/10,
                                         /*keep_recent=*/3);

    for (int i = 0; i < 5; ++i) {
        forge::Message m;
        m.role = "user";
        m.content = "Message " + std::to_string(i);
        conv.push(std::move(m));
    }

    REQUIRE(conv.size() == 5);
    REQUIRE(llm.call_count() == 0);
}

TEST_CASE("SummarizingConversation: triggers summarization at threshold", "[summarize]") {
    forge::testing::ScriptedMockLLM llm;
    llm.add_text_response("This is a summary of the conversation.");

    forge::SummarizingConversation conv(llm, /*max_messages=*/100,
                                         /*summarize_threshold=*/8,
                                         /*keep_recent=*/3);

    // Add system message.
    forge::Message sys;
    sys.role = "system";
    sys.content = "You are helpful.";
    conv.push(std::move(sys));

    // Add enough messages to trigger summarization.
    for (int i = 0; i < 8; ++i) {
        forge::Message m;
        m.role = (i % 2 == 0) ? "user" : "assistant";
        m.content = "Message " + std::to_string(i);
        conv.push(std::move(m));
    }

    // Summarization should have fired.
    REQUIRE(llm.call_count() == 1);
    // Should have: system + summary + 3 recent = 5 messages.
    REQUIRE(conv.size() < 9);

    // System message should be preserved.
    REQUIRE(conv.messages().front().role == "system");
}

TEST_CASE("SummarizingConversation: fallback on LLM error", "[summarize]") {
    forge::testing::ScriptedMockLLM llm;
    // All calls return errors — no summarization will succeed.
    llm.set_handler([](const forge::Json&, const forge::Json&) -> forge::LLMResponse {
        forge::LLMResponse resp;
        resp.error = "API failure";
        return resp;
    });

    forge::SummarizingConversation conv(llm, /*max_messages=*/100,
                                         /*summarize_threshold=*/5,
                                         /*keep_recent=*/2);

    for (int i = 0; i < 6; ++i) {
        forge::Message m;
        m.role = "user";
        m.content = "Message " + std::to_string(i);
        conv.push(std::move(m));
    }

    // LLM was called but failed; messages should remain unsummarized.
    REQUIRE(llm.call_count() >= 1);
    REQUIRE(conv.size() == 6);
}
