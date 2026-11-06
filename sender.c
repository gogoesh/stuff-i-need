#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

#define SIZE 1024
#define MAXPKT 10000
#define INIT_SSTHRESH 16
#define RTO_MS 200
#define MAX_FIN_TRIES 10
#define LOG_FILE "cc_log.csv"

typedef struct {
	int seq;
	int len;
	int done;
	unsigned short checksum;
	char data[SIZE];
} Packet;

unsigned short checksum(Packet *p) {
	unsigned int sum = 0;
	unsigned short old = p->checksum;
	p->checksum = 0;

	unsigned char *x = (unsigned char *)p;

	for (size_t i = 0; i < sizeof(Packet); i++)
		sum += x[i];

	p->checksum = old;
	return (unsigned short)(sum & 0xffff);
}

int valid(Packet *p) {
	return p->checksum == checksum(p);
}

static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double cwnd = 1.0;
static int ssthresh = INIT_SSTHRESH;
static FILE *logf = NULL;
static double t0 = 0;

static void log_event(const char *ev, int seq, int ack) {
	const char *state = (cwnd < ssthresh) ? "SLOW_START" : "CONG_AVOID";

	fprintf(logf, "%.2f,%s,%d,%d,%.2f,%d,%s\n",
			now_ms() - t0, ev, seq, ack, cwnd, ssthresh, state);
}

static void send_packet(int sock, Packet *p, struct sockaddr_in *addr,
						int loss, const char *ev) {
	log_event(ev, p->seq, -1);

	if ((rand() % 100) < loss) {
		printf("Dropped packet %d\n", p->seq);
		log_event("DROP", p->seq, -1);
		return;
	}

	sendto(sock, p, sizeof(Packet), 0, (struct sockaddr *)addr, sizeof(*addr));
	printf("%s packet %d  cwnd=%.2f ssthresh=%d\n", ev, p->seq, cwnd, ssthresh);
}

