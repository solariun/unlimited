// Host side of the AVR ISR gate (spec 3.9, 8). For every case of isr_cases.hpp it runs the image built from
// isr_harness.cpp (avr-objdump listing, flash image and symbol table) on a cycle-counting ATmega328P interpreter, one
// ISR call per sample, and checks:
//   - every sample equals the host encoder's (the interpreter and the AVR build agree with the reference);
//   - Timer2 at 8 kHz (2000 cycles per tick, one pending flag) loses no tick;
//   - the longest ISR and the mean ISR load stay within k_max_isr_cycles and k_max_load.
// The interpreter knows the instructions avr-gcc emits for the encoder; an unknown one stops it with its name.
// Cycle counts are the ATmega328P's (AVRe core, 16-bit PC).
// Usage: isr_cycles PREFIX   (reads PREFIX_N.dis, PREFIX_N.bin and PREFIX_N.sym, N = 0..k_cases - 1)

#include "isr_cases.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

using unlimited::Encoder;
using unlimited::EncoderConfig;

// Gate.
const double k_cpu_hz = 16e6;
const uint32_t k_tick_cycles = 2000;          // 16 MHz / 8 kHz
const uint32_t k_max_isr_cycles = 1600;       // 80 % of a tick: the next tick is never late by a whole tick
const double k_max_load = 0.5;                // the rest of the CPU is loop()'s: serial input, the queue, PTT
const uint32_t k_isr_entry_cycles = 4 + 3;    // interrupt response and the vector's jmp
const uint32_t k_main_cycles_after_reti = 1;  // an ISR cannot start before one instruction of main has run
const double k_ms_per_s = 1e3;
const uint32_t k_us_per_ms = 1000;

// ATmega328P data space.
const uint16_t k_data_size = 0x900;
const uint16_t k_io_offset = 0x20;
const uint16_t k_sreg = 0x5F;
const uint16_t k_spl = 0x5D;  // SPH follows at 0x5E
const uint16_t k_gpior0 = 0x3E;
const uint16_t k_stack_top = 0x8FF;
const uint8_t k_done = 0xAA;
const uint8_t k_failed = 0xEE;
const uint32_t k_data_address_base = 0x800000;  // avr-nm prints SRAM symbols at 0x800000 + address
const uint64_t k_step_limit = 4000000000ull;    // a runaway program stops the check

enum Flag { flag_c, flag_z, flag_n, flag_v, flag_s, flag_h, flag_t, flag_i };

enum class Op : uint8_t {
    invalid, add, adc, sub, sbc, subi, sbci, cp, cpc, cpi, cpse, and_, andi, or_, ori, eor, com, neg, inc, dec,
    lsr, ror, asr, mov, movw, ldi, lds, sts, ld, st, lpm, push, pop, in, out, sbrs, sbrc, branch, rjmp, jmp,
    call, rcall, icall, ijmp, ret, reti, mul, muls, mulsu, adiw, sbiw, clt, set, cli, sei, nop, bst, bld, swap
};

enum class Condition : uint8_t { ne, eq, cc, cs, pl, mi, ge, lt, tc, ts, vc, vs };

// ld/st/lpm operand: X, Y or Z, plain, post-increment, pre-decrement or with a displacement.
enum class Pointer : uint8_t { plain, post_increment, pre_decrement };

struct Instruction {
    Op op;
    Condition condition;
    Pointer pointer;
    uint8_t d;          // destination register (or the pointer register for st)
    uint8_t r;          // source register, or the pointer register for ld / lpm
    int32_t k;          // immediate, address, bit or displacement
    uint32_t target;    // branch / jump / call target (byte address)
    uint8_t size;       // bytes
    std::string mnemonic;
};

// Instruction cycle counts (AVR instruction set manual, AVRe).
const uint32_t k_cycles_plain = 1;
const uint32_t k_cycles_memory = 2;       // lds, sts, ld, ldd, st, std, push, pop
const uint32_t k_cycles_ld_decrement = 3;
const uint32_t k_cycles_lpm = 3;
const uint32_t k_cycles_taken = 2;        // branch taken, rjmp, ijmp, mul, adiw, sbiw
const uint32_t k_cycles_jmp = 3;
const uint32_t k_cycles_call = 4;
const uint32_t k_cycles_rcall = 3;
const uint32_t k_cycles_ret = 4;

