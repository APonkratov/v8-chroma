#include "editor_syntax.h"
#include "editor_syntax_discovery.h"
#include "editor_selection.h"
#include "textmate.h"
#include <MinHook.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace {
constexpr size_t maxSource = 1024 * 1024;
constexpr size_t maxDocuments = 24;
constexpr size_t maxRuns = 72; // The native vector uses an aligned allocator above this size.
constexpr ULONGLONG debounce = 180;

struct NativeString {
    alignas(void*) unsigned char storage[24]{};
    NativeString() { storage[22] = 11; }
    std::wstring_view value() const {
        uint16_t flags;
        std::memcpy(&flags, storage + 22, sizeof(flags));
        if (!(flags & 0xff80)) {
            return flags <= 11 ? std::wstring_view(reinterpret_cast<const wchar_t*>(storage), 11 - flags) : std::wstring_view{};
        }
        size_t length;
        const wchar_t* data;
        std::memcpy(&length, storage + 8, sizeof(length));
        std::memcpy(&data, storage, sizeof(data));
        return data && length <= maxSource ? std::wstring_view(data + 2, length) : std::wstring_view{};
    }
};

struct Run {
    uint32_t start, length, kind;
    COLORREF color;
    unsigned char attributes[40];
};
static_assert(sizeof(Run) == 56);

struct Runs {
    uint64_t referenceCount;
    Run* begin;
    Run* end;
    Run* capacity;
};
static_assert(sizeof(Runs) == 32);

using VoidCall = void (*)(void*);
using RefCall = int (*)(void*);
using Paint = LRESULT (*)(void*);
using Event = void (*)(void*, int, const void*, uint64_t);
using SetRef = void (*)(Runs**, Runs*);
struct Native {
    const void* viewTable;
    const void* ownerTable;
    const void* sourceTable;
    const void* lexerTable;
    unsigned viewContextOffset, contextOwnerOffset;
    Paint paint;
    VoidCall close, clearString, lock, unlock;
    Event change;
    bool (*save)(void*, NativeString&);
    bool (*getLine)(void*, int, NativeString*);
    void (*getRuns)(void*, int, NativeString*, Runs**);
    Runs** (*slot)(void*, size_t);
    SetRef setRef;
    void* (*allocate)(size_t);
    void (*release)(void*, size_t);
    void (*invalidate)(void*, uint64_t, uint64_t);
} api{};

struct Ref {
    Runs* ptr = nullptr;
    Ref() = default;
    Ref(Ref&& other) noexcept : ptr(std::exchange(other.ptr, nullptr)) {}
    ~Ref() { if (ptr) api.setRef(&ptr, nullptr); }
};

struct CacheLock {
    void* mutex;
    explicit CacheLock(void* source) : mutex(static_cast<char*>(source) + 0x20) { api.lock(mutex); }
    ~CacheLock() { api.unlock(mutex); }
};

struct Replacement { Ref original, overlay; };
struct View {
    void* manager;
    void* source;
    void* context;
    void* key() const { return source ? source : manager; }
};

using FormatLine = void (*)(void*, void*);
using SetForeground = void (*)(void*, uint64_t, uint64_t, DWORD);
using TextComparison = void (*)(void*, const void*, const void*, const void*, const void*);
struct Comparison {
    FormatLine formatLine;
    SetForeground setForeground;
    VoidCall structured;
    TextComparison text;
    const void *layoutTable, *differenceTable;
    unsigned differenceOffset, contextViewOffset;
} comparison{};
std::atomic<const BYTE*> frontendLexerTable{nullptr};
thread_local bool openingModuleComparison;

using Palette = std::array<COLORREF, SyntaxRoleCount>;
struct ThemeState {
    uint64_t revision = 0;
    bool enabled = false;
    Palette colors{};
};

struct Job { uint64_t revision; std::wstring text; };
struct Result { uint64_t revision; SyntaxLines lines; };

struct Document {
    View binding;
    uint64_t id;
    uint64_t revision = 0, themeRevision = 0;
    ULONGLONG due = 0;
    bool seen = false;
    std::wstring snapshot;
    SyntaxLines lines;
    std::vector<size_t> lineStarts;
    Palette comparisonColors{};
    std::map<Runs*, Replacement> patches;
    size_t paintedRows = 0;
};

