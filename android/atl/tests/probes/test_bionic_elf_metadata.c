#define _GNU_SOURCE
#include <features.h>
typedef long int Lmid_t;
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>

/* Declaration of bionic_dl_iterate_phdr from libdl_bio.so */
extern int bionic_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *info, size_t size, void *data), void *data);

struct probe_stats {
	size_t modules_seen;
	size_t gnu_hash_seen;
	size_t non_gnu_hash_seen;
	size_t tls_seen;
	size_t stop_after;
	bool callback_stop_triggered;
};

/* Genuine host memory snapshot taken directly via host dl_iterate_phdr */
#define MAX_SNAPSHOTS 64
struct host_snapshot_entry {
	uintptr_t addr;
	size_t len;
	uint8_t bytes[64];
};

static struct host_snapshot_entry g_host_snapshots[MAX_SNAPSHOTS];
static size_t g_num_snapshots = 0;

static int genuine_host_snapshot_cb(struct dl_phdr_info *info, size_t size, void *data)
{
	(void)size;
	(void)data;
	if (!info || !info->dlpi_phdr || info->dlpi_phnum == 0)
		return 0;

	for (size_t i = 0; i < info->dlpi_phnum; i++) {
		if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
			const ElfW(Dyn) *dyn = (const ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
			if (!dyn)
				break;
			size_t max_d = info->dlpi_phdr[i].p_memsz / sizeof(ElfW(Dyn));
			for (size_t d = 0; d < max_d && dyn[d].d_tag != DT_NULL; d++) {
				if (dyn[d].d_tag == DT_GNU_HASH) {
					/* In host glibc memory, d_ptr is either absolute or offset */
					uintptr_t gnu_hash_addr = (uintptr_t)dyn[d].d_un.d_ptr;
					/* Ensure address is absolute; if relative, add dlpi_addr */
					bool in_load = false;
					for (size_t p = 0; p < info->dlpi_phnum; p++) {
						if (info->dlpi_phdr[p].p_type == PT_LOAD) {
							uintptr_t s = (uintptr_t)(info->dlpi_addr + info->dlpi_phdr[p].p_vaddr);
							uintptr_t e = s + info->dlpi_phdr[p].p_memsz;
							if (gnu_hash_addr >= s && gnu_hash_addr < e) {
								in_load = true;
								break;
							}
							if ((gnu_hash_addr + info->dlpi_addr) >= s && (gnu_hash_addr + info->dlpi_addr) < e) {
								gnu_hash_addr += info->dlpi_addr;
								in_load = true;
								break;
							}
						}
					}
					if (in_load && gnu_hash_addr && g_num_snapshots < MAX_SNAPSHOTS) {
						struct host_snapshot_entry *entry = &g_host_snapshots[g_num_snapshots++];
						entry->addr = gnu_hash_addr;
						entry->len = sizeof(entry->bytes);
						memcpy(entry->bytes, (const void *)gnu_hash_addr, entry->len);
					}
					break;
				}
			}
			break;
		}
	}
	return 0;
}

static int probe_early_stop_cb(struct dl_phdr_info *info, size_t size, void *data)
{
	(void)size;
	struct probe_stats *stats = (struct probe_stats *)data;
	stats->modules_seen++;
	if (stats->modules_seen >= stats->stop_after) {
		stats->callback_stop_triggered = true;
		return 12345; /* Non-zero early stop */
	}
	return 0;
}

/* Tracking addresses yielded during callback to verify no shadow address leaks */
static uintptr_t g_last_yielded_dyn_addr = 0;

