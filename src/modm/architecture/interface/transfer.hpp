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
	enum class Kind : uint8_t { Data, Callback, Pause, Done };
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
			break;
		case ']': {
			if (!inGroup) return fail(at, "] without [");
			if (join) return fail(at, "+ must be followed by a write");
			const int v = number(star);
			if (star) slot('n'); else if (v < 0) return fail(at, "] needs a count or *");
			inGroup = false;
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

// ---- the transfer object -----------------------------------------------------------------------
// Owned by the caller (a device driver member). The args array is sized by the template below.

struct Transfer
{
	const char* descriptor{nullptr};
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
	uint16_t timeoutMs{0};
	uint8_t inlineBuf[16]{};
	uint8_t slotCount{0};
	uintptr_t* args{nullptr};
	volatile bool busy{false};

	bool isBusy() const { return busy; }
	const Result& wait() const
	{
		while (!result.done) {}  // the fiber version polls instead (modm::this_fiber::poll)
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
	static bool begin(Transfer& t, const char* descriptor, Role role, size_t argsGiven)
	{
		if (t.busy) { t.result = {}; t.result.error = Error::Refused; t.result.done = true; return false; }
		const Parsed p = parse(descriptor, role);
		t.result = {};
		if (!p.ok || p.slots != argsGiven || p.slots > t.slotCount)
		{
			t.result.error = Error::Descriptor;
			t.result.pos = p.ok ? uint8_t(std::string_view(descriptor).size()) : p.pos;
			t.result.done = true;
			return false;
		}
		t.descriptor = descriptor; t.role = role;
		t.pc = t.arg = t.groupPc = t.groupArg = 0; t.groupLeft = 0;
		t.inGroup = t.groupForever = false;
		t.haveAddress = t.joinNext = t.lastWasWrite = t.pingPending = false; t.afterStop = true;
		t.timeoutMs = 0;
		t.busy = true;
		return true;
	}

	// The next step for the engine. Data segments carry 'after': what the engine does when the
	// bytes are done (STOP, repeated START, joined write, or hold the bus and ask again).
	static Segment next(Transfer& t)
	{
		Segment seg;
		std::string_view s(t.descriptor);
		for (;;)
		{
			skipSeps(t, s);
			if (t.pc >= s.size())
			{
				if (t.pingPending) return ping(t, seg, uint8_t(s.size()));
				seg.kind = Segment::Kind::Done;
				return seg;
			}
			const uint8_t at = t.pc;
			const char c = s[t.pc++];
			switch (c)
			{
			case 'I': t.address = uint8_t(hexNumber(t, s)); t.haveAddress = t.pingPending = true; t.afterStop = false; break;
			case 'i': t.address = uint8_t(t.args[t.arg++] & 0x7f); t.haveAddress = t.pingPending = true; t.afterStop = false; break;
			case 'A': { bool star; const int v = hexNumberOrStar(t, s, star); t.address = uint8_t(star ? t.args[t.arg++] : v); t.haveAddress = true; break; }
			case 'g': break;
			case 'W': {
				seg.kind = Segment::Kind::Data; seg.pos = at; seg.address = t.address; seg.read = false;
				seg.start = !t.joinNext; seg.timeoutMs = t.timeoutMs;
				uint16_t n = 0;
				while (t.pc + 1 < s.size() && hexValue(s[t.pc]) >= 0 && hexValue(s[t.pc + 1]) >= 0 && n < sizeof(t.inlineBuf))
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
				seg.kind = Segment::Kind::Data; seg.pos = at; seg.address = t.address; seg.read = (c == 'r');
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
				if (t.pingPending) return ping(t, seg, at);
				t.haveAddress = false; t.afterStop = true; break;
			case 'c':
				if (t.pingPending) { t.pc = at; return ping(t, seg, at); }   // the ping comes before the callback
				seg.kind = Segment::Kind::Callback; seg.pos = at;
				seg.callback = reinterpret_cast<Callback>(t.args[t.arg++]);
				return seg;
			case 'C':
				if (t.pingPending) { t.pc = at; return ping(t, seg, at); }
				seg.kind = Segment::Kind::Pause; seg.pos = at;
				seg.asyncCallback = reinterpret_cast<AsyncCallback>(t.args[t.arg++]);
				return seg;
			case 't': { bool star; const int v = hexNumberOrStar(t, s, star); t.timeoutMs = uint16_t(star ? t.args[t.arg++] : v); break; }
			case '[': {
				t.inGroup = true; t.groupPc = t.pc; t.groupArg = t.arg;
				const uint16_t count = groupCount(t, s);   // the count is read at the [, so every lookahead knows it
				t.groupForever = (count == 0);
				t.groupLeft = count ? count - 1 : 0;
				break;
			}
			case ']': {
				if (t.pingPending) { t.pc = at; return ping(t, seg, at); }
				bool star; hexNumberOrStar(t, s, star);
				if (t.groupForever || t.groupLeft)
				{
					if (!t.groupForever) t.groupLeft--;
					t.pc = t.groupPc; t.arg = t.groupArg;   // the group's arguments are reused
				}
				else { if (star) t.arg++; t.inGroup = false; }
				break;
			}
			case 'D': while (t.pc + 1 < s.size() && hexValue(s[t.pc]) >= 0 && hexValue(s[t.pc + 1]) >= 0) t.pc += 2; break;
			case 's': break;
			default: break;   // parse() refused anything else
			}
		}
	}

	// The engine reports a finished data segment (ok, or with the error at byte 'index').
	// Returns true when the transfer goes on (the engine then calls next()).
	static bool dataDone(Transfer& t, const Segment& seg, Error error, uint16_t index)
	{
		t.result.count += (error == Error::None) ? seg.length : index;
		if (error == Error::None) return true;
		t.result.error = error; t.result.pos = seg.pos; t.result.index = index;
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
			if (t.inGroup) { skipToGroupEnd(t); t.inGroup = false; }
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
		t.result.done = true;
		t.busy = false;
		if (t.completion) t.completion(t);
		return false;
	}

	static bool fail(Transfer& t, Error e, uint8_t pos = 0, uint16_t index = 0)
	{
		t.result.error = e; t.result.pos = pos; t.result.index = index;
		return finish(t);
	}

private:
	// an address with no data token: START, address, STOP (v2: a write of NBYTES = 0)
	static Segment ping(Transfer& t, Segment& seg, uint8_t pos)
	{
		seg.kind = Segment::Kind::Data; seg.pos = pos; seg.address = t.address; seg.read = false;
		seg.start = true; seg.length = 0; seg.after = Segment::After::Stop; seg.timeoutMs = t.timeoutMs;
		t.pingPending = false;
		return seg;
	}
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
	// The count of the group that starts at t.pc: hex after the ], or the argument slot that
	// follows the group's own slots (the count argument comes last, left to right)
	static uint16_t groupCount(Transfer& t, std::string_view s)
	{
		size_t i = t.pc;
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
		if (i >= s.size()) return 1;
		i++;
		if (i < s.size() && s[i] == '*') return uint16_t(t.args[t.arg + slots]);
		int v = 0;
		while (i < s.size() && hexValue(s[i]) >= 0) { v = v * 16 + hexValue(s[i]); i++; }
		return uint16_t(v);
	}
	static bool peekIsCallback(Transfer& t)
	{
		std::string_view s(t.descriptor);
		size_t i = t.pc;
		while (i < s.size() && isSep(s[i])) i++;
		return i < s.size() && (s[i] == 'c' || s[i] == 'C');
	}
	static void skipToGroupEnd(Transfer& t)
	{
		std::string_view s(t.descriptor);
		while (t.pc < s.size() && s[t.pc] != ']') t.pc++;
		if (t.pc < s.size()) { t.pc++; bool star; hexNumberOrStar(t, s, star); if (star) t.arg++; }
	}
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
constexpr void bind(Transfer& t, D, Args... args)
{
	static_assert(sizeof...(Args) == D::slots, "wrong number of arguments for this descriptor");
	static_assert(matchesAll<D, Args...>(std::index_sequence_for<Args...>{}),
	              "argument kinds do not match the descriptor (p: buffer, n: length, a: address, c: callback)");
	size_t i = 0;
	((t.args[i++] = toSlot(args)), ...);
}

}  // namespace xfer
