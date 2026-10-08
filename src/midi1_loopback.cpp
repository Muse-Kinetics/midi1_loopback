// midi1_loopback -- a MIDI 1.0 virtual loopback bus to replace the IAC Driver.
//
// Why this exists (see thestk/rtmidi#320):
//   The macOS IAC Driver operates as a MIDI 2.0 endpoint internally. When a
//   MIDI 1.0 byte stream passes through it, CoreMIDI applies the MIDI 1.0<->2.0
//   default translation rules -- notably, a Note On with velocity 0 (90 nn 00)
//   is rewritten to a Note Off (80 nn 40). That breaks protocols like Mackie
//   Control that require Note On/vel 0 and ignore Note Off.
//
//   RtMidi creates its virtual endpoints with CoreMIDI's legacy MIDIPacketList
//   (MIDI 1.0) API, so an RtMidi source->destination path never triggers
//   protocol conversion. This program turns that into a drop-in IAC
//   replacement: a loopback "bus" that passes bytes through verbatim.
//
// How it works:
//   For each bus it creates two virtual endpoints that share one name:
//     - a virtual DESTINATION (via RtMidiIn::openVirtualPort)  -- apps send here
//     - a virtual SOURCE      (via RtMidiOut::openVirtualPort)  -- apps read here
//   Everything arriving on the destination is forwarded, unmodified, to the
//   source. Other apps see e.g. "RtMidi Loopback Bus 1" as both an available
//   MIDI input and output, exactly like "IAC Driver Bus 1".
//
//   There is no internal feedback loop: our source and destination are separate
//   endpoints and are never auto-connected to each other; only external apps
//   connect to them.
//
// Usage:
//   ./midi1_loopback [numBuses] [options]
//     numBuses             number of buses with default names (1-16)
//     --names "a,b,c"      create one bus per comma-separated name (sets count)
//     --config <path>      load bus definitions from a JSON file
//     --verbose, -v        print every forwarded message as hex
//
//   Precedence for choosing the buses:
//     1. --names "..."          (explicit names win)
//     2. numBuses positional    (default names "RtMidi Loopback Bus N")
//     3. JSON config            (used when no numBuses and no --names given)
//     4. built-in default       (2 buses, if no args and no config file)
//
//   With no port-selecting argument the app loads a JSON config. By default it
//   looks for ./port_config.json then ../port_config.json (so it finds the
//   repo-root file whether launched from the repo root or this subfolder);
//   override the location with --config <path>. Config schema:
//     {
//       "verbose": false,
//       "ports": [ { "name": "Mackie Control" }, { "name": "DAW Sync" } ]
//     }
//
// Build: see README.md. The normal path is CMake, which fetches RtMidi:
//   cmake -S . -B build && cmake --build build
// RtMidi is compiled with its CoreMIDI backend and linked in; this program
// relies on RtMidi using CoreMIDI's legacy (MIDI 1.0) MIDIPacketList API.

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <memory>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <thread>
#include <chrono>
#include <sys/sysctl.h>
#include "RtMidi.h"

static std::atomic<bool> g_running{true};
static void onSignal(int) { g_running = false; }

// Read a sysctl string value by name (e.g. "kern.osproductversion").
static std::string sysctlStr(const char *name) {
    size_t len = 0;
    if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0)
        return "";
    std::string val(len, '\0');
    if (sysctlbyname(name, &val[0], &len, nullptr, 0) != 0)
        return "";
    if (!val.empty() && val.back() == '\0') val.pop_back();
    return val;
}

// Human-readable macOS version, e.g. "macOS 15.3.1 (build 24D...), Darwin 24.3.0".
static std::string macosVersion() {
    std::string product = sysctlStr("kern.osproductversion"); // macOS 11+
    std::string build   = sysctlStr("kern.osversion");        // e.g. 24D60
    std::string darwin  = sysctlStr("kern.osrelease");         // e.g. 24.3.0
    std::string out = "macOS ";
    out += product.empty() ? "(unknown)" : product;
    if (!build.empty())  out += " (build " + build + ")";
    if (!darwin.empty()) out += ", Darwin " + darwin;
    return out;
}

