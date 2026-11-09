#include "textmate.h"
#include "protocol.h"
#include "textmate_assets.h"
#include <oniguruma.h>
#include <quickjs.h>
#include <memory>
#include <string>
#include <utility>

namespace {
struct Classes {
    JSClassID scanner = 0, string = 0;
    ULONGLONG deadline = 0;
};

struct Scanner {
    std::vector<OnigRegex> patterns;
    ~Scanner() { for (auto pattern : patterns) onig_free(pattern); }
};

Classes& classes(JSRuntime* runtime) {
    return *static_cast<Classes*>(JS_GetRuntimeOpaque(runtime));
}

Classes& classes(JSContext* context) { return classes(JS_GetRuntime(context)); }

struct CString {
    JSContext* context;
    const char* data;
    ~CString() { if (data) JS_FreeCString(context, data); }
};

void freeScanner(JSRuntime* runtime, JSValue value) {
    delete static_cast<Scanner*>(JS_GetOpaque(value, classes(runtime).scanner));
}

void freeString(JSRuntime* runtime, JSValue value) {
    delete static_cast<std::wstring*>(JS_GetOpaque(value, classes(runtime).string));
}

// CESU-8 preserves every UTF-16 code unit in QuickJS, including isolated surrogates.
JSValue makeString(JSContext* context, JSValueConst, int argc, JSValueConst* args) {
    if (argc != 1) return JS_ThrowTypeError(context, "Expected a string");
    size_t size = 0;
    CString input{context, JS_ToCStringLen2(context, &size, args[0], true)};
    if (!input.data) return JS_EXCEPTION;
    try {
        auto text = std::make_unique<std::wstring>();
        text->reserve(size);
        const auto* bytes = reinterpret_cast<const unsigned char*>(input.data);
        for (size_t i = 0; i < size;) {
            unsigned code = bytes[i++];
            if (code >= 0xe0 && i + 1 < size) {
                code = (code & 15) << 12 | (bytes[i] & 63) << 6 | (bytes[i + 1] & 63);
                i += 2;
            } else if (code >= 0xc0 && i < size) {
                code = (code & 31) << 6 | (bytes[i++] & 63);
            }
            text->push_back(static_cast<wchar_t>(code));
        }
        JSValue result = JS_NewObjectClass(context, classes(context).string);
        if (!JS_IsException(result)) {
            JS_SetOpaque(result, text.release());
            JS_SetPropertyStr(context, result, "content", JS_DupValue(context, args[0]));
        }
        return result;
    } catch (...) {
        return JS_ThrowOutOfMemory(context);
    }
}

JSValue dispose(JSContext*, JSValueConst, int, JSValueConst*) { return JS_UNDEFINED; }

JSValue findMatch(JSContext* context, JSValueConst self, int argc, JSValueConst* args) {
    auto* scanner = static_cast<Scanner*>(JS_GetOpaque(self, classes(context).scanner));
    auto* text = argc >= 2 ? static_cast<std::wstring*>(JS_GetOpaque(args[0], classes(context).string)) : nullptr;
    uint32_t start = 0;
    if (!scanner || !text || JS_ToUint32(context, &start, args[1]))
        return JS_ThrowTypeError(context, "Invalid TextMate match input");
    if (start > text->size()) return JS_NULL;
    // Never ask Oniguruma to start in the middle of a UTF-16 surrogate pair.
    if (start && start < text->size() && (*text)[start] >= 0xdc00 && (*text)[start] <= 0xdfff &&
        (*text)[start - 1] >= 0xd800 && (*text)[start - 1] <= 0xdbff) --start;
    OnigOptionType options = ONIG_OPTION_CAPTURE_GROUP;
    int64_t optionCount = 0;
    if (argc >= 3 && JS_IsArray(args[2]) && !JS_GetLength(context, args[2], &optionCount)) {
        for (int64_t i = 0; i < optionCount; ++i) {
            JSValue value = JS_GetPropertyUint32(context, args[2], static_cast<uint32_t>(i));
            uint32_t flag = 0;
            int converted = JS_ToUint32(context, &flag, value);
            JS_FreeValue(context, value);
            if (converted) return JS_EXCEPTION;
            // Anchor options from vscode-oniguruma's FindOption enum.
            if (flag == 21) options |= ONIG_OPTION_NOT_BEGIN_STRING;
            if (flag == 22) options |= ONIG_OPTION_NOT_END_STRING;
            if (flag == 23) options |= ONIG_OPTION_NOT_BEGIN_POSITION;
        }
    }
    auto freeRegion = +[](OnigRegion* region) { if (region) onig_region_free(region, 1); };
    std::unique_ptr<OnigRegion, decltype(freeRegion)> candidate(onig_region_new(), freeRegion), best(onig_region_new(), freeRegion);
    std::unique_ptr<OnigMatchParam, decltype(&onig_free_match_param)> limits(onig_new_match_param(), onig_free_match_param);
    if (!candidate || !best || !limits) return JS_ThrowOutOfMemory(context);
    onig_set_retry_limit_in_match_of_match_param(limits.get(), 100000);
    onig_set_retry_limit_in_search_of_match_param(limits.get(), 100000);
    const auto* begin = reinterpret_cast<const OnigUChar*>(text->data());
    const auto* end = begin + text->size() * sizeof(wchar_t);
    const auto* position = begin + start * sizeof(wchar_t);
    int nearest = INT_MAX;
    uint32_t selected = 0;
    for (uint32_t i = 0; i < scanner->patterns.size(); ++i) {
        int at = onig_search_with_param(scanner->patterns[i], begin, end, position, end,
                                       candidate.get(), options, limits.get());
        if (at < 0 && at != ONIG_MISMATCH) return JS_ThrowInternalError(context, "Oniguruma search limit or invalid text");
        if (at < 0 || at >= nearest) continue;
        nearest = at;
        selected = i;
        best.swap(candidate);
        if (begin + at == position) break;
    }
    if (nearest == INT_MAX) return JS_NULL;
    JSValue result = JS_NewObject(context), captures = JS_NewArray(context);
    for (int i = 0; i < best->num_regs; ++i) {
        const auto boundary = [&](int value) { return value < 0 ? static_cast<uint32_t>(text->size()) : static_cast<uint32_t>(value / 2); };
        uint32_t first = boundary(best->beg[i]), last = boundary(best->end[i]);
        JSValue capture = JS_NewObject(context);
        JS_SetPropertyStr(context, capture, "start", JS_NewUint32(context, first));
        JS_SetPropertyStr(context, capture, "end", JS_NewUint32(context, last));
        JS_SetPropertyStr(context, capture, "length", JS_NewUint32(context, last - first));
        JS_SetPropertyUint32(context, captures, i, capture);
    }
    JS_SetPropertyStr(context, result, "index", JS_NewUint32(context, selected));
    JS_SetPropertyStr(context, result, "captureIndices", captures);
    return result;
}

JSValue makeScanner(JSContext* context, JSValueConst, int argc, JSValueConst* args) {
    int64_t count = 0;
    if (argc != 1 || JS_GetLength(context, args[0], &count) || count < 0 || count > 1024)
        return JS_ThrowTypeError(context, "Invalid TextMate scanner patterns");
    try {
        auto scanner = std::make_unique<Scanner>();
        scanner->patterns.reserve(static_cast<size_t>(count));
        for (int64_t i = 0; i < count; ++i) {
            JSValue pattern = JS_GetPropertyUint32(context, args[0], static_cast<uint32_t>(i));
            JSValue encoded = makeString(context, JS_UNDEFINED, 1, &pattern);
            JS_FreeValue(context, pattern);
            if (JS_IsException(encoded)) return encoded;
            const auto& text = *static_cast<std::wstring*>(JS_GetOpaque(encoded, classes(context).string));
            const auto* begin = reinterpret_cast<const OnigUChar*>(text.data());
            OnigRegex regex = nullptr;
            OnigErrorInfo error{};
            int status = onig_new(&regex, begin, begin + text.size() * 2, ONIG_OPTION_CAPTURE_GROUP,
                                  ONIG_ENCODING_UTF16_LE, ONIG_SYNTAX_DEFAULT, &error);
            JS_FreeValue(context, encoded);
            if (status != ONIG_NORMAL) return JS_ThrowSyntaxError(context, "Oniguruma could not compile a TextMate pattern (%d)", status);
            scanner->patterns.push_back(regex);
        }
        JSValue result = JS_NewObjectClass(context, classes(context).scanner);
        if (!JS_IsException(result)) JS_SetOpaque(result, scanner.release());
        return result;
    } catch (...) {
        return JS_ThrowOutOfMemory(context);
    }
}

struct Value {
    JSContext* context;
    JSValue value;
    Value(JSContext* context, JSValue value) : context(context), value(value) {}
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
    ~Value() { JS_FreeValue(context, value); }
};

template<size_t N> JSValue eval(JSContext* context, const unsigned char (&bytes)[N], const char* name) {
    return JS_Eval(context, reinterpret_cast<const char*>(bytes), N - 1, name, JS_EVAL_TYPE_GLOBAL);
}
}

