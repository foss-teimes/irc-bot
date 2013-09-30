#include <stdio.h>
#include <yajl/yajl_tree.h>

int main(int argc, char *argv[]) {

	char buf[16384], errbuf[1024], *result;
	yajl_val root, val;
	size_t n;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <value>\n", argv[0]);
		return 1;
	}
	n = fread(buf, sizeof(char), sizeof(buf) - 1, stdin);
	buf[n] = '\0';

	root = yajl_tree_parse(buf, errbuf, sizeof(errbuf));
	if (!root) {
		fprintf(stderr, "%s\n", errbuf);
		return 1;
	}
	val = yajl_tree_get(root, (const char *[]) { argv[1], NULL }, yajl_t_string);
	if (!val)
		return 1;

	result = YAJL_GET_STRING(val);
	fputs(result, stdout);

	return 0;
}

