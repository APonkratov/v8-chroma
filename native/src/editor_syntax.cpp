#include "editor_syntax.h"
#include "editor_syntax_discovery.h"
#include "editor_selection.h"
#include "textmate.h"
#include <MinHook.h>
#include <algorithm>
#include <array>
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
struct View { void* manager; void* source; };

using Palette = std::array<COLORREF, SyntaxRoleCount>;
struct ThemeState {
    uint64_t revision = 0;
    bool enabled = false;
    Palette colors{};
};

struct Job { uint64_t revision; std::wstring text; };
struct Result { uint64_t revision; SyntaxLines lines; };

struct Document {
    void* source;
    uint64_t id;
    uint64_t revision = 0, themeRevision = 0;
    ULONGLONG due = 0;
    bool seen = false;
    std::wstring snapshot;
    SyntaxLines lines;
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
    // The three hooks and timer own the native editor state on one GUI thread.
    volatile LONG guiThread = 0;
    UINT_PTR timer = 0;
    uint64_t nextId = 0;
    bool updating = false;
    std::map<void*, View> views;
    std::map<void*, Document> documents;
};

// ThemeProbe is pinned. Keep this state alive through process teardown so a
// platform destructor cannot call a hook after a C++ global has been destroyed.
State* state = nullptr;
bool attempted = false;

template<typename T> T at(const void* object, size_t offset) {
    T value{};
    if (object) std::memcpy(&value, static_cast<const char*>(object) + offset, sizeof(value));
    return value;
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
        CacheLock lock(doc.source);
        auto* deque = static_cast<char*>(doc.source) + 0x30;
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
    auto line = input.value();
    while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) line.remove_suffix(1);
    if (line.empty()) return;
    Ref original;
    api.getRuns(doc.source, row, &input, &original.ptr);
    if (!original.ptr) return;
    CacheLock lock(doc.source);
    auto* deque = static_cast<char*>(doc.source) + 0x30;
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
        if (binding.source == doc.source && checkedPointer(view) == api.viewTable) api.invalidate(view, 0, count);
    }
}

void apply(Document& doc, void* manager, const ThemeState& theme) {
    restore(doc);
    if (theme.enabled) {
        for (size_t i = 0; i < doc.lines.size(); ++i) {
            if (!doc.lines[i].empty()) patchRow(doc, manager, static_cast<int>(i + 1), doc.lines[i], theme.colors);
        }
    }
    redraw(doc, doc.patches.empty() ? 0 : doc.lines.size());
    doc.themeRevision = theme.revision;
}

View bindingFor(void* view) {
    if (checkedPointer(view) != api.viewTable) return {};
    auto* owner = checkedPointer(checkedPointer(view, api.viewContextOffset), api.contextOwnerOffset);
    if (checkedPointer(owner) != api.ownerTable) return {};
    // Two editor facets independently point at the current text manager.
    auto* manager = checkedPointer(checkedPointer(owner, 0x3d8), 0xc8);
    if (!manager || manager != checkedPointer(checkedPointer(owner, 0x3d0), 0xe8)) return {};
    auto* source = checkedPointer(manager, 0x228);
    if (checkedPointer(source) != api.sourceTable || checkedPointer(checkedPointer(source, 0x58)) != api.lexerTable) return {};
    return {manager, source};
}

void forget(void* view) {
    auto found = state->views.find(view);
    if (found == state->views.end()) return;
    void* source = found->second.source;
    state->views.erase(found);
    for (const auto& entry : state->views) if (entry.second.source == source) return;
    auto doc = state->documents.find(source);
    restore(doc->second);
    {
        std::lock_guard lock(state->mutex);
        state->jobs.erase(doc->second.id);
        state->results.erase(doc->second.id);
        state->expected.erase(doc->second.id);
    }
    state->documents.erase(doc);
    method<RefCall>(source, 2)(source);
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
            if (current.manager != previous.manager || current.source != previous.source) forget(view);
        }
        for (auto& [source, doc] : state->documents) {
            auto view = std::find_if(state->views.begin(), state->views.end(), [&](const auto& pair) { return pair.second.source == source; });
            if (view != state->views.end()) update(doc, view->second.manager, theme, now);
        }
    } catch (...) {}
    state->updating = false;
}

void observe(void* view) {
    const LONG thread = static_cast<LONG>(GetCurrentThreadId());
    const LONG gui = InterlockedCompareExchange(&state->guiThread, 0, 0);
    if (gui && gui != thread) return;
    if (state->updating || state->views.count(view)) return;
    { std::lock_guard lock(state->mutex); if (!state->theme.enabled) return; }
    const auto binding = bindingFor(view);
    GUITHREADINFO guiInfo{};
    guiInfo.cbSize = sizeof(guiInfo);
    if (!binding.source || !GetGUIThreadInfo(static_cast<DWORD>(thread), &guiInfo)) return;
    if (InterlockedCompareExchange(&state->guiThread, thread, 0) != 0 && state->guiThread != thread) return;
    auto known = state->documents.find(binding.source);
    if (known == state->documents.end() && state->documents.size() >= maxDocuments) return;
    if (!state->timer) state->timer = SetTimer(nullptr, 0, 60, tick);
    if (!state->timer) return;
    if (known == state->documents.end()) {
        state->documents.try_emplace(binding.source, Document{binding.source, ++state->nextId});
        method<RefCall>(binding.source, 1)(binding.source);
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
    if (static_cast<DWORD>(state->guiThread) == GetCurrentThreadId()) forget(view);
    api.close(view);
}

void hookedChange(void* manager, int kind, const void* change, uint64_t parameter) {
    api.change(manager, kind, change, parameter);
    if (static_cast<DWORD>(state->guiThread) != GetCurrentThreadId() || (kind != 3 && kind != 5 && kind != 7)) return;
    for (const auto& [view, binding] : state->views) {
        if (binding.manager == manager) state->documents.at(binding.source).due = GetTickCount64() + debounce;
    }
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
}

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
    if (attempted) return;
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
        wchar_t path[MAX_PATH], expected[MAX_PATH];
        DWORD length = GetModuleFileNameW(module.handle, path, MAX_PATH);
        if (!length || length >= MAX_PATH || swprintf(expected, MAX_PATH, L"%ls\\%ls", directory, module.name) < 0 ||
            _wcsicmp(path, expected)) return;
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
    }
    CloseHandle(worker);
}
