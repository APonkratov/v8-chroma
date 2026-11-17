#include "../src/textmate.h"
#include "../src/protocol.h"
#include <algorithm>
#include <cstdio>

int main() {
    TextMate tokenizer;
    SyntaxLines lines;
    const auto sample =
        L"&НаСервере\r\n"
        L"Функция Рассчитать(Сумма) Экспорт\r\n"
        L"    Порог = 42; // комментарий\r\n"
        L"    Сообщить(\"😀\");\r\n"
        L"    Возврат Окр(Сумма, 2);\r\n"
        L"    Запрос = Новый Запрос;\r\n"
        L"    Запрос.Текст = \"ВЫБРАТЬ\r\n"
        L"        | 1 КАК Значение\";\r\n"
        L"КонецФункции";
    if (!tokenizer.tokenize(sample, lines) || lines.size() != 9) {
        std::fputs("Could not tokenize the module\n", stderr);
        return 1;
    }
    auto has = [&](size_t row, SyntaxRole role) {
        return std::any_of(lines[row].begin(), lines[row].end(), [role](auto span) { return span.role == role; });
    };
    if (!has(0, Directive) || !has(1, Function) || !has(1, Parameter) ||
        !has(2, Variable) || !has(2, Number) || !has(2, Comment) || !has(3, String) ||
        !has(4, Builtin) || !has(5, Type) || !has(6, QueryKeyword) || !has(7, QueryKeyword)) {
        for (size_t row = 0; row < lines.size(); ++row) {
            std::fprintf(stderr, "%zu:", row);
            for (auto span : lines[row]) std::fprintf(stderr, " %u/%u=%u", span.start, span.length, span.role);
            std::fputc('\n', stderr);
        }
        return 2;
    }
    if (!tokenizer.tokenize(L"Текст = \"😀\"; Порог = 42;", lines) || lines.size() != 1 ||
        !std::any_of(lines[0].begin(), lines[0].end(), [](auto span) {
            return span.role == Number && span.start == 22 && span.length == 2;
        })) return 3;
    const auto calls =
        L"Значение = Объект.Добавить(Подготовить(42));\n"
        L"Если (Проверить(Значение)) Тогда\n"
        L"    Записать(Окр(Значение, 2)); // Сохранить()\n"
        L"    Текст = \"Добавить()\";\n"
        L"    Запрос = Новый Запрос(\"ВЫБРАТЬ 1\");\n"
        L"КонецЕсли;";
    if (!tokenizer.tokenize(calls, lines) || lines.size() != 6) return 4;
    auto spanAt = [&](size_t row, unsigned start, unsigned length, SyntaxRole role) {
        return std::any_of(lines[row].begin(), lines[row].end(), [&](auto span) {
            return span.start == start && span.length == length && span.role == role;
        });
    };
    if (!spanAt(0, 18, 8, Function) || !spanAt(0, 27, 11, Function) ||
        !spanAt(1, 0, 4, Keyword) || !spanAt(1, 6, 9, Function) ||
        !spanAt(2, 4, 8, Function) || !spanAt(2, 13, 3, Builtin) ||
        std::count_if(lines[2].begin(), lines[2].end(), [](auto span) {
            return span.role == Function;
        }) != 1 ||
        has(3, Function) || !has(3, String) || has(4, Function) || !has(4, Type) ||
        !has(4, QueryKeyword)) {
        for (size_t row = 0; row < lines.size(); ++row) {
            std::fprintf(stderr, "%zu:", row);
            for (auto span : lines[row]) std::fprintf(stderr, " %u/%u=%u", span.start, span.length, span.role);
            std::fputc('\n', stderr);
        }
        return 5;
    }
    std::puts("TextMate BSL/SDBL roles and UTF-16 offsets verified");
}
