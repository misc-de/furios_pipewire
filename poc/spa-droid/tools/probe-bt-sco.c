/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* Opens a raw SCO socket to a Bluetooth headset and says what comes back.
 *
 * Why this exists: every earlier measurement of the headset microphone went
 * through PipeWire's bluez5 backend, and in the A2DP profile the node it
 * offers is a loopback stub - so "digital silence" may have said more about
 * the measurement than about the link. A socket of our own removes every
 * layer in between.
 *
 * The two settings that decide whether a link carries anything at all:
 *
 *   air coding   CVSD  - the controller runs the codec, the host gets 16 bit
 *                        linear PCM (narrow band, 8 kHz)
 *                transparent - the host gets the coded stream untouched, which
 *                        is what wide band speech (mSBC) needs
 *   MTU          48 bytes for CVSD, 60 for mSBC frames
 *
 * The link has to be set up before the headset will accept it: a service level
 * connection over RFCOMM (any HFP implementation does this on connect). This
 * tool only adds the audio link on top, so it needs no call.
 *
 * Reading alone is enough for a verdict, but a synchronous link that is never
 * fed stalls on some controllers - hence --feed, which sends silence.
 */
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sco.h>

#define MAX_PACKET 1024

struct stats {
	unsigned long packets;
	unsigned long bytes;
	unsigned long short_reads;
	bool seen[256];
	int16_t min, max;
	double square_sum;
	unsigned long samples;
};

static void account(struct stats *st, const uint8_t *buf, ssize_t len)
{
	ssize_t i;

	st->packets++;
	st->bytes += len;
	for (i = 0; i < len; i++)
		st->seen[buf[i]] = true;
	for (i = 0; i + 1 < len; i += 2) {
		int16_t s = (int16_t) (buf[i] | (buf[i + 1] << 8));
		if (s < st->min) st->min = s;
		if (s > st->max) st->max = s;
		st->square_sum += (double) s * (double) s;
		st->samples++;
	}
}

static unsigned distinct(const struct stats *st)
{
	unsigned i, n = 0;
	for (i = 0; i < 256; i++)
		if (st->seen[i]) n++;
	return n;
}

static void usage(const char *me)
{
	fprintf(stderr,
		"usage: %s --device AA:BB:CC:DD:EE:FF [options]\n"
		"  --transparent    ask for transparent air coding (mSBC), not CVSD\n"
		"  --mtu N          write in packets of N bytes (default: what the link says)\n"
		"  --seconds N      how long to listen (default 5)\n"
		"  --feed           send silence while listening\n"
		"  --out FILE       write everything received to FILE\n", me);
}

int main(int argc, char **argv)
{
	const char *address = NULL, *out_path = NULL;
	bool transparent = false, feed = false;
	int seconds = 5, mtu_arg = 0;
	int i, fd, err;
	struct sockaddr_sco local = { 0 }, remote = { 0 };
	struct bt_voice voice = { 0 };
	struct sco_options opts;
	socklen_t optlen;
	struct stats st = { .min = 32767, .max = -32768 };
	uint8_t buf[MAX_PACKET];
	uint8_t *silence;
	FILE *out = NULL;
	struct timespec t0, now;
	int mtu;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) address = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mtu") && i + 1 < argc) mtu_arg = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--transparent")) transparent = true;
		else if (!strcmp(argv[i], "--feed")) feed = true;
		else { usage(argv[0]); return 1; }
	}
	if (!address) { usage(argv[0]); return 1; }

	fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO);
	if (fd < 0) { perror("socket(SCO)"); return 1; }

	local.sco_family = AF_BLUETOOTH;
	bacpy(&local.sco_bdaddr, BDADDR_ANY);
	if (bind(fd, (struct sockaddr *) &local, sizeof(local)) < 0) {
		perror("bind"); close(fd); return 1;
	}

	/* Has to be set before connect - afterwards the link is already up. */
	voice.setting = transparent ? BT_VOICE_TRANSPARENT : BT_VOICE_CVSD_16BIT;
	if (setsockopt(fd, SOL_BLUETOOTH, BT_VOICE, &voice, sizeof(voice)) < 0)
		fprintf(stderr, "note: BT_VOICE (0x%04x) refused: %s\n",
			voice.setting, strerror(errno));

	remote.sco_family = AF_BLUETOOTH;
	str2ba(address, &remote.sco_bdaddr);

	printf("connecting to %s, air coding %s ...\n", address,
	       transparent ? "transparent" : "CVSD");
	if (connect(fd, (struct sockaddr *) &remote, sizeof(remote)) < 0) {
		err = errno;
		printf("  FAILED: %s\n", strerror(err));
		printf("  A refused link usually means no service level connection:\n"
		       "  connect the headset first so its HFP side is up.\n");
		close(fd);
		return 2;
	}

	optlen = sizeof(opts);
	if (getsockopt(fd, SOL_SCO, SCO_OPTIONS, &opts, &optlen) < 0) {
		perror("getsockopt(SCO_OPTIONS)"); close(fd); return 1;
	}
	mtu = mtu_arg ? mtu_arg : opts.mtu;
	if (mtu <= 0 || mtu > MAX_PACKET) mtu = 48;
	printf("  link up, MTU %u (using %d)\n", opts.mtu, mtu);

	if (out_path && !(out = fopen(out_path, "wb"))) {
		perror(out_path); close(fd); return 1;
	}
	silence = calloc(1, mtu);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN | (feed ? POLLOUT : 0) };
		double elapsed;
		int ready;

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
		if (elapsed >= seconds) break;

		ready = poll(&pfd, 1, 200);
		if (ready < 0) {
			if (errno == EINTR) continue;
			perror("poll"); break;
		}
		if (pfd.revents & (POLLHUP | POLLERR)) {
			printf("  link dropped after %.1f s\n", elapsed);
			break;
		}
		if (pfd.revents & POLLOUT)
			(void) write(fd, silence, mtu);
		if (!(pfd.revents & POLLIN))
			continue;

		{
			ssize_t len = read(fd, buf, sizeof(buf));
			if (len < 0) {
				if (errno == EINTR || errno == EAGAIN) continue;
				perror("read"); break;
			}
			if (len == 0) { printf("  link closed by the other side\n"); break; }
			if (len < mtu) st.short_reads++;
			account(&st, buf, len);
			if (out) fwrite(buf, 1, len, out);
		}
	}

	printf("\nreceived: %lu packets, %lu bytes\n", st.packets, st.bytes);
	if (st.packets == 0) {
		printf("VERDICT: nothing at all - the link carries no data to this host.\n");
	} else {
		unsigned d = distinct(&st);
		double rms = st.samples ? sqrt(st.square_sum / st.samples) : 0.0;
		printf("distinct byte values: %u of 256\n", d);
		printf("as 16 bit PCM: min %d, max %d, RMS %.1f\n", st.min, st.max, rms);
		if (d <= 2)
			printf("VERDICT: packets arrive, but they carry one constant value -\n"
			       "         digital silence, not a microphone.\n");
		else
			printf("VERDICT: real data. The link carries audio to this host.\n");
	}
	if (st.short_reads)
		printf("note: %lu reads shorter than the MTU\n", st.short_reads);

	if (out) { fclose(out); printf("written to %s\n", out_path); }
	free(silence);
	close(fd);
	return 0;
}