struct Program {
    std::vector<Instruction> code;  // indexed by byte address / 2
    std::vector<uint8_t> flash;
    uint32_t isr = 0;
    uint16_t sample = 0;
};

bool read_text(const std::string& path, std::string& text) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) return false;
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    const size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

uint8_t register_of(const std::string& operand) {
    return static_cast<uint8_t>(std::strtoul(operand.c_str() + 1, nullptr, 10));
}

int32_t number_of(const std::string& operand) {
    return static_cast<int32_t>(std::strtol(operand.c_str(), nullptr, 0));
}

uint8_t pointer_register(char name) {
    const uint8_t k_x = 26;
    const uint8_t k_y = 28;
    const uint8_t k_z = 30;
    return name == 'X' ? k_x : (name == 'Y' ? k_y : k_z);
}

// "X", "X+", "-X", "Y+12", "Z"
void parse_pointer(const std::string& operand, Instruction& in, uint8_t& reg) {
    in.pointer = Pointer::plain;
    in.k = 0;
    if (operand[0] == '-') {
        in.pointer = Pointer::pre_decrement;
        reg = pointer_register(operand[1]);
        return;
    }
    reg = pointer_register(operand[0]);
    if (operand.size() == 2 && operand[1] == '+') in.pointer = Pointer::post_increment;
    if (operand.size() > 2) in.k = number_of(operand.substr(2));
}

struct Mnemonic {
    const char* name;
    Op op;
};

const Mnemonic k_mnemonics[] = {
    {"add", Op::add},     {"adc", Op::adc},     {"lsl", Op::add},     {"rol", Op::adc},     {"sub", Op::sub},
    {"sbc", Op::sbc},     {"subi", Op::subi},   {"sbci", Op::sbci},   {"cp", Op::cp},       {"cpc", Op::cpc},
    {"cpi", Op::cpi},     {"cpse", Op::cpse},   {"and", Op::and_},    {"andi", Op::andi},   {"or", Op::or_},
    {"ori", Op::ori},     {"eor", Op::eor},     {"com", Op::com},     {"neg", Op::neg},     {"inc", Op::inc},
    {"dec", Op::dec},     {"lsr", Op::lsr},     {"ror", Op::ror},     {"asr", Op::asr},     {"mov", Op::mov},
    {"movw", Op::movw},   {"ldi", Op::ldi},     {"lds", Op::lds},     {"sts", Op::sts},     {"ld", Op::ld},
    {"ldd", Op::ld},      {"st", Op::st},       {"std", Op::st},      {"lpm", Op::lpm},     {"push", Op::push},
    {"pop", Op::pop},     {"in", Op::in},       {"out", Op::out},     {"sbrs", Op::sbrs},   {"sbrc", Op::sbrc},
    {"rjmp", Op::rjmp},   {"jmp", Op::jmp},     {"call", Op::call},   {"rcall", Op::rcall}, {"icall", Op::icall},
    {"ijmp", Op::ijmp},   {"ret", Op::ret},     {"reti", Op::reti},   {"mul", Op::mul},     {"muls", Op::muls},
    {"mulsu", Op::mulsu}, {"adiw", Op::adiw},   {"sbiw", Op::sbiw},   {"clt", Op::clt},     {"set", Op::set},
    {"cli", Op::cli},     {"sei", Op::sei},     {"nop", Op::nop},     {"bst", Op::bst},     {"bld", Op::bld},
    {"swap", Op::swap}};

struct Branch {
    const char* name;
    Condition condition;
};

const Branch k_branches[] = {{"brne", Condition::ne}, {"breq", Condition::eq}, {"brcc", Condition::cc},
                             {"brsh", Condition::cc}, {"brcs", Condition::cs}, {"brlo", Condition::cs},
                             {"brpl", Condition::pl}, {"brmi", Condition::mi}, {"brge", Condition::ge},
                             {"brlt", Condition::lt}, {"brtc", Condition::tc}, {"brts", Condition::ts},
                             {"brvc", Condition::vc}, {"brvs", Condition::vs}};

