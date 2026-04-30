#pragma once

#include "dtx/NSObject.h"
#include "MixDeviceBridge.h"

#include "lldb/API/LLDB.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ycode::debughost {

class LldbBackend {
public:
  LldbBackend();
  ~LldbBackend();

  llvm::dtx::ns::Object makeCapabilities() const;

  llvm::dtx::ns::Object createSession(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object closeSession(const llvm::dtx::ns::Object &Request);

  llvm::dtx::ns::Object launch(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object attach(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object continueExecution(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object pause(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object stepIn(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object stepOver(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object stepOut(const llvm::dtx::ns::Object &Request);

  llvm::dtx::ns::Object
  setSourceBreakpoints(const llvm::dtx::ns::Object &Request);

  llvm::dtx::ns::Object threads(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object stackTrace(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object scopes(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object variables(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object registers(const llvm::dtx::ns::Object &Request);

  llvm::dtx::ns::Object evaluate(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object readMemory(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object disassemble(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object modules(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object reloadSymbols(const llvm::dtx::ns::Object &Request);
  void setEventSink(DebugHostEventSink Sink);

private:
  struct EventState {
    std::atomic<bool> Stop{false};
    std::thread Thread;
    std::mutex Mutex;
    std::condition_variable Changed;
    lldb::StateType LastState = lldb::eStateInvalid;
    uint32_t LastStopID = 0;
    lldb::StateType LastEmittedState = lldb::eStateInvalid;
    uint32_t LastEmittedStopID = 0;
    // Monotonic counter incremented by updateSessionState on every consumed
    // process state-change event. Callers snapshot it before issuing an
    // action (SendAsyncInterrupt, Continue, ...) and wait for a strictly
    // newer generation to confirm the action actually landed — not just
    // that LastState happens to match a stale historical value.
    uint64_t Generation = 0;
  };

  struct TransitionWaitResult {
    lldb::StateType State = lldb::eStateInvalid;
    uint32_t StopID = 0;
    uint64_t Generation = 0;
    bool Observed = false;
  };

  struct Session {
    std::string Id;
    lldb::SBDebugger Debugger;
    lldb::SBTarget Target;
    lldb::SBProcess Process;
    std::map<std::string, std::vector<uint64_t>> SourceBreakpoints;
    std::map<uint64_t, std::vector<lldb::SBValue>> VariableRefs;
    uint64_t NextVariableRef = 1;
    std::shared_ptr<EventState> Events = std::make_shared<EventState>();
  };

  Session *findSession(const llvm::dtx::ns::Object &Request);
  const Session *findSession(const llvm::dtx::ns::Object &Request) const;
  uint64_t addVariableRef(Session &S, std::vector<lldb::SBValue> Values);
  void invalidateVariableRefs(Session &S);
  void startEventPump(Session &S);
  void stopEventPump(Session &S);
  lldb::StateType waitForSessionState(
      Session &S, uint32_t TimeoutMs,
      const std::function<bool(lldb::StateType)> &Predicate);
  // Snapshot the current event Generation; pair with waitForSessionTransition.
  uint64_t snapshotEventGeneration(Session &S) const;
  // Wait until a process state-change event is consumed AFTER BaselineGen
  // (i.e. Generation > BaselineGen) AND Predicate(LastState) holds, or the
  // timeout expires. Returns whatever LastState is at the point we give up.
  TransitionWaitResult waitForSessionTransition(
      Session &S, uint64_t BaselineGen, uint32_t TimeoutMs,
      const std::function<bool(lldb::StateType, uint32_t)> &Predicate);
  void updateSessionState(Session &S, lldb::StateType State);
  void emitProcessEvent(Session &S, lldb::StateType State, bool Force);
  void emitModuleLoadedEvents(Session &S, const lldb::SBEvent &Event);

  bool Initialized_ = false;
  uint64_t NextSessionId_ = 1;
  std::map<std::string, Session> Sessions_;
  mutable std::mutex EventSinkMutex_;
  DebugHostEventSink EventSink_;
};

} // namespace ycode::debughost
