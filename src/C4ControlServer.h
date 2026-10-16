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

// Local control port for tools (test automation, AI assistants): started with /control:<port>.
//
// Listens on 127.0.0.1 only and serves one client at a time. Requests are handled on the main thread,
// between frames, so they work while the game is paused.
//
// Protocol (all lengths in bytes):
//   request:  "<command> <payload length>\n<payload>"
//   response: "OK <length>\n<payload>" or "ERR <length>\n<message>"
// Commands:
//   ping                   -> "pong"
//   status                 -> "frame=<n> paused=<0|1> running=<0|1> players=<n> objects=<n> landscape=<w>x<h>"
//   eval <expression>      -> result of a C4Script expression (scenario script context, #strict 3)
//   exec <statements>      -> result of C4Script statements ("var x = ...; return x;"), same context
//   pause / resume         -> "paused=<0|1>"
//   step <frames>          -> runs that many frames, then pauses; answers "frame=<n>" when done
//   viewport               -> opens a viewport without player (free view) if there is none
//   view <x> <y>           -> centers the free viewport on landscape position x/y
//   quit                   -> closes the engine
// Single player games only: script execution bypasses the network control queue.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

class C4ControlServer
{
public:
	static C4ControlServer &Get();

	void SetPort(int port) { this->port = port; }
	bool IsEnabled() const { return port > 0; }

	// call regularly from the main thread while a game is running
	void Poll();
	void Stop();

	~C4ControlServer() { Stop(); }

private:
#ifdef _WIN32
	using Socket = std::uintptr_t;
#else
	using Socket = int;
#endif
	static constexpr Socket InvalidSocket = static_cast<Socket>(~0);

	int port{0};
	bool listenFailed{false};
	Socket listenSocket{InvalidSocket};
	Socket clientSocket{InvalidSocket};
	std::string inBuffer;
	std::string outBuffer;
	// "step" in progress: pause when this frame is reached, then answer
	int32_t stepUntilFrame{-1};

	bool Listen();
	void AcceptClient();
	void CloseClient();
	void ReadClient();
	void FlushOutput();
	void HandleRequests();
	void CheckStep();
	void Respond(bool ok, std::string_view payload);
	// returns false if the response is deferred (step)
	bool Handle(std::string_view command, const std::string &payload);
};
