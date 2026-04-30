include "DTXBase.td"

def Events : DTXService<"Events", "org.llvm.debughost.events", 1> {
  let Methods = [
    DTXMethod<"initialized", [DTXArg<"event", DTXObject<"InitializedEvent">>], DTXObject<"EventAck">, "initialized:">,
    DTXMethod<"stopped", [DTXArg<"event", DTXObject<"StoppedEvent">>], DTXObject<"EventAck">, "stopped:">,
    DTXMethod<"output", [DTXArg<"event", DTXObject<"OutputEvent">>], DTXObject<"EventAck">, "output:">,
    DTXMethod<"diagnostic", [DTXArg<"event", DTXObject<"DiagnosticEvent">>], DTXObject<"EventAck">, "diagnostic:">
  ];
}
