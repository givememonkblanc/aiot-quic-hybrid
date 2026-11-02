#!/usr/bin/env python3
"""Differential validation against actual pinned live-player method bodies."""

import ctypes
import hashlib
import json
import math
from pathlib import Path
import random
import re
import subprocess
import tempfile

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[2]


def method(source, name):
    start = source.index(name + " = (")
    begin = source.index("{", start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    signature = source[start:begin].replace(name + " = ", "const " + name + " = ", 1)
    signature = re.sub(r"\??: (?:any\[\]|number|string)", "", signature)
    body = source[begin:end]
    body = re.sub(r": any\[\]", "", body)
    return signature + body + ";\n"


def main():
    upstream = BASE / "vendor/public-moq-demo/player/src/player.ts"
    source = upstream.read_text()
    rng = random.Random(20260924)
    traces = [
        [[10000, 100000, True]] * 100,
        [[0, 0, True]] * 40 + [[1000, 1000, True]] * 30,
        [[1000, 10000, True]] * 25 + [[1000000, 1, True]] * 25,
        [[(i % 7 + 1) * 1000, (i % 7 + 1) * 1000 * (1 + i % 3), True] for i in range(150)],
        [
            [
                rng.randrange(0, 100001),
                rng.choice([0, 1, rng.randrange(1000, 3000001)]),
                i % 30 == 0,
            ]
            for i in range(2000)
        ],
    ]
    js = method(source, "filterStats") + method(source, "computeTPut")
    # Execute original IFA update block; only replace object state access.
    begin = source.index("const lastVal = (this.throughputs.get('ifa') || 0);")
    end = source.index("\n\t\t\t\t\t}", begin)
    ifa = source[begin:end].replace("(this.throughputs.get('ifa') || 0)", "(prev || 0)")
    ifa = ifa.replace("this.throughputs.set('ifa', val);", "prev = val;")
    js += "\nconsole.log=()=>{};\nconst traces=" + json.dumps(traces) + ";\n"
    js += """const results=traces.map(trace=>{
      let stats=[],swma=0,prev=0;
      return trace.map(([bytes,us,first],i)=>{
        const row=[i,bytes,us/1000];stats.push(row);
        if(first){const val=computeTPut([row]); REFERENCE_IFA }
        if(stats.length>=25 && stats.length%5===0){
          const t=computeTPut(filterStats(stats.slice(-25),5,'percentage',swma));
          if(t>0)swma=t;
        }
        return [swma/8,prev/8];
      });
    });process.stdout.write(JSON.stringify(results));""".replace("REFERENCE_IFA", ifa)
    with tempfile.TemporaryDirectory(prefix="moq-estimator-") as temporary:
        temp = Path(temporary)
        (temp / "golden.js").write_text(js)
        (temp / "wrapper.c").write_text("""#include "moqt_swma.h"
#include <string.h>
static moqt_swma s;static double prev;
void reset(void){memset(&s,0,sizeof(s));prev=0;}
double observe(size_t b,uint64_t us){moqt_swma_observe(&s,b,us);return s.value;}
double ifa(size_t b,uint64_t us){prev=moqt_ifa_observe(prev,b,us);return prev;}
""")
        subprocess.run(
            [
                "gcc",
                "-shared",
                "-fPIC",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I" + str(ROOT / "src/components/transport"),
                str(temp / "wrapper.c"),
                "-o",
                str(temp / "port.so"),
            ],
            check=True,
        )
        golden = json.loads(subprocess.check_output(["node", str(temp / "golden.js")]))
        lib = ctypes.CDLL(str(temp / "port.so"))
        for name in ("observe", "ifa"):
            fn = getattr(lib, name)
            fn.argtypes = [ctypes.c_size_t, ctypes.c_uint64]
            fn.restype = ctypes.c_double
        checked = 0
        for trace, expected in zip(traces, golden):
            lib.reset()
            last = 0
            for (b, us, first), (swma, ifa_value) in zip(trace, expected):
                got = lib.observe(b, us)
                if first:
                    last = lib.ifa(b, us)
                for a, e in ((got, swma), (last, ifa_value)):
                    assert (e is None and math.isnan(a)) or (
                        e is not None and math.isclose(a, e, rel_tol=1e-12, abs_tol=1e-8)
                    ), (a, e)
                    checked += 1
    report = dict(
        status="pass",
        values_compared=checked,
        traces=len(traces),
        source_sha256=hashlib.sha256(upstream.read_bytes()).hexdigest(),
        source_commit="2071137a5967e645ab97b6e914dc4f30c8c47d95",
        scope="live player SWMAth and first-chunk IFA arithmetic; not end-to-end paper reproduction",
    )
    (BASE / "results/estimator-golden.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
