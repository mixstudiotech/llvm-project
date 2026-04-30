#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Arg {
  std::string Name;
  std::string Type;
};

struct Method {
  std::string Name;
  std::vector<Arg> Args;
  std::string ReturnType;
  std::string Selector;
};

struct Service {
  std::string DefName;
  std::string Name;
  std::string Channel;
  unsigned Version = 0;
  std::vector<Method> Methods;
};

struct Schema {
  std::vector<Service> Services;
  std::set<std::string> ObjectTypes;
};

std::string readFile(const std::filesystem::path &Path) {
  std::ifstream In(Path, std::ios::binary);
  std::ostringstream OS;
  OS << In.rdbuf();
  return OS.str();
}

void writeFile(const std::filesystem::path &Path, const std::string &Text) {
  std::filesystem::create_directories(Path.parent_path());
  std::ofstream Out(Path, std::ios::binary);
  Out << Text;
}

std::string trim(std::string S) {
  auto NotSpace = [](unsigned char C) { return !std::isspace(C); };
  S.erase(S.begin(), std::find_if(S.begin(), S.end(), NotSpace));
  S.erase(std::find_if(S.rbegin(), S.rend(), NotSpace).base(), S.end());
  return S;
}

std::vector<std::string> splitTopLevel(const std::string &Text) {
  std::vector<std::string> Parts;
  int Angle = 0;
  int Square = 0;
  bool InString = false;
  size_t Start = 0;
  for (size_t I = 0; I < Text.size(); ++I) {
    char C = Text[I];
    if (C == '"' && (I == 0 || Text[I - 1] != '\\'))
      InString = !InString;
    if (InString)
      continue;
    if (C == '<')
      ++Angle;
    else if (C == '>')
      --Angle;
    else if (C == '[')
      ++Square;
    else if (C == ']')
      --Square;
    else if (C == ',' && Angle == 0 && Square == 0) {
      Parts.push_back(trim(Text.substr(Start, I - Start)));
      Start = I + 1;
    }
  }
  Parts.push_back(trim(Text.substr(Start)));
  return Parts;
}

std::string unquote(const std::string &S) {
  std::string T = trim(S);
  if (T.size() >= 2 && T.front() == '"' && T.back() == '"')
    return T.substr(1, T.size() - 2);
  return T;
}

std::string defaultSelector(const Method &M) {
  if (M.Args.empty())
    return M.Name;
  if (M.Args.size() == 1)
    return M.Name + ":";
  std::string S = M.Name + ":";
  for (size_t I = 1; I < M.Args.size(); ++I)
    S += M.Args[I].Name + ":";
  return S;
}

