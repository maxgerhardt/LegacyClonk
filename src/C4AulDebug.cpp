/*
 * LegacyClonk
 *
 * Copyright (c) 2017-2026, The LegacyClonk Team and contributors
 *
 * Distributed under the terms of the ISC license; see accompanying file
 * "COPYING" for details.
 *
 * "Clonk" is a registered trademark of Matthes Bender, used with permission.
 * See accompanying file "TRADEMARK" for details.
 *
 * To redistribute this file separately, substitute the full license texts
 * for the above references.
 */

#include <C4AulDebug.h>

#include <C4Application.h>
#include <C4Aul.h>
#include <C4ControlServer.h>
#include <C4Game.h>
#include <C4Log.h>
#include <C4NetIO.h>
#include <C4Object.h>
#include <C4ValueHash.h>
#include <C4Wrappers.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#endif

// C4AulExec.cpp: the interpreter's context stack (0 = innermost)
int AulExecContextCount();
C4AulScriptContext *AulExecContext(int index);
C4Value AulExecDebugEval(C4AulScript &script, C4Object *obj, const char *expression, std::string &error);

namespace
{
	bool WouldBlock()
	{
#ifdef _WIN32
		return WSAGetLastError() == WSAEWOULDBLOCK;
#else
		return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
	}

	template<typename S>
	void SetNonBlocking(const S sock)
	{
#ifdef _WIN32
		u_long nonBlocking = 1;
		ioctlsocket(static_cast<SOCKET>(sock), FIONBIO, &nonBlocking);
#else
		fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif
	}

