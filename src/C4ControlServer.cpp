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

#include <C4ControlServer.h>

#include <C4Application.h>
#include <C4Game.h>
#include <C4GraphicsSystem.h>
#include <C4Log.h>
#include <C4Player.h>
#include <C4PlayerList.h>
#include <C4NetIO.h>
#include <C4Viewport.h>
#include <C4Wrappers.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <vector>
#include <algorithm>

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
}

C4ControlServer &C4ControlServer::Get()
{
	static C4ControlServer server;
	return server;
}

bool C4ControlServer::Listen()
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
	LogNTr("Control server listening on 127.0.0.1:{}", port);
	return true;
}

void C4ControlServer::Stop()
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

void C4ControlServer::Poll()
{
	if (!IsEnabled() || listenFailed) return;
	if (listenSocket == InvalidSocket && !Listen())
	{
		LogNTr(spdlog::level::err, "Control server: could not listen on 127.0.0.1:{}", port);
		listenFailed = true;
		return;
	}
	if (clientSocket == InvalidSocket) AcceptClient();
	if (clientSocket == InvalidSocket) return;
	CheckStep();
	ReadClient();
	HandleRequests();
	FlushOutput();
}

void C4ControlServer::AcceptClient()
{
	const auto sock = accept(listenSocket, nullptr, nullptr);
	if (static_cast<Socket>(sock) == InvalidSocket) return;
	SetNonBlocking(sock);
	clientSocket = static_cast<Socket>(sock);
	inBuffer.clear();
	outBuffer.clear();
}

void C4ControlServer::CloseClient()
{
	if (clientSocket != InvalidSocket)
	{
		closesocket(clientSocket);
		clientSocket = InvalidSocket;
	}
	inBuffer.clear();
	outBuffer.clear();
	stepUntilFrame = -1;
}

void C4ControlServer::ReadClient()
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

void C4ControlServer::FlushOutput()
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

void C4ControlServer::Respond(const bool ok, const std::string_view payload)
{
	outBuffer += std::format("{} {}\n", ok ? "OK" : "ERR", payload.size());
	outBuffer += payload;
}

void C4ControlServer::HandleRequests()
{
	// one request at a time; a running "step" defers everything after it
	while (clientSocket != InvalidSocket && stepUntilFrame < 0)
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
		if (!Handle(command, payload)) return;
	}
}

void C4ControlServer::CheckStep()
{
	if (stepUntilFrame < 0) return;
	if (!Game.IsRunning)
	{
		stepUntilFrame = -1;
		Respond(false, "game ended while stepping");
		return;
	}
	if (Game.FrameCounter < stepUntilFrame) return;
	Game.Pause();
	stepUntilFrame = -1;
	Respond(true, std::format("frame={}", Game.FrameCounter));
}