// One avr-objdump -d line: "  addr:\tbytes\tmnemonic\toperands\t; comment".
bool parse_line(const std::string& line, uint32_t& address, Instruction& in) {
    std::vector<std::string> fields;
    std::string field;
    std::istringstream stream(line);
    while (std::getline(stream, field, '\t')) fields.push_back(field);
    if (fields.size() < 3) return false;
    const std::string head = trim(fields[0]);
    if (head.empty() || head.back() != ':') return false;
    char* end = nullptr;
    address = static_cast<uint32_t>(std::strtoul(head.c_str(), &end, 16));
    if (end != head.c_str() + head.size() - 1) return false;
    std::istringstream bytes(fields[1]);
    std::string byte;
    in = Instruction();
    while (bytes >> byte) ++in.size;
    in.mnemonic = trim(fields[2]);
    std::vector<std::string> operands;
    std::string comment;
    for (size_t f = 3; f < fields.size(); ++f) {
        const std::string text = trim(fields[f]);
        if (text.empty()) continue;
        if (text[0] == ';') {
            comment = text.substr(1);
            continue;
        }
        std::istringstream list(text);
        std::string operand;
        while (std::getline(list, operand, ',')) operands.push_back(trim(operand));
    }
    for (const Branch& branch : k_branches) {
        if (in.mnemonic != branch.name) continue;
        in.op = Op::branch;
        in.condition = branch.condition;
    }
    for (const Mnemonic& mnemonic : k_mnemonics) {
        if (in.mnemonic == mnemonic.name) in.op = mnemonic.op;
    }
    // Jump targets: the listing's comment holds the absolute address of relative ones.
    const std::string trimmed_comment = trim(comment);
    if (!trimmed_comment.empty() && trimmed_comment.compare(0, 2, "0x") == 0) {
        in.target = static_cast<uint32_t>(std::strtoul(trimmed_comment.c_str(), nullptr, 16));
    } else if (!operands.empty() && operands[0].compare(0, 2, "0x") == 0) {
        in.target = static_cast<uint32_t>(std::strtoul(operands[0].c_str(), nullptr, 16));
    }
    switch (in.op) {
    case Op::add: case Op::adc: case Op::sub: case Op::sbc: case Op::cp: case Op::cpc: case Op::cpse:
    case Op::and_: case Op::or_: case Op::eor: case Op::mov: case Op::movw: case Op::mul: case Op::muls:
    case Op::mulsu:
        if (operands.empty()) return false;
        in.d = register_of(operands[0]);
        in.r = operands.size() > 1 ? register_of(operands[1]) : in.d;  // lsl / rol: one operand
        break;
    case Op::subi: case Op::sbci: case Op::cpi: case Op::andi: case Op::ori: case Op::ldi: case Op::adiw:
    case Op::sbiw: case Op::sbrs: case Op::sbrc: case Op::bst: case Op::bld:
        if (operands.size() < 2) return false;
        in.d = register_of(operands[0]);
        in.k = number_of(operands[1]);
        break;
    case Op::com: case Op::neg: case Op::inc: case Op::dec: case Op::lsr: case Op::ror: case Op::asr:
    case Op::push: case Op::pop: case Op::swap:
        if (operands.empty()) return false;
        in.d = register_of(operands[0]);
        break;
    case Op::lds:
    case Op::in:
        if (operands.size() < 2) return false;
        in.d = register_of(operands[0]);
        in.k = number_of(operands[1]);
        break;
    case Op::sts:
    case Op::out:
        if (operands.size() < 2) return false;
        in.k = number_of(operands[0]);
        in.r = register_of(operands[1]);
        break;
    case Op::ld:
        if (operands.size() < 2) return false;
        in.d = register_of(operands[0]);
        parse_pointer(operands[1], in, in.r);
        break;
    case Op::st:
        if (operands.size() < 2) return false;
        parse_pointer(operands[0], in, in.d);
        in.r = register_of(operands[1]);
        break;
    case Op::lpm:
        in.d = 0;  // plain lpm loads r0 from (Z)
        in.pointer = Pointer::plain;
        if (operands.size() >= 2) {
            in.d = register_of(operands[0]);
            in.pointer = operands[1] == "Z+" ? Pointer::post_increment : Pointer::plain;
        }
        break;
    default:
        break;
    }
    return true;
}

class Machine {
public:
    explicit Machine(const Program& program) : program_(program), data_(k_data_size, 0) {
        set_pair(k_spl, k_stack_top);
    }

