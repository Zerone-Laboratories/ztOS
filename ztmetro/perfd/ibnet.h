/* ibnet: ztOSIntelligentBoost's model, run on the SCORE DSP (zrn-score) or,
 * when SCORE is not there or fails, on this core (see INTELLIGENT-BOOST.md). */
#ifndef IBNET_H
#define IBNET_H

#include "iblog.h"

#define IBNET_MODEL	"/var/lib/zrnperfd/ib/model.bin"
#define IBNET_FEATURES	12	/* V2: demand only, nothing that echoes the rung */
#define IBNET_RUNGS	5	/* idle0 nominal interactive big rapid */

/* load the model (false: none, IntelligentBoost stays off) */
bool ibnet_load(void);
/* the rung the model picks for this moment; -1 without a model */
int ibnet_predict(const struct iblog_sample *s, const struct iblog_row *r);
/* "score", "cpu" or "none", for the status file */
const char *ibnet_engine(void);

/* the last forward pass and the running counts, for Settings' Dynamic Boost page */
#define IBNET_MAX_HIDDEN	64
struct ibnet_trace {
	int n_hidden;
	float x[IBNET_FEATURES];	/* the inputs, normalised */
	float h[IBNET_MAX_HIDDEN];	/* the hidden layer, after the ReLU */
	float o[IBNET_RUNGS];		/* each rung's score */
	unsigned long predictions, on_score, on_cpu, checks, check_failures;
	unsigned last_us, avg_us;	/* one prediction's time */
};
const struct ibnet_trace *ibnet_trace(void);

/* where the network runs: on SCORE, or on this core. The network is ~456
 * multiply-adds, ~4 us here; a SCORE prediction costs ~0.23 ms of kernel time
 * with zero-copy buffers (measured 2026-10-25). SCORE is the default (the
 * user's choice: the network runs on the accelerator); "ib engine cpu" moves it. */
void ibnet_use_score(bool on);
bool ibnet_using_score(void);

#endif
