#include "recorder.h"
#include "voice_frame.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "c-code/channel.h"
#include "c-code/source.h"
}

// One 13-hour run recorded 12 distinct GSSIs. Three runs over 29 hours recorded 17.
// --max-gssi sets the limit. TasksMax of a systemd unit must stay above it,
// because a limit above TasksMax fails at fork() and logs nothing.

static bool read_frame(int fd, VoiceFrame& frame)
{
	char* p = (char*)&frame;
	size_t left = sizeof frame;
	while (left) {
		ssize_t n = read(fd, p, left);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return false;
		p += n;
		left -= n;
	}
	return true;
}

static bool decode(const VoiceFrame& frame, bool first, int16_t* pcm)
{
	int16_t soft[432], coded[432], reordered[286], serial[138], parm[24];
	for (int i = 0; i < 432; i++) soft[i] = frame.bits[i] ? -127 : 127;
	Desinterleaving_Speech(soft, coded);
	bool bad = Channel_Decoding(first, 0, coded, reordered);
	for (int half = 0; half < 2; half++) {
		serial[0] = bad;
		memcpy(serial + 1, reordered + half * 137, 137 * sizeof(int16_t));
		Bits2prm_Tetra(serial, parm);
		Decod_Tetra(parm, pcm + half * 240);
		Post_Process(pcm + half * 240, 240);
	}
	return !bad;
}

static void conceal(int16_t* pcm)
{
	int16_t serial[138] = { 1 }, parm[24];
	for (int half = 0; half < 2; half++) {
		Bits2prm_Tetra(serial, parm);
		Decod_Tetra(parm, pcm + half * 240);
		Post_Process(pcm + half * 240, 240);
	}
}

struct CallState {
	WavWriter wav;
	int log_fd;
	int map_fd;
	bool first = true;
	uint32_t previous = 0;
	uint32_t previous_hz = 0;
	uint64_t previous_sample = 0;
};

static void emit_frame(CallState& state, const std::vector<VoiceFrame>& copies)
{
	const VoiceFrame* frame = &copies.front();
	for (const auto& copy : copies)
		if (copy.hz == state.previous_hz)
			frame = &copy;
	int missing = 0;
	// An anchor marks the first frame of a worker.
	// An anchor also marks each point where the WAV moves by more than 480 samples.
	const char* event = state.previous_sample ? nullptr : "START";
	bool call_gap = state.previous_sample && frame->sample > state.previous_sample + 36000;
	if (!call_gap && frame->hz == state.previous_hz &&
	    state.previous && frame->frame > state.previous + 1)
		for (uint32_t n = state.previous + 1; n < frame->frame; n++)
			if (n % 18) missing++;
	if (call_gap || missing > 16) {
		int16_t separator[4000] = {};
		state.wav.append(separator, 4000);
		Init_Decod_Tetra();
		Init_Rcpc_Decoding();
		state.first = true;
		event = "SEP";
	} else if (missing) {
		int16_t pcm[480];
		for (int n = 0; n < missing; n++) {
			conceal(pcm);
			state.wav.append(pcm, 480);
		}
		state.first = true;
		event = "FILL";
	}
	int16_t pcm[480];
	bool good = decode(*frame, state.first, pcm);
	state.first = false;
	state.wav.append(pcm, 480);
	char line[200];
	int len = snprintf(line, sizeof line, "%llu %u %u %u %u %u %u %u %s\n",
			   (unsigned long long)frame->sample, frame->frame, frame->issi,
			   frame->gssi, frame->hz, frame->control_hz, frame->tn, frame->usage,
			   good ? "FRAME" : "BAD");
	write(state.log_fd, line, len);
	if (event) {
		// The anchor holds the WAV position after this frame. The rule stays one subtraction.
		// One write of one short line keeps the lines of concurrent workers whole.
		len = snprintf(line, sizeof line, "%llu %u %u %llu %s\n",
			       (unsigned long long)frame->sample, frame->frame, frame->gssi,
			       (unsigned long long)state.wav.samples(), event);
		write(state.map_fd, line, len);
	}
	state.previous = frame->frame;
	state.previous_hz = frame->hz;
	state.previous_sample = frame->sample;
}

static void emit_bucket(CallState& state, std::vector<VoiceFrame>& frames)
{
	if (state.previous_sample && frames.front().sample <= state.previous_sample)
		return;
	std::map<uint32_t, int> counts;
	for (const auto& frame : frames) counts[frame.hz]++;
	uint32_t selected = counts.count(state.previous_hz) ? state.previous_hz : counts.begin()->first;
	for (const auto& count : counts)
		if (count.second > counts[selected])
			selected = count.first;
	frames.erase(std::remove_if(frames.begin(), frames.end(),
				   [=](const auto& frame) { return frame.hz != selected; }),
		     frames.end());
	std::sort(frames.begin(), frames.end(),
		  [](const auto& a, const auto& b) { return a.frame < b.frame; });
	for (size_t i = 0; i < frames.size();) {
		size_t end = i + 1;
		while (end < frames.size() && frames[end].frame == frames[i].frame) end++;
		emit_frame(state, std::vector<VoiceFrame>(frames.begin() + i, frames.begin() + end));
		i = end;
	}
}