static std::string hex(const std::vector<unsigned char> &m) {
    std::string s;
    char buf[4];
    for (size_t i = 0; i < m.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%02X", m[i]);
        if (i) s += ' ';
        s += buf;
    }
    return s;
}

// One loopback bus: a virtual input (destination) forwarding to a virtual
// output (source) of the same name.
struct Bus {
    std::string name;
    std::unique_ptr<RtMidiIn>  in;
    std::unique_ptr<RtMidiOut> out;
    bool verbose = false;
};

// Forwarded verbatim from destination -> source. CoreMIDI does not translate
// because both endpoints are MIDI 1.0 (legacy MIDIPacketList API).
static void forward(double /*ts*/, std::vector<unsigned char> *msg, void *user) {
    if (!msg || msg->empty()) return;
    Bus *bus = static_cast<Bus *>(user);
    if (bus->verbose)
        std::cout << "[" << bus->name << "] " << hex(*msg) << "\n" << std::flush;
    bus->out->sendMessage(msg);
}

// Find the first input/output port whose name contains "IAC". Returns -1 if none.
template <typename T>
static int findIacPort(T &midi) {
    unsigned n = midi.getPortCount();
    for (unsigned i = 0; i < n; ++i)
        if (midi.getPortName(i).find("IAC") != std::string::npos)
            return static_cast<int>(i);
    return -1;
}