bool C4ControlServer::Handle(const std::string_view command, const std::string &payload)
{
	if (command == "ping")
	{
		Respond(true, "pong");
		return true;
	}
	if (command == "quit")
	{
		Respond(true, "bye");
		FlushOutput();
		Application.Quit();
		return true;
	}
	if (!Game.IsRunning)
	{
		Respond(false, "no game running");
		return true;
	}
	if (command == "status")
	{
		Respond(true, std::format("frame={} paused={} running={} players={} objects={} landscape={}x{}",
			Game.FrameCounter, Game.IsPaused() ? 1 : 0, Game.IsRunning ? 1 : 0, Game.Players.GetCount(),
			Game.Objects.ObjectCount(), GBackWdt, GBackHgt));
	}
	else if (command == "eval" || command == "exec")
	{
		// eval: one expression (like the console); exec: statements with return
		const C4Value result{Game.Script.DirectExec(nullptr, payload.c_str(), "control script", false, C4AulScriptStrict::MAXSTRICT, command == "exec")};
		Respond(true, result.GetDataString());
	}
	else if (command == "pause" || command == "resume")
	{
		if (command == "pause") Game.Pause(); else Game.Unpause();
		Respond(true, std::format("paused={}", Game.IsPaused() ? 1 : 0));
	}
	else if (command == "step")
	{
		int32_t frames = 1;
		std::from_chars(payload.data(), payload.data() + payload.size(), frames);
		if (frames < 1)
		{
			Respond(false, "step needs a positive number of frames");
			return true;
		}
		stepUntilFrame = Game.FrameCounter + frames;
		Game.Unpause();
		return false; // answered by CheckStep()
	}
	else if (command == "viewport")
	{
		if (!Game.GraphicsSystem.GetViewport(NO_OWNER)) Game.CreateViewport(NO_OWNER, true);
		Respond(Game.GraphicsSystem.GetViewport(NO_OWNER) != nullptr, "free viewport");
	}
	else if (command == "view")
	{
		int32_t x = 0, y = 0;
		const auto space = payload.find(' ');
		C4Viewport *const viewport{Game.GraphicsSystem.GetViewport(NO_OWNER)};
		if (space == std::string::npos || !viewport)
		{
			Respond(false, viewport ? "usage: view <x> <y>" : "no free viewport (use \"viewport\" first)");
			return true;
		}
		std::from_chars(payload.data(), payload.data() + space, x);
		std::from_chars(payload.data() + space + 1, payload.data() + payload.size(), y);
		viewport->ViewX = x - viewport->ViewWdt / 2;
		viewport->ViewY = y - viewport->ViewHgt / 2;
		Respond(true, std::format("view={} {}", viewport->ViewX, viewport->ViewY));
	}
	else if (command == "players")
	{
		std::string out;
		for (C4Player *plr = Game.Players.First; plr; plr = plr->Next)
		{
			C4Object *const cursor{plr->Cursor};
			out += std::format("number={} name={} cursor={} eliminated={} jnr={}\n", plr->Number, plr->GetName(),
				cursor ? cursor->Number : -1, plr->Eliminated ? 1 : 0, plr->ControlStyle ? 1 : 0);
		}
		Respond(true, out);
	}
	else if (command == "key")
	{
		// "<plr> <com> [up]"
		std::vector<std::string_view> words;
		for (std::size_t pos = 0; pos < payload.size();)
		{
			const auto end = std::min(payload.find(' ', pos), payload.size());
			if (end > pos) words.emplace_back(payload.data() + pos, end - pos);
			pos = end + 1;
		}
		static constexpr std::pair<std::string_view, int32_t> names[]{
			{"Left", COM_Left}, {"Right", COM_Right}, {"Up", COM_Up}, {"Down", COM_Down}, {"Throw", COM_Throw},
			{"Dig", COM_Dig}, {"Special", COM_Special}, {"Special2", COM_Special2}, {"PlayerMenu", COM_PlayerMenu}};
		int32_t plr = -1, com = COM_None;
		if (words.size() >= 2)
		{
			std::from_chars(words[0].data(), words[0].data() + words[0].size(), plr);
			for (const auto &[name, value] : names)
				if (words[1] == name) com = value;
			if (com == COM_None) std::from_chars(words[1].data(), words[1].data() + words[1].size(), com);
		}
		const bool release{words.size() >= 3 && words[2] == "up"};
		if (com == COM_None || !Game.Players.Get(plr))
		{
			Respond(false, Game.Players.Get(plr) || words.size() < 2 ? "usage: key <player number> <Left|Right|Up|Down|Throw|Dig|Special|Special2|PlayerMenu|number> [up]" : "no such player");
			return true;
		}
		if (release)
		{
			if (com < COM_Left || com > COM_Special2)
			{
				Respond(false, "only Left..Special2 can be released");
				return true;
			}
			com += 16; // COM_*_R
		}
		Game.LocalPlayerControl(plr, com);
		Respond(true, std::format("player={} com={}", plr, com));
	}
	else
	{
		Respond(false, std::format("unknown command: {}", command));
	}
	return true;
}
