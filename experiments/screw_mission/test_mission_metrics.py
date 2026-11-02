import unittest
from mission_metrics import evaluate

class MissionTests(unittest.TestCase):
    def gt(self):return [dict(event_id='s1',start_ms=0,end_ms=100,deadline_ms=1000)]
    def detection(self, **kwargs):
        d=dict(detection_id='d1',matched_event_id='s1',source_ms=50,source_id=5,
               event_receive_ms=500,evidence_receive_ms=600,evidence_source_id=5,evidence_valid=True)
        return dict(d,**kwargs)
    def test_missing_input_not_removed(self):
        self.assertEqual(evaluate(self.gt(),[],0)['model_miss'],1)
    def test_later_valid_evidence_can_complete_mission(self):
        first=self.detection(evidence_valid=False)
        second=self.detection(detection_id='d2',event_receive_ms=650,evidence_receive_ms=700)
        self.assertEqual(evaluate(self.gt(),[first,second],0)['success'],1)
    def test_wrong_source_evidence_is_failure(self):
        self.assertEqual(evaluate(self.gt(),[self.detection(evidence_source_id=9)],0)['delivery_miss'],1)
    def test_clock_uncertainty_not_called_success(self):
        self.assertEqual(evaluate(self.gt(),[self.detection(evidence_receive_ms=995)],10)['clock_uncertain'],1)
    def test_lost_prediction_stays_in_denominator(self):
        self.assertEqual(evaluate(self.gt(),[self.detection(event_receive_ms=None)],0)['delivery_miss'],1)
    def test_unknown_match_rejected(self):
        with self.assertRaises(ValueError):evaluate(self.gt(),[self.detection(matched_event_id='bad')],0)
    def test_false_positive_does_not_complete_event(self):
        r=evaluate(self.gt(),[self.detection(matched_event_id=None)],0)
        self.assertEqual((r['model_miss'],r['unmatched_detections']),(1,1))
    def test_duplicate_results_cannot_multiply_success(self):
        r=evaluate(self.gt(),[self.detection(),self.detection(detection_id='d2')],0)
        self.assertEqual((r['success'],r['repeat_observations']),(1,1))

if __name__=='__main__':unittest.main()
