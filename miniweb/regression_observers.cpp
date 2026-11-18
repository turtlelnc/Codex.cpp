#include "src/engine.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

bool click(Engine& engine, const char* id) {
    auto* target = engine.document()->get_element_by_id(id);
    check(target != nullptr, std::string("Missing observer control: ") + id);
    auto r = target->viewport_rect();
    return engine.click(r.x + r.w / 2, r.y + r.h / 2);
}

void load(Engine& engine, const std::filesystem::path& page) {
    Loader::set_access_root(page.parent_path().u8string());
    engine.set_wait_ms(0);
    std::string error;
    check(engine.load(page.u8string(), &error) && engine.render(false),
          "Observer fixture load: " + error);
}
} // namespace

void observer_records(const std::filesystem::path& page, bool report) {
    Engine engine(640, 480);
    load(engine, page);
    check(click(engine, "run"), "Observer record action");
    auto* body = engine.document()->body;
    check(!body->has_attr("data-error"), body->attr("data-error"));
    check(body->has_attr("data-result"), "Observer records did not complete");
    auto result = body->attr("data-result");
    for (const char* expected : {"\"styleNoop\":1", "\"stylePriority\":[100,200,1]", "\"cssText\":2", "\"invalid\":4",
                                "\"takeCallbacks\":0", "\"afterError\":1",
                                "\"comment\":[{\"type\":\"characterData\""}) {
        check(result.find(expected) != std::string::npos,
              std::string("Observer record regression: ") + expected);
    }
    if (report) std::cout << "RECORDS_RESULT " << result << '\n';
}

void observer_resize(const std::filesystem::path& page, bool report) {
    Engine engine(640, 480);
    load(engine, page);
    for (const char* id : {"border", "content", "unobserve", "exception", "disconnect"})
        check(click(engine, id), "Resize observer edge action");
    auto result = engine.document()->body->attr("data-result");
    check(result.find("\"following\":1") != std::string::npos &&
          result.find("[\"hidden\",0,0,0,0]") != std::string::npos,
          "Resize observer initial/exception regression");
    if (report) std::cout << "RESIZE_RESULT " << result << '\n';
}

void check_observers(const std::filesystem::path& directory, bool report) {
    Engine engine(640, 480);
    load(engine, directory / "observers.html");
    auto* body = engine.document()->body;
    check(body->attr("data-measure").find("[100,40,8,8,120,60]") != std::string::npos,
          "ResizeObserver initial content/border box snapshot");
    check(click(engine, "mutate"), "Mutation operation");
    auto state = body->attr("data-measure");
    check(state.find("\"old\":\"old\"") != std::string::npos &&
          state.find("\"old\":\"seed\"") != std::string::npos &&
          state.find("new-child") != std::string::npos && state.find("ignored") == std::string::npos,
          "Mutations include prior values, text and children; attribute filter excludes noise");
    check(state.find("[\"sync\",\"mutation\",\"promise\"]") != std::string::npos,
          "Mutation microtask runs before subsequently queued promise");
    check(click(engine, "resize") &&
          body->attr("data-measure").find("[160,70,8,8,180,90]") != std::string::npos,
          "Resize callback uses updated layout");
    check(click(engine, "hide") &&
          body->attr("data-measure").find("[0,0,0,0,0,0]") != std::string::npos &&
          click(engine, "show"), "Hidden and restored sizes notify");
    check(click(engine, "disconnect"), "Disconnect observers");
    state = body->attr("data-measure");
    check(engine.advance(0) && engine.render(false) && body->attr("data-measure") == state,
          "Disconnected observers do not receive later changes");
    if (report) std::cout << "OBSERVERS_RESULT " << state << '\n';
    check(click(engine, "loop") &&
          body->attr("data-measure").find("\"loopErrors\":0") == std::string::npos,
          "Resize feedback defers with error notification");
    check(click(engine, "disconnect"), "Stop resize feedback");

    Engine feedback(640, 480);
    load(feedback, directory / "observer-feedback.html");
    check(click(feedback, "start"), "Mutation feedback remains responsive");
    auto* feedback_body = feedback.document()->body;
    auto count = std::stoi(feedback_body->attr("data-count"));
    check(count > 0 && count <= 512, "Mutation checkpoint feedback budget");
    check(click(feedback, "stop") && feedback.advance(0) &&
          feedback_body->attr("data-count") == std::to_string(count),
          "Disconnect clears deferred mutation feedback");
    check(click(feedback, "budget") && feedback.advance(0) &&
          feedback_body->attr("data-budget") == "[true,true]",
          "Observer registration/queue limits reject excess and remain disconnectable");
    observer_records(directory / "observer-records.html", false);
    observer_resize(directory / "observer-resize.html", false);
    std::cerr << "MiniWeb observer regressions passed\n";
}