    // Runs until GPIOR0 reports the end; `isr_cycles` receives each ISR's cycles and `samples` its sample.
    bool run(std::vector<uint32_t>& isr_cycles, std::vector<int16_t>& samples, std::string& error) {
        uint64_t cycles = 0;
        uint64_t isr_start = 0;
        uint16_t isr_stack = 0;
        bool in_isr = false;
        for (uint64_t step = 0; step < k_step_limit; ++step) {
            if (data_[k_gpior0] == k_done) return true;
            if (data_[k_gpior0] == k_failed) {
                error = "the encoder refused the configuration";
                return false;
            }
            if (pc_ == program_.isr && !in_isr) {
                in_isr = true;
                isr_start = cycles;
                isr_stack = pair(k_spl);
            }
            const size_t index = pc_ / 2;
            if (index >= program_.code.size() || program_.code[index].op == Op::invalid) {
                char text[96];
                const std::string name = index < program_.code.size() ? program_.code[index].mnemonic : "";
                std::snprintf(text, sizeof(text), "no instruction the interpreter knows at 0x%x (%s)", pc_, name.c_str());
                error = text;
                return false;
            }
            const Instruction& in = program_.code[index];
            const bool returned = in.op == Op::reti;
            cycles += execute(in);
            if (in_isr && returned && pair(k_spl) == isr_stack + 2) {
                in_isr = false;
                isr_cycles.push_back(static_cast<uint32_t>(cycles - isr_start));
                samples.push_back(static_cast<int16_t>(data_[program_.sample] | (data_[program_.sample + 1] << 8)));
            }
        }
        error = "no end after the step limit";
        return false;
    }

private:
    bool flag(Flag f) const { return ((data_[k_sreg] >> f) & 1u) != 0; }

    void set_flag(Flag f, bool value) {
        data_[k_sreg] = static_cast<uint8_t>(value ? data_[k_sreg] | (1u << f) : data_[k_sreg] & ~(1u << f));
    }

    uint16_t pair(uint16_t low) const { return static_cast<uint16_t>(data_[low] | (data_[low + 1] << 8)); }

    void set_pair(uint16_t low, uint32_t value) {
        data_[low] = static_cast<uint8_t>(value);
        data_[low + 1] = static_cast<uint8_t>(value >> 8);
    }

    uint8_t read(uint32_t address) const { return address < data_.size() ? data_[address] : 0; }

    void write(uint32_t address, uint8_t value) {
        if (address < data_.size()) data_[address] = value;
    }

    void push(uint8_t value) {
        const uint16_t sp = pair(k_spl);
        write(sp, value);
        set_pair(k_spl, static_cast<uint16_t>(sp - 1));
    }

    uint8_t pop() {
        const uint16_t sp = static_cast<uint16_t>(pair(k_spl) + 1);
        set_pair(k_spl, sp);
        return read(sp);
    }

    // Effective address of a pointer operand, applying its increment or decrement.
    uint16_t address_of(const Instruction& in, uint8_t reg) {
        uint16_t value = pair(reg);
        if (in.pointer == Pointer::pre_decrement) {
            value = static_cast<uint16_t>(value - 1);
            set_pair(reg, value);
            return value;
        }
        if (in.pointer == Pointer::post_increment) {
            set_pair(reg, static_cast<uint16_t>(value + 1));
            return value;
        }
        return static_cast<uint16_t>(value + in.k);
    }

    uint8_t add_flags(uint8_t a, uint8_t b, bool carry) {
        const unsigned sum = a + b + (carry ? 1u : 0u);
        const uint8_t r = static_cast<uint8_t>(sum);
        set_flag(flag_h, ((a & 0xF) + (b & 0xF) + (carry ? 1u : 0u)) > 0xF);
        set_flag(flag_c, sum > 0xFF);
        set_flag(flag_n, (r & 0x80) != 0);
        set_flag(flag_v, ((a ^ r) & (b ^ r) & 0x80) != 0);
        set_flag(flag_s, flag(flag_n) != flag(flag_v));
        set_flag(flag_z, r == 0);
        return r;
    }

