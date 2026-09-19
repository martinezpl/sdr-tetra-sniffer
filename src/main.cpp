#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <dsp/channel/rx_vfo.h>
#include "allocations.h"
#include "demod_chain.h"
#include "radio.h"
#include "sweep.h"

int child_main(int fd, uint32_t hz, const std::string& dir, const std::string& start_utc, double iq_rate,
	       Allocations* allocations, int voice_fd, double span_center_hz, bool per_carrier,
	       size_t slot);
int stitch_main(int fd, const std::string& dir, size_t max_gssi);

// No correction by default. Every receiver has some frequency error, but it
// belongs to that receiver, so "sweep" measures it and prints the value to use.
static const double DEFAULT_TUNE_OFFSET = 0;
static const double DEFAULT_RATE = 3200000;
static const char* DEFAULT_OUT = "recordings";
static const size_t DEFAULT_MAX_GSSI = 256;
// The pool of carrier slots. The measured cost is about 1.6% of one core in
// the channelizer of the parent for each one, plus about 1.5% in its child.
static const size_t DEFAULT_MAX_CARRIERS = 15;
// One health line every five minutes. A month of running is then 8640 lines.
static const double DEFAULT_STATUS = 300;
static const size_t DEFAULT_QUEUE_BLOCKS = 400;

