# usage: gpufreq.py <trace> : time-weighted GPU frequency and per-CPU frequency (kHz)
import sys
from perfetto.trace_processor import TraceProcessor
tp = TraceProcessor(trace=sys.argv[1])
q = """
with c as (select t.name tn, coalesce(ct.cpu, -1) cpu, c.ts, c.value,
  coalesce(lead(c.ts) over (partition by c.track_id order by c.ts), (select end_ts from trace_bounds)) nts
  from counter c join counter_track t on c.track_id = t.id
  left join cpu_counter_track ct on ct.id = t.id where t.name like '%freq%')
select tn, cpu, count(*) n, min(value) mn, max(value) mx,
  sum(value * (nts - ts)) / sum(nts - ts) avg from c where nts is not null group by tn, cpu order by tn, cpu"""
for r in tp.query(q):
    print(f"{r.tn:8} cpu{r.cpu:2} n={r.n:5} min={r.mn/1000:.0f} max={r.mx/1000:.0f} avg={r.avg/1000:.0f} MHz")
# GPU frequency changes, in seconds before the end of the trace
for r in tp.query("""select (select end_ts from trace_bounds) - c.ts dt, c.value from counter c
    join counter_track t on c.track_id = t.id where t.name = 'gpufreq' order by c.ts"""):
    print(f"gpufreq at end-{r.dt/1e9:6.1f}s: {r.value/1000:.0f} MHz")
