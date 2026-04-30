#pragma once

#include "dtx/NSObject.h"

#include "lldb/API/LLDB.h"

#include <cstdint>
#include <map>
#include <string>
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

  llvm::dtx::ns::Object
  setSourceBreakpoints(const llvm::dtx::ns::Object &Request);

  llvm::dtx::ns::Object threads(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object stackTrace(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object scopes(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object variables(const llvm::dtx::ns::Object &Request);

  llvm::dtx::ns::Object evaluate(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object readMemory(const llvm::dtx::ns::Object &Request);
  llvm::dtx::ns::Object disassemble(const llvm::dtx::ns::Object &Request);

private:
  struct Session {
    std::string Id;
    lldb::SBDebugger Debugger;
    lldb::SBTarget Target;
    lldb::SBProcess Process;
    std::map<uint64_t, std::vector<lldb::SBValue>> VariableRefs;
    uint64_t NextVariableRef = 1;
  };

  Session *findSession(const llvm::dtx::ns::Object &Request);
  const Session *findSession(const llvm::dtx::ns::Object &Request) const;
  uint64_t addVariableRef(Session &S, std::vector<lldb::SBValue> Values);

  bool Initialized_ = false;
  uint64_t NextSessionId_ = 1;
  std::map<std::string, Session> Sessions_;
};

} // namespace ycode::debughost
