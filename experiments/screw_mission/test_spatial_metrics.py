import unittest
from spatial_metrics import match_points

def p(x,y=.5,cls=1):return dict(x=x,y=y,class_id=cls)
class SpatialTests(unittest.TestCase):
    def test_equal_counts_can_all_be_false(self):
        r=match_points([p(.1),p(.2)],[p(.8),p(.9)],.05)
        self.assertEqual((r['tp'],r['fp'],r['fn'],r['f1']),(0,2,2,0))
    def test_duplicate_prediction_not_double_match(self):
        r=match_points([p(.5)],[p(.5),p(.5)],.05)
        self.assertEqual((r['tp'],r['fp'],r['fn']),(1,1,0))
    def test_augmenting_match_avoids_greedy_failure(self):
        r=match_points([p(.1),p(.2)],[p(.15),p(.05)],.08)
        self.assertEqual(r['tp'],2)
    def test_wrong_class(self):
        self.assertEqual(match_points([p(.5)],[p(.5,cls=2)],.05)['tp'],0)
    def test_missing_and_empty(self):
        r=match_points([p(.5)],[],.05)
        self.assertEqual(r['recall'],0)
        self.assertIsNone(r['precision'])
    def test_invalid_coordinates(self):
        with self.assertRaises(ValueError):match_points([p(float('nan'))],[],.05)

if __name__=='__main__':unittest.main()