    // keep_zero: sbc / sbci / cpc clear Z on a non-zero result and leave it otherwise.
    uint8_t subtract_flags(uint8_t a, uint8_t b, bool carry, bool keep_zero) {
        const int difference = a - b - (carry ? 1 : 0);
        const uint8_t r = static_cast<uint8_t>(difference);
        set_flag(flag_h, (a & 0xF) - (b & 0xF) - (carry ? 1 : 0) < 0);
        set_flag(flag_c, difference < 0);
        set_flag(flag_n, (r & 0x80) != 0);
        set_flag(flag_v, ((a ^ b) & (a ^ r) & 0x80) != 0);
        set_flag(flag_s, flag(flag_n) != flag(flag_v));
        if (!keep_zero || r != 0) set_flag(flag_z, r == 0);
        return r;
    }

    uint8_t logic_flags(uint8_t r) {
        set_flag(flag_v, false);
        set_flag(flag_n, (r & 0x80) != 0);
        set_flag(flag_s, flag(flag_n));
        set_flag(flag_z, r == 0);
        return r;
    }

    void shift_flags(uint8_t before, uint8_t r) {
        set_flag(flag_c, (before & 1u) != 0);
        set_flag(flag_n, (r & 0x80) != 0);
        set_flag(flag_v, flag(flag_n) != flag(flag_c));
        set_flag(flag_s, flag(flag_n) != flag(flag_v));
        set_flag(flag_z, r == 0);
    }

    bool condition(Condition c) const {
        switch (c) {
        case Condition::ne: return !flag(flag_z);
        case Condition::eq: return flag(flag_z);
        case Condition::cc: return !flag(flag_c);
        case Condition::cs: return flag(flag_c);
        case Condition::pl: return !flag(flag_n);
        case Condition::mi: return flag(flag_n);
        case Condition::ge: return !flag(flag_s);
        case Condition::lt: return flag(flag_s);
        case Condition::tc: return !flag(flag_t);
        case Condition::ts: return flag(flag_t);
        case Condition::vc: return !flag(flag_v);
        case Condition::vs: return flag(flag_v);
        }
        return false;
    }

    uint32_t skip_next(uint32_t next) {
        const size_t index = next / 2;
        const uint8_t size = index < program_.code.size() ? program_.code[index].size : 2;
        pc_ = next + size;
        return size > 2 ? k_cycles_jmp : k_cycles_taken;
    }

    void call(uint32_t return_address, uint32_t target) {
        const uint32_t word = return_address / 2;
        push(static_cast<uint8_t>(word));
        push(static_cast<uint8_t>(word >> 8));
        pc_ = target;
    }

