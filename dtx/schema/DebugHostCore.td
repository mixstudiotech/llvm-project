include "DTXBase.td"

def Lifecycle : DTXService<"Lifecycle", "org.llvm.debughost.lifecycle", 1> {
  let Methods = [
    DTXMethod<"initialize", [DTXArg<"request", DTXObject<"InitializeRequest">>], DTXObject<"InitializeResult">, "initialize:">,
    DTXMethod<"shutdown", [DTXArg<"request", DTXObject<"ShutdownRequest">>], DTXObject<"ShutdownResult">, "shutdown:">
  ];
}

def Session : DTXService<"Session", "org.llvm.debughost.session", 1> {
  let Methods = [
    DTXMethod<"create", [DTXArg<"request", DTXObject<"SessionCreateRequest">>], DTXObject<"SessionCreateResult">, "create:">,
    DTXMethod<"close", [DTXArg<"request", DTXObject<"SessionCloseRequest">>], DTXObject<"SessionCloseResult">, "close:">
  ];
}

def Execution : DTXService<"Execution", "org.llvm.debughost.execution", 1> {
  let Methods = [
    DTXMethod<"launch", [DTXArg<"request", DTXObject<"LaunchRequest">>], DTXObject<"LaunchResult">, "launch:">,
    DTXMethod<"attach", [DTXArg<"request", DTXObject<"AttachRequest">>], DTXObject<"AttachResult">, "attach:">,
    DTXMethod<"continueExecution", [DTXArg<"request", DTXObject<"ContinueRequest">>], DTXObject<"ContinueResult">, "continue:">,
    DTXMethod<"pause", [DTXArg<"request", DTXObject<"PauseRequest">>], DTXObject<"PauseResult">, "pause:">
  ];
}

def Breakpoints : DTXService<"Breakpoints", "org.llvm.debughost.breakpoints", 1> {
  let Methods = [
    DTXMethod<"setSource", [DTXArg<"request", DTXObject<"SetSourceBreakpointsRequest">>], DTXObject<"SetSourceBreakpointsResult">, "setSource:">
  ];
}

def Threads : DTXService<"Threads", "org.llvm.debughost.threads", 1> {
  let Methods = [
    DTXMethod<"list", [DTXArg<"request", DTXObject<"ThreadsRequest">>], DTXObject<"ThreadsResult">, "threads:">,
    DTXMethod<"stackTrace", [DTXArg<"request", DTXObject<"StackTraceRequest">>], DTXObject<"StackTraceResult">, "stackTrace:">,
    DTXMethod<"scopes", [DTXArg<"request", DTXObject<"ScopesRequest">>], DTXObject<"ScopesResult">, "scopes:">,
    DTXMethod<"variables", [DTXArg<"request", DTXObject<"VariablesRequest">>], DTXObject<"VariablesResult">, "variables:">
  ];
}

def Expressions : DTXService<"Expressions", "org.llvm.debughost.expressions", 1> {
  let Methods = [
    DTXMethod<"evaluate", [DTXArg<"request", DTXObject<"EvaluateRequest">>], DTXObject<"EvaluateResult">, "evaluate:">
  ];
}

def Memory : DTXService<"Memory", "org.llvm.debughost.memory", 1> {
  let Methods = [
    DTXMethod<"read", [DTXArg<"request", DTXObject<"ReadMemoryRequest">>], DTXObject<"ReadMemoryResult">, "read:">,
    DTXMethod<"disassemble", [DTXArg<"request", DTXObject<"DisassembleRequest">>], DTXObject<"DisassembleResult">, "disassemble:">
  ];
}

def Device : DTXService<"Device", "org.llvm.debughost.device", 1> {
  let Methods = [
    DTXMethod<"list", [DTXArg<"request", DTXObject<"DeviceListRequest">>], DTXObject<"DeviceListResult">, "list:">,
    DTXMethod<"prepareDebug", [DTXArg<"request", DTXObject<"DevicePrepareDebugRequest">>], DTXObject<"DevicePrepareDebugResult">, "prepareDebug:">
  ];
}
