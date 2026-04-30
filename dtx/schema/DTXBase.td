// Minimal DTX schema vocabulary.
//
// This file documents the IDL surface consumed by tools/dtx-codegen. The MVP
// parser accepts the subset used by DebugHostCore.td; a later LLVM-integrated
// implementation can replace it with real TableGen records without changing
// service names, selectors, or generated output contracts.

// DTXService<DisplayName, ChannelIdentifier, Version>
// DTXMethod<Name, [DTXArg<Name, Type>...], ReturnType, OptionalSelector>

// Primitive type atoms used by the MVP generator:
//   DTXBool
//   DTXInt32
//   DTXUInt32
//   DTXInt64
//   DTXUInt64
//   DTXDouble
//   DTXString
//   DTXData
//   DTXObject<"Name">
//   DTXArray<Type>
//   DTXNullable<Type>