    uint32_t execute(const Instruction& in) {
        const uint32_t next = pc_ + in.size;
        pc_ = next;
        uint8_t* reg = &data_[0];
        switch (in.op) {
        case Op::add: reg[in.d] = add_flags(reg[in.d], reg[in.r], false); break;
        case Op::adc: reg[in.d] = add_flags(reg[in.d], reg[in.r], flag(flag_c)); break;
        case Op::sub: reg[in.d] = subtract_flags(reg[in.d], reg[in.r], false, false); break;
        case Op::sbc: reg[in.d] = subtract_flags(reg[in.d], reg[in.r], flag(flag_c), true); break;
        case Op::subi: reg[in.d] = subtract_flags(reg[in.d], static_cast<uint8_t>(in.k), false, false); break;
        case Op::sbci: reg[in.d] = subtract_flags(reg[in.d], static_cast<uint8_t>(in.k), flag(flag_c), true); break;
        case Op::cp: subtract_flags(reg[in.d], reg[in.r], false, false); break;
        case Op::cpc: subtract_flags(reg[in.d], reg[in.r], flag(flag_c), true); break;
        case Op::cpi: subtract_flags(reg[in.d], static_cast<uint8_t>(in.k), false, false); break;
        case Op::cpse:
            if (reg[in.d] == reg[in.r]) return skip_next(next);
            break;
        case Op::and_: reg[in.d] = logic_flags(reg[in.d] & reg[in.r]); break;
        case Op::andi: reg[in.d] = logic_flags(reg[in.d] & static_cast<uint8_t>(in.k)); break;
        case Op::or_: reg[in.d] = logic_flags(reg[in.d] | reg[in.r]); break;
        case Op::ori: reg[in.d] = logic_flags(reg[in.d] | static_cast<uint8_t>(in.k)); break;
        case Op::eor: reg[in.d] = logic_flags(reg[in.d] ^ reg[in.r]); break;
        case Op::com:
            reg[in.d] = logic_flags(static_cast<uint8_t>(~reg[in.d]));
            set_flag(flag_c, true);
            break;
        case Op::neg: reg[in.d] = subtract_flags(0, reg[in.d], false, false); break;
        case Op::inc:
        case Op::dec: {
            const uint8_t a = reg[in.d];
            const uint8_t r = static_cast<uint8_t>(in.op == Op::inc ? a + 1 : a - 1);
            set_flag(flag_v, in.op == Op::inc ? a == 0x7F : a == 0x80);
            set_flag(flag_n, (r & 0x80) != 0);
            set_flag(flag_s, flag(flag_n) != flag(flag_v));
            set_flag(flag_z, r == 0);
            reg[in.d] = r;
            break;
        }
        case Op::lsr:
        case Op::ror:
        case Op::asr: {
            const uint8_t a = reg[in.d];
            uint8_t r = static_cast<uint8_t>(a >> 1);
            if (in.op == Op::ror && flag(flag_c)) r = static_cast<uint8_t>(r | 0x80);
            if (in.op == Op::asr) r = static_cast<uint8_t>(r | (a & 0x80));
            shift_flags(a, r);
            reg[in.d] = r;
            break;
        }
        case Op::mov: reg[in.d] = reg[in.r]; break;
        case Op::movw:
            reg[in.d] = reg[in.r];
            reg[in.d + 1] = reg[in.r + 1];
            break;
        case Op::ldi: reg[in.d] = static_cast<uint8_t>(in.k); break;
        case Op::lds: reg[in.d] = read(static_cast<uint32_t>(in.k)); return k_cycles_memory;
        case Op::sts: write(static_cast<uint32_t>(in.k), reg[in.r]); return k_cycles_memory;
        case Op::ld:
            reg[in.d] = read(address_of(in, in.r));
            return in.pointer == Pointer::pre_decrement ? k_cycles_ld_decrement : k_cycles_memory;
        case Op::st: write(address_of(in, in.d), reg[in.r]); return k_cycles_memory;
        case Op::lpm: {
            const uint16_t z = pair(pointer_register('Z'));
            reg[in.d] = z < program_.flash.size() ? program_.flash[z] : 0;
            if (in.pointer == Pointer::post_increment) set_pair(pointer_register('Z'), static_cast<uint16_t>(z + 1));
            return k_cycles_lpm;
        }
        case Op::push: push(reg[in.d]); return k_cycles_memory;
        case Op::pop: reg[in.d] = pop(); return k_cycles_memory;
        case Op::in: reg[in.d] = read(static_cast<uint32_t>(in.k) + k_io_offset); break;
        case Op::out: write(static_cast<uint32_t>(in.k) + k_io_offset, reg[in.r]); break;
        case Op::sbrs:
        case Op::sbrc: {
            const bool set = ((reg[in.d] >> in.k) & 1u) != 0;
            if (set == (in.op == Op::sbrs)) return skip_next(next);
            break;
        }
        case Op::branch:
            if (condition(in.condition)) {
                pc_ = in.target;
                return k_cycles_taken;
            }
            break;
        case Op::rjmp: pc_ = in.target; return k_cycles_taken;
        case Op::jmp: pc_ = in.target; return k_cycles_jmp;
        case Op::call: call(next, in.target); return k_cycles_call;
        case Op::rcall: call(next, in.target); return k_cycles_rcall;
        case Op::icall: call(next, static_cast<uint32_t>(pair(pointer_register('Z'))) * 2); return k_cycles_rcall;
        case Op::ijmp: pc_ = static_cast<uint32_t>(pair(pointer_register('Z'))) * 2; return k_cycles_taken;
        case Op::ret:
        case Op::reti: {
            const uint8_t high = pop();
            const uint8_t low = pop();
            pc_ = static_cast<uint32_t>((high << 8) | low) * 2;
            if (in.op == Op::reti) set_flag(flag_i, true);
            return k_cycles_ret;
        }
        case Op::mul:
        case Op::muls:
        case Op::mulsu: {
            int a = reg[in.d];
            int b = reg[in.r];
            if (in.op != Op::mul && (a & 0x80) != 0) a -= 0x100;
            if (in.op == Op::muls && (b & 0x80) != 0) b -= 0x100;
            const uint16_t r = static_cast<uint16_t>(a * b);
            set_flag(flag_c, (r & 0x8000) != 0);
            set_flag(flag_z, r == 0);
            reg[0] = static_cast<uint8_t>(r);
            reg[1] = static_cast<uint8_t>(r >> 8);
            return k_cycles_taken;
        }
        case Op::adiw:
        case Op::sbiw: {
            const uint16_t a = pair(in.d);
            const int wide = in.op == Op::adiw ? a + in.k : a - in.k;
            const uint16_t r = static_cast<uint16_t>(wide);
            set_flag(flag_c, in.op == Op::adiw ? wide > 0xFFFF : wide < 0);
            set_flag(flag_v, in.op == Op::adiw ? ((a & 0x8000) == 0 && (r & 0x8000) != 0)
                                                : ((a & 0x8000) != 0 && (r & 0x8000) == 0));
            set_flag(flag_n, (r & 0x8000) != 0);
            set_flag(flag_s, flag(flag_n) != flag(flag_v));
            set_flag(flag_z, r == 0);
            set_pair(in.d, r);
            return k_cycles_taken;
        }
        case Op::clt: set_flag(flag_t, false); break;
        case Op::set: set_flag(flag_t, true); break;
        case Op::cli: set_flag(flag_i, false); break;
        case Op::sei: set_flag(flag_i, true); break;
        case Op::nop: break;
        case Op::bst: set_flag(flag_t, ((reg[in.d] >> in.k) & 1u) != 0); break;
        case Op::bld:
            reg[in.d] = static_cast<uint8_t>((reg[in.d] & ~(1u << in.k)) | (flag(flag_t) ? 1u << in.k : 0u));
            break;
        case Op::swap: reg[in.d] = static_cast<uint8_t>((reg[in.d] >> 4) | (reg[in.d] << 4)); break;
        case Op::invalid: break;
        }
        return k_cycles_plain;
    }