void collectObjectTypes(const std::string &Type, std::set<std::string> &Out) {
  static const std::regex ObjectRe(R"re(DTXObject<\s*"([^"]+)"\s*>)re");
  auto Begin = std::sregex_iterator(Type.begin(), Type.end(), ObjectRe);
  auto End = std::sregex_iterator();
  for (auto It = Begin; It != End; ++It)
    Out.insert((*It)[1].str());
}

std::vector<Arg> parseArgs(std::string Text) {
  Text = trim(Text);
  if (Text == "[]")
    return {};
  if (Text.size() < 2 || Text.front() != '[' || Text.back() != ']')
    throw std::runtime_error("expected argument list");
  Text = Text.substr(1, Text.size() - 2);
  std::vector<Arg> Args;
  for (const std::string &Part : splitTopLevel(Text)) {
    if (Part.empty())
      continue;
    static const std::regex ArgRe(R"re(DTXArg<\s*"([^"]+)"\s*,\s*(.+)\s*>)re");
    std::smatch M;
    if (!std::regex_match(Part, M, ArgRe))
      throw std::runtime_error("bad DTXArg: " + Part);
    Args.push_back({M[1].str(), trim(M[2].str())});
  }
  return Args;
}

std::string extractTemplateBody(const std::string &Text, size_t NamePos) {
  size_t Open = Text.find('<', NamePos);
  if (Open == std::string::npos)
    throw std::runtime_error("missing template open");
  int Depth = 0;
  bool InString = false;
  for (size_t I = Open; I < Text.size(); ++I) {
    char C = Text[I];
    if (C == '"' && (I == 0 || Text[I - 1] != '\\'))
      InString = !InString;
    if (InString)
      continue;
    if (C == '<')
      ++Depth;
    else if (C == '>') {
      --Depth;
      if (Depth == 0)
        return Text.substr(Open + 1, I - Open - 1);
    }
  }
  throw std::runtime_error("unclosed template body");
}

Schema parseSchema(const std::string &Text) {
  Schema S;
  static const std::regex ServiceRe(
      R"re(def\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*DTXService<\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*([0-9]+)\s*>\s*\{)re");
  auto Begin = std::sregex_iterator(Text.begin(), Text.end(), ServiceRe);
  auto End = std::sregex_iterator();
  for (auto It = Begin; It != End; ++It) {
    std::smatch Match = *It;
    Service Service;
    Service.DefName = Match[1].str();
    Service.Name = Match[2].str();
    Service.Channel = Match[3].str();
    Service.Version = static_cast<unsigned>(std::stoul(Match[4].str()));

    size_t BodyStart = Match.position() + Match.length();
    size_t BodyEnd = Text.find("\n}", BodyStart);
    if (BodyEnd == std::string::npos)
      throw std::runtime_error("service missing closing brace: " +
                               Service.DefName);
    std::string Body = Text.substr(BodyStart, BodyEnd - BodyStart);
    size_t Pos = 0;
    while ((Pos = Body.find("DTXMethod", Pos)) != std::string::npos) {
      const std::string Inner = extractTemplateBody(Body, Pos);
      std::vector<std::string> Fields = splitTopLevel(Inner);
      if (Fields.size() < 3 || Fields.size() > 4)
        throw std::runtime_error("DTXMethod requires 3 or 4 fields");
      Method Method;
      Method.Name = unquote(Fields[0]);
      Method.Args = parseArgs(Fields[1]);
      Method.ReturnType = trim(Fields[2]);
      Method.Selector = Fields.size() == 4 ? unquote(Fields[3]) : defaultSelector(Method);
      collectObjectTypes(Method.ReturnType, S.ObjectTypes);
      for (const Arg &A : Method.Args)
        collectObjectTypes(A.Type, S.ObjectTypes);
      Service.Methods.push_back(std::move(Method));
      Pos += 9;
    }
    S.Services.push_back(std::move(Service));
  }
  return S;
}

std::string cppType(const std::string &Type) {
  const std::string T = trim(Type);
  if (T == "DTXBool")
    return "bool";
  if (T == "DTXInt32")
    return "int32_t";
  if (T == "DTXUInt32")
    return "uint32_t";
  if (T == "DTXInt64")
    return "int64_t";
  if (T == "DTXUInt64")
    return "uint64_t";
  if (T == "DTXDouble")
    return "double";
  if (T == "DTXString")
    return "std::string";
  if (T == "DTXData")
    return "std::vector<uint8_t>";
  static const std::regex ObjectRe(R"re(DTXObject<\s*"([^"]+)"\s*>)re");
  std::smatch M;
  if (std::regex_match(T, M, ObjectRe))
    return M[1].str();
  static const std::regex ArrayRe(R"(DTXArray<\s*(.+)\s*>)");
  if (std::regex_match(T, M, ArrayRe))
    return "std::vector<" + cppType(M[1].str()) + ">";
  static const std::regex NullableRe(R"(DTXNullable<\s*(.+)\s*>)");
  if (std::regex_match(T, M, NullableRe))
    return "std::optional<" + cppType(M[1].str()) + ">";
  return "llvm::dtx::ns::Object";
}

std::string paramDecl(const Arg &A) {
  std::string Ty = cppType(A.Type);
  if (Ty == "std::string" || Ty.find("std::vector") == 0 ||
      Ty.find("std::optional") == 0 ||
      (Ty.size() > 0 && std::isupper(static_cast<unsigned char>(Ty[0]))))
    return "const " + Ty + " &" + A.Name;
  return Ty + " " + A.Name;
}

std::string objectToNSObjectExpr(const std::string &Name, const std::string &Type) {
  if (Type.rfind("DTXObject<", 0) == 0)
    return Name + ".toNSObject()";
  return Name;
}

bool isObjectType(const std::string &Type) {
  return trim(Type).rfind("DTXObject<", 0) == 0;
}

void emitCpp(const Schema &Schema, const std::filesystem::path &OutDir,
             const std::string &InputName) {
  std::ostringstream Types;
  Types << "// Generated from " << InputName << ". Do not edit.\n";
  Types << "#pragma once\n\n";
  Types << "#include \"dtx/NSObject.h\"\n";
  Types << "#include \"dtx/Errors.h\"\n\n";
  Types << "#include <cstdint>\n";
  Types << "#include <optional>\n";
  Types << "#include <string>\n";
  Types << "#include <vector>\n\n";
  Types << "namespace llvm::dtx::debughost {\n\n";
  for (const std::string &Name : Schema.ObjectTypes) {
    Types << "struct " << Name << " {\n";
    Types << "  llvm::dtx::ns::Object Raw;\n";
    Types << "  llvm::dtx::ns::Object toNSObject() const { return Raw; }\n";
    Types << "  static llvm::dtx::Expected<" << Name
          << "> fromNSObject(const llvm::dtx::ns::Object &Object) {\n";
    Types << "    " << Name << " Value;\n";
    Types << "    Value.Raw = Object;\n";
    Types << "    return Value;\n";
    Types << "  }\n";
    Types << "};\n\n";
  }
  Types << "} // namespace llvm::dtx::debughost\n";

  std::ostringstream Client;
  Client << "// Generated from " << InputName << ". Do not edit.\n";
  Client << "#pragma once\n\n";
  Client << "#include \"dtx/CodegenSupport.h\"\n";
  Client << "#include \"DebugHostTypes.h.inc\"\n\n";
  Client << "namespace llvm::dtx::debughost {\n\n";
  for (const Service &Svc : Schema.Services) {
    Client << "class " << Svc.Name << "Client {\n";
    Client << "public:\n";
    Client << "  static constexpr const char *ChannelName = \"" << Svc.Channel
           << "\";\n";
    Client << "  static constexpr uint32_t ChannelId = "
           << (&Svc - Schema.Services.data() + 1) << ";\n";
    Client << "  static constexpr unsigned Version = " << Svc.Version << ";\n";
    Client << "  " << Svc.Name
           << "Client(llvm::dtx::Connection &Connection, uint32_t Channel)\n";
    Client << "      : Connection_(Connection), Channel_(Channel) {}\n\n";
    for (const Method &M : Svc.Methods) {
      Client << "  llvm::dtx::Expected<" << cppType(M.ReturnType) << "> "
             << M.Name << "(";
      for (size_t I = 0; I < M.Args.size(); ++I) {
        if (I)
          Client << ", ";
        Client << paramDecl(M.Args[I]);
      }
      Client << ") {\n";
      Client << "    llvm::dtx::ArgWriter Args;\n";
      for (const Arg &A : M.Args)
        Client << "    Args.write(" << objectToNSObjectExpr(A.Name, A.Type)
               << ");\n";
      Client << "    auto Reply = Connection_.call(Channel_, \"" << M.Selector
             << "\", Args.aux(), true);\n";
      Client << "    if (!Reply)\n";
      Client << "      return Reply.error();\n";
      if (M.ReturnType.rfind("DTXObject<", 0) == 0)
        Client << "    return " << cppType(M.ReturnType)
               << "::fromNSObject(Reply.get());\n";
      else
        Client << "    return llvm::dtx::Error(\"primitive return decoding is not implemented yet\");\n";
      Client << "  }\n\n";
    }
    Client << "private:\n";
    Client << "  llvm::dtx::Connection &Connection_;\n";
    Client << "  uint32_t Channel_;\n";
    Client << "};\n\n";
  }
  Client << "} // namespace llvm::dtx::debughost\n";

  std::ostringstream ServiceOut;
  ServiceOut << "// Generated from " << InputName << ". Do not edit.\n";
  ServiceOut << "#pragma once\n\n";
  ServiceOut << "#include \"dtx/CodegenSupport.h\"\n";
  ServiceOut << "#include \"DebugHostTypes.h.inc\"\n\n";
  ServiceOut << "namespace llvm::dtx::debughost {\n\n";
  for (const Service &Svc : Schema.Services) {
    ServiceOut << "class I" << Svc.Name << "Processor {\n";
    ServiceOut << "public:\n";
    ServiceOut << "  virtual ~I" << Svc.Name << "Processor() = default;\n";
    for (const Method &M : Svc.Methods) {
      ServiceOut << "  virtual llvm::dtx::Expected<" << cppType(M.ReturnType)
                 << "> " << M.Name << "(";
      for (size_t I = 0; I < M.Args.size(); ++I) {
        if (I)
          ServiceOut << ", ";
        ServiceOut << paramDecl(M.Args[I]);
      }
      ServiceOut << ") = 0;\n";
    }
    ServiceOut << "};\n\n";
    ServiceOut << "struct " << Svc.Name << "ServiceDesc {\n";
    ServiceOut << "  static constexpr const char *Name = \"" << Svc.Name
               << "\";\n";
    ServiceOut << "  static constexpr const char *Channel = \"" << Svc.Channel
               << "\";\n";
    ServiceOut << "  static constexpr unsigned Version = " << Svc.Version
               << ";\n";
    ServiceOut << "  struct MethodDesc { const char *Name; const char *Selector; };\n";
    ServiceOut << "  static constexpr MethodDesc Methods[] = {\n";
    for (const Method &M : Svc.Methods)
      ServiceOut << "    {\"" << M.Name << "\", \"" << M.Selector << "\"},\n";
    ServiceOut << "  };\n";
    ServiceOut << "  static llvm::dtx::Expected<llvm::dtx::ns::Object> dispatch(I"
               << Svc.Name << "Processor &Processor, const std::string &Selector, const llvm::dtx::AuxList &Args) {\n";
    ServiceOut << "    llvm::dtx::ArgReader Reader(Args);\n";
    for (const Method &M : Svc.Methods) {
      ServiceOut << "    if (Selector == \"" << M.Selector << "\") {\n";
      ServiceOut << "      if (Reader.size() != " << M.Args.size() << ")\n";
      ServiceOut << "        return llvm::dtx::Error(\"wrong argument count for "
                 << Svc.Name << "." << M.Name << "\");\n";
      for (size_t I = 0; I < M.Args.size(); ++I) {
        const Arg &A = M.Args[I];
        if (!isObjectType(A.Type)) {
          ServiceOut << "      return llvm::dtx::Error(\"primitive dispatch arguments are not implemented yet\");\n";
          continue;
        }
        ServiceOut << "      auto RawArg" << I << " = Reader.readObject(" << I
                   << ");\n";
        ServiceOut << "      if (!RawArg" << I << ")\n";
        ServiceOut << "        return RawArg" << I << ".error();\n";
        ServiceOut << "      auto Arg" << I << " = " << cppType(A.Type)
                   << "::fromNSObject(RawArg" << I << ".get());\n";
        ServiceOut << "      if (!Arg" << I << ")\n";
        ServiceOut << "        return Arg" << I << ".error();\n";
      }
      ServiceOut << "      auto Result = Processor." << M.Name << "(";
      for (size_t I = 0; I < M.Args.size(); ++I) {
        if (I)
          ServiceOut << ", ";
        ServiceOut << "Arg" << I << ".get()";
      }
      ServiceOut << ");\n";
      ServiceOut << "      if (!Result)\n";
      ServiceOut << "        return Result.error();\n";
      if (isObjectType(M.ReturnType))
        ServiceOut << "      return Result.get().toNSObject();\n";
      else
        ServiceOut << "      return llvm::dtx::Error(\"primitive dispatch return values are not implemented yet\");\n";
      ServiceOut << "    }\n";
    }
    ServiceOut << "    return llvm::dtx::Error(\"unknown selector for " << Svc.Name
               << "\");\n";
    ServiceOut << "  }\n";
    ServiceOut << "};\n\n";
  }
  ServiceOut << "} // namespace llvm::dtx::debughost\n";

  std::ostringstream Server;
  Server << "// Generated from " << InputName << ". Do not edit.\n";
  Server << "#pragma once\n\n";
  Server << "#include \"DebugHostService.h.inc\"\n\n";
  Server << "namespace llvm::dtx::debughost {\n\n";
  Server << "class DebugHostServer {\n";
  Server << "public:\n";
  for (const Service &Svc : Schema.Services) {
    Server << "  void set" << Svc.Name << "Processor(I" << Svc.Name
           << "Processor *Processor) { " << Svc.Name
           << "Processor_ = Processor; }\n";
  }
  Server << "\n";
  Server << "  llvm::dtx::Expected<llvm::dtx::ns::Object> dispatch(uint32_t Channel, const std::string &Selector, const llvm::dtx::AuxList &Args) {\n";
  for (size_t I = 0; I < Schema.Services.size(); ++I) {
    const Service &Svc = Schema.Services[I];
    Server << "    if (Channel == " << (I + 1) << ") {\n";
    Server << "      if (!" << Svc.Name << "Processor_)\n";
    Server << "        return llvm::dtx::Error(\"no processor registered for "
           << Svc.Name << "\");\n";
    Server << "      return " << Svc.Name << "ServiceDesc::dispatch(*"
           << Svc.Name << "Processor_, Selector, Args);\n";
    Server << "    }\n";
  }
  Server << "    return llvm::dtx::Error(\"unknown DebugHost channel id\");\n";
  Server << "  }\n\n";
  Server << "private:\n";
  for (const Service &Svc : Schema.Services)
    Server << "  I" << Svc.Name << "Processor *" << Svc.Name
           << "Processor_ = nullptr;\n";
  Server << "};\n\n";
  Server << "} // namespace llvm::dtx::debughost\n";

  writeFile(OutDir / "DebugHostTypes.h.inc", Types.str());
  writeFile(OutDir / "DebugHostClient.h.inc", Client.str());
  writeFile(OutDir / "DebugHostService.h.inc", ServiceOut.str());
  writeFile(OutDir / "DebugHostServer.h.inc", Server.str());
}

std::string csharpType(const std::string &Type) {
  const std::string T = trim(Type);
  if (T == "DTXBool")
    return "bool";
  if (T == "DTXInt32")
    return "int";
  if (T == "DTXUInt32")
    return "uint";
  if (T == "DTXInt64")
    return "long";
  if (T == "DTXUInt64")
    return "ulong";
  if (T == "DTXDouble")
    return "double";
  if (T == "DTXString")
    return "string";
  if (T == "DTXData")
    return "byte[]";
  static const std::regex ObjectRe(R"re(DTXObject<\s*"([^"]+)"\s*>)re");
  std::smatch M;
  if (std::regex_match(T, M, ObjectRe))
    return M[1].str();
  static const std::regex ArrayRe(R"(DTXArray<\s*(.+)\s*>)");
  if (std::regex_match(T, M, ArrayRe))
    return "IReadOnlyList<" + csharpType(M[1].str()) + ">";
  static const std::regex NullableRe(R"(DTXNullable<\s*(.+)\s*>)");
  if (std::regex_match(T, M, NullableRe))
    return csharpType(M[1].str()) + "?";
  return "NSObject";
}

std::string csharpParamDecl(const Arg &A) {
  return csharpType(A.Type) + " " + A.Name;
}

std::string csharpMethodName(const std::string &Name) {
  if (Name.empty())
    return Name;
  std::string Result = Name;
  Result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(Result[0])));
  return Result;
}

std::string csharpToNSObjectExpr(const std::string &Name, const std::string &Type) {
  if (isObjectType(Type))
    return Name + ".ToNSObject()";
  return "NSObject.From(" + Name + ")";
}

void emitCSharp(const Schema &Schema, const std::filesystem::path &OutDir,
                const std::string &InputName) {
  std::ostringstream Types;
  Types << "// Generated from " << InputName << ". Do not edit.\n";
  Types << "#nullable enable\n\n";
  Types << "using System.Collections.Generic;\n";
  Types << "using LLVM.DTX;\n\n";
  Types << "namespace LLVM.DTX.DebugHost;\n\n";
  for (const std::string &Name : Schema.ObjectTypes) {
    Types << "public sealed partial class " << Name << "\n";
    Types << "{\n";
    Types << "    public NSObject Raw { get; set; } = NSObject.Null;\n";
    Types << "    public NSObject ToNSObject() => Raw;\n";
    Types << "    public static " << Name << " FromNSObject(NSObject value) => new() { Raw = value };\n";
    Types << "}\n\n";
  }

  std::ostringstream Client;
  Client << "// Generated from " << InputName << ". Do not edit.\n";
  Client << "#nullable enable\n\n";
  Client << "using System.Threading;\n";
  Client << "using System.Threading.Tasks;\n";
  Client << "using LLVM.DTX;\n\n";
  Client << "namespace LLVM.DTX.DebugHost;\n\n";
  for (size_t SvcIndex = 0; SvcIndex < Schema.Services.size(); ++SvcIndex) {
    const Service &Svc = Schema.Services[SvcIndex];
    Client << "public sealed partial class " << Svc.Name << "Client\n";
    Client << "{\n";
    Client << "    public const string ChannelName = \"" << Svc.Channel << "\";\n";
    Client << "    public const uint ChannelId = " << (SvcIndex + 1) << ";\n";
    Client << "    public const uint Version = " << Svc.Version << ";\n\n";
    Client << "    private readonly Connection _connection;\n";
    Client << "    private readonly uint _channel;\n\n";
    Client << "    public " << Svc.Name << "Client(Connection connection, uint channel = ChannelId)\n";
    Client << "    {\n";
    Client << "        _connection = connection;\n";
    Client << "        _channel = channel;\n";
    Client << "    }\n\n";
    for (const Method &M : Svc.Methods) {
      Client << "    public async Task<" << csharpType(M.ReturnType) << "> "
             << csharpMethodName(M.Name) << "Async(";
      for (size_t I = 0; I < M.Args.size(); ++I) {
        if (I)
          Client << ", ";
        Client << csharpParamDecl(M.Args[I]);
      }
      Client << ", CancellationToken cancellationToken = default)\n";
      Client << "    {\n";
      Client << "        var args = new AuxList();\n";
      for (const Arg &A : M.Args)
        Client << "        args.Add(Arg.Object(" << csharpToNSObjectExpr(A.Name, A.Type)
               << "));\n";
      Client << "        var reply = await _connection.CallAsync(_channel, \""
             << M.Selector << "\", args, true, cancellationToken).ConfigureAwait(false);\n";
      if (isObjectType(M.ReturnType))
        Client << "        return " << csharpType(M.ReturnType)
               << ".FromNSObject(reply);\n";
      else
        Client << "        throw new DtxException(\"Primitive return decoding is not implemented yet.\");\n";
      Client << "    }\n\n";
    }
    Client << "}\n\n";
  }

  writeFile(OutDir / "DebugHostTypes.g.cs", Types.str());
  writeFile(OutDir / "DebugHostClient.g.cs", Client.str());
}

std::string kotlinType(const std::string &Type) {
  const std::string T = trim(Type);
  if (T == "DTXBool")
    return "Boolean";
  if (T == "DTXInt32")
    return "Int";
  if (T == "DTXUInt32")
    return "Int";
  if (T == "DTXInt64")
    return "Long";
  if (T == "DTXUInt64")
    return "Long";
  if (T == "DTXDouble")
    return "Double";
  if (T == "DTXString")
    return "String";
  if (T == "DTXData")
    return "ByteArray";
  static const std::regex ObjectRe(R"re(DTXObject<\s*"([^"]+)"\s*>)re");
  std::smatch M;
  if (std::regex_match(T, M, ObjectRe))
    return M[1].str();
  static const std::regex ArrayRe(R"(DTXArray<\s*(.+)\s*>)");
  if (std::regex_match(T, M, ArrayRe))
    return "List<" + kotlinType(M[1].str()) + ">";
  static const std::regex NullableRe(R"(DTXNullable<\s*(.+)\s*>)");
  if (std::regex_match(T, M, NullableRe))
    return kotlinType(M[1].str()) + "?";
  return "NSObject";
}

std::string kotlinParamDecl(const Arg &A) {
  return A.Name + ": " + kotlinType(A.Type);
}

std::string kotlinToNSObjectExpr(const std::string &Name, const std::string &Type) {
  if (isObjectType(Type))
    return Name + ".toNSObject()";
  return "NSObject.from(" + Name + ")";
}

void emitKotlin(const Schema &Schema, const std::filesystem::path &OutDir,
                const std::string &InputName) {
  std::ostringstream Types;
  Types << "// Generated from " << InputName << ". Do not edit.\n";
  Types << "package org.llvm.dtx.debughost\n\n";
  Types << "import org.llvm.dtx.NSObject\n\n";
  for (const std::string &Name : Schema.ObjectTypes) {
    Types << "class " << Name << "(val raw: NSObject = NSObject.Null) {\n";
    Types << "    fun toNSObject(): NSObject = raw\n\n";
    Types << "    companion object {\n";
    Types << "        fun fromNSObject(value: NSObject): " << Name
          << " = " << Name << "(value)\n";
    Types << "    }\n";
    Types << "}\n\n";
  }

  std::ostringstream Client;
  Client << "// Generated from " << InputName << ". Do not edit.\n";
  Client << "package org.llvm.dtx.debughost\n\n";
  Client << "import org.llvm.dtx.Arg\n";
  Client << "import org.llvm.dtx.AuxList\n";
  Client << "import org.llvm.dtx.Connection\n";
  Client << "import org.llvm.dtx.DtxException\n";
  Client << "import org.llvm.dtx.NSObject\n\n";
  for (size_t SvcIndex = 0; SvcIndex < Schema.Services.size(); ++SvcIndex) {
    const Service &Svc = Schema.Services[SvcIndex];
    Client << "class " << Svc.Name
           << "Client(private val connection: Connection, private val channel: Int = CHANNEL_ID) {\n";
    Client << "    companion object {\n";
    Client << "        const val CHANNEL_NAME: String = \"" << Svc.Channel
           << "\"\n";
    Client << "        const val CHANNEL_ID: Int = " << (SvcIndex + 1)
           << "\n";
    Client << "        const val VERSION: Int = " << Svc.Version << "\n";
    Client << "    }\n\n";
    for (const Method &M : Svc.Methods) {
      Client << "    fun " << M.Name << "(";
      for (size_t I = 0; I < M.Args.size(); ++I) {
        if (I)
          Client << ", ";
        Client << kotlinParamDecl(M.Args[I]);
      }
      Client << "): " << kotlinType(M.ReturnType) << " {\n";
      Client << "        val args = AuxList()\n";
      for (const Arg &A : M.Args)
        Client << "        args.add(Arg.obj("
               << kotlinToNSObjectExpr(A.Name, A.Type) << "))\n";
      Client << "        val reply = connection.call(channel, \"" << M.Selector
             << "\", args, true)\n";
      if (isObjectType(M.ReturnType))
        Client << "        return " << kotlinType(M.ReturnType)
               << ".fromNSObject(reply)\n";
      else
        Client << "        throw DtxException(\"Primitive return decoding is not implemented yet.\")\n";
      Client << "    }\n\n";
    }
    Client << "}\n\n";
  }

  writeFile(OutDir / "DebugHostTypes.kt", Types.str());
  writeFile(OutDir / "DebugHostClient.kt", Client.str());
}

struct Options {
  std::filesystem::path Input;
  std::filesystem::path OutDir;
  std::string Lang = "cpp";
};

Options parseOptions(int argc, char **argv) {
  Options Opts;
  for (int I = 1; I < argc; ++I) {
    std::string Argv = argv[I];
    auto NeedValue = [&](const std::string &Name) -> std::string {
      if (I + 1 >= argc)
        throw std::runtime_error(Name + " requires a value");
      return argv[++I];
    };
    if (Argv == "--input")
      Opts.Input = NeedValue(Argv);
    else if (Argv == "--out-dir")
      Opts.OutDir = NeedValue(Argv);
    else if (Argv == "--lang")
      Opts.Lang = NeedValue(Argv);
    else if (Argv.rfind("--input=", 0) == 0)
      Opts.Input = Argv.substr(8);
    else if (Argv.rfind("--out-dir=", 0) == 0)
      Opts.OutDir = Argv.substr(10);
    else if (Argv.rfind("--lang=", 0) == 0)
      Opts.Lang = Argv.substr(7);
    else
      throw std::runtime_error("unknown argument: " + Argv);
  }
  if (Opts.Input.empty())
    throw std::runtime_error("--input is required");
  if (Opts.OutDir.empty())
    throw std::runtime_error("--out-dir is required");
  return Opts;
}

} // namespace

int main(int argc, char **argv) {
  try {
    Options Opts = parseOptions(argc, argv);
    Schema Parsed = parseSchema(readFile(Opts.Input));
    if (Parsed.Services.empty())
      throw std::runtime_error("no DTXService definitions found");
    if (Opts.Lang == "cpp")
      emitCpp(Parsed, Opts.OutDir, Opts.Input.filename().string());
    else if (Opts.Lang == "csharp")
      emitCSharp(Parsed, Opts.OutDir, Opts.Input.filename().string());
    else if (Opts.Lang == "kotlin")
      emitKotlin(Parsed, Opts.OutDir, Opts.Input.filename().string());
    else
      throw std::runtime_error("unsupported --lang=" + Opts.Lang);
    return 0;
  } catch (const std::exception &Ex) {
    std::cerr << "dtx-codegen: " << Ex.what() << "\n";
    return 1;
  }
}
