#include "server/guarddog_impl.h"

#include <sys/types.h>

#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include "envoy/common/time.h"
#include "envoy/config/bootstrap/v3/bootstrap.pb.h"
#include "envoy/server/configuration.h"
#include "envoy/server/guarddog.h"
#include "envoy/server/guarddog_config.h"
#include "envoy/stats/scope.h"

#include "common/common/assert.h"
#include "common/common/fmt.h"
#include "common/common/lock_guard.h"
#include "common/common/logger.h"
#include "common/config/utility.h"
#include "common/protobuf/utility.h"
#include "common/stats/symbol_table_impl.h"

#include "server/watchdog_impl.h"

#include "absl/synchronization/mutex.h"

namespace Envoy {
namespace Server {

GuardDogImpl::GuardDogImpl(Stats::Scope& stats_scope, const Server::Configuration::Watchdog& config,
                           Api::Api& api, absl::string_view name,
                           std::unique_ptr<TestInterlockHook>&& test_interlock)
    : test_interlock_hook_(std::move(test_interlock)), stats_scope_(stats_scope),
      time_source_(api.timeSource()), miss_timeout_(config.missTimeout()),
      megamiss_timeout_(config.megaMissTimeout()), kill_timeout_(config.killTimeout()),
      multi_kill_timeout_(config.multiKillTimeout()),
      multi_kill_fraction_(config.multiKillThreshold() / 100.0),
      loop_interval_([&]() -> std::chrono::milliseconds {
        // The loop interval is simply the minimum of all specified intervals,
        // but we must account for the 0=disabled case. This lambda takes care
        // of that and returns a value that initializes the const loop interval.
        const auto min_of_nonfatal = std::min(miss_timeout_, megamiss_timeout_);
        return std::min({killEnabled() ? kill_timeout_ : min_of_nonfatal,
                         multikillEnabled() ? multi_kill_timeout_ : min_of_nonfatal,
                         min_of_nonfatal});
      }()),
      watchdog_miss_counter_(stats_scope.counterFromStatName(
          Stats::StatNameManagedStorage(absl::StrCat(name, ".watchdog_miss"),
                                        stats_scope.symbolTable())
              .statName())),
      watchdog_megamiss_counter_(stats_scope.counterFromStatName(
          Stats::StatNameManagedStorage(absl::StrCat(name, ".watchdog_mega_miss"),
                                        stats_scope.symbolTable())
              .statName())),
      dispatcher_(api.allocateDispatcher(absl::StrCat(name, "_guarddog_thread"))),
      loop_timer_(dispatcher_->createTimer([this]() { step(); })),
      events_to_actions_([&](const Server::Configuration::Watchdog& config) -> EventToActionsMap {
        EventToActionsMap map;

        // We should be able to share the dispatcher since guard dog's lifetime
        // should eclipse those of actions.
        Configuration::GuardDogActionFactoryContext context = {api, *dispatcher_};

        const auto& actions = config.actions();
        for (const auto& action : actions) {
          // Get factory and add the created cb
          auto& factory = Config::Utility::getAndCheckFactory<Configuration::GuardDogActionFactory>(
              action.config());
          map[action.event()].push_back(factory.createGuardDogActionFromProto(action, context));
        }

        return map;
      }(config)),
      run_thread_(true) {
  start(api);
}

GuardDogImpl::GuardDogImpl(Stats::Scope& stats_scope, const Server::Configuration::Watchdog& config,
                           Api::Api& api, absl::string_view name)
    : GuardDogImpl(stats_scope, config, api, name, std::make_unique<TestInterlockHook>()) {}

GuardDogImpl::~GuardDogImpl() { stop(); }

void GuardDogImpl::step() {
  {
    Thread::LockGuard guard(mutex_);
    if (!run_thread_) {
      return;
    }
  }

  const auto now = time_source_.monotonicTime();
  std::vector<std::pair<Thread::ThreadId, MonotonicTime>> miss_threads;
  std::vector<std::pair<Thread::ThreadId, MonotonicTime>> mega_miss_threads;

  {
    std::vector<std::pair<Thread::ThreadId, MonotonicTime>> multi_kill_threads;
    Thread::LockGuard guard(wd_lock_);

    // Compute the multikill threshold
    const size_t required_for_multi_kill =
        std::max(static_cast<size_t>(2),
                 static_cast<size_t>(ceil(multi_kill_fraction_ * watched_dogs_.size())));

    for (auto& watched_dog : watched_dogs_) {
      const auto last_checkin = watched_dog->dog_->lastTouchTime();
      const auto tid = watched_dog->dog_->threadId();
      const auto delta = now - last_checkin;
      if (watched_dog->last_alert_time_ && watched_dog->last_alert_time_.value() < last_checkin) {
        watched_dog->miss_alerted_ = false;
        watched_dog->megamiss_alerted_ = false;
      }
      if (delta > miss_timeout_) {
        if (!watched_dog->miss_alerted_) {
          watchdog_miss_counter_.inc();
          watched_dog->miss_counter_.inc();
          watched_dog->last_alert_time_ = last_checkin;
          watched_dog->miss_alerted_ = true;
          miss_threads.emplace_back(tid, last_checkin);
        }
      }
      if (delta > megamiss_timeout_) {
        if (!watched_dog->megamiss_alerted_) {
          watchdog_megamiss_counter_.inc();
          watched_dog->megamiss_counter_.inc();
          watched_dog->last_alert_time_ = last_checkin;
          watched_dog->megamiss_alerted_ = true;
          mega_miss_threads.emplace_back(tid, last_checkin);
        }
      }
      if (killEnabled() && delta > kill_timeout_) {
        invokeGuardDogActions(WatchDogAction::KILL, {{tid, last_checkin}}, now);

        PANIC(fmt::format("GuardDog: one thread ({}) stuck for more than watchdog_kill_timeout",
                          watched_dog->dog_->threadId().debugString()));
      }
      if (multikillEnabled() && delta > multi_kill_timeout_) {
        multi_kill_threads.emplace_back(tid, last_checkin);

        if (multi_kill_threads.size() >= required_for_multi_kill) {
          invokeGuardDogActions(WatchDogAction::MULTIKILL, multi_kill_threads, now);

          PANIC(fmt::format("GuardDog: At least {} threads ({},...) stuck for more than "
                            "watchdog_multikill_timeout",
                            multi_kill_threads.size(), tid.debugString()));
        }
      }
    }
  }

  // Run megamiss and miss handlers
  if (!mega_miss_threads.empty()) {
    invokeGuardDogActions(WatchDogAction::MEGAMISS, mega_miss_threads, now);
  }

  if (!miss_threads.empty()) {
    invokeGuardDogActions(WatchDogAction::MISS, miss_threads, now);
  }

  {
    Thread::LockGuard guard(mutex_);
    test_interlock_hook_->signalFromImpl(now);
    if (run_thread_) {
      loop_timer_->enableTimer(loop_interval_);
    }
  }
}

WatchDogSharedPtr GuardDogImpl::createWatchDog(Thread::ThreadId thread_id,
                                               const std::string& thread_name) {
  // Timer started by WatchDog will try to fire at 1/2 of the interval of the
  // minimum timeout specified. loop_interval_ is const so all shared state
  // accessed out of the locked section below is const (time_source_ has no
  // state).
  const auto wd_interval = loop_interval_ / 2;
  WatchDogSharedPtr new_watchdog =
      std::make_shared<WatchDogImpl>(std::move(thread_id), time_source_, wd_interval);
  WatchedDogPtr watched_dog = std::make_unique<WatchedDog>(stats_scope_, thread_name, new_watchdog);
  {
    Thread::LockGuard guard(wd_lock_);
    watched_dogs_.push_back(std::move(watched_dog));
  }
  new_watchdog->touch();
  return new_watchdog;
}

void GuardDogImpl::stopWatching(WatchDogSharedPtr wd) {
  Thread::LockGuard guard(wd_lock_);
  auto found_wd = std::find_if(watched_dogs_.begin(), watched_dogs_.end(),
                               [&wd](const WatchedDogPtr& d) -> bool { return d->dog_ == wd; });
  if (found_wd != watched_dogs_.end()) {
    watched_dogs_.erase(found_wd);
  } else {
    ASSERT(false);
  }
}

void GuardDogImpl::start(Api::Api& api) {
  Thread::LockGuard guard(mutex_);
  // See comments in WorkerImpl::start for the naming convention.
  Thread::Options options{absl::StrCat("dog:", dispatcher_->name())};
  thread_ = api.threadFactory().createThread(
      [this]() -> void { dispatcher_->run(Event::Dispatcher::RunType::RunUntilExit); }, options);
  loop_timer_->enableTimer(std::chrono::milliseconds(0));
}

void GuardDogImpl::stop() {
  {
    Thread::LockGuard guard(mutex_);
    run_thread_ = false;
  }
  dispatcher_->exit();
  if (thread_) {
    thread_->join();
    thread_.reset();
  }
}

void GuardDogImpl::invokeGuardDogActions(
    WatchDogAction::WatchdogEvent event,
    std::vector<std::pair<Thread::ThreadId, MonotonicTime>> thread_last_checkin_pairs,
    MonotonicTime now) {
  const auto& registered_actions = events_to_actions_.find(event);
  if (registered_actions != events_to_actions_.end()) {
    for (auto& action : registered_actions->second) {
      action->run(event, thread_last_checkin_pairs, now);
    }
  }
}

GuardDogImpl::WatchedDog::WatchedDog(Stats::Scope& stats_scope, const std::string& thread_name,
                                     const WatchDogSharedPtr& watch_dog)
    : dog_(watch_dog),
      miss_counter_(stats_scope.counterFromStatName(
          Stats::StatNameManagedStorage(fmt::format("server.{}.watchdog_miss", thread_name),
                                        stats_scope.symbolTable())
              .statName())),
      megamiss_counter_(stats_scope.counterFromStatName(
          Stats::StatNameManagedStorage(fmt::format("server.{}.watchdog_mega_miss", thread_name),
                                        stats_scope.symbolTable())
              .statName())) {}

} // namespace Server
} // namespace Envoy