    const Program& program_;
    std::vector<uint8_t> data_;
    uint32_t pc_ = 0;
};

bool load_program(const std::string& base, Program& program, std::string& error) {
    std::string listing;
    std::string flash;
    std::string symbols;
    if (!read_text(base + ".dis", listing) || !read_text(base + ".bin", flash) || !read_text(base + ".sym", symbols)) {
        error = "cannot read " + base + ".dis/.bin/.sym";
        return false;
    }
    program.flash.assign(flash.begin(), flash.end());
    std::istringstream lines(listing);
    std::string line;
    while (std::getline(lines, line)) {
        uint32_t address = 0;
        Instruction in;
        if (!parse_line(line, address, in)) continue;
        const size_t index = address / 2;
        if (index >= program.code.size()) program.code.resize(index + 1);
        program.code[index] = in;
    }
    bool isr = false;
    bool sample = false;
    std::istringstream table(symbols);
    while (std::getline(table, line)) {
        std::istringstream fields(line);
        std::string value;
        std::string type;
        std::string name;
        if (!(fields >> value >> type >> name)) continue;
        const uint32_t address = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 16));
        if (name == "__vector_7") {
            program.isr = address;
            isr = true;
        }
        if (name == "avr_isr_sample") {
            program.sample = static_cast<uint16_t>(address - k_data_address_base);
            sample = true;
        }
    }
    if (!isr || !sample) error = base + ".sym has no __vector_7 or avr_isr_sample";
    return isr && sample;
}

const char* const k_segment_names[] = {"idle", "lead-in", "vox-lead", "gap", "window", "tail"};

// The host encoder's samples, and the segment each one belongs to.
std::vector<int16_t> reference_samples(uint8_t index, std::vector<uint8_t>& segments) {
    Encoder encoder(unlimited::avr_isr::case_config(index));
    uint8_t sent = 0;
    const auto refill = [&]() {
        while (sent < unlimited::avr_isr::k_bytes && encoder.write(unlimited::avr_isr::case_byte(sent))) ++sent;
    };
    refill();
    std::vector<int16_t> samples;
    if (!encoder.start()) return samples;
    while (encoder.busy()) {
        refill();
        segments.push_back(static_cast<uint8_t>(encoder.status().segment));
        samples.push_back(encoder.next_sample());
    }
    return samples;
}

struct Ticks {
    uint32_t lost = 0;
    uint64_t longest_busy = 0;  // cycles of the longest run of back-to-back ISRs
};