// --iac-test: send a Note On velocity 0 through a real IAC bus and report
// whether CoreMIDI translated it to a Note Off. Prints the macOS version so the
// result can be recorded against a specific OS release (to spot a future fix).
// Returns a process exit code.
static int runIacTest() {
    std::cout << "midi1_loopback --iac-test\n";
    std::cout << "OS: " << macosVersion() << "\n";
    std::cout << "RtMidi: " << RtMidi::getVersion() << "\n\n";

    std::cout << "Purpose: verify whether the macOS IAC Driver rewrites a\n"
                 "MIDI 1.0 \"Note On, velocity 0\" (90 3C 00) into a \"Note Off\"\n"
                 "(80 3C 40) as it passes through.\n\n";

    try {
        RtMidiOut midiOut;
        RtMidiIn  midiIn;
        midiIn.ignoreTypes(false, false, false);

        int outPort = findIacPort(midiOut);
        int inPort  = findIacPort(midiIn);
        if (outPort < 0 || inPort < 0) {
            std::cerr <<
                "RESULT: INCONCLUSIVE -- no IAC Driver port found.\n\n"
                "Enable it in Audio MIDI Setup:\n"
                "  Audio MIDI Setup -> Window -> Show MIDI Studio ->\n"
                "  double-click \"IAC Driver\" -> check \"Device is online\"\n"
                "  and make sure at least one bus exists.\n";
            return 2;
        }

        std::cout << "Using IAC output port [" << outPort << "] "
                  << midiOut.getPortName(outPort) << "\n";
        std::cout << "Using IAC input  port [" << inPort  << "] "
                  << midiIn.getPortName(inPort) << "\n";

        midiOut.openPort(outPort);
        midiIn.openPort(inPort);

        // Let CoreMIDI establish the connection, then drain any setup noise.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::vector<unsigned char> drain;
        do { midiIn.getMessage(&drain); } while (!drain.empty());

        std::vector<unsigned char> noteOnVel0 = {0x90, 0x3C, 0x00};
        std::cout << "\nSent:     " << hex(noteOnVel0)
                  << "   (Note On, note 60, velocity 0)\n";
        midiOut.sendMessage(&noteOnVel0);

        std::vector<unsigned char> rx;
        for (int i = 0; i < 200; ++i) {
            midiIn.getMessage(&rx);
            if (!rx.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (rx.empty()) {
            std::cerr << "\nRESULT: INCONCLUSIVE -- no message came back within "
                         "~2s.\nEnsure the IAC output and input are the same bus "
                         "and the bus is online.\n";
            return 3;
        }

        std::cout << "Received: " << hex(rx) << "\n\n";

        unsigned char status = rx[0] & 0xF0;
        if (status == 0x80) {
            std::cout <<
                "RESULT: TRANSLATED. The IAC Driver converted Note On/velocity 0\n"
                "Use midi1_loopback's virtual buses instead of IAC to avoid this.\n";
            return 0;
        } else if (status == 0x90) {
            std::cout <<
                "RESULT: NOT translated. The IAC Driver delivered Note On/velocity 0\n"
                "unchanged. If this macOS version previously translated, the behavior\n"
                "appears to be FIXED here.\n";
            return 0;
        } else {
            std::cout << "RESULT: UNEXPECTED -- first byte 0x" << std::hex
                      << std::uppercase << (int)rx[0] << std::dec
                      << ", neither Note On (0x9n) nor Note Off (0x8n).\n";
            return 0;
        }
    } catch (RtMidiError &error) {
        std::cerr << "\nRESULT: ERROR -- ";
        error.printMessage();
        return 1;
    }
}

static const int kMaxBuses = 16;

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Split "a, b ,c" into {"a","b","c"}, trimming and dropping empties.
static std::vector<std::string> splitNames(const std::string &csv) {
    std::vector<std::string> out;
    std::string item;
    std::stringstream ss(csv);
    while (std::getline(ss, item, ',')) {
        std::string t = trim(item);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

static void printHelp(const char *prog) {
    std::cout <<
        "midi1_loopback -- MIDI 1.0 virtual loopback buses (a non-translating\n"
        "IAC Driver replacement; passes Note On/velocity 0 through unchanged).\n\n"
        "Usage: " << prog << " [numBuses] [options]\n\n"
        "Arguments:\n"
        "  numBuses              Number of buses with default names\n"
        "                        (\"RtMidi Loopback Bus N\"); range 1-" << kMaxBuses << ".\n\n"
        "Options:\n"
        "  --names \"a,b,c\"       Create one bus per comma-separated name.\n"
        "                        The name list sets the bus count.\n"
        "  --config <path>       Load bus definitions from a JSON file.\n"
        "  --verbose, -v         Print every forwarded message as hex.\n"
        "  --iac-test            Test whether the IAC Driver still translates\n"
        "                        Note On/velocity 0 to Note Off, then exit.\n"
        "  --help, -h            Show this help and exit.\n\n"
        "Port selection precedence:\n"
        "  1. --names \"...\"       Explicit names win.\n"
        "  2. numBuses            Positional count with default names.\n"
        "  3. JSON config         Used when neither of the above is given.\n"
        "  4. Built-in default    2 buses, if no args and no config file.\n\n"
        "Config file:\n"
        "  With no port-selecting argument, looks for ./port_config.json then\n"
        "  ../port_config.json (override with --config <path>). Schema:\n"
        "    {\n"
        "      \"verbose\": false,\n"
        "      \"ports\": [ { \"name\": \"Mackie Control\" }, { \"name\": \"DAW Sync\" } ]\n"
        "    }\n\n"
        "Examples:\n"
        "  " << prog << "                         Use JSON config, else 2 default buses.\n"
        "  " << prog << " 4                       4 buses with default names.\n"
        "  " << prog << " --names \"MCU,Click\"      2 buses named MCU and Click.\n"
        "  " << prog << " --config ports.json -v  Load ports.json, log messages.\n"
        "\nRuns until Ctrl-C.\n";
}

// Minimal JSON reader for our fixed schema. Not a general JSON parser: it scans
// for the "ports" array and pulls each object's "name" string, plus a top-level
// "verbose" boolean. Handles whitespace and \" / \\ escapes inside strings.
// Returns true if the file was read and at least one named port was found.
struct Config {
    std::vector<std::string> names;
    bool verbose = false;
    bool verboseSet = false;
};

// Reads a JSON string literal starting at content[pos] (which must be the
// opening quote). Advances pos past the closing quote. Returns the decoded value.
static std::string readJsonString(const std::string &c, size_t &pos) {
    std::string out;
    ++pos;  // skip opening quote
    while (pos < c.size()) {
        char ch = c[pos++];
        if (ch == '\\' && pos < c.size()) {
            char esc = c[pos++];
            switch (esc) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"';  break;
                case '\\': out += '\\'; break;
                case '/': out += '/';  break;
                default: out += esc;   break;  // leave other escapes as-is
            }
        } else if (ch == '"') {
            break;
        } else {
            out += ch;
        }
    }
    return out;
}

// Find a top-level-ish key by scanning for "key" outside of string literals.
// Returns the index just past the closing quote of the key, or npos.
static size_t findKey(const std::string &c, const std::string &key, size_t from) {
    std::string needle = "\"" + key + "\"";
    return c.find(needle, from);
}

static bool loadConfig(const std::string &path, Config &cfg, std::string &err) {
    std::ifstream f(path);
    if (!f) { err = "could not open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string c = ss.str();

    // Optional top-level "verbose": true/false.
    size_t vpos = findKey(c, "verbose", 0);
    if (vpos != std::string::npos) {
        size_t colon = c.find(':', vpos);
        if (colon != std::string::npos) {
            size_t t = c.find("true", colon);
            size_t fpos = c.find("false", colon);
            // Whichever token appears first after the colon wins.
            if (t != std::string::npos && (fpos == std::string::npos || t < fpos)) {
                cfg.verbose = true; cfg.verboseSet = true;
            } else if (fpos != std::string::npos) {
                cfg.verbose = false; cfg.verboseSet = true;
            }
        }
    }

    // Locate the "ports" array and extract each object's "name".
    size_t ppos = findKey(c, "ports", 0);
    if (ppos == std::string::npos) { err = "no \"ports\" array in " + path; return false; }
    size_t lbracket = c.find('[', ppos);
    size_t rbracket = c.find(']', lbracket == std::string::npos ? ppos : lbracket);
    if (lbracket == std::string::npos || rbracket == std::string::npos) {
        err = "malformed \"ports\" array in " + path; return false;
    }

    size_t scan = lbracket;
    while (true) {
        size_t npos = findKey(c, "name", scan);
        if (npos == std::string::npos || npos > rbracket) break;
        size_t colon = c.find(':', npos);
        if (colon == std::string::npos || colon > rbracket) break;
        size_t q = c.find('"', colon);
        if (q == std::string::npos || q > rbracket) break;
        size_t qp = q;
        std::string name = trim(readJsonString(c, qp));
        if (!name.empty()) cfg.names.push_back(name);
        scan = qp;
    }

    if (cfg.names.empty()) { err = "no port names found in " + path; return false; }
    return true;
}

int main(int argc, char **argv) {
    const char *prog = (argc > 0 && argv[0]) ? argv[0] : "midi1_loopback";

    int numBuses = -1;                 // -1 = not specified on the command line
    bool verbose = false;
    bool verboseFlag = false;          // was --verbose passed explicitly?
    bool iacTest = false;              // --iac-test: run the IAC check and exit
    std::vector<std::string> names;    // from --names (empty = not given)
    std::string configPath;            // from --config (empty = use defaults)

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            printHelp(prog);
            return 0;
        } else if (a == "--iac-test") {
            iacTest = true;
        } else if (a == "--verbose" || a == "-v") {
            verbose = true; verboseFlag = true;
        } else if (a == "--names") {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: --names requires a comma-separated argument.\n";
                return 2;
            }
            names = splitNames(argv[++i]);
            if (names.empty()) {
                std::cerr << "ERROR: --names given but no valid names parsed.\n";
                return 2;
            }
        } else if (a == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: --config requires a path argument.\n";
                return 2;
            }
            configPath = argv[++i];
        } else if (!a.empty() && (a[0] == '-')) {
            std::cerr << "ERROR: unknown option '" << a << "'. Try --help.\n";
            return 2;
        } else {
            int n = std::atoi(a.c_str());
            if (n > 0) numBuses = n;
            else {
                std::cerr << "ERROR: invalid bus count '" << a << "'. Try --help.\n";
                return 2;
            }
        }
    }

    // --iac-test is a standalone diagnostic: run it and exit without creating
    // any loopback buses.
    if (iacTest)
        return runIacTest();

    // Resolve the final list of bus names by precedence.
    std::vector<std::string> busNames;
    if (!names.empty()) {
        // 1. Explicit --names.
        busNames = names;
    } else if (numBuses > 0) {
        // 2. Positional count with default names.
        for (int i = 0; i < numBuses; ++i)
            busNames.push_back("RtMidi Loopback Bus " + std::to_string(i + 1));
    } else {
        // 3. No port-selecting argument -> try a JSON config file.
        std::vector<std::string> candidates;
        if (!configPath.empty()) candidates.push_back(configPath);
        else { candidates.push_back("port_config.json"); candidates.push_back("../port_config.json"); }

        Config cfg; std::string err; bool loaded = false;
        for (const auto &path : candidates) {
            if (loadConfig(path, cfg, err)) {
                std::cout << "Loaded config from " << path << "\n";
                busNames = cfg.names;
                if (cfg.verboseSet && !verboseFlag) verbose = cfg.verbose;
                loaded = true;
                break;
            }
        }
        if (!loaded) {
            if (!configPath.empty()) {
                // User asked for a specific file; failing to load it is an error.
                std::cerr << "ERROR: " << err << "\n";
                return 2;
            }
            // 4. Built-in default.
            std::cout << "No port_config.json found; using 2 default buses.\n";
            busNames = {"RtMidi Loopback Bus 1", "RtMidi Loopback Bus 2"};
        }
    }

    if (busNames.empty()) {
        std::cerr << "ERROR: no buses to create.\n";
        return 2;
    }
    if ((int)busNames.size() > kMaxBuses) {
        std::cerr << "WARNING: " << busNames.size() << " buses requested; "
                  << "capping at " << kMaxBuses << ".\n";
        busNames.resize(kMaxBuses);
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::vector<std::unique_ptr<Bus>> buses;

    try {
        for (size_t i = 0; i < busNames.size(); ++i) {
            auto bus = std::unique_ptr<Bus>(new Bus());
            bus->name    = busNames[i];
            bus->verbose = verbose;
            bus->in.reset(new RtMidiIn(RtMidi::UNSPECIFIED, "midi1_loopback"));
            bus->out.reset(new RtMidiOut(RtMidi::UNSPECIFIED, "midi1_loopback"));

            // Pass everything through, including sysex / timing / active sensing.
            bus->in->ignoreTypes(false, false, false);

            // Create the two virtual endpoints that share the bus name.
            bus->out->openVirtualPort(bus->name);   // virtual SOURCE  (apps read)
            bus->in->openVirtualPort(bus->name);     // virtual DEST    (apps send)

            // setCallback stores the userData pointer; bus lives for the
            // program's lifetime so the pointer stays valid.
            bus->in->setCallback(&forward, bus.get());

            buses.push_back(std::move(bus));
        }
    } catch (RtMidiError &error) {
        error.printMessage();
        return 1;
    }

    std::cout << "midi1_loopback: created " << buses.size()
              << " MIDI 1.0 loopback bus(es):\n";
    for (auto &b : buses)
        std::cout << "  - \"" << b->name
                  << "\"  (appears as both a MIDI input and output)\n";
    std::cout << "\nThese pass Note On/velocity 0 through unchanged -- no "
                 "CoreMIDI Note Off translation, unlike IAC.\n";
    std::cout << "Point your apps at these ports instead of the IAC Driver.\n";
    std::cout << (verbose ? "Verbose mode on.\n" : "")
              << "Press Ctrl-C to quit.\n" << std::flush;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nShutting down; closing virtual ports.\n";
    buses.clear();  // destructors cancel callbacks and close the ports
    return 0;
}