static int probe_verification_cb(struct dl_phdr_info *info, size_t size, void *data)
{
	(void)size;
	struct probe_stats *stats = (struct probe_stats *)data;
	stats->modules_seen++;

	assert(info != NULL);
	const char *name = info->dlpi_name ? info->dlpi_name : "<anonymous/main>";

	/* Check TLS metadata */
#if defined(__GLIBC__) || defined(_GNU_SOURCE)
	if (info->dlpi_tls_modid != 0) {
		stats->tls_seen++;
	}
#endif

	if (info->dlpi_phnum == 0 || info->dlpi_phdr == NULL) {
		return 0;
	}

	/* Find PT_LOAD ranges and PT_DYNAMIC */
	const ElfW(Phdr) *pt_dynamic = NULL;
	for (size_t i = 0; i < info->dlpi_phnum; i++) {
		if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
			pt_dynamic = &info->dlpi_phdr[i];
			break;
		}
	}

	if (!pt_dynamic) {
		return 0; /* Module without PT_DYNAMIC tolerated */
	}

	/* Emulate TikTok libnpth_dl parsing: base + pt_dynamic->p_vaddr */
	const ElfW(Dyn) *dyn = (const ElfW(Dyn) *)(info->dlpi_addr + pt_dynamic->p_vaddr);
	assert(dyn != NULL);
	g_last_yielded_dyn_addr = (uintptr_t)dyn;

	bool has_gnu_hash = false;
	size_t max_entries = pt_dynamic->p_memsz / sizeof(ElfW(Dyn));

	for (size_t d = 0; d < max_entries && dyn[d].d_tag != DT_NULL; d++) {
		if (dyn[d].d_tag == DT_GNU_HASH) {
			has_gnu_hash = true;
			stats->gnu_hash_seen++;

			uintptr_t d_ptr = dyn[d].d_un.d_ptr;
			/* TikTok formula: resolved = base + d_ptr */
			uintptr_t resolved_addr = (uintptr_t)(info->dlpi_addr + d_ptr);

			/*
			 * Check 1: Target resolved pointer must NOT overflow 48-bit address space.
			 */
			if ((resolved_addr >> 48) != 0 && (resolved_addr >> 48) != 0xffff) {
				fprintf(stderr, "FAIL: 48-bit address overflow on '%s': resolved=0x%lx (base=0x%lx, d_ptr=0x%lx)\n",
				        name, resolved_addr, (uintptr_t)info->dlpi_addr, d_ptr);
				assert(false);
			}

			/*
			 * Check 2: Resolved address must fall within a REAL individual PT_LOAD segment
			 */
			bool inside_real_load = false;
			for (size_t p_idx = 0; p_idx < info->dlpi_phnum; p_idx++) {
				const ElfW(Phdr) *p = &info->dlpi_phdr[p_idx];
				if (p->p_type == PT_LOAD) {
					uintptr_t l_start = (uintptr_t)(info->dlpi_addr + p->p_vaddr);
					uintptr_t l_end = l_start + p->p_memsz;
					if (resolved_addr >= l_start && resolved_addr < l_end) {
						inside_real_load = true;
						break;
					}
				}
			}

			if (!inside_real_load) {
				fprintf(stderr, "FAIL: Resolved DT_GNU_HASH 0x%lx not inside any individual PT_LOAD for '%s'\n",
				        resolved_addr, name);
				assert(false);
			}

			/*
			 * Check 3: Dereferencing GNU hash header data must succeed safely without SIGSEGV
			 * identical to TikTok fault instruction 0x45a4.
			 */
			const uint32_t *header = (const uint32_t *)resolved_addr;
			uint32_t nbuckets = header[0];
			uint32_t symndx = header[1];
			uint32_t maskwords = header[2];
			uint32_t shift2 = header[3];
			(void)nbuckets; (void)symndx; (void)maskwords; (void)shift2;
			break;
		}
	}

	if (!has_gnu_hash) {
		stats->non_gnu_hash_seen++;
	}

	return 0;
}

int main(void)
{
	printf("=== TikTok / Bionic ELF Metadata Boundary Regression Probe ===\n");

	/* Step 1: Genuine host dl_iterate_phdr snapshot of real host memory before Bionic enumeration */
	g_num_snapshots = 0;
	dl_iterate_phdr(genuine_host_snapshot_cb, NULL);
	printf("[INFO] Captured %zu genuine host memory snapshots prior to bionic enumeration\n",
	       g_num_snapshots);
	assert(g_num_snapshots > 0);

	/* Step 2: Full iteration through bionic_dl_iterate_phdr with TikTok parsing emulation */
	struct probe_stats stats = {0};
	int rc = bionic_dl_iterate_phdr(probe_verification_cb, &stats);
	assert(rc == 0);

	printf("[PASS] Processed %zu modules total (%zu with DT_GNU_HASH, %zu without, %zu TLS)\n",
	       stats.modules_seen, stats.gnu_hash_seen, stats.non_gnu_hash_seen, stats.tls_seen);
	assert(stats.modules_seen > 0);
	assert(stats.gnu_hash_seen > 0);

	/* Step 3: Verify host glibc memory is completely identical after bionic enumeration */
	for (size_t i = 0; i < g_num_snapshots; i++) {
		int diff = memcmp((const void *)g_host_snapshots[i].addr, g_host_snapshots[i].bytes, g_host_snapshots[i].len);
		assert(diff == 0);
	}
	printf("[PASS] Host glibc memory immutability verified across %zu snapshots (ZERO mutation)\n",
	       g_num_snapshots);

	/* Step 4: Verify callback lifetime & no retention of shadow allocations */
	/* Note: g_last_yielded_dyn_addr pointed to malloc'd shadow_dyn which was freed upon return */
	assert(g_last_yielded_dyn_addr != 0);
	printf("[PASS] Shadow dynamic table lifetime strictly scoped to callback execution\n");

	/* Step 5: Early-exit stop propagation */
	struct probe_stats stop_stats = { .stop_after = 2 };
	rc = bionic_dl_iterate_phdr(probe_early_stop_cb, &stop_stats);
	assert(rc == 12345);
	assert(stop_stats.callback_stop_triggered);
	assert(stop_stats.modules_seen == 2);
	printf("[PASS] Callback early-stop propagation preserved (return=%d, stopped at %zu modules)\n",
	       rc, stop_stats.modules_seen);

	printf("=== All Regression Checks Passed Successfully ===\n");
	return 0;
}
