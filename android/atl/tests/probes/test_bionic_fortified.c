#define _GNU_SOURCE

#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern size_t bionic___strnlen_chk(const char *s, size_t size, size_t max_size);
extern size_t bionic___strlcat_chk(char *dst, const char *src, size_t size, size_t dst_size);
extern size_t bionic___strlcpy_chk(char *dst, const char *src, size_t size, size_t dst_size);

static void expect_abort(void (*test)(void))
{
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		struct rlimit limit = { 0, 0 };
		setrlimit(RLIMIT_CORE, &limit);
		test();
		_exit(0);
	}
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFSIGNALED(status));
}

static void strnlen_oob(void)
{
	char value[4] = { 'a', 'b', 'c', 'd' };
	(void)bionic___strnlen_chk(value, 8, sizeof(value));
}

static void strlcat_oob(void)
{
	char value[4] = "abc";
	(void)bionic___strlcat_chk(value, "x", 8, sizeof(value));
}

int main(void)
{
	assert(bionic___strnlen_chk("hello", 2, 5) == 2);
	assert(bionic___strnlen_chk("x", 8, 5) == 1);
	assert(bionic___strnlen_chk("", 0, 0) == 0);
	assert(bionic___strnlen_chk("12345", 5, 5) == 5);

	char dst[8] = "foo";
	assert(bionic___strlcat_chk(dst, "bar", 8, sizeof(dst)) == 6);
	assert(strcmp(dst, "foobar") == 0);
	char full[5] = { '1', '2', '3', '4', '5' };
	assert(bionic___strlcat_chk(full, "x", sizeof(full), sizeof(full)) == 6);
	assert(memcmp(full, "12345", sizeof(full)) == 0);
	assert(bionic___strlcat_chk(full, "src", 0, 0) == 3);
	assert(memcmp(full, "12345", sizeof(full)) == 0);

	char copy[4];
	assert(bionic___strlcpy_chk(copy, "hello", sizeof(copy), sizeof(copy)) == 5);
	assert(strcmp(copy, "hel") == 0);
	assert(bionic___strlcpy_chk(copy, "hello", 0, sizeof(copy)) == 5);
	assert(strcmp(copy, "hel") == 0);
	expect_abort(strnlen_oob);
	expect_abort(strlcat_oob);
	return 0;
}