struct State {
    std::mutex mutex;
    std::condition_variable jobsReady;
    bool workerStopping = false;
    ThemeState theme;
    std::map<uint64_t, Job> jobs;
    std::map<uint64_t, Result> results;
    std::map<uint64_t, uint64_t> expected;
    // The editor hooks and timer own the native view state on one GUI thread.
    volatile LONG guiThread = 0;
    UINT_PTR timer = 0;
    uint64_t nextId = 0;
    bool updating = false;
    std::map<void*, View> views;
    std::map<void*, Document> documents;
    // The same native difference control also renders arbitrary text files.
    // Keep the module origin for the lifetime of its view across mode switches.
    std::map<void*, void*> comparisonViews;
};

// ThemeProbe is pinned. Keep this state alive through process teardown so a
// platform destructor cannot call a hook after a C++ global has been destroyed.
State* state = nullptr;
bool attempted = false;

using CanvasFill = void (*)(void*, const RECT*, DWORD);
using TextMarginFill = void (*)(void*, const void*, void*, const RECT*);
CanvasFill realCanvasFill;
TextMarginFill realTextMarginFill;
const BYTE* canvasFillReturn;
thread_local bool paintingBackground;

class BackgroundFillScope {
public:
    explicit BackgroundFillScope(bool applies) : previous(std::exchange(paintingBackground, applies)) {}
    ~BackgroundFillScope() { paintingBackground = previous; }
private:
    bool previous;
};

template<typename T> T at(const void* object, size_t offset) {
    T value{};
    if (object) std::memcpy(&value, static_cast<const char*>(object) + offset, sizeof(value));
    return value;
}

void hookedCanvasFill(void* device, const RECT* area, DWORD color) {
#ifdef _MSC_VER
    const auto caller = _ReturnAddress();
#else
    const auto caller = __builtin_return_address(0);
#endif
    BackgroundFillScope fill(caller == canvasFillReturn);
    realCanvasFill(device, area, color);
}

void hookedTextMarginFill(void* owner, const void* column, void* device, const RECT* area) {
    // Column 2 uses the editor background. Column 1 paints the marker lane.
    constexpr size_t columnKind = 0, patternedFill = 0x3c;
    BackgroundFillScope fill(at<DWORD>(column, columnKind) == 2 && !at<BYTE>(column, patternedFill));
    realTextMarginFill(owner, column, device, area);
}

bool createBackgroundHooks(HMODULE windowBase, const ResolvedSyntax& found) {
    if (!windowBase || !found.textMarginFill) return false;
    auto* canvas = GetProcAddress(windowBase, "?fillSolidRect@wbase@@YAXPEAU_VDC@grphcs@@PEBUtagRECT@@K@Z");
    if (!canvas || *found.canvasFillImport != reinterpret_cast<const void*>(canvas)) return false;
    canvasFillReturn = found.canvasFillReturn;
    if (MH_CreateHook(reinterpret_cast<void*>(canvas), reinterpret_cast<void*>(hookedCanvasFill),
                      reinterpret_cast<void**>(&realCanvasFill)) != MH_OK) return false;
    if (MH_CreateHook(found.textMarginFill, reinterpret_cast<void*>(hookedTextMarginFill),
                      reinterpret_cast<void**>(&realTextMarginFill)) == MH_OK) return true;
    MH_RemoveHook(reinterpret_cast<void*>(canvas));
    return false;
}

void* pointer(const void* object, size_t offset = 0) { return at<void*>(object, offset); }
template<typename T> T method(void* object, size_t slot) { return at<T>(pointer(object), slot * sizeof(void*)); }

void* checkedPointer(const void* object, size_t offset = 0) {
    void* value = nullptr;
    if (!object || !ReadProcessMemory(GetCurrentProcess(), static_cast<const char*>(object) + offset,
                                      &value, sizeof(value), nullptr)) return nullptr;
    return value;
}

size_t cacheSize(void* deque) { return at<size_t>(deque, 0x20); }