	// tabs and newlines would break the record format
	std::string Clean(std::string_view text, std::size_t maxLength = 1000)
	{
		std::string out{text.substr(0, maxLength)};
		std::replace_if(out.begin(), out.end(), [](const char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
		if (text.size() > maxLength) out += "...";
		return out;
	}

	bool ObjectAlive(C4Object *const obj)
	{
		return obj && (Game.Objects.ObjectNumber(obj) || Game.Objects.InactiveObjects.ObjectNumber(obj));
	}

	// a value's children: array elements, map entries, an object's named locals
	struct Child
	{
		std::string name;
		C4Value value;
	};

	std::vector<Child> Children(const C4Value &value)
	{
		std::vector<Child> out;
		const C4Value &v{value.GetRefVal()};
		switch (v.GetType())
		{
		case C4V_Array:
			for (int32_t i = 0; i < v._getArray()->GetSize(); ++i)
				out.push_back({std::format("[{}]", i), v._getArray()->GetItem(i)});
			break;
		case C4V_Map:
			for (auto [key, val] : *v._getMap())
				out.push_back({key.GetDataString(), val});
			break;
		case C4V_C4Object:
		{
			C4Object *const obj{v._getObj()};
			if (!ObjectAlive(obj) || !obj->LocalNamed.pNames) break;
			for (int32_t i = 0; i < obj->LocalNamed.pNames->iSize; ++i)
				out.push_back({obj->LocalNamed.pNames->pNames[i], *obj->LocalNamed.GetItem(i)});
			break;
		}
		default:
			break;
		}
		return out;
	}

	std::size_t ChildCount(const C4Value &value)
	{
		const C4Value &v{value.GetRefVal()};
		switch (v.GetType())
		{
		case C4V_Array: return static_cast<std::size_t>(v._getArray()->GetSize());
		case C4V_Map: return v._getMap()->size();
		case C4V_C4Object:
			return ObjectAlive(v._getObj()) && v._getObj()->LocalNamed.pNames ? static_cast<std::size_t>(v._getObj()->LocalNamed.pNames->iSize) : 0;
		default: return 0;
		}
	}

	std::string Describe(const C4Value &value)
	{
		const C4Value &v{value.GetRefVal()};
		std::string text{v.GetDataString()};
		if (v.GetType() == C4V_C4Object && ObjectAlive(v._getObj()))
			text += std::format(" ({})", C4IdText(v._getObj()->Def->id));
		return std::format("{}\t{}\t{}", v.GetType() == C4V_Any ? "nil" : v.GetTypeName(), Clean(text, 300), ChildCount(v));
	}

	bool IsIdentifier(const std::string &text)
	{
		if (text.empty() || std::isdigit(static_cast<unsigned char>(text[0]))) return false;
		return std::all_of(text.begin(), text.end(), [](const char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
	}
}

C4AulDebug &C4AulDebug::Get()
{
	static C4AulDebug debugger;
	return debugger;
}

bool C4AulDebug::Listen()
{
#ifdef _WIN32
	if (!AcquireWinSock()) return false;
#endif
	const auto sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (static_cast<Socket>(sock) == InvalidSocket) return false;
	int reuse = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(static_cast<std::uint16_t>(port));
	if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(sock, 1) != 0)
	{
		closesocket(sock);
		return false;
	}
	SetNonBlocking(sock);
	listenSocket = static_cast<Socket>(sock);
	LogNTr("Script debugger listening on 127.0.0.1:{}", port);
	return true;
}

void C4AulDebug::Stop()
{
	CloseClient();
	if (listenSocket != InvalidSocket)
	{
		closesocket(listenSocket);
		listenSocket = InvalidSocket;
#ifdef _WIN32
		ReleaseWinSock();
#endif
	}
}

void C4AulDebug::Poll()
{
	if (!IsEnabled() || listenFailed) return;
	if (listenSocket == InvalidSocket && !Listen())
	{
		LogNTr(spdlog::level::err, "Script debugger: could not listen on 127.0.0.1:{}", port);
		listenFailed = true;
		return;
	}
	if (clientSocket == InvalidSocket) AcceptClient();
	if (clientSocket == InvalidSocket) return;
	ReadClient();
	HandleRequests();
	FlushOutput();
}

void C4AulDebug::WaitForClient()
{
	if (!IsEnabled() || !waitForClient) return;
	waitForClient = false;
	Poll();
	if (listenFailed) return;
	LogNTr("Script debugger: waiting for a client to send \"start\"");
	started = false;
	while (!started && !Application.fQuitMsgReceived)
	{
		Application.HandleMessage(20, false);
		Poll();
	}
}

void C4AulDebug::AcceptClient()
{
	const auto sock = accept(listenSocket, nullptr, nullptr);
	if (static_cast<Socket>(sock) == InvalidSocket) return;
	SetNonBlocking(sock);
	clientSocket = static_cast<Socket>(sock);
	inBuffer.clear();
	outBuffer.clear();
	breakpoints.clear();
	scriptBreakpoints.clear();
	lineTables.clear();
	mode = Mode::Run;
	breakOnErrors = false;
	lastCallId = 0;
	lastError = nullptr;
	active = true;
}

void C4AulDebug::CloseClient()
{
	if (clientSocket != InvalidSocket)
	{
		closesocket(clientSocket);
		clientSocket = InvalidSocket;
	}
	inBuffer.clear();
	outBuffer.clear();
	Detach();
}

void C4AulDebug::Detach()
{
	breakpoints.clear();
	scriptBreakpoints.clear();
	mode = Mode::Run;
	breakOnErrors = false;
	active = clientSocket != InvalidSocket;
	resume = true;
	started = true;
}

void C4AulDebug::ReadClient()
{
	char buf[4096];
	for (;;)
	{
		const auto received = recv(clientSocket, buf, sizeof(buf), 0);
		if (received > 0)
		{
			inBuffer.append(buf, static_cast<std::size_t>(received));
			continue;
		}
		if (received < 0 && WouldBlock()) return;
		// closed or error
		CloseClient();
		return;
	}
}

void C4AulDebug::FlushOutput()
{
	while (clientSocket != InvalidSocket && !outBuffer.empty())
	{
		const auto sent = send(clientSocket, outBuffer.data(), static_cast<int>(outBuffer.size()), 0);
		if (sent > 0)
		{
			outBuffer.erase(0, static_cast<std::size_t>(sent));
			continue;
		}
		if (sent < 0 && WouldBlock()) return;
		CloseClient();
	}
}

void C4AulDebug::Respond(const bool ok, const std::string_view payload)
{
	outBuffer += std::format("{} {}\n", ok ? "OK" : "ERR", payload.size());
	outBuffer += payload;
}

void C4AulDebug::Event(const std::string_view payload)
{
	outBuffer += std::format("EVT {}\n", payload.size());
	outBuffer += payload;
}

void C4AulDebug::HandleRequests()
{
	while (clientSocket != InvalidSocket)
	{
		const auto newline = inBuffer.find('\n');
		if (newline == std::string::npos) return;
		const std::string_view header{inBuffer.data(), newline};
		const auto space = header.rfind(' ');
		std::size_t length = 0;
		if (space == std::string_view::npos ||
			std::from_chars(header.data() + space + 1, header.data() + header.size(), length).ec != std::errc{})
		{
			Respond(false, "malformed request header, expected \"<command> <payload length>\"");
			inBuffer.erase(0, newline + 1);
			continue;
		}
		if (inBuffer.size() < newline + 1 + length) return; // wait for the payload
		const std::string command{header.substr(0, space)};
		const std::string payload = inBuffer.substr(newline + 1, length);
		inBuffer.erase(0, newline + 1 + length);
		Handle(command, payload);
		// a resume ends the halt loop: the rest is handled after the script continues
		if (resume) return;
	}
}

std::string C4AulDebug::NormalizeName(const std::string_view name)
{
	std::string out{name};
	for (char &c : out)
		c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

class C4AulDebugScriptWalker
{
public:
	template<typename F>
	static void Walk(C4AulScript *script, F &f)
	{
		f(script);
		for (C4AulScript *child = script->Child0; child; child = child->Next)
			Walk(child, f);
	}

	static std::pair<C4AulBCC *, int> Code(C4AulScript *script) { return {script->Code, script->CodeSize}; }
};

namespace
{
	template<typename F>
	void ForEachScript(C4AulScript *script, F &&f)
	{
		C4AulDebugScriptWalker::Walk(script, f);
	}
}

C4AulScript *C4AulDebug::FindScript(const std::string_view name)
{
	const std::string wanted{NormalizeName(name)};
	C4AulScript *found{nullptr};
	ForEachScript(&Game.ScriptEngine, [&](C4AulScript *script)
	{
		if (!found && !script->Temporary && script->GetScript() && NormalizeName(script->ScriptName) == wanted)
			found = script;
	});
	return found;
}

int C4AulDebug::Line(const C4AulScript *const script, const char *const pos)
{
	const char *const text{script->GetScript()};
	if (!text || !pos) return 0;
	LineTable &table{lineTables[script]};
	if (table.text != text)
	{
		table.text = text;
		table.starts.assign(1, 0);
		for (const char *c = text; *c; ++c)
			if (*c == '\n') table.starts.push_back(static_cast<std::size_t>(c - text) + 1);
	}
	if (pos < text) return 0;
	const auto offset = static_cast<std::size_t>(pos - text);
	// C4ComponentHost::LoadAppend puts a newline before each loaded file: line 1 of the file is line 2 of the text
	const int fileOffset{*text == '\n' ? 1 : 0};
	return static_cast<int>(std::upper_bound(table.starts.begin(), table.starts.end(), offset) - table.starts.begin()) - fileOffset;
}

const std::set<int> *C4AulDebug::BreakpointsOf(const C4AulScript *const script)
{
	const auto it = scriptBreakpoints.find(script);
	if (it != scriptBreakpoints.end() && it->second.first == script->GetScript()) return it->second.second;
	const auto bp = breakpoints.find(NormalizeName(script->ScriptName));
	const std::set<int> *const lines{bp != breakpoints.end() && !bp->second.empty() ? &bp->second : nullptr};
	scriptBreakpoints[script] = {script->GetScript(), lines};
	return lines;
}

void C4AulDebug::OnStep(C4AulScriptContext *const ctx, C4AulBCC *const cpos, const int depth)
{
	if (halted || !ctx->Func || !cpos->SPos) return;
	const C4AulScript *const script{ctx->Func->pOrgScript};
	if (!script || script->Temporary) return;

	// pause requests stop anywhere
	if (mode == Mode::Pause)
	{
		HaltHere(ctx, cpos, depth, "pause", "");
		return;
	}

	const std::set<int> *const lines{BreakpointsOf(script)};
	if (mode == Mode::Run && !lines) return;

	const int line{Line(script, cpos->SPos)};
	// the same call is still on the line of the last stop (several byte codes per line)?
	const bool sameCall{ctx->CallId == lastCallId};
	const bool sameLine{sameCall && line == lastLine};
	if (sameCall && !sameLine) lastCallId = 0; // left that line: a loop coming back stops again

	switch (mode)
	{
	case Mode::StepIn:
		if (!sameLine) { HaltHere(ctx, cpos, depth, "step", ""); return; }
		break;
	case Mode::StepOver:
		// the next line of this call, or the caller (also the next script the engine calls when there's none)
		if (depth < lastDepth || (depth == lastDepth && !sameLine)) { HaltHere(ctx, cpos, depth, "step", ""); return; }
		break;
	case Mode::StepOut:
		if (depth < lastDepth || (depth == lastDepth && !sameCall && lastCallId != 0)) { HaltHere(ctx, cpos, depth, "step", ""); return; }
		break;
	default:
		break;
	}

	if (lines && !sameLine && lines->contains(line))
		HaltHere(ctx, cpos, depth, "breakpoint", "");
}

void C4AulDebug::OnError(C4AulScriptContext *const ctx, C4AulBCC *const cpos, const C4AulError &error)
{
	if (halted || !breakOnErrors || &error == lastError || !ctx || !ctx->Func) return;
	lastError = &error;
	HaltHere(ctx, cpos, AulExecContextCount(), "error", error.GetErrorMessage());
}

void C4AulDebug::HaltHere(C4AulScriptContext *const ctx, C4AulBCC *const cpos, const int depth, const std::string_view reason, const std::string_view text)
{
	halted = true;
	resume = false;
	halt = {ctx, cpos, depth};
	lastCallId = ctx->CallId;
	const C4AulScript *const script{ctx->Func ? ctx->Func->pOrgScript : nullptr};
	lastLine = script && cpos ? Line(script, cpos->SPos) : 0;
	lastDepth = depth;
	mode = Mode::Run;
	Event(std::format("stopped\t{}\t{}", reason, Clean(text)));
	FlushOutput();
	while (!resume && clientSocket != InvalidSocket && !Application.fQuitMsgReceived)
	{
		Application.HandleMessage(20, false);
		Poll();
		C4ControlServer::Get().Poll();
	}
	halted = false;
	evalResults.clear();
	if (clientSocket != InvalidSocket)
	{
		Event("continued");
		FlushOutput();
	}
}

C4AulScriptContext *C4AulDebug::Frame(const int index)
{
	if (!halted || index < 0) return nullptr;
	// the halted context is the innermost one; nested halts can't happen (OnStep returns while halted)
	return index < AulExecContextCount() ? AulExecContext(index) : nullptr;
}

C4AulBCC *C4AulDebug::FramePos(const int index)
{
	C4AulScriptContext *const ctx{Frame(index)};
	if (!ctx) return nullptr;
	return index == 0 ? halt.cpos : ctx->CPos;
}

std::string C4AulDebug::Stack()
{
	std::string out;
	for (int i = 0; C4AulScriptContext *ctx = Frame(i); ++i)
	{
		C4AulBCC *const pos{FramePos(i)};
		const C4AulScript *const script{ctx->Func ? ctx->Func->pOrgScript : nullptr};
		std::string name{ctx->Func && *ctx->Func->Name ? ctx->Func->GetFullName() : std::string{"(eval)"}};
		std::string where;
		if (ctx->Obj && ObjectAlive(ctx->Obj))
			where = std::format("{} #{} ({})", ctx->Obj->GetName(), ctx->Obj->Number, C4IdText(ctx->Obj->Def->id));
		else if (ctx->Def)
			where = std::format("definition {}", C4IdText(ctx->Def->id));
		out += std::format("{}\t{}\t{}\t{}\n", Clean(name), script ? Clean(script->ScriptName) : "",
			script && pos ? Line(script, pos->SPos ? pos->SPos : ctx->Func->Script) : 0, Clean(where));
	}
	return out;
}

std::string C4AulDebug::Vars(const int frame, const int scope, const std::vector<int> &path)
{
	C4AulScriptContext *const ctx{Frame(frame)};
	if (!ctx && scope != 3) return {};
	std::vector<Child> items;
	if (scope == 0)
	{
		C4AulScriptFunc *const func{ctx->Func};
		const int32_t namedPars{func->ParNamed.iSize};
		for (int32_t i = 0; i < static_cast<int32_t>(ctx->ParCnt()); ++i)
		{
			if (i < namedPars)
				items.push_back({func->ParNamed.pNames[i], ctx->Pars[i]});
			else if (ctx->Pars[i].GetType() != C4V_Any)
				items.push_back({std::format("Par({})", i), ctx->Pars[i]});
		}
		for (int32_t i = 0; i < func->VarNamed.iSize; ++i)
			items.push_back({func->VarNamed.pNames[i], ctx->Vars[i]});
	}
	else if (scope == 1)
	{
		if (ctx->Obj && ObjectAlive(ctx->Obj)) items = Children(C4VObj(ctx->Obj));
	}
	else if (scope == 2)
	{
		C4ValueMapData &globals{Game.ScriptEngine.GlobalNamed};
		if (globals.pNames)
			for (int32_t i = 0; i < globals.pNames->iSize; ++i)
				items.push_back({globals.pNames->pNames[i], *globals.GetItem(i)});
	}
	else if (scope == 3)
	{
		for (std::size_t i = 0; i < evalResults.size(); ++i)
			items.push_back({std::format("${}", i), evalResults[i]});
	}
	for (const int index : path)
	{
		if (index < 0 || static_cast<std::size_t>(index) >= items.size()) return {};
		const C4Value value{items[static_cast<std::size_t>(index)].value};
		items = Children(value);
	}
	std::string out;
	for (const Child &item : items)
		out += std::format("{}\t{}\n", Clean(item.name, 200), Describe(item.value));
	return out;
}

std::string C4AulDebug::Eval(const int frame, const std::string &expression)
{
	C4AulScriptContext *const ctx{Frame(frame)};
	if (!ctx) return "ERR\tno such frame";
	C4Value result;
	bool found{false};
	if (IsIdentifier(expression))
	{
		C4AulScriptFunc *const func{ctx->Func};
		if (const int32_t i{func->VarNamed.GetItemNr(expression.c_str())}; i >= 0)
		{
			result = ctx->Vars[i];
			found = true;
		}
		else if (const int32_t p{func->ParNamed.GetItemNr(expression.c_str())}; p >= 0)
		{
			result = ctx->Pars[p];
			found = true;
		}
		else if (ctx->Obj && ObjectAlive(ctx->Obj) && ctx->Obj->LocalNamed.pNames && ctx->Obj->LocalNamed.GetItem(expression.c_str()))
		{
			result = *ctx->Obj->LocalNamed.GetItem(expression.c_str());
			found = true;
		}
		else if (Game.ScriptEngine.GlobalNamed.pNames && Game.ScriptEngine.GlobalNamed.GetItem(expression.c_str()))
		{
			result = *Game.ScriptEngine.GlobalNamed.GetItem(expression.c_str());
			found = true;
		}
	}
	if (!found)
	{
		C4Object *const obj{ctx->Obj && ObjectAlive(ctx->Obj) ? ctx->Obj : nullptr};
		C4AulScript &script{obj ? static_cast<C4AulScript &>(obj->Def->Script) : static_cast<C4AulScript &>(Game.Script)};
		std::string error;
		result = AulExecDebugEval(script, obj, expression.c_str(), error);
		if (!error.empty()) return std::format("ERR\t{}", Clean(error));
	}
	result.Deref();
	evalResults.push_back(result);
	return std::format("{}\t{}", Describe(result), evalResults.size() - 1);
}

void C4AulDebug::Handle(const std::string_view command, const std::string &payload)
{
	if (command == "ping")
	{
		Respond(true, "pong");
	}
	else if (command == "start")
	{
		started = true;
		Respond(true, "started");
	}
	else if (command == "scripts")
	{
		std::string out;
		ForEachScript(&Game.ScriptEngine, [&](C4AulScript *script)
		{
			if (!script->Temporary && script->GetScript() && !script->ScriptName.empty())
				out += Clean(script->ScriptName) + '\n';
		});
		Respond(true, out);
	}
	else if (command == "source")
	{
		C4AulScript *const script{FindScript(payload)};
		// without the newline LoadAppend puts before the file, so the lines are the file's
		const char *const text{script ? script->GetScript() : nullptr};
		if (text) Respond(true, *text == '\n' ? text + 1 : text); else Respond(false, "no such script");
	}
	else if (command == "break")
	{
		// "<script>\n<line>\n<line>..."
		std::vector<int> requested;
		const auto firstNewline = payload.find('\n');
		const std::string name{payload.substr(0, firstNewline)};
		for (std::size_t pos = firstNewline == std::string::npos ? payload.size() : firstNewline + 1; pos < payload.size();)
		{
			const auto end = std::min(payload.find('\n', pos), payload.size());
			int line = 0;
			std::from_chars(payload.data() + pos, payload.data() + end, line);
			if (line > 0) requested.push_back(line);
			pos = end + 1;
		}
		// lines with code: byte codes of every script whose source position lies in this script (functions of
		// #include'd scripts are compiled into each includer)
		std::set<int> codeLines;
		C4AulScript *const target{FindScript(name)};
		if (target)
		{
			const char *const begin{target->GetScript()};
			const char *const end{begin + std::strlen(begin)};
			ForEachScript(&Game.ScriptEngine, [&](C4AulScript *script)
			{
				const auto [code, size] = C4AulDebugScriptWalker::Code(script);
				for (int i = 0; i < size; ++i)
					if (code[i].SPos >= begin && code[i].SPos < end && code[i].bccType != AB_EOF)
						codeLines.insert(Line(target, code[i].SPos));
			});
		}
		std::set<int> &lines{breakpoints[NormalizeName(name)]};
		lines.clear();
		std::string out;
		for (const int line : requested)
		{
			// a line without code: the next line with code
			const auto it = codeLines.lower_bound(line);
			const int actual{it != codeLines.end() ? *it : 0};
			if (actual) lines.insert(actual);
			out += std::format("{}\t{}\n", line, actual);
		}
		scriptBreakpoints.clear();
		Respond(true, out);
	}
	else if (command == "breakerrors")
	{
		breakOnErrors = payload == "1";
		Respond(true, breakOnErrors ? "1" : "0");
	}
	else if (command == "pause")
	{
		if (!halted) mode = Mode::Pause;
		Respond(true, halted ? "already halted" : "pausing");
	}
	else if (command == "continue" || command == "next" || command == "stepin" || command == "stepout")
	{
		if (!halted)
		{
			started = true;
			Respond(command == "continue", command == "continue" ? "running" : "not halted");
			return;
		}
		mode = command == "continue" ? Mode::Run : command == "next" ? Mode::StepOver : command == "stepin" ? Mode::StepIn : Mode::StepOut;
		resume = true;
		Respond(true, "resumed");
	}
	else if (command == "detach")
	{
		Detach();
		Respond(true, "detached");
	}
	else if (command == "stack" || command == "vars" || command == "eval")
	{
		if (!halted)
		{
			Respond(false, "not halted");
			return;
		}
		if (command == "stack")
		{
			Respond(true, Stack());
			return;
		}
		// "<frame> <scope> <i>..." / "<frame> <expression>"
		int frame = 0;
		const auto space = payload.find(' ');
		std::from_chars(payload.data(), payload.data() + std::min(space, payload.size()), frame);
		if (command == "eval")
		{
			const std::string result{Eval(frame, space == std::string::npos ? std::string{} : payload.substr(space + 1))};
			if (result.starts_with("ERR\t")) Respond(false, result.substr(4)); else Respond(true, result);
			return;
		}
		std::vector<int> numbers;
		for (std::size_t pos = space == std::string::npos ? payload.size() : space + 1; pos < payload.size();)
		{
			const auto end = std::min(payload.find(' ', pos), payload.size());
			int n = 0;
			std::from_chars(payload.data() + pos, payload.data() + end, n);
			numbers.push_back(n);
			pos = end + 1;
		}
		if (numbers.empty())
		{
			Respond(false, "usage: vars <frame> <scope> [<child index>...]");
			return;
		}
		const int scope{numbers.front()};
		numbers.erase(numbers.begin());
		Respond(true, Vars(frame, scope, numbers));
	}
	else
	{
		Respond(false, std::format("unknown command: {}", command));
	}
}
