/* Read-only QMI query against a specified QRTR service-registry endpoint. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "../third_party/pd-mapper-n20/servreg_loc.h"

int main(int argc, char **argv)
{
	if (argc != 3) { fprintf(stderr, "usage: %s NODE PORT\n", argv[0]); return 2; }
	char *end;
	unsigned long node = strtoul(argv[1], &end, 10);
	if (*end || node > UINT32_MAX) return 2;
	unsigned long port = strtoul(argv[2], &end, 10);
	if (*end || port > UINT32_MAX) return 2;
	int fd = qrtr_open(0);
	if (fd < 0) { perror("qrtr_open"); return 1; }
	struct timeval timeout = {.tv_sec = 5};
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
		perror("setsockopt"); close(fd); return 1;
	}
	const char name[] = "n20/nonexistent-check";
	unsigned char request[10 + sizeof(name) - 1] = {
		QMI_REQUEST, 73, 0, SERVREG_LOC_GET_DOMAIN_LIST, 0,
		3 + sizeof(name) - 1, 0, 1, sizeof(name) - 1, 0
	};
	memcpy(request + 10, name, sizeof(name) - 1);
	if (qrtr_sendto(fd, (uint32_t)node, (uint32_t)port, request, sizeof(request)) < 0) {
		fprintf(stderr, "request failed\n"); close(fd); return 1;
	}
	char bytes[4096]; struct sockaddr_qrtr from = {0}; socklen_t from_len = sizeof(from);
	ssize_t n = recvfrom(fd, bytes, sizeof(bytes), 0, (struct sockaddr *)&from, &from_len);
	if (n <= 0) { perror("response"); close(fd); return 1; }
	struct qrtr_packet in = {0}; struct servreg_loc_get_domain_list_resp response = {0};
	unsigned int transaction = 0;
	if (qrtr_decode(&in, bytes, (size_t)n, &from) < 0 ||
	    qmi_decode_message(&response, &transaction, &in, QMI_RESPONSE,
	                       SERVREG_LOC_GET_DOMAIN_LIST, servreg_loc_get_domain_list_resp_ei) < 0 ||
	    transaction != 73 || response.result.result != QMI_RESULT_SUCCESS || response.domain_list_len != 0) {
		fprintf(stderr, "unexpected registry response\n"); close(fd); return 1;
	}
	close(fd); puts("LIVE_PD_MAPPER_QMI_QUERY_PASSED"); return 0;
}