void restore(Document& doc) {
    if (doc.patches.empty()) return;
    {
        CacheLock lock(doc.binding.source);
        auto* deque = static_cast<char*>(doc.binding.source) + 0x30;
        const size_t count = cacheSize(deque);
        for (size_t row = 0; row < count; ++row) {
            auto** slot = api.slot(deque, row);
            auto previous = doc.patches.find(*slot);
            if (previous != doc.patches.end()) api.setRef(slot, previous->second.original.ptr);
        }
    }
    doc.patches.clear();
}

bool boundary(std::wstring_view text, size_t position) {
    if (!position || position >= text.size()) return true;
    return !(text[position - 1] >= 0xd800 && text[position - 1] <= 0xdbff &&
             text[position] >= 0xdc00 && text[position] <= 0xdfff);
}

std::wstring_view trimLine(std::wstring_view line) {
    while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) line.remove_suffix(1);
    return line;
}

bool split(const Runs& original, const std::vector<SyntaxSpan>& spans, std::wstring_view line,
           const Palette& colors, std::vector<Run>& output) {
    const auto begin = reinterpret_cast<uintptr_t>(original.begin);
    const auto end = reinterpret_cast<uintptr_t>(original.end);
    if (!begin || begin >= end || end - begin > maxRuns * sizeof(Run) || (end - begin) % sizeof(Run)) return false;
    bool changed = false;
    for (const auto* entry = original.begin; entry != original.end; ++entry) {
        const auto stop = uint64_t(entry->start) + entry->length;
        if (stop > line.size() + 2) return false;
        if (entry->kind || !entry->length) {
            output.push_back(*entry);
        } else {
            uint32_t position = entry->start;
            const size_t first = output.size();
            const auto append = [&](uint32_t next, COLORREF ink) {
                if (next <= position) return;
                if (output.size() > first && output.back().color == ink) {
                    output.back().length += next - position;
                } else {
                    Run run = *entry;
                    run.start = position;
                    run.length = next - position;
                    run.color = ink;
                    output.push_back(run);
                }
                changed |= ink != entry->color;
                position = next;
            };
            auto span = std::lower_bound(spans.begin(), spans.end(), position,
                [](const SyntaxSpan& token, uint32_t offset) { return token.start + token.length <= offset; });
            for (; span != spans.end() && span->start < stop; ++span) {
                const uint32_t finish = span->start + span->length;
                if (finish > line.size() || !boundary(line, span->start) || !boundary(line, finish)) continue;
                append(span->start, entry->color);
                append(static_cast<uint32_t>(std::min<uint64_t>(stop, finish)), colors[span->role]);
                if (position >= stop) break;
            }
            append(static_cast<uint32_t>(stop), entry->color);
        }
        if (output.size() > maxRuns) return false;
    }
    return changed;
}

void patchRow(Document& doc, void* manager, int row, const std::vector<SyntaxSpan>& spans, const Palette& colors) {
    NativeString input;
    struct Clear { NativeString& value; ~Clear() { api.clearString(&value); } } clear{input};
    if (!api.getLine(manager, row, &input)) return;
    const auto line = trimLine(input.value());
    if (line.empty()) return;
    Ref original;
    api.getRuns(doc.binding.source, row, &input, &original.ptr);
    if (!original.ptr) return;
    CacheLock lock(doc.binding.source);
    auto* deque = static_cast<char*>(doc.binding.source) + 0x30;
    if (static_cast<size_t>(row) >= cacheSize(deque)) return;
    auto** slot = api.slot(deque, row);
    if (*slot != original.ptr) return;
    std::vector<Run> runs;
    if (!split(*original.ptr, spans, line, colors, runs)) return;
    const auto bytes = runs.size() * sizeof(Run);
    auto* data = static_cast<Run*>(api.allocate(bytes));
    if (!data) return;
    auto* overlay = static_cast<Runs*>(api.allocate(sizeof(Runs)));
    if (!overlay) { api.release(data, bytes); return; }
    std::memcpy(data, runs.data(), bytes);
    *overlay = {0, data, data + runs.size(), data + runs.size()};
    Ref owned;
    api.setRef(&owned.ptr, overlay);
    doc.patches.emplace(overlay, Replacement{std::move(original), std::move(owned)});
    api.setRef(slot, overlay);
}

