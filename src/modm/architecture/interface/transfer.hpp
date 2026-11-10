// Copyright 2026, Niklas Hauser
// SPDX-License-Identifier: MPL-2.0
//
// The transfer language core: a transaction is a string of tokens plus pointer-sized arguments
// (docs/transfer_language.md). This header has no platform dependencies; it is the part that is
// shared between peripherals and between the host tests and the firmware.
//
//   S<hex> s      START + target address (inline / argument)  W<hex..>  inline write bytes (<= 16)
//   w<n> w*       write n bytes from a buffer                r<n> r*   read n bytes into a buffer
//   +             the next write continues the previous one without a repeated START
//   p             STOP here, the next S/s starts again       t<ms> t*  timeout for what follows
//   c             synchronous callback (interrupt context)   C         asynchronous callback (pause)
//   [ ... ]<n> ]* repeat the group n times (0: until a callback breaks out)
//   d<us> d*      busy-wait in the engine, microseconds; the bus stays held between data tokens
//
// A descriptor is checked by parse() and compiled to bytecode by bytecode::compile(), both
// constexpr: a "..."_desc literal does it at compile time, Interpreter::begin(t, string, ...)
// at run time into a buffer of the caller (a C API, a debug shell, descriptors received as data).
// The engine (the peripheral driver) asks the Interpreter for one Segment at a time: a data
// segment with its address, direction, buffer, length and what follows it (STOP, repeated START,
// a joined write, or undecided because a callback comes first), a callback to run, a pause, or
// the end. The engine reports how each data segment ended; the interpreter keeps the Result.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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
	enum class Kind : uint8_t { Data, Callback, Pause, Stop, Done, Finished, Delay };   // Stop: release the bus (p after a hold); Finished: a callback ended the transfer, only from nextData(); Delay: busy-wait 'length' microseconds, then ask again
	enum class After : uint8_t { Stop, Restart, Join, Hold };  // Hold: a callback decides, keep the bus

	Kind kind{Kind::Done};
	uint8_t address{0};        // 7-bit
	After after{After::Stop};
	bool read : 1 {false};
	bool start : 1 {true};     // this segment begins with START or repeated START (false: joined write)
	bool last : 1 {false};     // the transfer's last segment: dataDone() without an error finishes the transfer
	uint8_t pos{0};            // character offset of the token
	uint8_t addressPos{0};     // character offset of the S/s that started this segment (an address NACK is reported there)
	uint16_t length{0};
	uint16_t timeoutMs{0};     // 0: none
	union                      // one pointer: what the segment's kind needs
	{
		const uint8_t* wbuf{nullptr};
		uint8_t* rbuf;
		Callback callback;
		AsyncCallback asyncCallback;
	};
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
// C async callback, t timeout, d delay
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
	bool hasCallback{false};   // c or C: the segments cannot be prepared ahead
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
	bool groupYields = false;   // the group holds an address, data, p, c or C
	size_t i = 0;
	auto yields = [&](char c) { return c == 'S' || c == 's' || c == 'W' || c == 'w' || c == 'r' || c == 'x' || c == 'p' || c == 'c' || c == 'C' || c == 'A'; };
	while (i < s.size())
	{
		const char c = s[i];
		if (isSep(c)) { i++; continue; }
		const size_t at = i++;
		if (yields(c)) groupYields = true;
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
		case 'S': case 's': {
			if (role != Role::I2cController) return fail(at, "S/s only in a controller descriptor");
			if (join) return fail(at, "+ must be followed by a write");
			if (c == 'S') { const int v = number(star); if (star || v < 0 || v > 0x7f) return fail(at, "S needs a 7-bit hex address"); }
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
		case 'c': slot('c'); p.hasCallback = true; lastWrite = false; break;
		case 'C':
			if (role == Role::Uart) return fail(at, "C cannot pause a UART receive");
			slot('C'); p.hasAsync = p.hasCallback = true; lastWrite = false;
			break;
		case 't': { const int v = number(star); if (star) slot('t'); else if (v < 0) return fail(at, "t needs milliseconds"); break; }
		case 'd': {
			if (join) return fail(at, "+ must be followed by a write");
			const int v = number(star);
			if (star) slot('d'); else if (v < 0) return fail(at, "d needs microseconds (hex, at most ffff)");
			lastWrite = false;
			break;
		}
		case '[':
			if (inGroup) return fail(at, "groups do not nest");
			if (join) return fail(at, "+ must be followed by a write");
			inGroup = true; groupStart = at; p.hasLoop = true; groupYields = false;
			lastWrite = false;   // a join cannot cross the [: on the next pass it would follow the group's last token
			break;
		case ']': {
			if (!inGroup) return fail(at, "] without [");
			if (join) return fail(at, "+ must be followed by a write");
			const int v = number(star);
			if (!star && v == 0 && !groupYields) return fail(at, "a group repeated for ever needs a token that reaches the bus or a callback");
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
		case '|':
			if (role == Role::I2cController || role == Role::I2cTarget) return fail(at, "| has no meaning on I2C");
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
// The compile-time form of a descriptor: two bytes per token (the opcode and the token's character
// position in the string, so Result::pos means the same in both forms), inline data as bytes, no
// parse at run time (lang/study/README.md has the size comparison).
// Data ops also carry what follows them (Segment::After, once for a group pass that ends and once
// for one that repeats), and LOOP the argument slots of its body, so the VM never scans ahead in
// the interrupt.
//   ADDR p a | ADDRARG p | INLINE p f n b.. | WRITE p f n | WRITEARG p f | READ p f n | READARG p f |
//   JOIN p | STOP p | CALL p | PAUSE p | TIMEOUT p lo hi | TIMEOUTARG p |
//   LOOP p lo hi slots (count 0: argument, 0xffff: forever) | END_LOOP p | END p | DELAY p lo hi | DELAYARG p
//   f = after (2 bits) | after when the group repeats (2 bits) << 2
namespace bytecode
{
enum Op : uint8_t { ADDR, ADDRARG, INLINE, WRITE, WRITEARG, READ, READARG, JOIN, STOP, CALL, PAUSE, TIMEOUT, TIMEOUTARG, LOOP, END_LOOP, END, DELAY, DELAYARG };   // new ops go last: host-compiled arrays keep their values

struct Code
{
	uint8_t bytes[192]{};
	uint8_t size{0};
	bool ok{true};
};
struct Compiled { size_t size; bool ok; };   // ok false: does not fit, or a token without a bytecode form

// the index of the op after the one at i
constexpr size_t skipOp(const uint8_t* code, size_t i)
{
	switch (code[i])
	{
	case ADDR: case WRITEARG: case READARG: return i + 3;
	case WRITE: case READ: case TIMEOUT: case DELAY: return i + 4;
	case INLINE: return i + 4 + code[i + 3];
	case LOOP: return i + 5;
	default: return i + 2;   // ADDRARG JOIN STOP CALL PAUSE TIMEOUTARG END_LOOP END DELAYARG
	}
}

constexpr bool isData(uint8_t op) { return op == INLINE || op == WRITE || op == WRITEARG || op == READ || op == READARG; }

// Segment::After values, duplicated here as plain numbers so the compiler has no dependency on the Segment type
enum AfterCode : uint8_t { A_STOP = 0, A_RESTART = 1, A_JOIN = 2, A_HOLD = 3 };

// what follows the data op at i: the token after it decides (a joined write, another data token,
// a callback, the end or p); at END_LOOP with 'repeat' the group's first token is next
constexpr uint8_t afterOf(const uint8_t* code, size_t i, size_t bodyStart, bool repeat)
{
	bool join = false;
	size_t j = skipOp(code, i);
	for (int guard = 0; guard < 2; )
	{
		switch (code[j])
		{
		case JOIN: join = true; break;
		case INLINE: case WRITE: case WRITEARG: return join ? A_JOIN : A_RESTART;
		case READ: case READARG: case ADDR: case ADDRARG: return A_RESTART;
		case CALL: case PAUSE: return A_HOLD;
		// DELAY: looked through, the delay runs where the engine fetches the next segment (v2: at TC
		// before the repeated START, SCL low; after a STOP: on a free bus). Hold would cost v1 an
		// address byte after a read, and a delay has nothing to decide.
		case STOP: case END: return A_STOP;
		case END_LOOP: if (repeat && bodyStart) { j = bodyStart; join = false; guard++; continue; } break;
		default: break;
		}
		j = skipOp(code, j);
	}
	return A_RESTART;
}

// Compiles a descriptor that parse() accepted into 'out'. Only the I2C controller form has a
// bytecode (and an engine) so far; target, UART and SPI tokens make it fail.
constexpr Compiled compile(std::string_view s, uint8_t* out, size_t capacity)
{
	struct { uint8_t* bytes; size_t size; bool ok; } c{out, 0, true};
	size_t at = 0;
	int loopAt = -1;   // where the current group's LOOP header is (groups do not nest)
	auto put = [&](int b) { if (c.size < capacity) c.bytes[c.size++] = uint8_t(b); else c.ok = false; };
	auto op = [&](int o) { put(o); put(int(at)); };   // every op carries its character position
	auto data = [&](int o) { op(o); put(0); };         // data ops: the 'after' byte, filled in below
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
		case 'S': op(ADDR); put(number(star)); break;
		case 's': op(ADDRARG); break;
		case 'W': {
			data(INLINE); const size_t lenAt = c.size; put(0);
			int n = 0;
			while (i + 1 < s.size() && hexValue(s[i]) >= 0 && hexValue(s[i + 1]) >= 0) { put(hexValue(s[i]) << 4 | hexValue(s[i + 1])); i += 2; n++; }
			c.bytes[lenAt] = uint8_t(n);
			break;
		}
		case 'w': case 'r': {
			const int v = number(star);
			if (star) data(ch == 'w' ? WRITEARG : READARG);
			else { data(ch == 'w' ? WRITE : READ); put(v); }
			break;
		}
		case 'x': { bool st; number(st); c.ok = false; break; }   // SPI exchange: no bytecode form (no engine) yet
		case '+': op(JOIN); break;
		case 'p': op(STOP); break;
		case 'c': op(CALL); break;
		case 'C': op(PAUSE); break;
		case 't': { const int v = number(star); if (star) op(TIMEOUTARG); else { op(TIMEOUT); put(v & 0xff); put(v >> 8); } break; }
		case 'd': { const int v = number(star); if (star) op(DELAYARG); else { op(DELAY); put(v & 0xff); put(v >> 8); } break; }
		case '[': loopAt = c.size; op(LOOP); put(0); put(0); put(0); break;   // count and slots are patched at the ]
		case ']': {
			const int v = number(star);
			const int count = star ? 0 : (v == 0 ? 0xffff : v);
			if (loopAt >= 0)
			{   // never search the bytes for the header: inline data may hold an opcode value
				c.bytes[loopAt + 2] = uint8_t(count & 0xff); c.bytes[loopAt + 3] = uint8_t(count >> 8);
				uint8_t slots = 0;   // argument slots of the body
				for (size_t j = loopAt + 5; j < c.size; j = skipOp(c.bytes, j))
					switch (c.bytes[j])
					{
					case ADDRARG: case CALL: case PAUSE: case TIMEOUTARG: case DELAYARG: case WRITE: case READ: slots++; break;
					case WRITEARG: case READARG: slots += 2; break;
					default: break;
					}
				c.bytes[loopAt + 4] = slots;
			}
			loopAt = -1;
			op(END_LOOP);
			break;
		}
		case '|': case 'g': break;
		case 'A': case 'D': default: c.ok = false; i = s.size(); break;   // target and UART forms: no bytecode (no engine) yet
		}
	}
	at = s.size();
	op(END);
	if (!c.ok) return {c.size, false};
	// the 'after' of every data op, for a group pass that ends and for one that repeats
	size_t body = 0;
	for (size_t j = 0; j < c.size; j = skipOp(c.bytes, j))
	{
		if (c.bytes[j] == LOOP) body = j + 5;
		else if (c.bytes[j] == END_LOOP) body = 0;
		else if (isData(c.bytes[j])) c.bytes[j + 2] = uint8_t(afterOf(c.bytes, j, body, false) | afterOf(c.bytes, j, body, true) << 2);
	}
	return {c.size, true};
}
constexpr Code compile(std::string_view s)
{
	Code c;
	const Compiled r = compile(s, c.bytes, sizeof(c.bytes));
	c.size = uint8_t(r.size); c.ok = r.ok;
	return c;
}
}  // namespace bytecode

// ---- the transfer object -----------------------------------------------------------------------
// Owned by the caller (a device driver member). The args array is sized by the template below.

struct Transfer
{
	const uint8_t* code{nullptr};     // the descriptor's bytecode
	Transfer* next{nullptr};          // intrusive queue link
	Completion completion{nullptr};
	void* context{nullptr};           // for the owner
	Result result{};
	Error abortError{Error::Aborted}; // what Action::Abort reports
	// interpreter state
	uint8_t pc{0};                    // offset of the next op
	uint8_t arg{0};                   // next argument slot
	uint8_t groupPc{0}, groupArg{0};  // where the group started
	uint16_t groupLeft{0};            // iterations left after the current one
	bool inGroup : 1 {false};
	bool groupForever : 1 {false};
	bool joinNext : 1 {false};
	bool pingPending : 1 {false};     // an address without data so far: a ping (START, address, STOP)
	uint8_t address{0};
	uint8_t addressPos{0};            // character offset of the last S/s token (the ping's position)
	uint16_t timeoutMs{0};
	uint8_t slotCount{0};
	uintptr_t* args{nullptr};
	// segments prepared in thread context by begin() for descriptors without callbacks:
	// the interrupt then copies a struct instead of running the interpreter (TransferN<Slots, Prepared>)
	Segment* pre{nullptr};
	uint8_t preCapacity{0}, preCount{0}, preIndex{0};
	volatile bool busy{false};

	[[gnu::noinline]] Transfer() {}   // one copy of the member initialization for every driver type

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

template<size_t N, size_t Prepared = 0>
struct TransferN : Transfer
{
	// Neither array is initialized: bind() writes the slots, prepare() the segments, both before
	// they are read. Zeroing them cost every driver type that holds a transfer constructor code.
	union { uintptr_t storage[N ? N : 1]; };
	union { Segment prepared[Prepared ? Prepared : 1]; };
	TransferN() { args = storage; slotCount = N; pre = prepared; preCapacity = Prepared; }
};

// ---- the interpreter ---------------------------------------------------------------------------
// Stateless over a Transfer: next() yields the next Segment, finished()/failed() end the transfer.

struct Interpreter
{
	// Starts a transfer from bytecode; the arguments are already in t.args. 'straight' (no
	// callbacks): the segments are prepared now, in thread context, when they fit. Returns false
	// when the transfer is still busy (Refused: the running result is not touched) or the
	// arguments do not fit (result.error = Descriptor).
	static bool begin(Transfer& t, const uint8_t* code, size_t slots, bool straight)
	{
		if (t.busy) return false;
		if (slots > t.slotCount) return refuse(t, 0);
		t.code = code;
		t.result = {};
		reset(t);
		t.busy = true;
		if (straight) prepare(t);
		return true;
	}
	// a "..."_desc descriptor: checked and compiled at compile time
	template<typename D>
	static bool begin(Transfer& t, D)
	{
		return begin(t, D::code.data(), D::slots, !D::parsed.hasCallback);
	}
	// A descriptor known only at run time: parse() checks it and the argument count, compile()
	// writes its bytecode into 'code', which must live as long as the transfer. On a bad descriptor
	// result.error is Descriptor and result.pos the character where parse() stopped.
	static bool begin(Transfer& t, std::string_view descriptor, size_t argsGiven, uint8_t* code, size_t capacity)
	{
		if (t.busy) return false;   // before touching 'code': the running transfer may use it
		const Parsed p = parse(descriptor);
		if (!p.ok) return refuse(t, p.pos);
		if (p.slots != argsGiven) return refuse(t, uint8_t(descriptor.size()));
		if (!bytecode::compile(descriptor, code, capacity).ok) return refuse(t, uint8_t(descriptor.size()));
		return begin(t, code, p.slots, !p.hasCallback);
	}

	static void reset(Transfer& t)
	{
		t.pc = t.arg = t.groupPc = t.groupArg = 0; t.groupLeft = 0;
		t.inGroup = t.groupForever = false;
		t.joinNext = t.pingPending = false;
		t.timeoutMs = 0;
		t.preCount = t.preIndex = 0;
	}

	// Runs the interpreter now, in thread context, and keeps the segments for the interrupt. Only
	// for descriptors without loops or callbacks (their segments depend on nothing at run time) and
	// only when they fit; otherwise the interrupt interprets as usual.
	static void prepare(Transfer& t)
	{
		if (!t.preCapacity) return;
		uint8_t n = 0;
		for (;;)
		{
			Segment seg = step(t);
			if (seg.kind == Segment::Kind::Done) break;
			if (n == t.preCapacity || (seg.kind != Segment::Kind::Data && seg.kind != Segment::Kind::Stop && seg.kind != Segment::Kind::Delay)) { reset(t); return; }   // does not fit: interpret in the interrupt
			std::construct_at(&t.pre[n++], seg);   // the array is raw storage until here
		}
		if (n && t.pre[n - 1].kind == Segment::Kind::Data && t.pre[n - 1].after == Segment::After::Stop) t.pre[n - 1].last = true;
		t.preCount = n;
		t.preIndex = 0;
	}

	// The next data segment for an engine that does not want to see callbacks: Callback segments are
	// executed here, so only Data, Stop, Pause, Delay, Done and Finished come back. Finished means a callback
	// ended the transfer (the completion has run): the engine releases the bus and calls nothing else.
	// The same without copies: a pointer into the prepared segments, or to 'scratch' (the engine's)
	// when the interpreter runs. Valid until the next call.
	// The prepared case is inlined into the engine (an index and a pointer); everything else is a call.
	[[gnu::always_inline]] static const Segment* nextData(Transfer& t, Segment& scratch)
	{
		if (t.preIndex < t.preCount) [[likely]] return &t.pre[t.preIndex++];
		return interpretData(t, scratch);
	}
	[[gnu::always_inline]] static const Segment* next(Transfer& t, Segment& scratch)
	{
		if (t.preIndex < t.preCount) [[likely]] return &t.pre[t.preIndex++];
		return interpret(t, scratch);
	}
	[[gnu::noinline]] static const Segment* interpretData(Transfer& t, Segment& scratch)
	{
		for (;;)
		{
			const Segment* seg = interpret(t, scratch);
			if (seg->kind != Segment::Kind::Callback) return seg;
			if (!act(t, seg->callback(t))) { scratch.kind = Segment::Kind::Finished; return &scratch; }   // callbacks are never prepared: seg is scratch
		}
	}
	static const Segment* interpret(Transfer& t, Segment& scratch)
	{
		if (t.preCount) { scratch = Segment{}; return &scratch; }   // the prepared segments are used up: Done
		scratch = step(t);
		return &scratch;
	}

	// by value, for tests and simple engines
	static Segment nextData(Transfer& t) { Segment s; return *nextData(t, s); }
	static Segment next(Transfer& t) { Segment s; return *next(t, s); }

	// one step of the interpreter proper
	[[gnu::noinline]] static Segment step(Transfer& t)   // one copy for prepare() and the interrupt
	{
		using namespace bytecode;
		Segment seg;
		const uint8_t* code = t.code;
		for (;;)
		{
			const uint8_t op = code[t.pc];
			const uint8_t at = code[t.pc + 1];   // the token's character position
			if (t.pingPending && op != INLINE && op != WRITE && op != WRITEARG && op != READ && op != READARG && op != TIMEOUT && op != TIMEOUTARG)
			{
				ping(t, seg); seg.last = op == END; return seg;
			}
			t.pc += 2;
			switch (op)
			{
			case ADDR: t.address = code[t.pc++]; t.pingPending = true; t.addressPos = at; break;
			case ADDRARG: t.address = uint8_t(t.args[t.arg++] & 0x7f); t.pingPending = true; t.addressPos = at; break;
			case INLINE: {
				seg.kind = Segment::Kind::Data; seg.pos = at; seg.addressPos = t.addressPos; seg.address = t.address;
				seg.start = !t.joinNext; seg.timeoutMs = t.timeoutMs;
				seg.after = afterFrom(t, code[t.pc++]);
				seg.length = code[t.pc++];
				seg.wbuf = code + t.pc; t.pc += seg.length;
				t.joinNext = false; t.pingPending = false;
				seg.last = code[t.pc] == END && seg.after == Segment::After::Stop; return seg;
			}
			case WRITE: case READ: case WRITEARG: case READARG:
				seg.kind = Segment::Kind::Data; seg.pos = at; seg.addressPos = t.addressPos; seg.address = t.address;
				seg.read = (op == READ || op == READARG); seg.start = !t.joinNext; seg.timeoutMs = t.timeoutMs;
				seg.after = afterFrom(t, code[t.pc++]);
				if (seg.read) seg.rbuf = reinterpret_cast<uint8_t*>(t.args[t.arg++]); else seg.wbuf = reinterpret_cast<const uint8_t*>(t.args[t.arg++]);
				seg.length = (op == WRITE || op == READ) ? code[t.pc++] : uint16_t(t.args[t.arg++]);
				t.joinNext = false; t.pingPending = false;
				seg.last = code[t.pc] == END && seg.after == Segment::After::Stop; return seg;
			case JOIN: t.joinNext = true; break;
			case STOP: seg.kind = Segment::Kind::Stop; seg.pos = at; return seg;
			case CALL: seg.kind = Segment::Kind::Callback; seg.pos = at; seg.callback = reinterpret_cast<Callback>(t.args[t.arg++]); return seg;
			case PAUSE: seg.kind = Segment::Kind::Pause; seg.pos = at; seg.asyncCallback = reinterpret_cast<AsyncCallback>(t.args[t.arg++]); return seg;
			case TIMEOUT: t.timeoutMs = uint16_t(code[t.pc] | code[t.pc + 1] << 8); t.pc += 2; break;
			case TIMEOUTARG: t.timeoutMs = uint16_t(t.args[t.arg++]); break;
			case DELAY: seg.kind = Segment::Kind::Delay; seg.pos = at; seg.length = uint16_t(code[t.pc] | code[t.pc + 1] << 8); t.pc += 2; return seg;
			case DELAYARG: seg.kind = Segment::Kind::Delay; seg.pos = at; seg.length = uint16_t(t.args[t.arg++]); return seg;
			case LOOP: {
				const uint16_t count = uint16_t(code[t.pc] | code[t.pc + 1] << 8);
				const uint8_t slots = code[t.pc + 2]; t.pc += 3;
				t.inGroup = true; t.groupPc = t.pc; t.groupArg = t.arg;
				const uint16_t n = count ? count : uint16_t(t.args[t.arg + slots]);   // the count argument follows the body's
				t.groupForever = (count == 0xffff) || (count == 0 && n == 0);
				t.groupLeft = t.groupForever ? 0 : uint16_t(n - 1);
				break;
			}
			case END_LOOP:
				if (t.groupForever || t.groupLeft) { if (!t.groupForever) t.groupLeft--; t.pc = t.groupPc; t.arg = t.groupArg; }
				else { if (countIsArgument(t)) t.arg++; t.inGroup = false; }
				break;
			case END: default: t.pc -= 2; seg.kind = Segment::Kind::Done; return seg;   // stay on END
			}
		}
	}

	// the data op's precomputed 'after': the half for a group pass that repeats when this one does
	static Segment::After afterFrom(const Transfer& t, uint8_t f)
	{
		const bool repeats = t.inGroup && (t.groupForever || t.groupLeft);
		return Segment::After((f >> (repeats ? 2 : 0)) & 3);
	}
	static bool countIsArgument(const Transfer& t) { return t.code[t.groupPc - 3] == 0 && t.code[t.groupPc - 2] == 0; }


	// The engine reports a finished data segment (ok, or with the error at byte 'index').
	// Returns true when the transfer goes on (the engine then calls next()).
	// The good case is inlined into the engine (an add, and a call only after the last segment).
	[[gnu::always_inline]] static bool dataDone(Transfer& t, const Segment& seg, Error error, uint16_t index)
	{
		if (error == Error::None) [[likely]]
		{
			t.result.count += seg.length;
			return seg.last ? finish(t) : true;
		}
		return dataFailed(t, seg, error, index);
	}
	[[gnu::noinline]] static bool dataFailed(Transfer& t, const Segment& seg, Error error, uint16_t index)
	{
		t.result.count += index;
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
			else { t.pc = 0; t.arg = 0; }
			return true;
		case Action::Break:
			t.result.error = Error::None;
			if (!t.inGroup) return finish(t);   // outside a group the whole string is the group
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
		const uint8_t* code = t.code;
		t.arg = uint8_t(t.groupArg + code[t.groupPc - 1]);   // the body's slots, all consumed or skipped
		size_t i = t.pc;
		while (code[i] != END_LOOP && code[i] != END) i = skipOp(code, i);
		if (code[i] == END_LOOP) { i = skipOp(code, i); if (countIsArgument(t)) t.arg++; }
		t.pc = uint8_t(i);
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
	static bool peekIsCallback(Transfer& t)
	{
		if (t.preCount) return false;   // prepared segments never hold callbacks
		return t.code[t.pc] == bytecode::CALL || t.code[t.pc] == bytecode::PAUSE;
	}
	static bool refuse(Transfer& t, uint8_t pos)
	{
		t.result = {}; t.result.error = Error::Descriptor; t.result.pos = pos; t.result.done = true;
		return false;
	}
};

// ---- compile-time descriptors (C++) ------------------------------------------------------------
// "S2a W01 r*"_desc parses at compile time; transfer() checks the argument count and kinds.

template<size_t N>
struct Literal
{
	char text[N]{};
	constexpr Literal(const char (&s)[N]) { for (size_t i = 0; i < N; i++) text[i] = s[i]; }
	constexpr std::string_view view() const { return {text, N - 1}; }
};

template<Literal L>
struct Descriptor
{
	static constexpr std::string_view text = L.view();
	static constexpr Parsed parsed = parse(text);
	static_assert(parsed.ok, "bad transfer descriptor (see Parsed::message)");
	static constexpr size_t slots = parsed.slots;
	static constexpr bytecode::Code compiled = bytecode::compile(text);
	static_assert(compiled.ok, "descriptor too long for the bytecode form");
	static constexpr auto code = [] {
		std::array<uint8_t, compiled.size> a{};
		for (size_t i = 0; i < compiled.size; i++) a[i] = compiled.bytes[i];
		return a;
	}();
	constexpr const char* c_str() const { return L.text; }   // for messages; the engine never reads it
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
	case 'n': case 'a': case 't': case 'd': return std::is_integral_v<U> || std::is_enum_v<U>;
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
