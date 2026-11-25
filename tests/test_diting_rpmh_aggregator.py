"""ASan/UBSan host unit tests for DitingRpmhAggregator.h."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

DEVICE = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include "DitingRpmhAggregator.h"

int main(void) {
    DitingRpmhAggregator agg;
    diting_rpmh_agg_init(&agg);
    DitingRpmhAggPlan plan;

    const uint32_t mx = 0x30010u, cx = 0x30020u, lcx = 0x30030u, lmx = 0x30040u;
    const uint32_t rails[2] = {mx, cx};
    const uint32_t floor_v[2] = {3u, 3u}; /* SVS */
    const uint32_t nom_v[2] = {7u, 7u};   /* NOM */
    const uint32_t high_v[2] = {9u, 9u};
    const uint32_t zero_v[2] = {0u, 0u};

    /* Floor establishes SVS baseline. */
    assert(diting_rpmh_agg_prepare(&agg, DitingRpmhFloor, rails, floor_v, 2, &plan));
    assert(plan.hw_count == 2 && plan.hw_values[0] == 3u && plan.hw_values[1] == 3u);
    diting_rpmh_agg_commit(&agg, &plan);
    assert(diting_rpmh_agg_active(&agg, mx) == 3u && diting_rpmh_agg_active(&agg, cx) == 3u);

    /* GPU bringup votes NOM (7), WLAN votes NOM (7) -> WLAN is a hardware no-op. */
    assert(diting_rpmh_agg_prepare(&agg, DitingRpmhGpu, rails, nom_v, 2, &plan));
    assert(plan.hw_count == 2 && plan.hw_values[0] == 7u && plan.hw_values[1] == 7u);
    diting_rpmh_agg_commit(&agg, &plan);
    assert(diting_rpmh_agg_prepare(&agg, DitingRpmhWlan, rails, nom_v, 2, &plan));
    assert(plan.hw_count == 0);
    diting_rpmh_agg_commit(&agg, &plan);
    assert(agg.skipped == 1 && agg.hw_writes == 2);

    /* GPU releases its APSS vote (0) after GMU startup -> WLAN NOM (7) still holds! */
    assert(diting_rpmh_agg_prepare(&agg, DitingRpmhGpu, rails, zero_v, 2, &plan));
    assert(plan.hw_count == 0);
    diting_rpmh_agg_commit(&agg, &plan);
    assert(diting_rpmh_agg_active(&agg, mx) == 7u && diting_rpmh_agg_active(&agg, cx) == 7u);

    /* WLAN releases its vote (0) -> drops cleanly to Floor SVS (3), never 0! */
    assert(diting_rpmh_agg_prepare(&agg, DitingRpmhWlan, rails, zero_v, 2, &plan));
    assert(plan.hw_count == 2 && plan.hw_values[0] == 3u && plan.hw_values[1] == 3u);
    diting_rpmh_agg_commit(&agg, &plan);
    assert(diting_rpmh_agg_active(&agg, mx) == 3u && diting_rpmh_agg_active(&agg, cx) == 3u);

    /* Uncommitted hardware failure leaves committed state intact. */
    assert(diting_rpmh_agg_prepare(&agg, DitingRpmhGpu, rails, high_v, 2, &plan));
    assert(plan.hw_count == 2 && plan.hw_values[0] == 9u);
    /* Simulate hardware failure by NOT committing. */
    assert(diting_rpmh_agg_active(&agg, mx) == 3u && diting_rpmh_agg_active(&agg, cx) == 3u);

    /* ADSP lcx/lmx boot vote and handover release to 0. */
    const uint32_t adsp_rails[2] = {lcx, lmx};
    assert(diting_rpmh_agg_prepare(&agg, DitingRpmhAdsp, adsp_rails, high_v, 2, &plan));
    assert(plan.hw_count == 2 && plan.hw_values[0] == 9u && plan.hw_values[1] == 9u);
    diting_rpmh_agg_commit(&agg, &plan);
    assert(diting_rpmh_agg_active(&agg, lcx) == 9u);
    assert(diting_rpmh_agg_prepare(&agg, DitingRpmhAdsp, adsp_rails, zero_v, 2, &plan));
    assert(plan.hw_count == 2 && plan.hw_values[0] == 0u && plan.hw_values[1] == 0u);
    diting_rpmh_agg_commit(&agg, &plan);
    assert(diting_rpmh_agg_active(&agg, lcx) == 0u && diting_rpmh_agg_active(&agg, lmx) == 0u);

    /* Reject invalid inputs: duplicate addresses in batch, zero address, invalid consumer. */
    const uint32_t dup[2] = {mx, mx};
    assert(!diting_rpmh_agg_prepare(&agg, DitingRpmhGpu, dup, nom_v, 2, &plan));
    const uint32_t zero_addr[1] = {0};
    assert(!diting_rpmh_agg_prepare(&agg, DitingRpmhGpu, zero_addr, nom_v, 1, &plan));
    assert(!diting_rpmh_agg_prepare(&agg, DitingRpmhConsumerCount, rails, nom_v, 2, &plan));

    printf("PASS rpmh_aggregator\n");
    return 0;
}
'''


class RpmhAggregatorTests(unittest.TestCase):
    def test_consumer_max_aggregation_floor_and_rollback(self):
        with tempfile.TemporaryDirectory(prefix='diting-rpmh-agg-') as tmp:
            src = Path(tmp) / 'test_agg.c'
            exe = Path(tmp) / 'test_agg'
            src.write_text(HARNESS)
            subprocess.run([
                os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra',
                '-Werror', '-pedantic', '-fsanitize=address,undefined',
                '-I' + str(DEVICE / 'drivers/DitingPower'),
                str(src), '-o', str(exe),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.stdout.strip(), 'PASS rpmh_aggregator')


if __name__ == '__main__':
    unittest.main()