void redraw(Document& doc, size_t newRows) {
    const size_t count = std::max(doc.paintedRows, newRows);
    doc.paintedRows = newRows;
    if (count) for (const auto& [view, binding] : state->views) {
        if (binding.key() == doc.binding.key() && checkedPointer(view) == api.viewTable) api.invalidate(view, 0, count);
    }
}

void apply(Document& doc, void* manager, const ThemeState& theme) {
    if (!doc.binding.source) {
        doc.comparisonColors = theme.colors;
        redraw(doc, theme.enabled ? doc.lines.size() : 0);
    } else {
        restore(doc);
        for (size_t i = 0; i < doc.lines.size(); ++i) {
            if (!doc.lines[i].empty()) patchRow(doc, manager, static_cast<int>(i + 1), doc.lines[i], theme.colors);
        }
        redraw(doc, doc.patches.empty() ? 0 : doc.lines.size());
    }
    doc.themeRevision = theme.revision;
}

View nativeBinding(void* view) {
    if (checkedPointer(view) != api.viewTable) return {};
    auto* context = checkedPointer(view, api.viewContextOffset);
    auto* owner = checkedPointer(context, api.contextOwnerOffset);
    if (checkedPointer(owner) != api.ownerTable) return {};
    // Two editor facets independently point at the current text manager.
    auto* manager = checkedPointer(checkedPointer(owner, 0x3d8), 0xc8);
    if (!manager || manager != checkedPointer(checkedPointer(owner, 0x3d0), 0xe8)) return {};
    return {manager, checkedPointer(manager, 0x228), context};
}

View bindingFor(void* view) {
    const auto binding = nativeBinding(view);
    if (binding.source) {
        if (checkedPointer(binding.source) != api.sourceTable) return {};
        const auto lexer = checkedPointer(checkedPointer(binding.source, 0x58));
        return lexer && (lexer == api.lexerTable || lexer == frontendLexerTable.load()) ? binding : View{};
    }
    const auto compared = state->comparisonViews.find(view);
    return compared != state->comparisonViews.end() && compared->second == binding.context ? binding : View{};
}

void forget(void* view) {
    auto found = state->views.find(view);
    if (found == state->views.end()) return;
    const auto binding = found->second;
    state->views.erase(found);
    for (const auto& entry : state->views) if (entry.second.key() == binding.key()) return;
    auto doc = state->documents.find(binding.key());
    restore(doc->second);
    {
        std::lock_guard lock(state->mutex);
        state->jobs.erase(doc->second.id);
        state->results.erase(doc->second.id);
        state->expected.erase(doc->second.id);
    }
    state->documents.erase(doc);
    if (binding.source) method<RefCall>(binding.source, 2)(binding.source);
    if (state->documents.empty() && state->timer) {
        KillTimer(nullptr, state->timer);
        state->timer = 0;
    }
}

std::wstring snapshot(void* manager) {
    NativeString native;
    struct Clear { NativeString& value; ~Clear() { api.clearString(&value); } } clear{native};
    return api.save(manager, native) ? std::wstring(native.value()) : std::wstring{};
}

DWORD WINAPI tokenizeWorker(void*) {
    std::unique_ptr<TextMate> tokenizer;
    for (;;) {
        uint64_t id;
        Job job;
        {
            std::unique_lock lock(state->mutex);
            state->jobsReady.wait(lock, [] { return state->workerStopping || !state->jobs.empty(); });
            if (state->workerStopping) return 0;
            auto queued = state->jobs.begin();
            id = queued->first;
            job = std::move(queued->second);
            state->jobs.erase(queued);
        }
        Result result{job.revision, {}};
        try {
            if (!tokenizer) tokenizer = std::make_unique<TextMate>();
            // An interrupted call can leave lazily compiled rules incomplete.
            if (!tokenizer->tokenize(job.text, result.lines)) tokenizer.reset();
        } catch (...) {
            tokenizer.reset();
        }
        try {
            std::lock_guard lock(state->mutex);
            auto expected = state->expected.find(id);
            if (expected != state->expected.end() && expected->second == result.revision)
                state->results.insert_or_assign(id, std::move(result));
        } catch (...) {}
    }
}

