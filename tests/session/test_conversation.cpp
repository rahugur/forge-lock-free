// Tests for the conversation history ring buffer.
#include <catch2/catch_test_macros.hpp>

#include "session/conversation.h"
#include "llm/message.h"

TEST_CASE("Conversation: push and access", "[conversation]") {
    forge::Conversation conv(10);

    REQUIRE(conv.empty());
    REQUIRE(conv.size() == 0);

    forge::Message m1;
    m1.role = "user";
    m1.content = "hello";
    conv.push(m1);

    REQUIRE(conv.size() == 1);
    REQUIRE(conv.back().content == "hello");
}

TEST_CASE("Conversation: eviction preserves system message", "[conversation]") {
    forge::Conversation conv(4);  // capacity of 4

    // Add system message first.
    forge::Message sys;
    sys.role = "system";
    sys.content = "You are helpful.";
    conv.push(sys);

    // Add 5 more messages (will exceed capacity of 4).
    for (int i = 0; i < 5; ++i) {
        forge::Message m;
        m.role = "user";
        m.content = "msg " + std::to_string(i);
        conv.push(m);
    }

    // Should have 4 messages: system + last 3 user messages.
    REQUIRE(conv.size() == 4);
    REQUIRE(conv.messages().front().role == "system");
    REQUIRE(conv.messages().front().content == "You are helpful.");
    REQUIRE(conv.messages().back().content == "msg 4");
}

TEST_CASE("Conversation: eviction without system message", "[conversation]") {
    forge::Conversation conv(3);

    for (int i = 0; i < 5; ++i) {
        forge::Message m;
        m.role = "user";
        m.content = "msg " + std::to_string(i);
        conv.push(m);
    }

    REQUIRE(conv.size() == 3);
    // Oldest messages should be evicted.
    REQUIRE(conv.messages().front().content == "msg 2");
    REQUIRE(conv.messages().back().content == "msg 4");
}

TEST_CASE("Conversation: to_json serializes correctly", "[conversation]") {
    forge::Conversation conv;

    forge::Message m1;
    m1.role = "user";
    m1.content = "What is 2+2?";
    conv.push(m1);

    forge::Message m2;
    m2.role = "assistant";
    m2.content = "4";
    conv.push(m2);

    auto j = conv.to_json();
    REQUIRE(j.is_array());
    REQUIRE(j.size() == 2);
    REQUIRE(j[0]["role"] == "user");
    REQUIRE(j[0]["content"] == "What is 2+2?");
    REQUIRE(j[1]["role"] == "assistant");
    REQUIRE(j[1]["content"] == "4");
}

TEST_CASE("Conversation: clear resets", "[conversation]") {
    forge::Conversation conv;
    forge::Message m;
    m.role = "user";
    m.content = "test";
    conv.push(m);

    REQUIRE(conv.size() == 1);
    conv.clear();
    REQUIRE(conv.empty());
}
