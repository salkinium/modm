// Copyright 2026, Niklas Hauser
// SPDX-License-Identifier: MPL-2.0
//
// The transfer language core: a transaction is a string of tokens plus pointer-sized arguments
// (docs/transfer_language.md). This header has no platform dependencies; it is the part that is
// shared between peripherals and between the host tests and the firmware.
//
//   I<hex> i      target address (inline / argument)         W<hex..>  inline write bytes (<= 16)
//   w<n> w*       write n bytes from a buffer                r<n> r*   read n bytes into a buffer
//   +             the next write continues the previous one without a repeated START
//   p             STOP here, the next I/i starts again       t<ms> t*  timeout for what follows
//   c             synchronous callback (interrupt context)   C         asynchronous callback (pause)
//   [ ... ]<n> ]* repeat the group n times (0: until a callback breaks out)
//
// The engine (the peripheral driver) asks the Interpreter for one Segment at a time: a data
// segment with its address, direction, buffer, length and what follows it (STOP, repeated START,
// a joined write, or undecided because a callback comes first), a callback to run, a pause, or
// the end. The engine reports how each data segment ended; the interpreter keeps the Result.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace xfer
{

enum class Error : uint8_t
{
	None,
	AddressNack,      // the target did not acknowledge its address
	DataNack,         // the target did not acknowledge a written byte
	ArbitrationLost,  // another controller won the bus
	BusError,         // a misplaced START or STOP ended the transfer
	Timeout,          // SCL held, or the token's timeout expired
	BusBusy,          // the bus was not free at START
	Descriptor,       // the string is wrong (position in Result::pos)
	Refused,          // the transfer object is still in use
	Aborted,          // reset by software, or Action::Abort from a callback
	Internal,         // the engine saw an event its state does not expect
};

inline constexpr const char* name(Error e)
{
	constexpr const char* names[] = {"ok", "address NACK", "data NACK", "arbitration lost", "bus error",
	                                 "timeout", "bus busy", "descriptor", "refused", "aborted", "internal"};
	return names[uint8_t(e)];
}

// what a synchronous callback returns
enum class Action : uint8_t
{
	Continue,  // the next token
	Repeat,    // the enclosing group again (or the whole string outside a group)
	Break,     // leave the enclosing group
	Stop,      // end the transfer now with the result so far (success)
	Abort,     // end the transfer with Transfer::abortError
};

struct Result
{
	Error error{Error::None};
	uint8_t pos{0};        // character offset of the token that ended the transfer
	uint16_t index{0};     // byte index inside that token
	uint16_t count{0};     // bytes moved in the whole transfer
	bool busErrorSeen{false};  // a BERR that did not end the transfer (the controller went on)
	bool done{false};
};

struct Transfer;
using Callback = Action (*)(Transfer&);
using AsyncCallback = void (*)(Transfer&);
using Completion = void (*)(Transfer&);

// one step for the engine
struct Segment
{
	enum class Kind : uint8_t { Data, Callback, Pause, Stop, Done };   // Stop: release the bus (p after a hold)
	enum class After : uint8_t { Stop, Restart, Join, Hold };  // Hold: a callback decides, keep the bus

	Kind kind{Kind::Done};
	uint8_t address{0};        // 7-bit
	bool read{false};
	bool start{true};          // this segment begins with START or repeated START (false: joined write)
	After after{After::Stop};
	uint16_t length{0};
	uint16_t timeoutMs{0};     // 0: none
	const uint8_t* wbuf{nullptr};
	uint8_t* rbuf{nullptr};
	Callback callback{nullptr};
	AsyncCallback asyncCallback{nullptr};
	uint8_t pos{0};            // character offset of the token
	uint8_t addressPos{0};     // character offset of the I/i that started this segment (an address NACK is reported there)
};

// ---- the parser --------------------------------------------------------------------------------
// Walks the string without storing anything but a position. Used three times: at compile time to
// check the string and count the argument slots, at run time by the C API for the same checks,
// and by the interpreter to step through the tokens.

inline constexpr bool isSep(char c) { return c == '-' || c == ' '; }
inline constexpr int hexValue(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// kinds of argument slots, as a signature string: p pointer, n length, a address, c callback,
// C async callback, t timeout
struct Parsed
{
	bool ok{false};
	uint8_t slots{0};
	uint8_t pos{0};             // error position when !ok
	const char* message{""};
	char signature[32]{};        // one letter per slot
	uint8_t dataTokens{0};
	bool hasAsync{false};
	bool hasLoop{false};
};

// Peripheral roles decide which tokens are allowed
enum class Role : uint8_t { I2cController, I2cTarget, Spi, Uart, Adc };

constexpr Parsed parse(std::string_view s, Role role = Role::I2cController)
{
	Parsed p;
	auto fail = [&](size_t at, const char* why) { p.ok = false; p.pos = uint8_t(at); p.message = why; return p; };
	auto slot = [&](char kind) {
		if (p.slots < sizeof(p.signature) - 1) p.signature[p.slots] = kind;
		p.slots++;
	};
	bool join = false, inGroup = false, haveAddress = false, lastWrite = false, anyData = false;
	size_t groupStart = 0;
	size_t i = 0;
	while (i < s.size())
	{
		const char c = s[i];
		if (isSep(c)) { i++; continue; }
		const size_t at = i++;
		auto number = [&](bool& star) -> int {   // <hex> or *
			star = false;
			if (i < s.size() && s[i] == '*') { i++; star = true; return 0; }
			int v = 0, digits = 0;
			while (i < s.size() && hexValue(s[i]) >= 0 && digits < 4) { v = v * 16 + hexValue(s[i]); i++; digits++; }
			return digits ? v : -1;
		};
		bool star = false;
		switch (c)
		{
		case 'I': case 'i': {
			if (role != Role::I2cController) return fail(at, "I/i only in a controller descriptor");
			if (join) return fail(at, "+ must be followed by a write");
			if (c == 'I') { const int v = number(star); if (star || v < 0 || v > 0x7f) return fail(at, "I needs a 7-bit hex address"); }
			else slot('a');
			haveAddress = true; lastWrite = false;
			break;
		}
		case 'A': case 'g':
			if (role != Role::I2cTarget) return fail(at, "A/g only in a target descriptor");
			if (c == 'A') { const int v = number(star); if (star) slot('a'); else if (v < 0 || v > 0x7f) return fail(at, "A needs a 7-bit hex address"); }
			haveAddress = true;
			break;
		case 'W': {
			if (role == Role::I2cController && !haveAddress) return fail(at, "a write needs an address first");
			int bytes = 0;
			while (i + 1 < s.size() && hexValue(s[i]) >= 0 && hexValue(s[i + 1]) >= 0) { i += 2; bytes++; }
			if (!bytes) return fail(at, "W needs hex byte pairs");
			if (bytes > 16) return fail(at, "W takes at most 16 inline bytes");
			join = false; lastWrite = true; anyData = true; p.dataTokens++;
			break;
		}
		case 'w': case 'r': case 'x': {
			if (c == 'x' && role != Role::Spi) return fail(at, "x only on SPI");
			if (role == Role::I2cController && !haveAddress) return fail(at, "a data token needs an address first");
			if (join && c != 'w') return fail(at, "+ must be followed by a write");
			const int v = number(star);
			if (!star && v <= 0) return fail(at, c == 'r' ? "r needs a length > 0 (a read of 0 bytes hangs)" : "w needs a length > 0");
			slot('p');
			if (c == 'x') slot('p');
			if (star) slot('n');
			join = false; lastWrite = (c == 'w'); anyData = true; p.dataTokens++;
			break;
		}
		case '+':
			if (role == Role::Uart || role == Role::Adc) return fail(at, "+ has no meaning here");
			if (!lastWrite) return fail(at, "+ continues a write, there is none");
			join = true;
			break;
		case 'p':
			if (join) return fail(at, "+ must be followed by a write");
			if (!anyData && !haveAddress) return fail(at, "p before any data");
			haveAddress = false; lastWrite = false;
			break;
		case 'c': slot('c'); lastWrite = false; break;
		case 'C':
			if (role == Role::Uart) return fail(at, "C cannot pause a UART receive");
			slot('C'); p.hasAsync = true; lastWrite = false;
			break;
		case 't': { const int v = number(star); if (star) slot('t'); else if (v < 0) return fail(at, "t needs milliseconds"); break; }
		case '[':
			if (inGroup) return fail(at, "groups do not nest");
			if (join) return fail(at, "+ must be followed by a write");
			inGroup = true; groupStart = at; p.hasLoop = true;
			lastWrite = false;   // a join cannot cross the [: on the next pass it would follow the group's last token
			break;
		case ']': {
			if (!inGroup) return fail(at, "] without [");
			if (join) return fail(at, "+ must be followed by a write");
			const int v = number(star);
			if (star) slot('n'); else if (v < 0) return fail(at, "] needs a count or *");
			inGroup = false;
			lastWrite = false;   // a join cannot cross the ] either: a callback may leave the group after a read
			break;
		}
		case 'D': {
			if (role != Role::Uart) return fail(at, "D only on UART");
			int bytes = 0;
			while (i + 1 < s.size() && hexValue(s[i]) >= 0 && hexValue(s[i + 1]) >= 0) { i += 2; bytes++; }
			if (!bytes) return fail(at, "D needs hex byte pairs");
			break;
		}
		case 's':
			if (role == Role::I2cController || role == Role::I2cTarget) return fail(at, "s has no meaning on I2C");
			break;
		default:
			return fail(at, "unknown token");
		}
	}
	if (join) return fail(s.size(), "+ at the end");
	if (inGroup) return fail(groupStart, "[ without ]");
	if (role == Role::I2cController && !haveAddress && !anyData) return fail(0, "empty descriptor");
	p.ok = true;
	return p;
}

// ---- bytecode ----------------------------------------------------------------------------------
// The compile-time form of a descriptor: one byte per token, inline data as bytes, no parse at run
// time (lang/study/README.md has the size comparison). Positions in a Result are code offsets for
// this form; Descriptor::sourcePos maps them back to the string.
//   ADDR a | ADDRARG | INLINE n b.. | WRITE n | WRITEARG | READ n | READARG | JOIN | STOP | CALL |
//   PAUSE | TIMEOUT lo hi | TIMEOUTARG | LOOP lo hi (0: argument, 0xffff: forever) | END_LOOP | END
namespace bytecode
{
enum Op : uint8_t { ADDR, ADDRARG, INLINE, WRITE, WRITEARG, READ, READARG, JOIN, STOP, CALL, PAUSE, TIMEOUT, TIMEOUTARG, LOOP, END_LOOP, END };

struct Code
{
	uint8_t bytes[160]{};
	uint8_t pos[160]{};      // source position of the token that emitted each byte
	uint8_t size{0};
	bool ok{true};
};

constexpr Code compile(std::string_view s)
{
	Code c;
	size_t at = 0;
	int loopAt = -1;   // where the current group's LOOP header is (groups do not nest)
	auto put = [&](int b) { if (c.size < sizeof(c.bytes)) { c.pos[c.size] = uint8_t(at); c.bytes[c.size++] = uint8_t(b); } else c.ok = false; };
	size_t i = 0;
	while (i < s.size())
	{
		const char ch = s[i];
		if (isSep(ch)) { i++; continue; }
		at = i++;
		auto number = [&](bool& star) -> int {
			star = false;
			if (i < s.size() && s[i] == '*') { i++; star = true; return 0; }
			int v = 0;
			while (i < s.size() && hexValue(s[i]) >= 0) { v = v * 16 + hexValue(s[i]); i++; }
			return v;
		};
		bool star = false;
		switch (ch)
		{
		case 'I': put(ADDR); put(number(star)); break;
		case 'i': put(ADDRARG); break;
		case 'W': {
			const size_t lenAt = c.size + 1; put(INLINE); put(0);
			int n = 0;
			while (i + 1 < s.size() && hexValue(s[i]) >= 0 && hexValue(s[i + 1]) >= 0) { put(hexValue(s[i]) << 4 | hexValue(s[i + 1])); i += 2; n++; }
			c.bytes[lenAt] = uint8_t(n);
			break;
		}
		case 'w': case 'r': {
			const int v = number(star);
			if (star) put(ch == 'w' ? WRITEARG : READARG);
			else { put(ch == 'w' ? WRITE : READ); put(v); }
			break;
		}
		case 'x': { bool st; number(st); c.ok = false; break; }   // SPI exchange: not in the bytecode form yet
		case '+': put(JOIN); break;
		case 'p': put(STOP); break;
		case 'c': put(CALL); break;
		case 'C': put(PAUSE); break;
		case 't': { const int v = number(star); if (star) put(TIMEOUTARG); else { put(TIMEOUT); put(v & 0xff); put(v >> 8); } break; }
		case '[': loopAt = c.size; put(LOOP); put(0); put(0); break;   // the count is patched at the ]
		case ']': {
			const int v = number(star);
			const int count = star ? 0 : (v == 0 ? 0xffff : v);
			if (loopAt >= 0) { c.bytes[loopAt + 1] = uint8_t(count & 0xff); c.bytes[loopAt + 2] = uint8_t(count >> 8); }   // never search the bytes: inline data may hold an opcode value
			loopAt = -1;
			put(END_LOOP);
			break;
		}
		case 's': case 'g': break;
		case 'A': case 'D': default: c.ok = false; i = s.size(); break;   // target and UART forms: string path only
		}
	}
	put(END);
	return c;
}
}  // namespace bytecode

// ---- the transfer object -----------------------------------------------------------------------
// Owned by the caller (a device driver member). The args array is sized by the template below.

struct Transfer
{
	const char* descriptor{nullptr};
	const uint8_t* code{nullptr};     // the bytecode form, when the descriptor was compiled at compile time
	Transfer* next{nullptr};          // intrusive queue link
	Completion completion{nullptr};
	void* context{nullptr};           // for the owner
	Result result{};
	Error abortError{Error::Aborted}; // what Action::Abort reports
	Role role{Role::I2cController};
	// interpreter state
	uint8_t pc{0};                    // character offset of the next token
	uint8_t arg{0};                   // next argument slot
	uint8_t groupPc{0}, groupArg{0};  // where the group started
	uint16_t groupLeft{0};            // iterations left after the current one
	bool inGroup{false}, groupForever{false};
	uint8_t address{0};
	bool haveAddress{false}, joinNext{false}, lastWasWrite{false}, afterStop{true};
	bool pingPending{false};          // an address without data so far: a ping (START, address, STOP)
	uint8_t addressPos{0};            // character offset of the last I/i token (the ping's position)
	uint16_t timeoutMs{0};
	uint8_t inlineBuf[16]{};
	uint8_t slotCount{0};
	uintptr_t* args{nullptr};
	volatile bool busy{false};

	bool isBusy() const { return busy; }
	// Waits on the volatile busy flag, which finish() clears after the result is final: a loop on
	// result.done (a plain bool written from the interrupt) is folded into one read by the compiler.
	// The fiber version polls the same flag (modm::this_fiber::poll).
	const Result& wait() const
	{
		while (busy) {}
		return result;
	}
};

template<size_t N>
struct TransferN : Transfer
{
	uintptr_t storage[N ? N : 1]{};
	TransferN() { args = storage; slotCount = N; }
};

// ---- the interpreter ---------------------------------------------------------------------------
// Stateless over a Transfer: next() yields the next Segment, finished()/failed() end the transfer.

struct Interpreter
{
	// Prepares a transfer: parses, binds the arguments (already in t.args), resets the state.
	// Returns false with result.error = Descriptor or Refused.
#ifndef XFER_NO_STRING
	static bool begin(Transfer& t, const char* descriptor, Role role, size_t argsGiven)
	{
		if (t.busy) return false;   // Refused: the running transfer's result is not touched
		const Parsed p = parse(descriptor, role);
		t.result = {};
		if (!p.ok || p.slots != argsGiven || p.slots > t.slotCount)
		{
			t.result.error = Error::Descriptor;
			t.result.pos = p.ok ? uint8_t(std::string_view(descriptor).size()) : p.pos;
			t.result.done = true;
			return false;
		}
		t.descriptor = descriptor; t.code = nullptr; t.role = role;
		t.pc = t.arg = t.groupPc = t.groupArg = 0; t.groupLeft = 0;
		t.inGroup = t.groupForever = false;
		t.haveAddress = t.joinNext = t.lastWasWrite = t.pingPending = false; t.afterStop = true;
		t.timeoutMs = 0;
		t.busy = true;
		return true;
	}
#endif

	// The same for a descriptor checked at compile time: no parse at run time, the slot count is
	// known; with 'code' the bytecode form runs instead of the string.
	static bool beginChecked(Transfer& t, const char* descriptor, Role role, size_t slots, const uint8_t* code = nullptr)
	{
		if (t.busy) return false;   // Refused: the running transfer's result is not touched
		if (slots > t.slotCount) { t.result = {}; t.result.error = Error::Descriptor; t.result.done = true; return false; }
		t.descriptor = descriptor; t.code = code; t.role = role;
		t.result = {};
		t.pc = t.arg = t.groupPc = t.groupArg = 0; t.groupLeft = 0;
		t.inGroup = t.groupForever = false;
		t.haveAddress = t.joinNext = t.lastWasWrite = t.pingPending = false; t.afterStop = true;
		t.timeoutMs = 0;
		t.busy = true;
		return true;
	}
	template<typename D>
	static bool begin(Transfer& t, D d) { return beginChecked(t, d.c_str(), D::role, D::slots, D::code.data()); }

	// The next data segment for an engine that runs the synchronous callbacks itself: Callback
	// segments are executed here, so only Data, Stop, Pause and Done come back. Done with
	// t.busy == false means a callback already finished the transfer (the completion has run).
	static Segment nextData(Transfer& t)
	{
		for (;;)
		{
			Segment seg = next(t);
			if (seg.kind != Segment::Kind::Callback) return seg;
			if (!act(t, seg.callback(t))) { seg.kind = Segment::Kind::Done; return seg; }
		}
	}

	static Segment next(Transfer& t)
	{
		if (t.code) return nextCode(t);
#ifndef XFER_NO_STRING
		return nextString(t);
#else
		Segment seg; seg.kind = Segment::Kind::Done; return seg;
#endif
	}

	// ---- the bytecode form
	static Segment nextCode(Transfer& t)
	{
		using namespace bytecode;
		Segment seg;
		const uint8_t* code = t.code;
		for (;;)
		{
			const uint8_t at = t.pc;
			const uint8_t op = code[at];
			if (t.pingPending && op != INLINE && op != WRITE && op != WRITEARG && op != READ && op != READARG && op != TIMEOUT && op != TIMEOUTARG)
				return ping(t, seg);
			t.pc++;
			switch (op)
			{
			case ADDR: t.address = code[t.pc++]; t.pingPending = true; t.addressPos = at; break;
			case ADDRARG: t.address = uint8_t(t.args[t.arg++] & 0x7f); t.pingPending = true; t.addressPos = at; break;
			case INLINE:
				seg.kind = Segment::Kind::Data; seg.pos = at; seg.addressPos = t.addressPos; seg.address = t.address;
				seg.start = !t.joinNext; seg.timeoutMs = t.timeoutMs; seg.length = code[t.pc++];
				seg.wbuf = code + t.pc; t.pc += seg.length;
				t.joinNext = false; t.pingPending = false; seg.after = lookaheadCode(t); return seg;
			case WRITE: case READ: case WRITEARG: case READARG:
				seg.kind = Segment::Kind::Data; seg.pos = at; seg.addressPos = t.addressPos; seg.address = t.address;
				seg.read = (op == READ || op == READARG); seg.start = !t.joinNext; seg.timeoutMs = t.timeoutMs;
				if (seg.read) seg.rbuf = reinterpret_cast<uint8_t*>(t.args[t.arg++]); else seg.wbuf = reinterpret_cast<const uint8_t*>(t.args[t.arg++]);
				seg.length = (op == WRITE || op == READ) ? code[t.pc++] : uint16_t(t.args[t.arg++]);
				t.joinNext = false; t.pingPending = false; seg.after = lookaheadCode(t); return seg;
			case JOIN: t.joinNext = true; break;
			case STOP: seg.kind = Segment::Kind::Stop; seg.pos = at; return seg;
			case CALL: seg.kind = Segment::Kind::Callback; seg.pos = at; seg.callback = reinterpret_cast<Callback>(t.args[t.arg++]); return seg;
			case PAUSE: seg.kind = Segment::Kind::Pause; seg.pos = at; seg.asyncCallback = reinterpret_cast<AsyncCallback>(t.args[t.arg++]); return seg;
			case TIMEOUT: t.timeoutMs = uint16_t(code[t.pc] | code[t.pc + 1] << 8); t.pc += 2; break;
			case TIMEOUTARG: t.timeoutMs = uint16_t(t.args[t.arg++]); break;
			case LOOP: {
				const uint16_t count = uint16_t(code[t.pc] | code[t.pc + 1] << 8); t.pc += 2;
				t.inGroup = true; t.groupPc = t.pc; t.groupArg = t.arg;
				const uint16_t n = count ? count : uint16_t(t.args[t.arg + slotsToLoopEnd(t)]);
				t.groupForever = (count == 0xffff) || (count == 0 && n == 0);
				t.groupLeft = t.groupForever ? 0 : uint16_t(n - 1);
				break;
			}
			case END_LOOP:
				if (t.groupForever || t.groupLeft) { if (!t.groupForever) t.groupLeft--; t.pc = t.groupPc; t.arg = t.groupArg; }
				else { if (code[t.groupPc - 2] == 0 && code[t.groupPc - 1] == 0) t.arg++; t.inGroup = false; }
				break;
			case END: default: seg.kind = Segment::Kind::Done; return seg;
			}
		}
	}

	static Segment::After lookaheadCode(const Transfer& t)
	{
		using namespace bytecode;
		const uint8_t* code = t.code;
		size_t i = t.pc; bool j = false;
		for (;;)
		{
			switch (code[i++])
			{
			case JOIN: j = true; break;
			case INLINE: case WRITE: case WRITEARG: return j ? Segment::After::Join : Segment::After::Restart;
			case READ: case READARG: case ADDR: case ADDRARG: return Segment::After::Restart;
			case CALL: case PAUSE: return Segment::After::Hold;
			case STOP: case END: return Segment::After::Stop;
			case END_LOOP: if (t.groupForever || t.groupLeft) i = t.groupPc; break;
			case TIMEOUT: case LOOP: i += 2; break;
			default: break;
			}
		}
	}

	static uint8_t slotsToLoopEnd(const Transfer& t)
	{
		using namespace bytecode;
		const uint8_t* code = t.code;
		uint8_t n = 0;
		for (size_t i = t.pc; code[i] != END_LOOP && code[i] != END;)
		{
			switch (code[i++])
			{
			case ADDRARG: case CALL: case PAUSE: case TIMEOUTARG: n++; break;
			case WRITE: case READ: n++; i++; break;
			case WRITEARG: case READARG: n += 2; break;
			case INLINE: i += code[i] + 1; break;
			case ADDR: i++; break;
			case TIMEOUT: i += 2; break;
			default: break;
			}
		}
		return n;
	}

#ifndef XFER_NO_STRING
	// ---- the string form
	// The next step for the engine. Data segments carry 'after': what the engine does when the
	// bytes are done (STOP, repeated START, joined write, or hold the bus and ask again).
	static Segment nextString(Transfer& t)
	{
		Segment seg;
		std::string_view s(t.descriptor);
		for (;;)
		{
			skipSeps(t, s);
			if (t.pc >= s.size())
			{
				if (t.pingPending) return ping(t, seg);
				seg.kind = Segment::Kind::Done;
				return seg;
			}
			const uint8_t at = t.pc;
			const char c = s[t.pc++];
			switch (c)
			{
			case 'I': case 'i':
				if (t.pingPending) { t.pc = at; return ping(t, seg); }   // two addresses in a row: the first is a ping
				t.address = (c == 'I') ? uint8_t(hexNumber(t, s)) : uint8_t(t.args[t.arg++] & 0x7f);
				t.haveAddress = t.pingPending = true; t.afterStop = false; t.addressPos = at;
				break;
			case 'A': { bool star; const int v = hexNumberOrStar(t, s, star); t.address = uint8_t(star ? t.args[t.arg++] : v); t.haveAddress = true; break; }
			case 'g': break;
			case 'W': {
				seg.kind = Segment::Kind::Data; seg.pos = at; seg.addressPos = t.addressPos; seg.address = t.address; seg.read = false;
				seg.start = !t.joinNext; seg.timeoutMs = t.timeoutMs;
				uint16_t n = 0;
				while (size_t(t.pc) + 1 < s.size() && hexValue(s[t.pc]) >= 0 && hexValue(s[t.pc + 1]) >= 0 && n < sizeof(t.inlineBuf))
				{
					t.inlineBuf[n++] = uint8_t(hexValue(s[t.pc]) << 4 | hexValue(s[t.pc + 1]));
					t.pc += 2;
				}
				seg.wbuf = t.inlineBuf; seg.length = n;
				t.joinNext = false; t.lastWasWrite = true; t.pingPending = false;
				seg.after = lookahead(t, s);
				return seg;
			}
			case 'w': case 'r': case 'x': {
				seg.kind = Segment::Kind::Data; seg.pos = at; seg.addressPos = t.addressPos; seg.address = t.address; seg.read = (c == 'r');
				seg.start = !t.joinNext; seg.timeoutMs = t.timeoutMs;
				bool star; const int v = hexNumberOrStar(t, s, star);
				if (c == 'r') seg.rbuf = reinterpret_cast<uint8_t*>(t.args[t.arg++]);
				else seg.wbuf = reinterpret_cast<const uint8_t*>(t.args[t.arg++]);
				if (c == 'x') seg.rbuf = reinterpret_cast<uint8_t*>(t.args[t.arg++]);
				seg.length = uint16_t(star ? t.args[t.arg++] : v);
				t.joinNext = false; t.lastWasWrite = (c == 'w'); t.pingPending = false;
				seg.after = lookahead(t, s);
				return seg;
			}
			case '+': t.joinNext = true; break;
			case 'p':
				if (t.pingPending) { t.pc = at; return ping(t, seg); }
				t.haveAddress = false; t.afterStop = true;
				seg.kind = Segment::Kind::Stop; seg.pos = at;   // the engine generates the STOP if the bus is still held
				return seg;
			case 'c':
				if (t.pingPending) { t.pc = at; return ping(t, seg); }   // the ping comes before the callback
				seg.kind = Segment::Kind::Callback; seg.pos = at;
				seg.callback = reinterpret_cast<Callback>(t.args[t.arg++]);
				return seg;
			case 'C':
				if (t.pingPending) { t.pc = at; return ping(t, seg); }
				seg.kind = Segment::Kind::Pause; seg.pos = at;
				seg.asyncCallback = reinterpret_cast<AsyncCallback>(t.args[t.arg++]);
				return seg;
			case 't': { bool star; const int v = hexNumberOrStar(t, s, star); t.timeoutMs = uint16_t(star ? t.args[t.arg++] : v); break; }
			case '[': {
				if (t.pingPending) { t.pc = at; return ping(t, seg); }
				t.inGroup = true; t.groupPc = t.pc; t.groupArg = t.arg;
				const uint16_t count = groupCount(t, s);   // the count is read at the [, so every lookahead knows it
				t.groupForever = (count == 0);
				t.groupLeft = count ? count - 1 : 0;
				break;
			}
			case ']': {
				if (t.pingPending) { t.pc = at; return ping(t, seg); }
				bool star; hexNumberOrStar(t, s, star);
				if (t.groupForever || t.groupLeft)
				{
					if (!t.groupForever) t.groupLeft--;
					t.pc = t.groupPc; t.arg = t.groupArg;   // the group's arguments are reused
				}
				else { if (star) t.arg++; t.inGroup = false; }
				break;
			}
			case 'D': while (size_t(t.pc) + 1 < s.size() && hexValue(s[t.pc]) >= 0 && hexValue(s[t.pc + 1]) >= 0) t.pc += 2; break;
			case 's': break;
			default: break;   // parse() refused anything else
			}
		}
	}

#endif  // XFER_NO_STRING

	// The engine reports a finished data segment (ok, or with the error at byte 'index').
	// Returns true when the transfer goes on (the engine then calls next()).
	static bool dataDone(Transfer& t, const Segment& seg, Error error, uint16_t index)
	{
		t.result.count += (error == Error::None) ? seg.length : index;
		if (error == Error::None) return true;
		t.result.error = error; t.result.pos = (error == Error::AddressNack) ? seg.addressPos : seg.pos; t.result.index = index;
		// an address NACK inside a group may be polled by a callback: the next token decides
		if (error == Error::AddressNack && peekIsCallback(t)) return true;
		return finish(t);
	}

	// The engine ran a synchronous callback and got its action.
	// Returns true when the transfer goes on.
	static bool act(Transfer& t, Action a)
	{
		switch (a)
		{
		case Action::Continue:
			t.result.error = Error::None;   // a polled NACK that the callback accepted
			return true;
		case Action::Repeat:
			t.result.error = Error::None;
			if (t.inGroup) { t.pc = t.groupPc; t.arg = t.groupArg; }
			else { t.pc = 0; t.arg = 0; t.haveAddress = false; }
			return true;
		case Action::Break:
			t.result.error = Error::None;
			if (!t.inGroup) return finish(t);   // outside a group the whole string is the group
#ifndef XFER_NO_STRING
			if (!t.code) skipToGroupEnd(t); else
#endif
			skipToLoopEnd(t);
			t.inGroup = false;
			return true;
		case Action::Stop:
			t.result.error = Error::None;
			return finish(t);
		case Action::Abort:
			t.result.error = t.abortError;
			return finish(t);
		}
		return finish(t);
	}

	// The engine saw a BERR that did not end the transfer
	static void busErrorSeen(Transfer& t) { t.result.busErrorSeen = true; }

	// Ends the transfer: the result is final, the completion runs, the object is free again.
	static bool finish(Transfer& t)
	{
		if (!t.busy) return false;   // already finished (a callback ended the transfer)
		t.result.done = true;
		t.busy = false;
		if (t.completion) t.completion(t);
		return false;
	}

	static void skipToLoopEnd(Transfer& t)
	{
		using namespace bytecode;
		t.arg += slotsToLoopEnd(t);
		const uint8_t* code = t.code;
		while (code[t.pc] != END_LOOP && code[t.pc] != END) t.pc++;
		if (code[t.pc] == END_LOOP) { t.pc++; if (code[t.groupPc - 2] == 0 && code[t.groupPc - 1] == 0) t.arg++; }
	}

	static bool fail(Transfer& t, Error e, uint8_t pos = 0, uint16_t index = 0)
	{
		t.result.error = e; t.result.pos = pos; t.result.index = index;
		return finish(t);
	}

private:
	// an address with no data token: START, address, STOP (v2: a write of NBYTES = 0)
	static Segment ping(Transfer& t, Segment& seg)
	{
		seg.kind = Segment::Kind::Data; seg.pos = seg.addressPos = t.addressPos; seg.address = t.address; seg.read = false;
		seg.start = true; seg.length = 0; seg.after = Segment::After::Stop; seg.timeoutMs = t.timeoutMs;
		t.pingPending = false;
		return seg;
	}
#ifndef XFER_NO_STRING
	static void skipSeps(Transfer& t, std::string_view s) { while (t.pc < s.size() && isSep(s[t.pc])) t.pc++; }
	static int hexNumber(Transfer& t, std::string_view s)
	{
		int v = 0, digits = 0;
		while (t.pc < s.size() && hexValue(s[t.pc]) >= 0 && digits < 4) { v = v * 16 + hexValue(s[t.pc]); t.pc++; digits++; }
		return v;
	}
	static int hexNumberOrStar(Transfer& t, std::string_view s, bool& star)
	{
		star = (t.pc < s.size() && s[t.pc] == '*');
		if (star) { t.pc++; return 0; }
		return hexNumber(t, s);
	}
	// What follows the data token at t.pc, without moving: a joined write, another data token
	// (repeated START), a callback (hold), the end or p (STOP). Inside a group the ] may jump back.
	static Segment::After lookahead(Transfer& t, std::string_view s)
	{
		size_t i = t.pc;
		bool join = false;
		for (;;)
		{
			while (i < s.size() && isSep(s[i])) i++;
			if (i >= s.size()) return Segment::After::Stop;
			const char c = s[i++];
			switch (c)
			{
			case '+': join = true; break;
			case 'W': case 'w': return join ? Segment::After::Join : Segment::After::Restart;
			case 'r': case 'x': case 'I': case 'i': case 'A': return Segment::After::Restart;
			case 'c': case 'C': return Segment::After::Hold;
			case 'p': return Segment::After::Stop;
			case ']':   // the group repeats unless this is its last pass
				if (t.groupForever || t.groupLeft) { i = t.groupPc; join = false; break; }
				skipNumber(s, i);
				break;
			case 't': case 'D': case 's': case 'g': skipNumber(s, i); break;
			case '[': break;
			default: return Segment::After::Stop;
			}
		}
	}
	static void skipNumber(std::string_view s, size_t& i)
	{
		if (i < s.size() && s[i] == '*') { i++; return; }
		while (i < s.size() && hexValue(s[i]) >= 0) i++;
	}
	// Argument slots used by the tokens from i up to the ] (i is moved there)
	static uint8_t slotsUntilGroupEnd(std::string_view s, size_t& i)
	{
		uint8_t slots = 0;
		while (i < s.size() && s[i] != ']')
		{
			const char c = s[i++];
			auto star = [&] { if (i < s.size() && s[i] == '*') { i++; return true; } while (i < s.size() && hexValue(s[i]) >= 0) i++; return false; };
			switch (c)
			{
			case 'i': case 'c': case 'C': slots++; break;
			case 'A': case 't': if (star()) slots++; break;
			case 'w': case 'r': slots += 1 + star(); break;
			case 'x': slots += 2 + star(); break;
			case 'W': case 'D': while (i < s.size() && hexValue(s[i]) >= 0) i++; break;
			case 'I': star(); break;
			default: break;
			}
		}
		return slots;
	}
	// The count of the group that starts at t.pc: hex after the ], or the argument slot that
	// follows the group's own slots (the count argument comes last, left to right)
	static uint16_t groupCount(Transfer& t, std::string_view s)
	{
		size_t i = t.pc;
		const uint8_t slots = slotsUntilGroupEnd(s, i);
		if (i >= s.size()) return 1;
		i++;
		if (i < s.size() && s[i] == '*') return uint16_t(t.args[t.arg + slots]);
		int v = 0;
		while (i < s.size() && hexValue(s[i]) >= 0) { v = v * 16 + hexValue(s[i]); i++; }
		return uint16_t(v);
	}
#endif
	static bool peekIsCallback(Transfer& t)
	{
		if (t.code) return t.code[t.pc] == bytecode::CALL || t.code[t.pc] == bytecode::PAUSE;
#ifndef XFER_NO_STRING
		std::string_view s(t.descriptor);
		size_t i = t.pc;
		while (i < s.size() && isSep(s[i])) i++;
		return i < s.size() && (s[i] == 'c' || s[i] == 'C');
#else
		return false;
#endif
	}
#ifndef XFER_NO_STRING
	static void skipToGroupEnd(Transfer& t)
	{
		std::string_view s(t.descriptor);
		size_t i = t.pc;
		t.arg += slotsUntilGroupEnd(s, i);   // the arguments of the tokens left in the group
		t.pc = uint8_t(i);
		if (t.pc < s.size()) { t.pc++; bool star; hexNumberOrStar(t, s, star); if (star) t.arg++; }
	}
#endif
};

// ---- compile-time descriptors (C++) ------------------------------------------------------------
// "I2a W01 r*"_desc parses at compile time; transfer() checks the argument count and kinds.

template<size_t N>
struct Literal
{
	char text[N]{};
	constexpr Literal(const char (&s)[N]) { for (size_t i = 0; i < N; i++) text[i] = s[i]; }
	constexpr std::string_view view() const { return {text, N - 1}; }
};

template<Literal L, Role R = Role::I2cController>
struct Descriptor
{
	static constexpr std::string_view text = L.view();
	static constexpr Parsed parsed = parse(text, R);
	static_assert(parsed.ok, "bad transfer descriptor (see Parsed::message)");
	static constexpr size_t slots = parsed.slots;
	static constexpr Role role = R;
	static constexpr bytecode::Code compiled = bytecode::compile(text);
	static constexpr bool hasCode = compiled.ok;
	// the bytecode (an empty END program when the form has no bytecode: target and UART descriptors)
	static constexpr auto code = [] {
		std::array<uint8_t, compiled.ok ? compiled.size : 1> a{};
		if constexpr (compiled.ok) for (size_t i = 0; i < compiled.size; i++) a[i] = compiled.bytes[i];
		else a[0] = bytecode::END;
		return a;
	}();
	// source position of a code offset (Result::pos of the bytecode form)
	static constexpr uint8_t sourcePos(uint8_t codeOffset) { return codeOffset < compiled.size ? compiled.pos[codeOffset] : uint8_t(text.size()); }
	constexpr const char* c_str() const { return L.text; }
};

template<Literal L>
constexpr Descriptor<L> operator""_desc() { return {}; }

template<typename T>
constexpr bool matchesSlot(char kind)
{
	using U = std::remove_cvref_t<T>;
	switch (kind)
	{
	case 'p': return std::is_pointer_v<U> && !std::is_function_v<std::remove_pointer_t<U>>;
	case 'n': case 'a': case 't': return std::is_integral_v<U> || std::is_enum_v<U>;
	case 'c': return std::is_convertible_v<U, Callback>;
	case 'C': return std::is_convertible_v<U, AsyncCallback>;
	}
	return false;
}

template<typename D, typename... Args, size_t... I>
constexpr bool matchesAll(std::index_sequence<I...>)
{
	return (matchesSlot<Args>(D::parsed.signature[I]) && ...);
}

template<typename T>
constexpr uintptr_t toSlot(T v)
{
	if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(v);
	else if constexpr (std::is_enum_v<T>) return uintptr_t(static_cast<std::underlying_type_t<T>>(v));
	else return uintptr_t(v);
}

// Fills the transfer's argument slots from typed arguments; the engine then calls Interpreter::begin.
template<typename D, typename... Args>
[[gnu::always_inline]] constexpr void bind(Transfer& t, D, Args... args)
{
	static_assert(sizeof...(Args) == D::slots, "wrong number of arguments for this descriptor");
	static_assert(matchesAll<D, Args...>(std::index_sequence_for<Args...>{}),
	              "argument kinds do not match the descriptor (p: buffer, n: length, a: address, c: callback)");
	size_t i = 0;
	((t.args[i++] = toSlot(args)), ...);
}

}  // namespace xfer
