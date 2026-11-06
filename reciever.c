#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define SIZE 1024
#define MAXPKT 10000

typedef struct {
	int seq;
	int len;
	int done;
	unsigned short checksum;
	char data[SIZE];
} Packet;

unsigned short checksum(Packet *p);
int valid(Packet *p);

static void send_ack(int sock, int ack, struct sockaddr_in *to, socklen_t len) {
	sendto(sock, &ack, sizeof(ack), 0, (struct sockaddr *)to, len);
}

void run_receiver(char *file, int port) {
	FILE *fp = fopen(file, "wb");
	if (!fp) {
		perror("file");
		exit(1);
	}

	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		perror("socket");
		exit(1);
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		exit(1);
	}

	Packet *buf = calloc(MAXPKT, sizeof(Packet));
	char *have = calloc(MAXPKT, 1);
	if (!buf || !have) {
		perror("calloc");
		exit(1);
	}

	int expected = 0;

	printf("Waiting on port %d\n", port);

	while (1) {
		Packet p;
		struct sockaddr_in sender;
		socklen_t slen = sizeof(sender);

		ssize_t n = recvfrom(sock, &p, sizeof(p), 0,
							 (struct sockaddr *)&sender, &slen);
		if (n != (ssize_t)sizeof(Packet) || !valid(&p))
			continue;

		if (p.done) {
			if (p.seq == expected) {
				send_ack(sock, expected + 1, &sender, slen);
				printf("Transfer complete\n");
				break;
			}
			send_ack(sock, expected, &sender, slen);
			continue;
		}

		if (p.seq < 0 || p.seq >= MAXPKT || p.len < 0 || p.len > SIZE)
			continue;

		if (p.seq >= expected && !have[p.seq]) {
			buf[p.seq] = p;
			have[p.seq] = 1;

			if (p.seq == expected)
				printf("Received packet %d\n", p.seq);
			else
				printf("Buffered packet %d, expected %d\n", p.seq, expected);
		}

		while (expected < MAXPKT && have[expected]) {
			fwrite(buf[expected].data, 1, buf[expected].len, fp);
			expected++;
		}

		send_ack(sock, expected, &sender, slen);
	}

	free(buf);
	free(have);
	fclose(fp);
	close(sock);
}