static const char* USAGE =
	"tetra-sniff - an unattended recorder for clear TETRA speech.\n"
	"\n"
	"usage:\n"
	"  tetra-sniff run --carriers HZ,HZ,... [options]\n"
	"  tetra-sniff run [options] DL_HZ DL_HZ ...\n"
	"  tetra-sniff sweep [options]\n"
	"  tetra-sniff help\n"
	"\n"
	"\"run\" opens the receiver, follows every carrier in the carrier list and\n"
	"writes each clear call to its own WAV file. Ctrl-C, SIGTERM or the end of the\n"
	"input stops the run. Every log line goes to stdout, one line at a time, so a\n"
	"redirect works in the background:\n"
	"\n"
	"  ./tetra-sniff run > tetra-sniff.log 2>&1 &\n"
	"\n"
	"Each run makes one directory, DIR/<start_utc>/, which holds:\n"
	"  calls/<GSSI>.wav   the speech of one talkgroup, 8 kHz mono s16, no silence\n"
	"  calls.log          one line for each decoded voice frame, and one for\n"
	"                     each clear call that this run could not record:\n"
	"                       UNTUNED  no slot followed that carrier yet, and a\n"
	"                                free slot has now taken it\n"
	"                       NOSLOT   no slot followed it and the pool is full\n"
	"                       OUTSIDE  the carrier lies outside the captured span,\n"
	"                                so no slot can reach it. Move --center. The\n"
	"                                default rate is already the highest the\n"
	"                                receiver takes, so the span cannot widen\n"
	"                       BADFREQ  the grant named a frequency megahertz away\n"
	"                                from the span, so that grant decoded wrong.\n"
	"                                Nothing can be done about it\n"
	"  timemap.log        anchors that tie a WAV position to a capture sample\n"
	"  clock.log          UTC against the sample counter, one line each second\n"
	"\n"
	"radio options:\n"
	"  --center HZ        Centre frequency of the receiver. Default: the midpoint\n"
	"                     of the carrier list, which always holds every carrier\n"
	"                     in it. Give one to leave room on one side for a\n"
	"                     carrier that a grant has yet to name. A baseband WAV\n"
	"                     file gives its own centre, and raw replay input needs\n"
	"                     this option, because the centre of a capture is a\n"
	"                     fact about the file and not a choice.\n"
	"  --carriers LIST    Downlink carrier frequencies in Hz, separated by\n"
	"                     commas. Required, and it has no default: the carriers\n"
	"                     of one network mean nothing on another. \"sweep\" finds\n"
	"                     them.\n"
	"                     The list must hold every control carrier, because\n"
	"                     every grant arrives on one of those, and a free slot\n"
	"                     of the pool then follows each traffic carrier that a\n"
	"                     grant names. Naming a traffic carrier as well is only\n"
	"                     a convenience: a slot is already on it when its first\n"
	"                     call starts, so the head of that call survives the\n"
	"                     time a retune costs.\n"
	"                     DL_HZ arguments name the same list. Give one form or\n"
	"                     the other, not both.\n"
	"  --tune-offset HZ   Correction for the frequency error of the receiver, which\n"
	"                     is the amount its own oscillator is off by. It shifts\n"
	"                     where a carrier is looked for and does not change the\n"
	"                     carrier identity in the logs. \"sweep\" measures it and\n"
	"                     prints the value to use. Default 0.\n"
	"  --rate HZ          Sample rate of the receiver, and so the width of one\n"
	"                     span. \"run\" defaults to 3200000. \"sweep\" instead asks\n"
	"                     for a rate that holds the TETRA band (or a narrower\n"
	"                     --band) in one span, with a little extra so a stick\n"
	"                     that snaps or lists a coarse rate still covers, and\n"
	"                     reports it. Give this only to hold it below that.\n"
	"                     Watch the dropped column of clock.log if the host\n"
	"                     cannot keep up.\n"
	"  --gain DB          Tuner gain of 0 to 100 dB, or \"auto\". The tuner takes\n"
	"                     the nearest gain that it supports, and the start line\n"
	"                     reports the gain that it took. Default auto.\n"
	"  --device INDEX     Index of the receiver. Default 0.\n"
	"                     A live run auto-detects the receiver through SoapySDR.\n"
	"                     If Soapy finds none, USB is scanned for a known stick.\n"
	"                     No stick prints \"no SDR found\". A known stick without\n"
	"                     its Soapy module prints \"<name> found, install <module>\".\n"
	"  --rx CHANNEL       RX channel of the receiver. Default 0.\n"
	"  --antenna NAME     RX antenna of the receiver (LNAL, LNAH, LNAW, ...).\n"
	"                     Default: the driver default.\n"
	"\n"
	"output options:\n"
	"  --out DIR          Parent directory for the run directories. Default\n"
	"                     \"recordings\", below the current directory.\n"
	"  --per-carrier      Also decode and write one WAV file and one log file for\n"
	"                     each carrier, beside the calls. This costs much more CPU\n"
	"                     time, because it decodes the speech a second time.\n"
	"                     Default off.\n"
	"  --max-gssi N       Limit on the number of talkgroup writer processes. A\n"
	"                     corrupt GSSI on the air cannot then exhaust the process\n"
	"                     table. Default 256.\n"
	"\n"
	"carrier pool options:\n"
	"  --max-carriers N   Size of the carrier pool. --carriers seeds it, and each\n"
	"                     free slot waits for a control carrier to grant a clear\n"
	"                     call on a carrier that no slot follows. A slot is never\n"
	"                     taken back, because a retune costs the head of a call\n"
	"                     and a carrier that the network used once it uses again.\n"
	"                     Each slot costs about 1.6% of one core. Default 15.\n"
	"  --no-learn         Do not read or write DIR/carriers. That file remembers\n"
	"                     the carriers that a grant revealed, so a restart keeps\n"
	"                     them instead of learning them again.\n"
	"\n"
	"replay options (no receiver):\n"
	"  --iq FILE          Read IQ samples from FILE. \"-\" means stdin. An SDR++\n"
	"                     baseband WAV gives its own format, rate, centre and\n"
	"                     start time. Raw input needs --fmt, --rate and --center.\n"
	"  --fmt FORMAT       cf32, cs16, cu8 or cs8.\n"
	"  --start-utc TIME   Start time as YYYY-MM-DDTHH:MM:SSZ. It names the run\n"
	"                     directory and anchors the wall-clock map.\n"
	"\n"
	"tuning options:\n"
	"  --queue-blocks N   Depth of the queue between the receiver and the carriers,\n"
	"                     in blocks. The receiver makes about 400 blocks each\n"
	"                     second, so the default holds one second. A full queue\n"
	"                     drops the new block and counts it in clock.log.\n"
	"                     Default 400.\n"
	"  --status SECONDS   How often one health line goes to stdout: how long the\n"
	"                     run is, the queue depth, and the blocks lost since the\n"
	"                     line before it. Zero turns the line off. Default 300.\n"
	"\n"
	"The VFO rate is 36000 S/s and the VFO bandwidth is 30000 Hz. The TETRA symbol\n"
	"rate fixes both, so neither is an option.\n"
	"\n"
	"\"sweep\" finds the carriers of a network, so that \"run\" can be given a real\n"
	"carrier list. It measures the power of every channel of the raster, then puts\n"
	"a demodulator on each peak. A peak that reaches frame lock is a TETRA carrier.\n"
	"One that also broadcasts system information is a control carrier. It prints a\n"
	"table and a ready \"run\" command line.\n"
	"\n"
	"A power scan sees a carrier only while it transmits. A control carrier always\n"
	"transmits, so a sweep finds every one of them. An idle traffic carrier sends\n"
	"nothing, so a sweep misses it, and \"run\" reports it later as a grant that the\n"
	"carrier list does not hold. Treat a sweep as the start of a list, not the end.\n"
	"\n"
	"sweep options:\n"
	"  --band LOW:HIGH    The band to search, in Hz. Default 380000000:430000000,\n"
	"                     the whole of the spectrum that TETRA is given below\n"
	"                     470 MHz. A band wider than one span takes several, and\n"
	"                     the default band takes a few minutes. Narrow it once\n"
	"                     the carriers are known.\n"
	"  --step HZ          Raster of the power scan. Default 12500, which finds a\n"
	"                     carrier on either alignment of the 25 kHz TETRA raster.\n"
	"  --threshold DB     How far a peak must stand above the noise floor of its\n"
	"                     own span. Default 6.\n"
	"  --scan SECONDS     Power measurement for each group of channels.\n"
	"                     Default 0.2.\n"
	"  --dwell SECONDS    Decode time on the candidates. A strong carrier locks in\n"
	"                     0.2 to 1.7 s, and a weak one can need far longer.\n"
	"                     Default 15.\n"
	"  --max-carriers N   How many candidates the decode stage takes, strongest\n"
	"                     first. Default 15.\n"
	"\n"
	"\"sweep\" also takes --rate, --gain, --device, --rx, --antenna and --tune-offset, with the\n"
	"same defaults as \"run\".\n"
	"\n"
	"Close SDR++ before a run if it holds the receiver.\n";

enum class Fmt { cf32, cs16, cu8, cs8 };

struct Args {
	std::string iq;
	std::string fmt;
	double rate = 0, center = 0, tune_offset = DEFAULT_TUNE_OFFSET;
	std::string out = DEFAULT_OUT;
	std::string start_utc;
	std::vector<uint32_t> hz;
	int device = 0;
	int rx = 0;
	const char* antenna = nullptr;
	// Below zero means the automatic gain of the tuner.
	double gain_db = -1;
	bool per_carrier = false;
	size_t max_gssi = DEFAULT_MAX_GSSI;
	size_t max_carriers = DEFAULT_MAX_CARRIERS;
	bool learn = true;
	size_t queue_blocks = DEFAULT_QUEUE_BLOCKS;
	double status = DEFAULT_STATUS;
};

