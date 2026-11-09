/* Exercise the firmware attribute reader without touching the live radio. */
#define main pd_mapper_main
#include "../third_party/pd-mapper-n20/pd-mapper.c"
#undef main
#include <assert.h>

int main(int argc, char **argv)
{
	char value[8];
	int fds[2];
	if (argc == 2 && strcmp(argv[1], "--fuzz") == 0) {
		const char alphabet[] = "{}[]:,\\\"0123456789truefalsnul abcXYZ";
		unsigned seed = 0x12345678;
		char input[257];
		for (int trial = 0; trial < 20000; trial++) {
			seed = seed * 1664525u + 1013904223u;
			unsigned len = seed % 256;
			for (unsigned i = 0; i < len; i++) {
				seed = seed * 1664525u + 1013904223u;
				input[i] = alphabet[seed % (sizeof(alphabet) - 1)];
			}
			input[len] = 0;
			struct json_value *parsed = json_parse(input);
			if (parsed) json_free(parsed);
		}
		puts("PD_MAPPER_JSON_FUZZ_PASSED");
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--device-maps") == 0) {
		assert(pd_load_maps() == 0);
		assert(pd_maps != NULL);
		struct servreg_loc_get_domain_list_resp *r = calloc(1, sizeof(*r));
		assert(r);
		for (const struct pd_map *m = pd_maps; m->service; m++) {
			DEFINE_QRTR_PACKET(packet, 256);
			memset(r, 0, sizeof(*r));
			assert(populate_domain_list(r, m->service, pd_maps) == 0);
			r->result.result = QMI_RESULT_SUCCESS;
			r->db_revision_valid = 1; r->db_revision = 1;
			r->domain_list_valid = r->domain_list_len != 0;
			r->total_domains_valid = 1; r->total_domains = r->domain_list_len;
			assert(qmi_encode_message(&packet, QMI_RESPONSE, SERVREG_LOC_GET_DOMAIN_LIST,
			                          1, r, servreg_loc_get_domain_list_resp_ei) >= 0);
		}
		free(r);
		puts("PD_MAPPER_DEVICE_MAP_LOAD_PASSED");
		return 0;
	}
	assert(argc == 1);
	assert(read_firmware_value(-1, value, sizeof(value)) < 0);
	assert(read_firmware_value(-1, value, 1) < 0);
	assert(pipe(fds) == 0);
	assert(write(fds[1], "modem", 5) == 5);
	close(fds[1]);
	assert(read_firmware_value(fds[0], value, sizeof(value)) == 0);
	assert(strcmp(value, "modem") == 0);
	close(fds[0]);
	assert(pipe(fds) == 0);
	assert(write(fds[1], "1234567", 7) == 7);
	close(fds[1]);
	assert(read_firmware_value(fds[0], value, sizeof(value)) < 0);
	close(fds[0]);
	assert(pipe(fds) == 0);
	close(fds[1]);
	assert(read_firmware_value(fds[0], value, sizeof(value)) < 0);
	close(fds[0]);
	struct servreg_loc_get_domain_list_resp *resp = calloc(1, sizeof(*resp));
	struct pd_map *maps = calloc(257, sizeof(*maps));
	assert(resp && maps);
	maps[0] = (struct pd_map){"service", "soc/domain", 7};
	assert(populate_domain_list(resp, "service", maps) == 0);
	assert(resp->domain_list_len == 1 && resp->domain_list[0].instance_id == 7);
	char long_domain[257]; memset(long_domain, 'x', 256); long_domain[256] = 0;
	memset(resp, 0, sizeof(*resp)); maps[0].domain = long_domain;
	assert(populate_domain_list(resp, "service", maps) < 0);
	memset(resp, 0, sizeof(*resp));
	for (int i = 0; i < 256; i++) maps[i] = (struct pd_map){"service", "domain", i};
	assert(populate_domain_list(resp, "service", maps) < 0);
	assert(resp->domain_list_len == 255);
	free(maps); free(resp);
	char path[] = "/tmp/n20-pd-map-test-XXXXXX";
	int file = mkstemp(path); assert(file >= 0);
	const char *bad = "{\"sr_domain\":{\"soc\":\"s\",\"domain\":\"d\",\"subdomain\":\"x\",\"qmi_instance_id\":2147483648},\"sr_service\":[{\"provider\":\"p\",\"service\":\"s\"}]}";
	assert(write(file, bad, strlen(bad)) == (ssize_t)strlen(bad));
	assert(pd_load_map(path) < 0);
	const char *good = "{\"sr_domain\":{\"soc\":\"s\",\"domain\":\"d\",\"subdomain\":\"x\",\"qmi_instance_id\":1},\"sr_service\":[{\"provider\":\"p\",\"service\":\"s\"}]}";
	assert(ftruncate(file, 0) == 0 && lseek(file, 0, SEEK_SET) == 0);
	assert(write(file, good, strlen(good)) == (ssize_t)strlen(good));
	assert(pd_load_map(path) == 0 && pd_maps && pd_maps[0].instance == 1);
	close(file); assert(unlink(path) == 0);
	assert(json_get_child(NULL, "missing") == NULL);
	assert(json_parse("") == NULL);
	assert(json_parse("{\"x\":\"unterminated") == NULL);
	assert(json_parse("[0] trailing") == NULL);
	struct json_value *spaced = json_parse("[0] \n");
	assert(spaced); json_free(spaced);
	char oversized[160];
	memset(oversized, 'a', sizeof(oversized)); oversized[0] = '"';
	oversized[sizeof(oversized) - 2] = '"'; oversized[sizeof(oversized) - 1] = 0;
	assert(json_parse(oversized) == NULL);
	char nested[142];
	memset(nested, '[', 70); nested[70] = '0';
	memset(nested + 71, ']', 70); nested[141] = 0;
	assert(json_parse(nested) == NULL);
	struct json_value *shallow = json_parse("[[0]]");
	assert(shallow); json_free(shallow);
	char empty_path[64] = {0};
	memcpy(empty_path, "/tmp/n20-pd-json-test-XXXXXX", sizeof("/tmp/n20-pd-json-test-XXXXXX"));
	file = mkstemp(empty_path); assert(file >= 0);
	assert(json_parse_file(empty_path) == NULL);
	assert(ftruncate(file, 1024 * 1024 + 1) == 0);
	assert(json_parse_file(empty_path) == NULL);
	close(file); assert(unlink(empty_path) == 0);
	puts("PD_MAPPER_FIRMWARE_READ_GUARDS_PASSED");
	return 0;
}