void queue(Document& doc) {
    std::lock_guard lock(state->mutex);
    state->expected.insert_or_assign(doc.id, doc.revision);
    state->results.erase(doc.id);
    state->jobs.insert_or_assign(doc.id, Job{doc.revision, doc.snapshot});
    state->jobsReady.notify_one();
}

void indexLines(Document& doc) {
    doc.lineStarts.clear();
    doc.lineStarts.push_back(0);
    for (size_t i = 0; i < doc.snapshot.size(); ++i) {
        const auto ch = doc.snapshot[i];
        if (ch != L'\r' && ch != L'\n') continue;
        if (ch == L'\r' && i + 1 < doc.snapshot.size() && doc.snapshot[i + 1] == L'\n') ++i;
        doc.lineStarts.push_back(i + 1);
    }
}

std::wstring_view snapshotLine(const Document& doc, size_t row) {
    if (row >= doc.lineStarts.size()) return {};
    const auto start = doc.lineStarts[row];
    const auto end = row + 1 < doc.lineStarts.size() ? doc.lineStarts[row + 1] : doc.snapshot.size();
    return trimLine(std::wstring_view(doc.snapshot).substr(start, end - start));
}

void update(Document& doc, void* manager, const ThemeState& theme, ULONGLONG now) {
    if (doc.due && now < doc.due) return;
    if (doc.due || !doc.seen) {
        doc.due = 0;
        auto text = snapshot(manager);
        if (!doc.seen || doc.snapshot != text) {
            doc.seen = true;
            doc.snapshot = std::move(text);
            ++doc.revision;
            doc.lines.clear();
            if (!doc.binding.source) indexLines(doc);
            restore(doc);
            redraw(doc, 0);
            queue(doc);
        }
    }
    Result result{};
    {
        std::lock_guard lock(state->mutex);
        auto ready = state->results.find(doc.id);
        if (ready != state->results.end()) {
            result = std::move(ready->second);
            state->results.erase(ready);
        }
    }
    if (result.revision == doc.revision && result.revision) {
        doc.lines = std::move(result.lines);
        apply(doc, manager, theme);
    } else if (doc.themeRevision != theme.revision && !doc.lines.empty()) {
        apply(doc, manager, theme);
    }
}

void CALLBACK tick(HWND, UINT, UINT_PTR timer, DWORD) {
    if (timer != state->timer || state->updating) return;
    state->updating = true;
    try {
        ThemeState theme;
        { std::lock_guard lock(state->mutex); theme = state->theme; }
        if (!theme.enabled) {
            for (auto& [source, doc] : state->documents) {
                restore(doc);
                redraw(doc, 0);
            }
            while (!state->views.empty()) forget(state->views.begin()->first);
            state->updating = false;
            return;
        }
        const auto now = GetTickCount64();
        // A view can change its binding without being destroyed. A snapshot or
        // cached response is only used while the source and manager still match.
        for (auto it = state->views.begin(); it != state->views.end();) {
            const auto [view, previous] = *it++;
            const auto current = bindingFor(view);
            if (current.manager != previous.manager || current.source != previous.source ||
                current.context != previous.context) {
                forget(view);
                // A zero row count invalidates the whole view, including its layout cache.
                if (current.manager && checkedPointer(view) == api.viewTable) api.invalidate(view, 0, 0);
            }
        }
        for (auto& [key, doc] : state->documents) {
            auto view = std::find_if(state->views.begin(), state->views.end(), [&](const auto& pair) { return pair.second.key() == key; });
            if (view != state->views.end()) update(doc, view->second.manager, theme, now);
        }
    } catch (...) {}
    state->updating = false;
}

bool claimGuiThread() {
    const LONG thread = static_cast<LONG>(GetCurrentThreadId());
    const LONG gui = InterlockedCompareExchange(&state->guiThread, 0, 0);
    if (gui) return gui == thread;
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(static_cast<DWORD>(thread), &info)) return false;
    const LONG previous = InterlockedCompareExchange(&state->guiThread, thread, 0);
    return !previous || previous == thread;
}