struct IqSource {
	int fd = -1;
	Radio* radio = nullptr;
	Fmt fmt = Fmt::cu8;
	double rate, center;
	time_t start;
	std::string run_dir;
	std::vector<uint8_t> pending;
};

[[noreturn]] static void die(const std::string& msg)
{
	std::cerr << "tetra-sniff: " << msg << "\n";
	exit(2);
}

// strtoul takes a leading minus and wraps it, and it reports an overflow only
// through errno. Every whole number of the CLI goes through this one function.
static unsigned long parse_ulong(const std::string& s, unsigned long limit, const std::string& what)
{
	if (s.empty() || (!isdigit((unsigned char)s[0]))) die(what + " needs a whole number, not " + s);
	errno = 0;
	char* end;
	unsigned long v = strtoul(s.c_str(), &end, 10);
	if (*end || errno == ERANGE || v > limit) die(what + " is out of range: " + s);
	return v;
}

static double parse_double(const std::string& s, const std::string& what)
{
	errno = 0;
	char* end;
	double v = strtod(s.c_str(), &end);
	if (s.empty() || *end || errno == ERANGE || !std::isfinite(v))
		die(what + " needs a number, not " + s);
	return v;
}

static uint32_t parse_hz(const std::string& s)
{
	unsigned long v = parse_ulong(s, 0xffffffffUL, "frequency");
	if (!v) die("frequency 0 is not a carrier");
	return (uint32_t)v;
}

// A carrier list is comma separated. A space also separates, so a quoted list works.
static std::vector<uint32_t> parse_carriers(const std::string& s)
{
	std::vector<uint32_t> hz;
	std::string item;
	for (size_t i = 0; i <= s.size(); i++) {
		if (i < s.size() && s[i] != ',' && !isspace((unsigned char)s[i])) {
			item += s[i];
			continue;
		}
		if (!item.empty()) hz.push_back(parse_hz(item));
		item.clear();
	}
	if (hz.empty()) die("--carriers holds no frequency");
	return hz;
}

static size_t parse_count(const std::string& s, const char* what)
{
	unsigned long v = parse_ulong(s, 1000000UL, what);
	if (!v) die(std::string(what) + " needs a whole number above zero");
	return (size_t)v;
}

static SweepArgs parse_sweep_args(int argc, char** argv)
{
	// The whole of the spectrum that TETRA is given below 470 MHz, so a sweep
	// with no arguments finds a network wherever it sits in it. A rate of 0
	// means the widest span that still holds that allocation.
	SweepArgs s = { TETRA_BAND_LO, TETRA_BAND_HI, 0, 12500, DEFAULT_TUNE_OFFSET,
			-1, 0, 0.2, 15, 6, 15, 0, nullptr };
	for (int i = 1; i < argc; i++) {
		std::string t = argv[i];
		auto val = [&]() -> std::string { if (++i >= argc) die(t + " needs a value"); return argv[i]; };
		if (t == "--help" || t == "-h") { std::cout << USAGE; exit(0); }
		else if (t == "--band") {
			std::string b = val();
			size_t colon = b.find(':');
			if (colon == std::string::npos) die("--band needs LOW:HIGH");
			s.band_lo = parse_double(b.substr(0, colon), "--band");
			s.band_hi = parse_double(b.substr(colon + 1), "--band");
		}
		else if (t == "--step") s.step = parse_double(val(), "--step");
		else if (t == "--threshold") s.threshold_db = parse_double(val(), "--threshold");
		else if (t == "--scan") s.scan = parse_double(val(), "--scan");
		else if (t == "--dwell") s.dwell = parse_double(val(), "--dwell");
		else if (t == "--max-carriers") s.max_carriers = parse_count(val(), "--max-carriers");
		else if (t == "--rate") s.rate = parse_double(val(), "--rate");
		else if (t == "--tune-offset") s.tune_offset = parse_double(val(), "--tune-offset");
		else if (t == "--device") s.device = (int)parse_ulong(val(), 255, "--device");
		else if (t == "--rx") s.rx = (int)parse_ulong(val(), 255, "--rx");
		else if (t == "--antenna") {
			if (++i >= argc) die(t + " needs a value");
			s.antenna = argv[i];
		}
		else if (t == "--gain") {
			std::string g = val();
			s.gain_db = g == "auto" ? -1 : parse_double(g, "--gain");
			if (g != "auto" && (s.gain_db < 0 || s.gain_db > 100))
				die("--gain needs a gain of 0 to 100 dB, or \"auto\"");
		}
		else die("unknown option " + t + "\nRun \"tetra-sniff help\" for the options.");
	}
	if (s.step <= 0) die("--step needs a step above zero");
	if (s.scan <= 0 || s.dwell <= 0) die("--scan and --dwell need a time above zero");
	return s;
}

