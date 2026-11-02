"""Regression: exercise the real block allocator with every ESP 4-byte alignment."""
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
PREFIX='\n#include <stdint.h>\n#include <stddef.h>\n#include <stdlib.h>\n#include <string.h>\n#include <assert.h>\n#include <stdio.h>\n#define NGTCP2_ERR_NOMEM -1\ntypedef struct hd { union { struct hd *next; uint64_t pad; }; } ngtcp2_memblock_hd;\ntypedef struct { uint8_t *last, *end; } ngtcp2_buf;\ntypedef struct { const void *mem; size_t blklen; ngtcp2_memblock_hd *head; ngtcp2_buf buf; } ngtcp2_balloc;\nstatic uint8_t storage[4096] __attribute__((aligned(16)));\nstatic unsigned offset; static size_t allocated;\nstatic void *ngtcp2_mem_malloc(const void *mem, size_t n) { (void)mem; allocated=n; memset(storage,0xA5,sizeof(storage)); return storage+offset; }\nstatic size_t ngtcp2_buf_left(ngtcp2_buf *b) { return b->last ? b->end-b->last : 0; }\nstatic void ngtcp2_buf_init(ngtcp2_buf *b, void *p,size_t n) { b->last=p;b->end=b->last+n; }\n'
SUFFIX='int main(void) {\n for(offset=0;offset<16;offset+=4) {\n  ngtcp2_balloc b={.blklen=256};void *p=0;\n  assert(ngtcp2_balloc_get(&b,&p,256)==0);\n  memset(p,0,256);\n  for(size_t i=offset+allocated;i<sizeof(storage);++i) if(storage[i]!=0xA5) { fprintf(stderr,"overrun at base alignment %u\\n",offset);return 1; }\n }\n puts("PASS all 4-byte base alignments, full block tail canaries intact");return 0;\n}\n'
class AlignmentTest(unittest.TestCase):
    def test_full_block_fits_all_base_alignments(self):
        source=(ROOT/'src/third_party/ngtcp2/ngtcp2/lib/ngtcp2_balloc.c').read_text()
        function=source[source.index('int ngtcp2_balloc_get('):]
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'test.c';exe=Path(tmp)/'test'
            for legacy in (False,True):
                code=function.replace('sizeof(ngtcp2_memblock_hd) + 0xfu + balloc->blklen',
                                      'sizeof(ngtcp2_memblock_hd) + 0x8u + balloc->blklen') if legacy else function
                p.write_text(PREFIX+code+SUFFIX)
                subprocess.run(['cc','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
                run=subprocess.run([str(exe)],capture_output=True,text=True)
                if legacy:
                    self.assertNotEqual(run.returncode,0)
                    self.assertIn('overrun at base alignment 12',run.stderr)
                else:self.assertEqual(run.returncode,0,run.stderr)
if __name__=='__main__':unittest.main()
