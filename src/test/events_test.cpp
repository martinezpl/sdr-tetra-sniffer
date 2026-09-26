#include "recorder.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <vector>

static tetra_mac_event ev(tetra_ev_kind k, int tn, int dl_usage = -1, uint32_t ssi = 0, int um = -1,
			  int encr = -1, int type = -1, uint32_t hz = 0, int atn = 0,
			  uint32_t issi = 0, int atn_mask = 0)
{
	return tetra_mac_event{ k, (uint8_t)tn, dl_usage, ssi, um, encr, type, hz, (uint8_t)atn,
		(uint8_t)atn_mask, issi };
}

static uint32_t rd32(const std::string& s, size_t o) { return (uint8_t)s[o] | (uint8_t)s[o + 1] << 8 | (uint8_t)s[o + 2] << 16 | (uint32_t)(uint8_t)s[o + 3] << 24; }

static std::string slurp(const std::string& p)
{
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static int fails = 0;
static void check(bool ok, const std::string& what)
{
	if (!ok) { fails++; std::cerr << "FAIL " << what << "\n"; }
}

static void check_wav(const std::string& p, uint64_t samples)
{
	std::string w = slurp(p);
	check(w.size() == 44 + samples * 2, "wav size " + p);
	check(w.substr(0, 4) == "RIFF" && w.substr(8, 8) == "WAVEfmt " && w.substr(36, 4) == "data", "wav magic");
	check(rd32(w, 4) == 36 + samples * 2, "wav riff size");
	check(rd32(w, 40) == samples * 2, "wav data size");
	check(rd32(w, 24) == 8000 && rd32(w, 28) == 16000, "wav rate");
}

int main()
{
	char dir[] = "/tmp/events_test.XXXXXX";
	if (!mkdtemp(dir)) return 2;
	std::string d = dir, wavp = d + "/419962500.wav";
	int16_t pcm[480] = { 0 };
	{
		Recorder r(419962500, d, "2026-09-04T11:05:00Z", 2400000);
		check_wav(wavp, 0);
		r.advance(0.412);
		r.on_event(ev(TETRA_EV_SYSINFO, 1));
		r.on_event(ev(TETRA_EV_SLOT, 1, 7));
		r.advance(3.573);
		r.on_event(ev(TETRA_EV_VOICE, 1, -1, 1001, -1, 0));
		r.on_voice(pcm, 480);
		check_wav(wavp, 480);
		r.advance(0.06);
		r.on_event(ev(TETRA_EV_VOICE, 1, -1, 1001, -1, 0, -1, 0, 0, 2001));
		r.on_voice(pcm, 480);
		r.on_event(ev(TETRA_EV_VOICE, 1, -1, 1001, -1, 0, -1, 0, 0, 2002));
		r.on_voice(pcm, 480);
		r.on_event(ev(TETRA_EV_RESOURCE, 1, -1, 1001, 7, 0, 0, 420762500, 1));
		r.on_event(ev(TETRA_EV_SLOT, 1, 3));
		r.on_event(ev(TETRA_EV_RESOURCE, 2, -1, 1005, 11, 0, 1, 419962500, 3, 3005001));
		r.on_event(ev(TETRA_EV_RESOURCE, 2, -1, 1005, 11, 0));
		r.on_event(ev(TETRA_EV_VOICE, 2, -1, 1005, -1, 0, -1, 0, 0, 3005001));
		r.on_voice(pcm, 480);
		r.on_event(ev(TETRA_EV_VOICE, 3, -1, 0, -1, 0));
		r.on_voice(pcm, 480);
		r.advance(1.5);
		r.on_event(ev(TETRA_EV_SLOT, 3, 1));
		r.on_event(ev(TETRA_EV_VOICE, 3, -1, 0, -1, 0));
		r.finish();
		check_wav(wavp, 2400);
	}
	std::vector<std::string> want = {
		"# tetra-analyze hz=419962500 start_utc=2026-09-04T11:05:00Z iq_rate=2400000 wav=419962500.wav",
		"# offset_s ISSI GSSI event hz TN usage_marker encr dl_usage chanalloc_hz chanalloc_tn iq_s",
		"0.000 - - SYSINFO 419962500 1 - - - - - 0.412",
		"0.000 - 1001 PLAY 419962500 1 7 0 7 - - 3.985",
		"0.000 - 1001 FRAME 419962500 1 7 0 7 - - 3.985",
		"0.060 2001 1001 TALKER 419962500 1 7 0 7 - - 4.045",
		"0.060 2001 1001 FRAME 419962500 1 7 0 7 - - 4.045",
		"0.120 2001 1001 END 419962500 1 7 0 - - - 4.045",
		"0.120 2002 1001 PLAY 419962500 1 7 0 7 - - 4.045",
		"0.120 2002 1001 FRAME 419962500 1 7 0 7 - - 4.045",
		"0.180 - 1001 REPLACE 419962500 1 7 0 - 420762500 1 4.045",
		"0.180 3005001 1005 ALLOC 419962500 2 11 0 - 419962500 3 4.045",
		"0.180 2002 1001 END 419962500 1 - 0 - - - 4.045",
		"0.180 3005001 1005 PLAY 419962500 2 - 0 - - - 4.045",
		"0.180 3005001 1005 FRAME 419962500 2 - 0 - - - 4.045",
		"0.240 3005001 1005 END 419962500 2 - 0 - - - 4.045",
		"0.240 - - PLAY 419962500 3 - 0 - - - 4.045",
		"0.240 - - FRAME 419962500 3 - 0 - - - 4.045",
		"0.300 - - END 419962500 3 - 0 - - - 5.545",
		"0.300 - - PLAY 419962500 3 - 0 1 - - 5.545",
		"0.300 - - END 419962500 3 - 0 - - - 5.545",
	};
	std::istringstream got(slurp(d + "/419962500.log"));
	std::string l;
	size_t i = 0;
	while (std::getline(got, l)) {
		check(i < want.size() && l == want[i], "line " + std::to_string(i) + ": " + l);
		i++;
	}
	check(i == want.size(), "line count " + std::to_string(i));
	{
		// A second run must add to the WAV file. It must not start the file again.
		Recorder r(419962500, d, "2026-09-04T11:05:00Z", 2400000);
		check_wav(wavp, 2400);
		r.on_event(ev(TETRA_EV_VOICE, 1, -1, 1001, -1, 0));
		r.on_voice(pcm, 480);
		r.finish();
		check_wav(wavp, 2880);
	}
	return fails ? 1 : 0;
}