static Args parse_args(int argc, char** argv)
{
	Args a;
	// --carriers and positional frequencies both name the carrier list. Two
	// lists in one command line cannot be merged without a silent loss, so
	// the parser refuses them together.
	bool list_from_flag = false, saw_positional = false;
	for (int i = 1; i < argc; i++) {
		std::string s = argv[i];
		auto val = [&]() -> std::string { if (++i >= argc) die(s + " needs a value"); return argv[i]; };
		if (s == "--help" || s == "-h") { std::cout << USAGE; exit(0); }
		else if (s == "--iq") a.iq = val();
		else if (s == "--fmt") a.fmt = val();
		else if (s == "--rate") a.rate = parse_double(val(), "--rate");
		else if (s == "--center") a.center = parse_double(val(), "--center");
		else if (s == "--tune-offset") a.tune_offset = parse_double(val(), "--tune-offset");
		else if (s == "--out") a.out = val();
		else if (s == "--start-utc") a.start_utc = val();
		else if (s == "--carriers") {
			a.hz = parse_carriers(val());
			list_from_flag = true;
		}
		else if (s == "--device") a.device = (int)parse_ulong(val(), 255, "--device");
		else if (s == "--rx") a.rx = (int)parse_ulong(val(), 255, "--rx");
		else if (s == "--antenna") {
			if (++i >= argc) die(s + " needs a value");
			a.antenna = argv[i];
		}
		else if (s == "--gain") {
			std::string g = val();
			a.gain_db = g == "auto" ? -1 : parse_double(g, "--gain");
			if (g != "auto" && (a.gain_db < 0 || a.gain_db > 100))
				die("--gain needs a gain of 0 to 100 dB, or \"auto\"");
		}
		else if (s == "--per-carrier") a.per_carrier = true;
		else if (s == "--max-gssi") a.max_gssi = parse_count(val(), "--max-gssi");
		else if (s == "--queue-blocks") a.queue_blocks = parse_count(val(), "--queue-blocks");
		else if (s == "--status") a.status = parse_double(val(), "--status");
		else if (s == "--max-carriers") a.max_carriers = parse_count(val(), "--max-carriers");
		else if (s == "--no-learn") a.learn = false;
		else if (s.size() > 1 && s[0] == '-' && !isdigit((unsigned char)s[1]))
			die("unknown option " + s + "\nRun \"tetra-sniff help\" for the options.");
		else {
			saw_positional = true;
			a.hz.push_back(parse_hz(s));
		}
	}
	if (list_from_flag && saw_positional)
		die("give the carriers with --carriers, or as DL_HZ arguments, but not both");
	if (a.hz.empty())
		die("give at least one carrier, with --carriers or as DL_HZ arguments.\n"
		    "The list must hold every control carrier of the network, because every\n"
		    "grant arrives on one of those. Run \"tetra-sniff sweep\" to find them.");
	std::vector<uint32_t> u = a.hz;
	std::sort(u.begin(), u.end());
	if (std::adjacent_find(u.begin(), u.end()) != u.end()) die("duplicate frequency");
	if (!a.fmt.empty() && a.fmt != "cf32" && a.fmt != "cs16" && a.fmt != "cu8" && a.fmt != "cs8")
		die("bad --fmt " + a.fmt);
	if (!a.start_utc.empty()) {
		struct tm t = {};
		if (!strptime(a.start_utc.c_str(), "%Y-%m-%dT%H:%M:%SZ", &t)) die("bad --start-utc " + a.start_utc);
	}
	return a;
}

static void read_exact(int fd, void* p, size_t n, const char* what)
{
	char* c = (char*)p;
	while (n) {
		ssize_t r = read(fd, c, n);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) die(std::string("short read in ") + what);
		c += r; n -= r;
	}
}

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t* p) { return p[0] | p[1] << 8; }

static std::string utc(time_t t, const char* fmt)
{
	char b[32];
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(b, sizeof b, fmt, &tm);
	return b;
}

static bool parse_sdrpp_name(const std::string& path, double& center, time_t& start)
{
	std::string base = path.substr(path.find_last_of('/') + 1);
	unsigned hz;
	struct tm t = {};
	if (sscanf(base.c_str(), "baseband_%uHz_%d-%d-%d_%d-%d-%d", &hz, &t.tm_hour, &t.tm_min, &t.tm_sec,
		   &t.tm_mday, &t.tm_mon, &t.tm_year) != 7)
		return false;
	t.tm_mon -= 1;
	t.tm_year -= 1900;
	t.tm_isdst = -1;
	center = hz;
	start = mktime(&t);
	return true;
}

static void finish_source(IqSource& s, const Args& a)
{
	if (!s.rate) die("raw input needs --rate");
	if (!s.center) die("raw input needs --center");
	for (uint32_t hz : a.hz)
		if (std::fabs((double)hz + a.tune_offset - s.center) + 15e3 >= s.rate / 2)
			die(std::to_string(hz) + " is outside the captured span");
	if (!a.start_utc.empty()) {
		struct tm t = {};
		strptime(a.start_utc.c_str(), "%Y-%m-%dT%H:%M:%SZ", &t);
		s.start = timegm(&t);
	}
	if (!s.start) {
		s.start = time(nullptr);
		// The Pi has no RTC battery. A clock before NTP must not name a run directory.
		if (s.start < 1767225600) die("system clock is before 2026-01-01; wait for NTP");
	}
	if (mkdir(a.out.c_str(), 0755) && errno != EEXIST) die(a.out + ": " + strerror(errno));
	s.run_dir = a.out + "/" + utc(s.start, "%Y-%m-%dT%H%M%SZ");
	if (mkdir(s.run_dir.c_str(), 0755)) die(s.run_dir + ": " + strerror(errno));
}

static IqSource open_rtl(const Args& a)
{
	IqSource s;
	s.fmt = Fmt::cu8;
	s.rate = a.rate ? a.rate : DEFAULT_RATE;
	s.center = a.center;
	s.start = 0;
	if (!s.center) {
		// The midpoint of the carrier list always holds every carrier in it, so
		// --center is only worth giving to leave room for one not yet known.
		auto lo = *std::min_element(a.hz.begin(), a.hz.end());
		auto hi = *std::max_element(a.hz.begin(), a.hz.end());
		s.center = ((double)lo + (double)hi) / 2 + a.tune_offset;
	}
	finish_source(s, a);
	return s;
}

