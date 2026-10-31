#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace penguin {
// JSON strings cross the module boundary; no application objects or ownership do.
using Emit = std::function<void(const std::string&, const std::string&)>;
using Next = std::function<std::string(const std::string&, const Emit&)>;
using Handler = std::function<std::string(const std::string&, const Emit&, const Next&)>;

struct Contribution {
  std::string service;
  std::string replaces; // Explicit current owner; never silently shadow a service.
  Handler handler;
};
struct Module {
  std::string id;
  std::vector<std::string> dependencies;
  std::vector<Contribution> contributions;
};
struct ServiceInfo { std::string name, owner; std::vector<std::string> layers; };

class Runtime {
  struct Binding { std::string owner; Handler handler; };
  mutable std::recursive_mutex mutex_;
  std::map<std::string, std::vector<Binding>> services_;
  std::map<std::string, Module> modules_;
  bool frozen_ = false;
  static bool valid_name(const std::string& name) {
    return !name.empty() && name.size() <= 128 &&
      name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-") == std::string::npos;
  }
public:
  // Builtins use exactly the same module/contribution machinery as extensions.
  // An empty builtin handler means the caller supplies a per-call implementation:
  // it keeps Session/TUI/approval objects inside their owning thread and lifetime.
  void mount(Module module) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (frozen_) throw std::runtime_error("Penguin runtime is frozen; restart to change modules");
    if (!valid_name(module.id) || modules_.count(module.id)) throw std::runtime_error("Invalid/duplicate Penguin module: " + module.id);
    for (const auto& dependency : module.dependencies)
      if (!modules_.count(dependency)) throw std::runtime_error("Missing Penguin dependency: " + dependency);
    std::set<std::string> names;
    for (const auto& c : module.contributions) {
      if (!valid_name(c.service) || !names.insert(c.service).second)
        throw std::runtime_error("Invalid/duplicate Penguin service: " + c.service);
      auto it = services_.find(c.service);
      if (it == services_.end()) {
        if (!c.replaces.empty()) throw std::runtime_error("Penguin replacement target missing: " + c.service);
      } else if (c.replaces.empty() || c.replaces != it->second.back().owner)
        throw std::runtime_error("Penguin replacement must name current owner of " + c.service);
    }
    // Validate the complete module before publishing any contribution.
    auto services = services_;
    auto modules = modules_;
    for (const auto& c : module.contributions) services[c.service].push_back({module.id, c.handler});
    const auto id = module.id;
    modules.emplace(id, std::move(module));
    services_.swap(services);
    modules_.swap(modules);
  }
  void unmount(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (frozen_) throw std::runtime_error("Penguin runtime is frozen; restart to change modules");
    if (!modules_.count(id)) throw std::runtime_error("Unknown Penguin module: " + id);
    for (const auto& entry : modules_) if (entry.first != id &&
      std::find(entry.second.dependencies.begin(), entry.second.dependencies.end(), id) != entry.second.dependencies.end())
      throw std::runtime_error("Penguin module still required by " + entry.first);
    for (const auto& entry : services_) for (size_t i = 0; i < entry.second.size(); ++i)
      if (entry.second[i].owner == id && i + 1 < entry.second.size())
        throw std::runtime_error("Penguin module still replaced by " + entry.second.back().owner);
    for (auto it = services_.begin(); it != services_.end();) {
      auto& stack = it->second;
      if (stack.back().owner == id) stack.pop_back();
      if (stack.empty()) it = services_.erase(it); else ++it;
    }
    modules_.erase(id);
  }
  void freeze() { std::lock_guard<std::recursive_mutex> lock(mutex_); frozen_ = true; }
  std::vector<ServiceInfo> inspect() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<ServiceInfo> result;
    for (const auto& entry : services_) {
      ServiceInfo info{entry.first, entry.second.back().owner, {}};
      for (const auto& layer : entry.second) info.layers.push_back(layer.owner);
      result.push_back(std::move(info));
    }
    return result;
  }
  bool has_module(const std::string& id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_); return modules_.count(id) != 0;
  }
  bool contains(const std::string& name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_); return services_.count(name) != 0;
  }
  std::string call(const std::string& name, const std::string& request,
                   const Emit& emit = {}, const Next& builtin = {}) const {
    std::vector<Binding> bindings;
    { std::lock_guard<std::recursive_mutex> lock(mutex_);
      auto it = services_.find(name);
      if (it == services_.end()) throw std::runtime_error("Unknown Penguin service: " + name);
      bindings = it->second;
    }
    static thread_local unsigned depth = 0;
    if (depth >= 32) throw std::runtime_error("Penguin service recursion limit");
    struct Depth { unsigned& depth; explicit Depth(unsigned& d) : depth(d) { ++depth; } ~Depth() { --depth; } } guard(depth);
    // No registry lock is held while running a callback or waiting for I/O.
    std::function<std::string(size_t, const std::string&, const Emit&)> dispatch;
    dispatch = [&](size_t count, const std::string& input, const Emit& sink) -> std::string {
      if (!count) { if (!builtin) throw std::runtime_error("Penguin service has no builtin implementation: " + name); return builtin(input, sink); }
      const auto& binding = bindings[count - 1];
      if (!binding.handler) return dispatch(count - 1, input, sink);
      return binding.handler(input, sink, [&](const std::string& next_request, const Emit& next_sink) {
        return dispatch(count - 1, next_request, next_sink);
      });
    };
    return dispatch(bindings.size(), request, emit);
  }
};
} // namespace penguin
