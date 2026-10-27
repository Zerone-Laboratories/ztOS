/* ibtest: run ibnet's model on made-up samples, SCORE against this core.
 *   ibtest N [score|cpu]	N predictions; prints the engine, the rungs and the time each */
#define _GNU_SOURCE
#include "../ibnet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 100;
	int counts[IBNET_RUNGS] = { 0 };
	if (!ibnet_load())
		return 1;
	ibnet_use_score(argc > 2 && !strcmp(argv[2], "score"));
	srand(1);
	struct timespec t0, t1;
	double total = 0, worst = 0;
	for (int i = 0; i < n; i++) {
		struct iblog_sample s = { .ms = i * 250, .app = "superscan", .used_front = (rand() % 150) / 100.0,
			.used_shell = (rand() % 40) / 100.0, .rung = rand() % 5, .profile = 1, .touching = rand() % 2 };
		struct iblog_row r = { .load = { (rand() % 100) / 100.0, (rand() % 100) / 100.0 },
			.khz = { 1500000, 1066000 }, .online = { 4, 1 }, .gpu_hz = 260000000 };
		clock_gettime(CLOCK_MONOTONIC, &t0);
		int rung = ibnet_predict(&s, &r);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
		total += ms;
		worst = ms > worst ? ms : worst;
		if (rung >= 0)
			counts[rung]++;
	}
	printf("engine %s; %d predictions, %.3f ms each, worst %.3f; rungs %d %d %d %d %d\n", ibnet_engine(), n,
	       total / n, worst, counts[0], counts[1], counts[2], counts[3], counts[4]);
	return 0;
}