static IqSource open_iq(const Args& a)
{
	if (a.iq.empty()) return open_rtl(a);
	IqSource s;
	s.fd = a.iq == "-" ? 0 : open(a.iq.c_str(), O_RDONLY);
	if (s.fd < 0) die(a.iq + ": " + strerror(errno));
	s.rate = a.rate;
	s.center = a.center;
	s.start = 0;
	bool have_fmt = !a.fmt.empty();
	if (have_fmt) s.fmt = a.fmt == "cf32" ? Fmt::cf32 : a.fmt == "cs16" ? Fmt::cs16 : a.fmt == "cu8" ? Fmt::cu8 : Fmt::cs8;

	uint8_t h[12];
	read_exact(s.fd, h, 12, "IQ header");
	if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
		for (;;) {
			uint8_t ch[8];
			read_exact(s.fd, ch, 8, "WAV chunk");
			uint32_t len = le32(ch + 4);
			if (!memcmp(ch, "data", 4)) break;
			std::vector<uint8_t> body(len + (len & 1));
			read_exact(s.fd, body.data(), body.size(), "WAV chunk body");
			if (memcmp(ch, "fmt ", 4) || len < 16) continue;
			uint16_t tag = le16(&body[0]), chans = le16(&body[2]), bits = le16(&body[14]);
			if (chans != 2) die("WAV must have 2 channels (I/Q)");
			if (tag == 1 && bits == 8) s.fmt = Fmt::cu8;
			else if (tag == 1 && bits == 16) s.fmt = Fmt::cs16;
			else if (tag == 3 && bits == 32) s.fmt = Fmt::cf32;
			else die("unsupported WAV sample format");
			have_fmt = true;
			s.rate = le32(&body[4]);
		}
		double name_center;
		time_t name_start;
		if (a.iq != "-" && parse_sdrpp_name(a.iq, name_center, name_start)) {
			if (!s.center) s.center = name_center;
			s.start = name_start;
		}
	} else {
		s.pending.assign(h, h + 12);
	}
	if (!have_fmt) die("raw input needs --fmt");
	finish_source(s, a);
	return s;
}

// The carriers that grants revealed, kept beside the recordings. A restart at
// the UTC day boundary would otherwise forget them and lose the head of the
// first call on each one all over again.
static std::vector<uint32_t> read_learned(const std::string& path)
{
	std::vector<uint32_t> hz;
	FILE* f = fopen(path.c_str(), "r");
	if (!f) return hz;
	char line[64];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#') continue;
		unsigned long v = strtoul(line, nullptr, 10);
		if (v && v <= 0xffffffffUL) hz.push_back((uint32_t)v);
	}
	fclose(f);
	return hz;
}

static void write_learned(const std::string& path, const Allocations& alloc,
			  const std::set<uint32_t>& learned)
{
	std::string tmp = path + ".tmp";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f) return;
	fprintf(f, "# tetra-sniff remembers the carriers that a grant revealed.\n"
		   "# Delete this file to forget them. --no-learn stops it being written.\n");
	// A grant revealed the ones marked "learned". The rest came from the command
	// line. strtoul stops at the space, so an older reader still parses this.
	for (size_t i = 0; i < alloc.slots(); i++)
		if (uint32_t hz = alloc.assigned(i))
			fprintf(f, "%u%s\n", hz, learned.count(hz) ? " learned" : "");
	fclose(f);
	// A rename keeps the file whole if the run ends part way through a write.
	rename(tmp.c_str(), path.c_str());
}

static volatile sig_atomic_t stop_flag = 0;
static void on_signal(int) { stop_flag = 1; }

// --status sets this before the run starts. Zero turns the status line off.
static double status_seconds = DEFAULT_STATUS;

static bool write_all(int fd, const void* p, size_t n)
{
	const char* c = (const char*)p;
	while (n) {
		ssize_t w = write(fd, c, n);
		if (w < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		c += w; n -= w;
	}
	return true;
}

// systemd watchdog. One datagram to $NOTIFY_SOCKET. A leading @ names an abstract socket.
// Nothing happens outside systemd, because the variable is absent.
static void sd_notify(const char* msg)
{
	const char* path = getenv("NOTIFY_SOCKET");
	if (!path || !*path) return;
	struct sockaddr_un addr = {};
	size_t n = strlen(path);
	if (n >= sizeof addr.sun_path) return;
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, path, n);
	if (addr.sun_path[0] == '@') addr.sun_path[0] = '\0';
	int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0) return;
	socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n);
	sendto(fd, msg, strlen(msg), 0, (struct sockaddr*)&addr, len);
	close(fd);
}

// The day rotation restarts the unit at 00:00 UTC, so a call across the boundary is cut.
// Name the run before this one. The cut then reads as an explained gap.
static void log_previous_run(int fd, const std::string& out, const std::string& run_dir)
{
	std::string self = run_dir.substr(run_dir.find_last_of('/') + 1);
	DIR* dir = opendir(out.c_str());
	if (!dir) return;
	std::string prev;
	while (struct dirent* e = readdir(dir)) {
		std::string name = e->d_name;
		// The run names are fixed width, so the previous run is the greatest below this one.
		if (name.size() == self.size() && name < self && name > prev) prev = name;
	}
	closedir(dir);
	if (prev.empty()) return;
	// Report how far that run reached, from the last line of its own clock.log.
	std::string last;
	if (FILE* f = fopen((out + "/" + prev + "/clock.log").c_str(), "r")) {
		char b[300];
		while (fgets(b, sizeof b, f))
			if (b[0] != '#') last = b;
		fclose(f);
		if (!last.empty() && last.back() == '\n') last.pop_back();
	}
	char line[600];
	int len = snprintf(line, sizeof line, "# PREV run=%s reached: %s\n", prev.c_str(),
			   last.empty() ? "(no clock.log)" : last.c_str());
	write(fd, line, len);
}