struct TextMate::State {
    Classes classes;
    JSRuntime* runtime = JS_NewRuntime();
    JSContext* context = nullptr;
    JSValue tokenize = JS_UNDEFINED;
    bool ready = false;

    State() {
        if (!runtime) return;
        JS_SetRuntimeOpaque(runtime, &classes);
        JS_SetMemoryLimit(runtime, 64 * 1024 * 1024);
        JS_SetMaxStackSize(runtime, 512 * 1024);
        JS_SetInterruptHandler(runtime, [](JSRuntime*, void* data) {
            return GetTickCount64() > static_cast<Classes*>(data)->deadline ? 1 : 0;
        }, &classes);
        classes.deadline = GetTickCount64() + 2000;
        context = JS_NewContext(runtime);
        if (!context) return;
        JSClassDef scannerClass{}, stringClass{};
        scannerClass.class_name = "NativeOnigScanner";
        scannerClass.finalizer = freeScanner;
        stringClass.class_name = "NativeOnigString";
        stringClass.finalizer = freeString;
        JS_NewClassID(runtime, &classes.scanner);
        JS_NewClassID(runtime, &classes.string);
        if (JS_NewClass(runtime, classes.scanner, &scannerClass) || JS_NewClass(runtime, classes.string, &stringClass)) return;
        JSValue scannerPrototype = JS_NewObject(context), stringPrototype = JS_NewObject(context);
        JS_SetPropertyStr(context, scannerPrototype, "findNextMatchSync", JS_NewCFunction(context, findMatch, "findNextMatchSync", 3));
        JS_SetPropertyStr(context, scannerPrototype, "dispose", JS_NewCFunction(context, dispose, "dispose", 0));
        JS_SetPropertyStr(context, stringPrototype, "dispose", JS_NewCFunction(context, dispose, "dispose", 0));
        JS_SetClassProto(context, classes.scanner, scannerPrototype);
        JS_SetClassProto(context, classes.string, stringPrototype);
        Value global{context, JS_GetGlobalObject(context)};
        JS_SetPropertyStr(context, global.value, "nativeScanner", JS_NewCFunction(context, makeScanner, "nativeScanner", 1));
        JS_SetPropertyStr(context, global.value, "nativeString", JS_NewCFunction(context, makeString, "nativeString", 1));
        for (auto engine : {true, false}) {
            Value code{context, engine ? eval(context, embedded::engine, "vscode-textmate.js") : eval(context, embedded::mapping, "textmate.js")};
            if (JS_IsException(code.value)) {
                Value error{context, JS_GetException(context)};
                return;
            }
        }
        Value initialize{context, JS_GetPropertyStr(context, global.value, "initializeTextMate")};
        Value bsl{context, JS_NewStringLen(context, reinterpret_cast<const char*>(embedded::bsl), sizeof(embedded::bsl) - 1)};
        Value sdbl{context, JS_NewStringLen(context, reinterpret_cast<const char*>(embedded::sdbl), sizeof(embedded::sdbl) - 1)};
        JSValue args[] = {bsl.value, sdbl.value};
        Value initialized{context, JS_Call(context, initialize.value, global.value, 2, args)};
        if (JS_IsException(initialized.value)) {
            Value error{context, JS_GetException(context)};
            return;
        }
        JSContext* job = nullptr;
        int status = 1;
        for (unsigned count = 0; count < 1024 && status > 0; ++count) status = JS_ExecutePendingJob(runtime, &job);
        if (status < 0) { Value error{job, JS_GetException(job)}; }
        if (status || JS_PromiseState(context, initialized.value) != JS_PROMISE_FULFILLED) return;
        tokenize = JS_GetPropertyStr(context, global.value, "tokenizeModule");
        ready = JS_IsFunction(context, tokenize);
    }

