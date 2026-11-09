#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

struct SyntaxSpan {
    uint32_t start, length, role;
};

using SyntaxLines = std::vector<std::vector<SyntaxSpan>>;

// A tokenizer is owned by a single worker thread.
class TextMate {
public:
    TextMate();
    ~TextMate();
    TextMate(const TextMate&) = delete;
    TextMate& operator=(const TextMate&) = delete;

    bool tokenize(std::wstring_view source, SyntaxLines& lines);

private:
    struct State;
    State* state_;
};