// clock.log ties UTC to the real VFO sample count. It shows a stall and a clock step.
static void clock_tick(int fd, uint64_t vfo_samples, size_t queue, uint64_t dropped)
{
	static time_t last = 0;
	static double first_skew = 0;
	static bool have_skew = false;
	static uint64_t last_samples = 0;
	struct timespec rt, mono;
	clock_gettime(CLOCK_REALTIME, &rt);
	clock_gettime(CLOCK_MONOTONIC, &mono);
	// CLOCK_MONOTONIC paces the line. A step of CLOCK_REALTIME must not stop the log.
	if (mono.tv_sec == last) return;
	last = mono.tv_sec;
	double skew = (double)(rt.tv_sec - mono.tv_sec) + (rt.tv_nsec - mono.tv_nsec) / 1e9;
	if (!have_skew) { first_skew = skew; have_skew = true; }
	const char* event = "OK";
	// The two clocks separate only if something sets CLOCK_REALTIME.
	if (std::fabs(skew - first_skew) > 1.0) { event = "STEP"; first_skew = skew; }
	char line[200];
	int len = snprintf(line, sizeof line, "%s.%03dZ %llu %zu %llu %s\n",
			   utc(rt.tv_sec, "%Y-%m-%dT%H:%M:%S").c_str(), (int)(rt.tv_nsec / 1000000),
			   (unsigned long long)vfo_samples, queue, (unsigned long long)dropped, event);
	write(fd, line, len);
	// The watchdog is a liveness check, not a heartbeat. Silence if no sample arrived.
	if (vfo_samples > last_samples) sd_notify("WATCHDOG=1");
	last_samples = vfo_samples;

	// One health line for the log. clock.log holds a line each second, but a
	// person reads this one, so it says how long the run is and what it lost.
	static time_t first_mono = 0, last_status = 0;
	static uint64_t status_dropped = 0;
	if (!first_mono) { first_mono = mono.tv_sec; last_status = mono.tv_sec; }
	if (status_seconds <= 0 || mono.tv_sec - last_status < (time_t)status_seconds) return;
	last_status = mono.tv_sec;
	long up = (long)(mono.tv_sec - first_mono);
	printf("tetra-sniff: up %ldh%02ldm  queue %zu  dropped %llu (+%llu)  samples %llu\n",
	       up / 3600, (up % 3600) / 60, queue, (unsigned long long)dropped,
	       (unsigned long long)(dropped - status_dropped), (unsigned long long)vfo_samples);
	status_dropped = dropped;
}

static void to_cf32(Fmt fmt, const uint8_t* raw, int n, dsp::complex_t* out)
{
	switch (fmt) {
	case Fmt::cf32: memcpy(out, raw, n * 8); break;
	case Fmt::cs16: { auto* p = (const int16_t*)raw; for (int i = 0; i < n; i++) out[i] = { p[2 * i] / 32768.0f, p[2 * i + 1] / 32768.0f }; break; }
	case Fmt::cu8: for (int i = 0; i < n; i++) out[i] = { (raw[2 * i] - 127.4f) / 128.0f, (raw[2 * i + 1] - 127.4f) / 128.0f }; break;
	case Fmt::cs8: { auto* p = (const int8_t*)raw; for (int i = 0; i < n; i++) out[i] = { p[2 * i] / 128.0f, p[2 * i + 1] / 128.0f }; break; }
	}
}