// Timer2 in CTC mode: a tick every k_tick_cycles sets OCF2A; the ISR clears it on entry, and a tick that finds it
// still set is lost. The main loop runs one instruction between two ISRs.
Ticks timer_ticks(const std::vector<uint32_t>& isr_cycles) {
    Ticks ticks;
    uint64_t next_tick = 0;
    uint64_t busy_until = 0;
    uint64_t run_start = 0;
    bool pending = false;
    size_t i = 0;
    while (i < isr_cycles.size()) {
        if (!pending) {
            pending = true;
            next_tick += k_tick_cycles;
            continue;
        }
        const uint64_t tick = next_tick - k_tick_cycles;
        const uint64_t start = std::max(tick, busy_until);
        if (start > busy_until) run_start = start;
        while (next_tick <= start) {
            ++ticks.lost;
            next_tick += k_tick_cycles;
        }
        pending = false;
        const uint64_t end = start + k_isr_entry_cycles + isr_cycles[i++];
        busy_until = end + k_main_cycles_after_reti;
        ticks.longest_busy = std::max(ticks.longest_busy, end - run_start);
        bool first = true;
        while (next_tick <= end) {
            if (first) {
                pending = true;
                first = false;
            } else {
                ++ticks.lost;
            }
            next_tick += k_tick_cycles;
        }
    }
    return ticks;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: isr_cycles PREFIX\n");
        return 2;
    }
    const std::string prefix = argv[1];
    bool ok = true;
    for (uint8_t c = 0; c < unlimited::avr_isr::k_cases; ++c) {
        const std::string base = prefix + "_" + std::to_string(c);
        Program program;
        std::string error;
        std::vector<uint32_t> isr_cycles;
        std::vector<int16_t> samples;
        if (!load_program(base, program, error) || !Machine(program).run(isr_cycles, samples, error)) {
            std::printf("isr_cycles case %u: %s\n", unsigned(c), error.c_str());
            ok = false;
            continue;
        }
        std::vector<uint8_t> segments;
        const std::vector<int16_t> reference = reference_samples(c, segments);
        size_t mismatches = reference.size() == samples.size() ? 0 : std::max(reference.size(), samples.size());
        for (size_t n = 0; n < std::min(reference.size(), samples.size()); ++n) mismatches += samples[n] != reference[n];
        uint64_t total = 0;
        uint32_t longest = 0;
        size_t longest_at = 0;
        for (size_t n = 0; n < isr_cycles.size(); ++n) {
            total += isr_cycles[n];
            if (isr_cycles[n] > longest) {
                longest = isr_cycles[n];
                longest_at = n;
            }
        }
        const char* longest_segment = longest_at < segments.size() ? k_segment_names[segments[longest_at]] : "?";
        const double mean = isr_cycles.empty() ? 0.0 : static_cast<double>(total) / isr_cycles.size();
        const double load = (mean + k_isr_entry_cycles) / k_tick_cycles;
        const Ticks ticks = timer_ticks(isr_cycles);
        const bool pass = mismatches == 0 && ticks.lost == 0 && longest <= k_max_isr_cycles && load <= k_max_load;
        const EncoderConfig config = unlimited::avr_isr::case_config(c);
        std::printf("isr_cycles case %u (%.2f bytes/s, T %.3f ms, %u Hz%s): %zu samples, %zu mismatches, ISR mean %.0f "
                    "max %u cycles (sample %zu, %s), load %.1f %%, lost ticks %u, longest busy %.2f ms: %s\n",
                    unsigned(c), static_cast<double>(unlimited::bytes_per_second(config.slot_us)),
                    static_cast<double>(config.slot_us) / k_us_per_ms, unsigned(config.tone_hz),
                    config.vox_lead_ms != 0 ? ", VOX lead" : "", samples.size(), mismatches, mean, longest, longest_at,
                    longest_segment, 100.0 * load, ticks.lost, ticks.longest_busy * k_ms_per_s / k_cpu_hz,
                    pass ? "PASS" : "FAIL");
        ok = ok && pass;
    }
    std::printf("isr_cycles: gate max %u cycles per sample, mean load <= %.0f %%, no lost tick: %s\n", k_max_isr_cycles,
                100.0 * k_max_load, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