void observe(void* view) {
    const LONG gui = InterlockedCompareExchange(&state->guiThread, 0, 0);
    if (gui && gui != static_cast<LONG>(GetCurrentThreadId())) return;
    if (state->updating || state->views.count(view)) return;
    { std::lock_guard lock(state->mutex); if (!state->theme.enabled) return; }
    const auto binding = bindingFor(view);
    if (!binding.manager || !claimGuiThread()) return;
    auto known = state->documents.find(binding.key());
    if (known == state->documents.end() && state->documents.size() >= maxDocuments) return;
    if (!state->timer) state->timer = SetTimer(nullptr, 0, 60, tick);
    if (!state->timer) return;
    if (known == state->documents.end()) {
        state->documents.try_emplace(binding.key(), Document{binding, ++state->nextId});
        if (binding.source) method<RefCall>(binding.source, 1)(binding.source);
    }
    state->views.emplace(view, binding);
}

LRESULT hookedPaint(void* view) {
    LRESULT result;
    {
        const bool tracked = static_cast<DWORD>(state->guiThread) == GetCurrentThreadId() && state->views.count(view);
        EditorSelectionPaint selection(tracked);
        result = api.paint(view);
    }
    try { observe(view); } catch (...) {}
    return result;
}

void hookedClose(void* view) {
    if (static_cast<DWORD>(state->guiThread) == GetCurrentThreadId()) {
        state->comparisonViews.erase(view);
        forget(view);
    }
    api.close(view);
}

void hookedChange(void* manager, int kind, const void* change, uint64_t parameter) {
    api.change(manager, kind, change, parameter);
    if (static_cast<DWORD>(state->guiThread) != GetCurrentThreadId() || (kind != 3 && kind != 5 && kind != 7)) return;
    for (const auto& [view, binding] : state->views) {
        if (binding.manager == manager) state->documents.at(binding.key()).due = GetTickCount64() + debounce;
    }
}

class ModuleComparisonScope {
public:
    ModuleComparisonScope() : previous(std::exchange(openingModuleComparison, true)) {}
    ~ModuleComparisonScope() { openingModuleComparison = previous; }
private:
    bool previous;
};

void hookedStructuredComparison(void* controller) {
    ModuleComparisonScope scope;
    comparison.structured(controller);
}

void hookedTextComparison(void* controller, const void* left, const void* right,
                          const void* leftName, const void* rightName) {
    ModuleComparisonScope scope;
    comparison.text(controller, left, right, leftName, rightName);
}

void hookedFormatLine(void* context, void* layout) {
    comparison.formatLine(context, layout);
    try {
        if (openingModuleComparison && checkedPointer(layout) == comparison.layoutTable &&
            checkedPointer(checkedPointer(context, comparison.differenceOffset)) == comparison.differenceTable &&
            claimGuiThread()) {
            auto* view = checkedPointer(context, comparison.contextViewOffset);
            const auto binding = nativeBinding(view);
            if (binding.manager && binding.context == context && !binding.source) {
                state->comparisonViews.insert_or_assign(view, context);
                observe(view);
                const auto doc = state->documents.find(binding.key());
                if (doc != state->documents.end()) doc->second.due = GetTickCount64() + debounce;
            }
        }
        if (static_cast<DWORD>(state->guiThread) != GetCurrentThreadId() || state->comparisonViews.empty()) return;
        auto* view = checkedPointer(context, comparison.contextViewOffset);
        const auto bound = state->views.find(view);
        if (bound == state->views.end() || bound->second.context != context || bound->second.source ||
            checkedPointer(layout) != comparison.layoutTable) return;
        const auto& doc = state->documents.at(bound->second.key());
        const auto row = at<size_t>(layout, 0x30);
        if (row >= doc.paintedRows || row >= doc.lines.size()) return;
        const auto* text = at<const NativeString*>(layout, 0x38);
        if (!text) return;
        const auto line = trimLine(text->value());
        if (line.empty() || line != snapshotLine(doc, row)) return;
        // The native formatter has already applied diff backgrounds and other
        // styles. Its foreground setter splits the same layout ranges in place.
        for (const auto& span : doc.lines[row]) {
            const auto end = span.start + span.length;
            if (end > line.size() || !boundary(line, span.start) || !boundary(line, end)) continue;
            comparison.setForeground(layout, span.start, end, doc.comparisonColors[span.role]);
        }
    } catch (...) {}
}

