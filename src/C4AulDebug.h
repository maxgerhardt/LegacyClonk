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

// Script debugger: started with /debug:<port>, used by debug adapters (c4dap of legacy-clonk-ls).
//
// Listens on 127.0.0.1 only and serves one client at a time. While no client is attached, scripts run as usual
// (one flag check per byte code). With a client, the interpreter calls OnStep before every byte code; a stop
// (breakpoint, step, pause request, script error) halts the engine inside the script call until the client
// continues: the window stays responsive, no frames run, the control port (/control) keeps answering.
//
// Framing as in C4ControlServer:
//   request:  "<command> <payload length>\n<payload>"
//   response: "OK <length>\n<payload>" or "ERR <length>\n<message>"
//   event:    "EVT <length>\n<payload>" (sent on its own, between responses)
// Lines are 1-based. Fields in payloads are separated by tabs, records by newlines; tabs and newlines inside
// values are sent as spaces.
// Commands:
//   ping                          -> "pong"
//   scripts                       -> one script name per line (C4AulScript::ScriptName of every loaded script)
//   source <script>               -> the script's text as the engine compiled it
//   break <script>\n<line>\n...    -> replaces the breakpoints of that script; one line per requested breakpoint:
//                                    "<requested>\t<actual line or 0 if no code there>"
//   breakerrors <0|1>             -> stop when a script error is raised (before the call stack unwinds)
//   pause                         -> stop at the next byte code that runs
//   continue / next / stepin / stepout -> resume (halted only): run / to the next line in this function or the
//                                    caller / to the next line anywhere / until this function returns
//   stack                         -> one frame per line, innermost first:
//                                    "<function>\t<script>\t<line>\t<context>" (context: object or definition)
//   vars <frame> <scope> [<i>...] -> variables: scope 0 = parameters + local vars, 1 = the context object's locals,
//                                    2 = global statics; the indices select children (array element, map entry,
//                                    object local) step by step.
//                                    One per line: "<name>\t<type>\t<value>\t<child count>"
//   eval <frame> <expression>     -> "<type>\t<value>\t<child count>": a variable of the frame (as for vars, also
//                                    obj locals and statics), else the expression run in the frame's object
//                                    "	<eval index>" is appended: its children are "vars <frame> 3 <eval index> ..."
//   start                         -> ends the wait of /debugwait
//   detach                        -> clears all breakpoints and resumes
// Events:
//   "stopped\t<reason>\t<text>"    reason: breakpoint, step, pause, error (text: the error message)
//   "continued"

#pragma once

#include <C4Value.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class C4AulScript;
struct C4AulBCC;
struct C4AulScriptContext;
class C4AulError;

class C4AulDebug
{
public:
	static C4AulDebug &Get();

	void SetPort(int port) { this->port = port; }
	void SetWaitForClient(bool wait) { waitForClient = wait; }
	bool IsEnabled() const { return port > 0; }

	// with /debugwait: blocks after the scripts are linked until a client sends "start" (or "continue"), so its
	// breakpoints are set before the first script runs
	void WaitForClient();

	// true while a client is attached: the interpreter then calls OnStep before each byte code
	static bool IsActive() { return active; }

	// call regularly from the main thread
	void Poll();
	void Stop();

	// from the interpreter (C4AulExec)
	void OnStep(C4AulScriptContext *ctx, C4AulBCC *cpos, int depth);
	void OnError(C4AulScriptContext *ctx, C4AulBCC *cpos, const C4AulError &error);

	~C4AulDebug() { Stop(); }

private:
#ifdef _WIN32
	using Socket = std::uintptr_t;
#else
	using Socket = int;
#endif
	static constexpr Socket InvalidSocket = static_cast<Socket>(~0);

	enum class Mode { Run, Pause, StepIn, StepOver, StepOut };

	struct LineTable
	{
		const char *text{nullptr};
		std::vector<std::size_t> starts; // offsets of line beginnings
	};

	struct Halt
	{
		C4AulScriptContext *ctx{nullptr};
		C4AulBCC *cpos{nullptr};
		int depth{0};
	};

	inline static bool active{false};
	int port{0};
	bool listenFailed{false};
	Socket listenSocket{InvalidSocket};
	Socket clientSocket{InvalidSocket};
	std::string inBuffer;
	std::string outBuffer;

	Mode mode{Mode::Run};
	bool breakOnErrors{false};
	bool halted{false};
	bool resume{false};
	bool waitForClient{false};
	bool started{false};
	Halt halt; // where the engine is halted (valid while halted)
	// where the last stop was (call id, see C4AulScriptContext::CallId): no new breakpoint stop on that line until
	// this call leaves it
	std::uint64_t lastCallId{0};
	int lastLine{0}, lastDepth{0};
	const C4AulError *lastError{nullptr};

	std::map<std::string, std::set<int>> breakpoints; // normalized script name -> lines
	std::unordered_map<const C4AulScript *, std::pair<const char *, const std::set<int> *>> scriptBreakpoints; // cache (script text, lines)
	std::unordered_map<const C4AulScript *, LineTable> lineTables;
	std::vector<C4Value> evalResults; // "eval" results while halted (scope 3 of "vars")

	bool Listen();
	void AcceptClient();
	void CloseClient();
	void ReadClient();
	void FlushOutput();
	void HandleRequests();
	void Respond(bool ok, std::string_view payload);
	void Event(std::string_view payload);
	void Handle(std::string_view command, const std::string &payload);

	void Detach();
	void HaltHere(C4AulScriptContext *ctx, C4AulBCC *cpos, int depth, std::string_view reason, std::string_view text);
	int Line(const C4AulScript *script, const char *pos);
	const std::set<int> *BreakpointsOf(const C4AulScript *script);
	C4AulScript *FindScript(std::string_view name);
	static std::string NormalizeName(std::string_view name);

	// frame 0 = the halted context, 1 = its caller (the context below on the stack), ...
	C4AulScriptContext *Frame(int index);
	C4AulBCC *FramePos(int index);
	std::string Stack();
	std::string Vars(int frame, int scope, const std::vector<int> &path);
	std::string Eval(int frame, const std::string &expression);
};