int main(int argc, char** argv)
{
	// Line buffered stdout. A redirect then gets each log line at once, and a fork
	// cannot copy a half written line into the child.
	setvbuf(stdout, nullptr, _IOLBF, 0);
	// Each forked process keeps this disposition. A dead pipe gives EPIPE, not death.
	signal(SIGPIPE, SIG_IGN);
	if (argc < 2) {
		std::cerr << USAGE;
		return 2;
	}
	std::string cmd = argv[1];
	if (cmd == "help" || cmd == "--help" || cmd == "-h") {
		std::cout << USAGE;
		return 0;
	}
	if (cmd == "sweep") return sweep_main(parse_sweep_args(argc - 1, argv + 1));
	if (cmd != "run") die("unknown command " + cmd + "\nRun \"tetra-sniff help\" for the commands.");
	Args a = parse_args(argc - 1, argv + 1);
	status_seconds = a.status;
	IqSource src = open_iq(a);

	// The carrier list seeds the pool. A free slot waits for a control carrier
	// to grant a clear call on a carrier that no slot follows. This must settle
	// before the shared table is sized, because the table cannot grow later.
	std::string learn_path = a.out + "/carriers";
	std::string learned_note;
	if (a.learn && !a.per_carrier) {
		size_t before = a.hz.size();
		size_t unreachable = 0;
		for (uint32_t hz : read_learned(learn_path)) {
			if (std::find(a.hz.begin(), a.hz.end(), hz) != a.hz.end() ||
			    a.hz.size() >= a.max_carriers)
				continue;
			// --center or --rate can have moved since that carrier was learned.
			// A command line carrier outside the span is a mistake and stops the
			// run; a remembered one is stale, so drop it and say so.
			if (std::fabs((double)hz + a.tune_offset - src.center) + 15e3 >= src.rate / 2) {
				unreachable++;
				continue;
			}
			a.hz.push_back(hz);
		}
		if (a.hz.size() > before)
			learned_note = "tetra-sniff: " + std::to_string(a.hz.size() - before) +
				       " carrier(s) remembered from " + learn_path + "\n";
		if (unreachable)
			learned_note += "tetra-sniff: " + std::to_string(unreachable) +
					" remembered carrier(s) fall outside this span and were"
					" dropped\n";
	}
	size_t pool = a.max_carriers;
	if (a.per_carrier) {
		// A per-carrier WAV is named for its frequency, so a slot that moves
		// would have to rotate its files. Keep the list fixed instead.
		if (pool != a.hz.size())
			learned_note += "tetra-sniff: --per-carrier keeps the carrier list fixed,"
					" so there are no free slots\n";
		pool = a.hz.size();
	}
	if (pool < a.hz.size()) {
		learned_note += "tetra-sniff: --max-carriers " + std::to_string(a.max_carriers) +
				" is below the " + std::to_string(a.hz.size()) +
				" carriers given, so the pool grows to fit them\n";
		pool = a.hz.size();
	}
	Allocations allocations = Allocations::create(a.hz, pool);
	const std::string& run_dir = src.run_dir;
	std::string start_iso = utc(src.start, "%Y-%m-%dT%H:%M:%SZ");
	std::cout << "tetra-sniff: run dir " << run_dir << " start_utc " << start_iso
		  << " rate " << (long long)src.rate << " center " << (long long)src.center << "\n";
	// A log read months later must say which carriers the run actually followed.
	{
		std::string line = "tetra-sniff: " + std::to_string(a.hz.size()) + " carriers ";
		for (size_t i = 0; i < a.hz.size(); i++)
			line += (i ? "," : "") + std::to_string(a.hz[i]);
		line += a.per_carrier ? " (per-carrier files on)\n" : "\n";
		std::cout << line;
		std::cout << learned_note;
		if (pool > a.hz.size())
			std::cout << "tetra-sniff: " + std::to_string(pool - a.hz.size()) +
					 " free carrier slot(s) for carriers that a grant names\n";
	}

	size_t n = pool;
	int voice_pipe[2];
	if (pipe(voice_pipe)) die(std::string("voice pipe: ") + strerror(errno));
	pid_t stitch_pid = fork();
	if (stitch_pid < 0) die(std::string("fork: ") + strerror(errno));
	if (stitch_pid == 0) {
		signal(SIGINT, SIG_IGN);
		signal(SIGTERM, SIG_IGN);
		close(voice_pipe[1]);
		exit(stitch_main(voice_pipe[0], run_dir, a.max_gssi));
	}
	close(voice_pipe[0]);
	std::vector<int> pipes(n, -1);
	std::vector<pid_t> pids(n);
	for (size_t i = 0; i < n; i++) {
		int p[2];
		if (pipe(p)) die(std::string("pipe: ") + strerror(errno));
		pids[i] = fork();
		if (pids[i] < 0) die(std::string("fork: ") + strerror(errno));
		if (pids[i] == 0) {
			close(p[1]);
			for (size_t j = 0; j < i; j++) close(pipes[j]);
			exit(child_main(p[0], i < a.hz.size() ? a.hz[i] : 0, run_dir, start_iso,
					src.rate, &allocations, voice_pipe[1],
					src.center - a.tune_offset, a.per_carrier, i));
		}
		close(p[0]);
		pipes[i] = p[1];
	}
	close(voice_pipe[1]);

	// Before radio_open(): a child dying during USB enumeration must not have its
	// SIGCHLD delivered against the default disposition, which drops it.
	struct sigaction sa = {};
	sa.sa_handler = on_signal;
	sigaction(SIGCHLD, &sa, nullptr);

	if (src.fd < 0) {
		RadioOpen cfg{};
		cfg.center_hz = (uint32_t)src.center;
		cfg.rate_hz = (uint32_t)src.rate;
		cfg.index = a.device;
		cfg.channel = a.rx;
		cfg.antenna = a.antenna;
		cfg.gain_tenth_db = a.gain_db < 0 ? -1 : (int)llround(a.gain_db * 10);
		int saved = quiet_begin();
		RadioErr rc = radio_open(&src.radio, cfg);
		quiet_end(saved);
		if (rc == RadioErr::bad_index)
			die("there is no receiver at --device " + std::to_string(a.device));
		if (rc == RadioErr::bad_channel)
			die("there is no RX channel at --rx " + std::to_string(a.rx));
		if (rc == RadioErr::bad_antenna)
			die("the receiver has no antenna " + std::string(a.antenna ? a.antenna : ""));
		if (rc != RadioErr::ok) die(radio_error(rc));
		src.fmt = Fmt::cf32;
		int applied = radio_gain_tenth_db(src.radio);
		char gain[32] = "auto";
		if (applied >= 0) snprintf(gain, sizeof gain, "%d.%d dB", applied / 10, applied % 10);
		const char* drv = radio_driver(src.radio);
		const char* ant = radio_antenna(src.radio);
		std::cout << "tetra-sniff: radio " + std::string(drv && *drv ? drv : "?") + " " +
				 std::to_string(a.device) + " " +
				 std::to_string((long long)src.center) + " Hz @ " +
				 std::to_string((long long)src.rate) + " S/s gain " + gain +
				 " rx " + std::to_string(a.rx) +
				 (ant && *ant ? std::string(" ") + ant : "") + "\n";
	}

	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);
	// A dead stitch or carrier child ends the run. The handler has no SA_RESTART,
	// so it also breaks the blocking read. Systemd starts a new tree.

	std::vector<dsp::channel::RxVFO> vfos(n);
	{
		// The resampler prints a line for each VFO. Keep it out of the log.
		int saved = quiet_begin();
		// A free slot is parked at the centre. Its child discards the samples
		// until the parent gives the slot a frequency.
		for (size_t i = 0; i < n; i++)
			vfos[i].init(nullptr, src.rate, VFO_RATE, VFO_BW,
				     i < a.hz.size() ? (double)a.hz[i] + a.tune_offset - src.center : 0);
		quiet_end(saved);
	}

	int bytes_per = src.fmt == Fmt::cf32 ? 8 : src.fmt == Fmt::cs16 ? 4 : 2;
	int block = iq_block(src.rate);
	std::vector<uint8_t> raw(block * bytes_per);
	auto* in = dsp::buffer::alloc<dsp::complex_t>(block);
	std::vector<dsp::complex_t*> tmp(n);
	for (auto& p : tmp) p = dsp::buffer::alloc<dsp::complex_t>(block);
	size_t have = src.pending.size();
	memcpy(raw.data(), src.pending.data(), have);

	int clock_fd = open((run_dir + "/clock.log").c_str(), O_CREAT | O_APPEND | O_WRONLY, 0644);
	if (clock_fd < 0) die(run_dir + "/clock.log: " + strerror(errno));
	// dprintf has no fixed buffer, so a long run_dir cannot overflow one.
	// The centre and the carriers make the run directory describe its own run,
	// which is what lets a reader place the recordings in the band.
	dprintf(clock_fd, "# tetra-sniff run=%s start_utc=%s iq_rate=%.0f vfo_rate=%.0f center=%.0f\n"
			  "# utc vfo_sample queue dropped event\n",
		run_dir.c_str(), start_iso.c_str(), src.rate, VFO_RATE, src.center);
	log_previous_run(clock_fd, a.out, run_dir);
	sd_notify("READY=1");
	// Which carriers a grant revealed, as against those the command line gave.
	std::set<uint32_t> learned_hz;
	uint64_t vfo_samples = 0;
	size_t queue = 0;
	uint64_t dropped = 0;

	while (!stop_flag) {
		int cnt = 0;
		if (src.radio) {
			int got = radio_read(src.radio, (float*)in, block);
			if (got < 0) break;
			if (got == 0) continue;
			cnt = got;
			dropped = radio_overflows(src.radio);
		} else {
			ssize_t r = read(src.fd, raw.data() + have, raw.size() - have);
			if (r < 0) {
				if (errno == EINTR) continue;
				std::cout << "tetra-sniff: read: " + std::string(strerror(errno)) + "\n";
				break;
			}
			if (r == 0) break;
			have += r;
			cnt = (int)(have / bytes_per);
			to_cf32(src.fmt, raw.data(), cnt, in);
			have -= (size_t)cnt * bytes_per;
			memmove(raw.data(), raw.data() + (size_t)cnt * bytes_per, have);
		}
		for (size_t i = 0; i < n; i++) {
			if (pipes[i] < 0) continue;
			int m = vfos[i].process(cnt, in, tmp[i]);
			// Every VFO gets the same input count. Child 0 counts exactly these samples.
			if (!i) vfo_samples += m;
			if (write_all(pipes[i], tmp[i], m * sizeof(dsp::complex_t))) continue;
			std::cout << "tetra-sniff: " + std::to_string(a.hz[i]) + " child stopped reading: " +
					 strerror(errno) + "\n";
			close(pipes[i]);
			pipes[i] = -1;
			stop_flag = 1;
			break;
		}
		// Give a free slot to a carrier that a control carrier granted. One
		// pass for each input block is 10 Hz, and a grant stays valid for 30 s.
		Allocations::Demand d;
		while (allocations.take(&d)) {
			int slot = allocations.assign(d.hz);
			if (slot < 0) continue;
			vfos[slot].setOffset((double)d.hz + a.tune_offset - src.center);
			std::cout << "tetra-sniff: slot " + std::to_string(slot) + " takes " +
					 std::to_string(d.hz) + " Hz, granted to GSSI " +
					 std::to_string(d.ssi) + " by " + std::to_string(d.control_hz) +
					 " Hz\n";
			learned_hz.insert(d.hz);
			if (a.learn && !a.per_carrier)
				write_learned(learn_path, allocations, learned_hz);
		}
		clock_tick(clock_fd, vfo_samples, queue, dropped);
	}
	close(clock_fd);
	if (src.radio) radio_stop(src.radio);

	for (size_t i = 0; i < n; i++)
		if (pipes[i] >= 0) close(pipes[i]);
	int worst = 0;
	for (size_t i = 0; i < n; i++) {
		int st = 0;
		while (waitpid(pids[i], &st, 0) < 0 && errno == EINTR) {}
		int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
		if (code) std::cout << "tetra-sniff: " << a.hz[i] << " exited " << code << "\n";
		worst = std::max(worst, code);
	}
	int stitch_status = 0;
	while (waitpid(stitch_pid, &stitch_status, 0) < 0 && errno == EINTR) {}
	int stitch_code = WIFEXITED(stitch_status) ? WEXITSTATUS(stitch_status) : 128 + WTERMSIG(stitch_status);
	if (stitch_code) std::cout << "tetra-sniff: stitch exited " << stitch_code << "\n";
	worst = std::max(worst, stitch_code);
	dsp::buffer::free(in);
	for (auto* p : tmp) dsp::buffer::free(p);
	radio_close(src.radio);
	return worst;
}