void createComparisonHooks(const ResolvedComparisonSyntax& found) {
    const struct {
        void* target;
        void* hook;
        void** original;
    } hooks[] = {
        {found.formatLine, reinterpret_cast<void*>(hookedFormatLine), reinterpret_cast<void**>(&comparison.formatLine)},
        {found.refreshStructuredComparison, reinterpret_cast<void*>(hookedStructuredComparison), reinterpret_cast<void**>(&comparison.structured)},
        {found.openTextComparison, reinterpret_cast<void*>(hookedTextComparison), reinterpret_cast<void**>(&comparison.text)},
    };
    size_t count = 0;
    for (const auto& hook : hooks) {
        if (MH_CreateHook(hook.target, hook.hook, hook.original) != MH_OK) {
            while (count) MH_RemoveHook(hooks[--count].target);
            return;
        }
        ++count;
    }
    // Publish every trampoline and its metadata before enabling any of the
    // hooks. If MinHook can only enable a prefix, each detour is still valid.
    comparison.setForeground = reinterpret_cast<SetForeground>(found.setForeground);
    comparison.layoutTable = found.layoutTable;
    comparison.differenceTable = found.differenceTable;
    comparison.differenceOffset = found.differenceOffset;
    comparison.contextViewOffset = found.contextViewOffset;
    for (const auto& hook : hooks) MH_QueueEnableHook(hook.target);
    if (MH_ApplyQueued() != MH_OK) {
        for (const auto& hook : hooks) MH_QueueDisableHook(hook.target);
        // Keep the trampolines alive for detours that may already be running.
        MH_ApplyQueued();
    }
}

