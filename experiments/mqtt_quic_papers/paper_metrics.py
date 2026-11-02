"""Paper KPI definitions; missing measurements are never replaced with proxies."""
import statistics


def quantile(values, percent):
    values = sorted(values)
    rank = (len(values) - 1) * percent / 100
    lower = int(rank)
    upper = min(lower + 1, len(values) - 1)
    return values[lower] + (values[upper] - values[lower]) * (rank - lower)


def delay_metrics(values):
    mean = statistics.mean(values)
    # Paper gives SD/mean, but not ddof. Export both and identify the estimator.
    return {
        **{f'latency_p{p}_ms': quantile(values, p) for p in (5, 25, 75)},
        'latency_population_sd_ms': statistics.pstdev(values),
        'latency_rsd_population': statistics.pstdev(values) / mean if mean else None,
        'latency_rsd_sample': statistics.stdev(values) / mean if mean and len(values) > 1 else None,
    }


def allocation_metrics(path):
    import memray
    python_allocators = {memray.AllocatorType.PYMALLOC_MALLOC,
                         memray.AllocatorType.PYMALLOC_CALLOC,
                         memray.AllocatorType.PYMALLOC_REALLOC}
    ignored = {memray.AllocatorType.PYMALLOC_FREE, memray.AllocatorType.FREE,
               memray.AllocatorType.MUNMAP}
    python_bytes = native_bytes = 0
    for record in memray.FileReader(str(path)).get_allocation_records():
        if record.allocator in python_allocators:
            python_bytes += record.size
        elif record.allocator not in ignored:
            native_bytes += record.size
    # Python arenas and their native backing storage overlap. Never add these.
    return dict(python_allocation_total_bytes=python_bytes,
                native_allocation_total_bytes=native_bytes,
                allocation_scope='whole host Python process during trial; excludes broker; '
                                 'Python/native layers overlap and must not be summed',
                allocation_tool=f'memray {memray.__version__}; trace_python_allocators=True')


def metric_status(profiled):
    return {
        'latency_distribution_rsd': 'measured; same-host monotonic clock; linear quantiles; both SD estimators',
        'execution': 'measured; publisher connect API to final unique delivery/PUBACK; shutdown excluded',
        'cpu': 'measured; getrusage cumulative user+system seconds; clients+proxies, broker excluded',
        'memory': ('measured separately for Python/native allocation layers; not Go pprof or ESP32 RAM'
                   if profiled else 'not measured in timing pass; use --allocation-profile'),
        'first_data_frame': 'unavailable without decrypted packet capture; API timing is separate',
        'wire_bytes_packets': 'unavailable in this run; offline pcap_metrics.py accepts actual captures',
        'power': 'unavailable; no process power meter or external meter attached to this experiment',
        'footprint': 'not comparable; interpreted Python with shared native dependencies, not a compiled architecture',
        'handoff': 'not exercised; loopback baseline only',
    }