void run_sender(char *file, char *ip, int port, int loss, int reorder) {
	FILE *fp = fopen(file, "rb");
	if (!fp) {
		perror("file");
		exit(1);
	}

	Packet *packets = malloc(MAXPKT * sizeof(Packet));
	if (!packets) {
		perror("malloc");
		exit(1);
	}

	int total = 0;
	while (total < MAXPKT) {
		memset(&packets[total], 0, sizeof(Packet));
		int n = fread(packets[total].data, 1, SIZE, fp);
		if (n <= 0)
			break;
		packets[total].seq = total;
		packets[total].len = n;
		packets[total].checksum = checksum(&packets[total]);
		total++;
	}

	if (total == MAXPKT && fgetc(fp) != EOF) {
		fprintf(stderr, "File too large (max %d bytes)\n", MAXPKT * SIZE);
		exit(1);
	}
	fclose(fp);

	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		perror("socket");
		exit(1);
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
		fprintf(stderr, "Invalid IP address\n");
		exit(1);
	}

	logf = fopen(LOG_FILE, "w");
	if (!logf) {
		perror("log file");
		exit(1);
	}
	fprintf(logf, "time_ms,event,seq,ack,cwnd,ssthresh,state\n");
	t0 = now_ms();

	int base = 0;
	int next = 0;
	int dup_acks = 0;
	double timer_start = now_ms();

	printf("Sending %d packets\n", total);

	while (base < total) {
		while (next < total && next < base + (int)cwnd) {
			if (base == next)
				timer_start = now_ms();

			if ((rand() % 100) < reorder && next + 1 < total &&
				next + 1 < base + (int)cwnd) {
				send_packet(sock, &packets[next + 1], &addr, loss, "SEND_REORDER");
				send_packet(sock, &packets[next], &addr, loss, "SEND");
				next += 2;
			} else {
				send_packet(sock, &packets[next], &addr, loss, "SEND");
				next++;
			}
		}

		double remain = RTO_MS - (now_ms() - timer_start);
		if (remain < 0)
			remain = 0;

		fd_set set;
		FD_ZERO(&set);
		FD_SET(sock, &set);

		struct timeval tv;
		tv.tv_sec = (long)(remain / 1000);
		tv.tv_usec = ((long)remain % 1000) * 1000;

		int result = select(sock + 1, &set, NULL, NULL, &tv);

		if (result < 0) {
			perror("select");
			continue;
		}

		if (result > 0) {
			int ack;
			if (recvfrom(sock, &ack, sizeof(ack), 0, NULL, NULL) != sizeof(ack))
				continue;

			if (ack > base) {
				base = ack;
				dup_acks = 0;

				if (cwnd < ssthresh)
					cwnd += 1.0;
				else
					cwnd += 1.0 / cwnd;

				timer_start = now_ms();
				printf("ACK %d  cwnd=%.2f ssthresh=%d\n", ack, cwnd, ssthresh);
				log_event("ACK", -1, ack);

			} else if (ack == base) {
				dup_acks++;
				printf("DUPACK %d (#%d)\n", ack, dup_acks);
				log_event("DUPACK", -1, ack);

				if (dup_acks == 3) {
					ssthresh = ((int)(cwnd / 2) > 2) ? (int)(cwnd / 2) : 2;
					cwnd = ssthresh;
					printf("FAST RETRANSMIT %d  cwnd=%.2f ssthresh=%d\n",
						   base, cwnd, ssthresh);
					log_event("FAST_RETX", base, ack);
					send_packet(sock, &packets[base], &addr, loss, "RETX");
					timer_start = now_ms();
				}
			}

		} else {
			ssthresh = ((int)(cwnd / 2) > 2) ? (int)(cwnd / 2) : 2;
			cwnd = 1.0;
			dup_acks = 0;

			printf("TIMEOUT on %d  cwnd=%.2f ssthresh=%d\n", base, cwnd, ssthresh);
			log_event("TIMEOUT", base, -1);

			send_packet(sock, &packets[base], &addr, loss, "RETX");
			timer_start = now_ms();
		}
	}

	Packet end;
	memset(&end, 0, sizeof(end));
	end.seq = total;
	end.done = 1;
	end.checksum = checksum(&end);

	int finished = 0;
	for (int tries = 0; tries < MAX_FIN_TRIES && !finished; tries++) {
		send_packet(sock, &end, &addr, loss, "FIN");

		double fin_start = now_ms();
		while (!finished) {
			double left = RTO_MS - (now_ms() - fin_start);
			if (left <= 0)
				break;

			fd_set set;
			FD_ZERO(&set);
			FD_SET(sock, &set);
			struct timeval tv;
			tv.tv_sec = 0;
			tv.tv_usec = (long)(left * 1000);

			if (select(sock + 1, &set, NULL, NULL, &tv) <= 0)
				break;

			int ack;
			if (recvfrom(sock, &ack, sizeof(ack), 0, NULL, NULL) == sizeof(ack) &&
				ack == total + 1)
				finished = 1;
		}
	}

	if (finished) {
		printf("Transfer complete\n");
		log_event("FIN_ACK", total, total + 1);
	} else {
		printf("FIN not acknowledged after %d tries\n", MAX_FIN_TRIES);
		log_event("FIN_GIVEUP", total, -1);
	}

	fclose(logf);
	free(packets);
	close(sock);
}

void run_receiver(char *file, int port);

int main(int argc, char **argv) {
	int sender = 0, receiver = 0, port = 0, loss = 0, reorder = 0;
	char *file = NULL, *ip = NULL;
	int c;

	while ((c = getopt(argc, argv, "SRf:l:r:p:i:")) != -1) {
		switch (c) {
			case 'S': sender = 1; break;
			case 'R': receiver = 1; break;
			case 'f': file = optarg; break;
			case 'l': loss = atoi(optarg); break;
			case 'r': reorder = atoi(optarg); break;
			case 'p': port = atoi(optarg); break;
			case 'i': ip = optarg; break;
		}
	}

	srand(time(NULL));

	if (sender && file && ip && port)
		run_sender(file, ip, port, loss, reorder);
	else if (receiver && file && port)
		run_receiver(file, port);
	else {
		printf("Sender:\n");
		printf("./mini-tcp-cc -S -f file -l loss -r reorder -p port -i ip\n");
		printf("Receiver:\n");
		printf("./mini-tcp-cc -R -p port -f file\n");
	}

	return 0;
}