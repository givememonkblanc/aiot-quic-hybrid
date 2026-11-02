import ctypes
import subprocess
import tempfile
import unittest
from pathlib import Path
import numpy as np
import tensorflow as tf
from model import contrastive, objective, DIMS
from infer import cosine, iou, proposals, quantize_rgb

ROOT=Path(__file__).resolve().parents[2]


class TinyVLMTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory()
        lib=Path(cls.tmp.name)/'scorer.so'
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',
            '-I'+str(ROOT/'src/components/tinyvlm/include'),str(ROOT/'src/components/tinyvlm/tinyvlm.c'),'-lm','-o',str(lib)],check=True)
        cls.lib=ctypes.CDLL(str(lib));cls.score=cls.lib.tinyvlm_cosine_i8
        cls.score.restype=ctypes.c_bool
        cls.score.argtypes=[ctypes.POINTER(ctypes.c_int8),ctypes.c_size_t,ctypes.c_int,
            ctypes.POINTER(ctypes.c_int8),ctypes.c_size_t,ctypes.c_size_t,ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_float),ctypes.POINTER(ctypes.c_size_t)]

    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()

    def test_c_python_cosine_and_zero_point_all_prefixes(self):
        rng=np.random.default_rng(12)
        image=rng.integers(-128,128,256,dtype=np.int8)
        proto=rng.integers(-127,128,(5,256),dtype=np.int8)
        for dim in DIMS:
            scores=np.zeros(5,dtype=np.float32);best=ctypes.c_size_t()
            okay=self.score(image.ctypes.data_as(ctypes.POINTER(ctypes.c_int8)),256,-17,
                proto.ctypes.data_as(ctypes.POINTER(ctypes.c_int8)),5,256,dim,
                scores.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),ctypes.byref(best))
            self.assertTrue(okay)
            expected=cosine(image[:dim],-17,proto[:,:dim])
            np.testing.assert_allclose(scores,expected,atol=1e-6)
            self.assertEqual(best.value,int(np.argmax(expected)))

    def test_zero_vectors_rejected(self):
        image=(ctypes.c_int8*16)(*([0]*16));proto=(ctypes.c_int8*16)(*([1]*16))
        scores=(ctypes.c_float*1)();best=ctypes.c_size_t()
        self.assertFalse(self.score(image,16,0,proto,1,16,16,scores,ctypes.byref(best)))
        self.assertFalse(self.score(proto,16,0,proto,1,16,17,scores,ctypes.byref(best)))

    def test_input_rounding_matches_firmware_for_every_rgb_value(self):
        quantize=self.lib.tinyvlm_quantize_rgb
        quantize.restype=ctypes.c_bool
        quantize.argtypes=[ctypes.POINTER(ctypes.c_uint8),ctypes.c_size_t,ctypes.c_float,
                          ctypes.c_int,ctypes.POINTER(ctypes.c_int8)]
        rgb=np.arange(256,dtype=np.uint8)
        for scale,zp in [(2/255,-1),(.01,0),(.02,30)]:
            output=np.zeros(256,dtype=np.int8)
            self.assertTrue(quantize(rgb.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),256,scale,zp,
                                    output.ctypes.data_as(ctypes.POINTER(ctypes.c_int8))))
            np.testing.assert_array_equal(output,quantize_rgb(rgb,scale,zp))

    def test_loss_alignment_and_gradients(self):
        tf.keras.utils.set_random_seed(9)
        identity=tf.eye(4)
        self.assertLess(float(contrastive(identity,identity)),float(contrastive(identity,tf.roll(identity,1,axis=0))))
        image=tf.Variable(tf.random.normal((4,256)));text=tf.Variable(tf.random.normal((4,256)))
        proj=tf.keras.layers.Dense(512,use_bias=False)
        with tf.GradientTape() as tape:
            loss,parts=objective(image,text,tf.random.normal((4,512)),tf.random.normal((4,512)),proj)
        gradients=tape.gradient(loss,[image,text]+proj.trainable_variables)
        self.assertTrue(np.isfinite(float(loss)))
        self.assertEqual(set(parts),{'contrastive','embedding','matryoshka'})
        for g in gradients:self.assertTrue(g is not None and np.isfinite(g.numpy()).all())

    def test_region_baseline_geometry(self):
        boxes=list(proposals(640,480));self.assertEqual(len(boxes),14)
        self.assertEqual(iou(boxes[0],boxes[0]),1)
        self.assertEqual(iou((0,0,10,10),(20,20,30,30)),0)
        for x,y,r,b in boxes:self.assertTrue(0<=x<r<=640 and 0<=y<b<=480)

if __name__=='__main__':unittest.main()