static int call_main(int fd, const std::string& dir, uint32_t gssi)
{
	CallState state;
	state.wav.open(dir + "/calls/" + std::to_string(gssi) + ".wav");
	state.log_fd = open((dir + "/calls.log").c_str(), O_WRONLY | O_APPEND);
	if (state.log_fd < 0) throw std::runtime_error(strerror(errno));
	state.map_fd = open((dir + "/timemap.log").c_str(), O_WRONLY | O_APPEND);
	if (state.map_fd < 0) throw std::runtime_error(strerror(errno));
	Init_Decod_Tetra();
	Init_Rcpc_Decoding();
	std::map<uint64_t, std::vector<VoiceFrame>> pending;
	uint64_t newest = 0;
	VoiceFrame frame;
	while (read_frame(fd, frame)) {
		pending[frame.sample].push_back(frame);
		newest = std::max(newest, frame.sample);
		while (!pending.empty() && pending.begin()->first + 36000 <= newest) {
			emit_bucket(state, pending.begin()->second);
			pending.erase(pending.begin());
		}
	}
	for (auto& item : pending) emit_bucket(state, item.second);
	state.wav.close();
	close(state.log_fd);
	close(state.map_fd);
	return 0;
}

static bool write_frame(int fd, const VoiceFrame& frame)
{
	const char* p = (const char*)&frame;
	size_t left = sizeof frame;
	while (left) {
		ssize_t n = write(fd, p, left);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return false;
		p += n;
		left -= n;
	}
	return true;
}

int stitch_main(int fd, const std::string& dir, size_t max_gssi)
{
	std::string calls = dir + "/calls";
	if (mkdir(calls.c_str(), 0755) && errno != EEXIST)
		throw std::runtime_error(calls + ": " + strerror(errno));
	int log_fd = open((dir + "/calls.log").c_str(), O_CREAT | O_APPEND | O_WRONLY, 0644);
	if (log_fd < 0) throw std::runtime_error(strerror(errno));
	const char* header =
		"# capture_sample tdma_frame ISSI GSSI carrier_hz control_hz TN usage status\n";
	// The header goes only into an empty file. A second run keeps the lines of the first run.
	if (lseek(log_fd, 0, SEEK_END) == 0)
		write(log_fd, header, strlen(header));
	close(log_fd);

	int map_fd = open((dir + "/timemap.log").c_str(), O_CREAT | O_APPEND | O_WRONLY, 0644);
	if (map_fd < 0) throw std::runtime_error(strerror(errno));
	const char* map_header = "# capture_sample tdma_frame GSSI wav_sample event\n";
	// The header goes only into an empty file. A second run keeps the lines of the first run.
	if (lseek(map_fd, 0, SEEK_END) == 0)
		write(map_fd, map_header, strlen(map_header));
	close(map_fd);

	struct Worker { pid_t pid; int fd; };
	std::map<uint32_t, Worker> workers;
	int worst = 0;
	bool cap_logged = false;
	VoiceFrame frame;
	while (read_frame(fd, frame)) {
		if (!frame.gssi) continue;
		auto it = workers.find(frame.gssi);
		if (it == workers.end()) {
			// A corrupt GSSI field on the air makes an unlimited number of workers.
			// ponytail: one line for the whole run, a line per GSSI needs a second set
			if (workers.size() >= max_gssi) {
				if (!cap_logged)
					std::cout << "tetra-sniff: " << max_gssi
						  << " GSSI workers is the limit. GSSI "
						  << frame.gssi << " gets no worker.\n";
				cap_logged = true;
				continue;
			}
			int pipe_fd[2];
			if (pipe(pipe_fd)) throw std::runtime_error(strerror(errno));
			pid_t pid = fork();
			if (pid < 0) throw std::runtime_error(strerror(errno));
			if (pid == 0) {
				close(pipe_fd[1]);
				close(fd);
				for (const auto& worker : workers) close(worker.second.fd);
				exit(call_main(pipe_fd[0], dir, frame.gssi));
			}
			close(pipe_fd[0]);
			it = workers.emplace(frame.gssi, Worker{ pid, pipe_fd[1] }).first;
			// One line for each talkgroup. The log then shows real activity,
			// not only faults, and names every WAV file that the run makes.
			std::cout << "tetra-sniff: talkgroup " + std::to_string(frame.gssi) +
					 " first heard on " + std::to_string(frame.hz) + " Hz, writing calls/" +
					 std::to_string(frame.gssi) + ".wav\n";
		}
		if (!write_frame(it->second.fd, frame)) {
			// A dead call worker points to a full disk. The run stops.
			std::cout << "tetra-sniff: GSSI " << frame.gssi << " worker died: " << strerror(errno) << "\n";
			worst = 5;
			break;
		}
	}
	for (const auto& worker : workers) close(worker.second.fd);
	for (const auto& worker : workers) {
		int status = 0;
		while (waitpid(worker.second.pid, &status, 0) < 0 && errno == EINTR) {}
		int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
		worst = std::max(worst, code);
	}
	return worst;
}