    ~State() {
        if (context) {
            JS_FreeValue(context, tokenize);
            JS_FreeContext(context);
        }
        if (runtime) JS_FreeRuntime(runtime);
    }
};

TextMate::TextMate() : state_(new State) {}
TextMate::~TextMate() { delete state_; }

bool TextMate::tokenize(std::wstring_view source, SyntaxLines& lines) {
    if (!state_->ready || source.size() > 1024 * 1024) return false;
    static_assert(sizeof(wchar_t) == 2, "TextMate offsets must be UTF-16");
    auto* context = state_->context;
    state_->classes.deadline = GetTickCount64() + 500;
    Value input{context, JS_NewTwoByteString(context, reinterpret_cast<const uint16_t*>(source.data()), source.size())};
    Value result{context, JS_Call(context, state_->tokenize, JS_UNDEFINED, 1, &input.value)};
    if (JS_IsException(result.value)) {
        Value error{context, JS_GetException(context)};
        return false;
    }
    int64_t count = 0;
    if (JS_GetLength(context, result.value, &count) || count <= 0 || count > 100000) return false;
    SyntaxLines parsed(static_cast<size_t>(count));
    for (uint32_t row = 0; row < parsed.size(); ++row) {
        Value line{context, JS_GetPropertyUint32(context, result.value, row)};
        int64_t length = 0;
        if (JS_GetLength(context, line.value, &length) || length % 3 || length < 0 || length > 65536) return false;
        auto& spans = parsed[row];
        spans.reserve(static_cast<size_t>(length / 3));
        for (uint32_t col = 0; col < length; col += 3) {
            SyntaxSpan span{};
            uint32_t* fields[] = {&span.start, &span.length, &span.role};
            for (uint32_t field = 0; field < 3; ++field) {
                Value entry{context, JS_GetPropertyUint32(context, line.value, col + field)};
                if (JS_ToUint32(context, fields[field], entry.value)) return false;
            }
            if (span.role >= SyntaxRoleCount || !span.length) return false;
            spans.push_back(span);
        }
    }
    lines.swap(parsed);
    return true;
}