bool installedModule(HMODULE module, const wchar_t* directory, const wchar_t* name) {
    if (!module) return false;
    wchar_t path[MAX_PATH], expected[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    return length && length < MAX_PATH && swprintf(expected, MAX_PATH, L"%ls\\%ls", directory, name) >= 0 &&
           !_wcsicmp(path, expected);
}

template<typename T> T resolve(HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    T function = nullptr;
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

BOOL CALLBACK repaintWindow(HWND window, LPARAM) {
    DWORD process;
    GetWindowThreadProcessId(window, &process);
    if (process == GetCurrentProcessId()) RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    return TRUE;
}

// Called by the theme writer under the lifecycle lock, after base hooks are
// active. A missing DLL can be retried without rediscovering the base editor.
void initializeComparisonSyntax(const wchar_t* directory) {
    static bool comparisonAttempted = false;
    if (comparisonAttempted) return;
    const HMODULE frontend = GetModuleHandleW(L"frntend.dll");
    if (!frontend) return;
    comparisonAttempted = true;
    if (!installedModule(frontend, directory, L"frntend.dll")) return;

    frontendLexerTable.store(discoverFrontendLexer(frontend));
    ResolvedComparisonSyntax found{};
    if (discoverComparisonSyntax(GetModuleHandleW(L"frame.dll"), GetModuleHandleW(L"txtedui.dll"),
                                 frontend, api.contextOwnerOffset, found)) createComparisonHooks(found);
    if (frontendLexerTable.load()) EnumWindows(repaintWindow, 0);
}
}

bool editorBackgroundFill() { return paintingBackground; }

void configureEditorSyntax(const ProbeConfig* config) {
    if (!state) return;
    Palette colors{};
    unsigned roles = 0;
    if (config && config->mode) for (DWORD i = 0; i < config->mapCount; ++i) {
        const auto& mapping = config->maps[i];
        if (mapping.kind != SyntaxColor) continue;
        const DWORD rgb = mapping.target;
        colors[mapping.source] = RGB(rgb >> 16, rgb >> 8, rgb);
        ++roles;
    }
    const bool enabled = roles == SyntaxRoleCount;
    bool activate = false;
    {
        std::lock_guard lock(state->mutex);
        activate = enabled && !state->theme.enabled;
        if (state->theme.enabled != enabled || state->theme.colors != colors) {
            state->theme = {state->theme.revision + 1, enabled, colors};
        }
    }
    if (activate) EnumWindows(repaintWindow, 0);
}

void initializeEditorSyntax(const wchar_t* directory) {
    if (attempted) {
        if (state) initializeComparisonSyntax(directory);
        return;
    }
    const bool core83 = GetModuleHandleW(L"core83.dll") != nullptr;
    const bool core85 = GetModuleHandleW(L"core85.dll") != nullptr;
    if (core83 == core85) return;
    struct Module { const wchar_t* name; HMODULE handle; } modules[] = {
        {L"frame.dll", nullptr}, {L"txtedui.dll", nullptr}, {L"txtedt.dll", nullptr},
        {core83 ? L"core83.dll" : L"core85.dll", nullptr},
        {core83 ? L"nuke83.dll" : L"nuke85.dll", nullptr}, {L"debug.dll", nullptr},
    };
    for (auto& module : modules) {
        module.handle = GetModuleHandleW(module.name);
        if (!module.handle) return;
    }
    attempted = true;
    for (const auto& module : modules) {
        if (!installedModule(module.handle, directory, module.name)) return;
    }
    const auto frame = modules[0].handle, ui = modules[1].handle, text = modules[2].handle;
    const auto core = modules[3].handle, nuke = modules[4].handle, debug = modules[5].handle;
    ResolvedSyntax found{};
    if (!discoverSyntax(frame, ui, text, core, debug, found)) return;
    api.viewTable = found.viewTable;
    api.ownerTable = found.ownerTable;
    api.sourceTable = found.sourceTable;
    api.lexerTable = found.lexerTable;
    api.viewContextOffset = found.viewContextOffset;
    api.contextOwnerOffset = found.contextOwnerOffset;
    api.clearString = reinterpret_cast<VoidCall>(found.clearString);
    api.lock = resolve<VoidCall>(core, "?lock@RWMutex@core@@QEAAXXZ");
    api.unlock = resolve<VoidCall>(core, "?unlock@RWMutex@core@@QEAAXXZ");
    api.save = reinterpret_cast<decltype(api.save)>(found.save);
    api.getLine = reinterpret_cast<decltype(api.getLine)>(found.getLine);
    api.getRuns = reinterpret_cast<decltype(api.getRuns)>(found.getRuns);
    api.slot = reinterpret_cast<decltype(api.slot)>(found.slot);
    api.setRef = reinterpret_cast<SetRef>(found.setRef);
    api.allocate = resolve<decltype(api.allocate)>(nuke, "?nuke_new@@YAPEAX_K@Z");
    api.release = resolve<decltype(api.release)>(nuke, "?nuke_delete_size@@YAXPEAX_K@Z");
    api.invalidate = reinterpret_cast<decltype(api.invalidate)>(found.invalidate);
    if (!api.lock || !api.unlock || !api.allocate || !api.release || MH_Initialize() != MH_OK) return;
    const auto paint = found.paint, close = found.close, change = found.change;
    if (MH_CreateHook(paint, reinterpret_cast<void*>(hookedPaint), reinterpret_cast<void**>(&api.paint)) != MH_OK ||
        MH_CreateHook(close, reinterpret_cast<void*>(hookedClose), reinterpret_cast<void**>(&api.close)) != MH_OK ||
        MH_CreateHook(change, reinterpret_cast<void*>(hookedChange), reinterpret_cast<void**>(&api.change)) != MH_OK) {
        MH_Uninitialize();
        return;
    }
    HMODULE windowBase = GetModuleHandleW(core83 ? L"wbase83.dll" : L"wbase85.dll");
    const bool backgroundHooks = createBackgroundHooks(windowBase, found);
    try { state = new State; } catch (...) { MH_Uninitialize(); return; }
    HANDLE worker = CreateThread(nullptr, 0, tokenizeWorker, nullptr, 0, nullptr);
    if (!worker) { delete state; state = nullptr; MH_Uninitialize(); return; }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        MH_Uninitialize();
        { std::lock_guard lock(state->mutex); state->workerStopping = true; }
        state->jobsReady.notify_one();
        WaitForSingleObject(worker, INFINITE);
        delete state;
        state = nullptr;
    } else {
        initializeComparisonSyntax(directory);
        if (backgroundHooks) EnumWindows(repaintWindow, 0);
    }
    CloseHandle(worker);
}
