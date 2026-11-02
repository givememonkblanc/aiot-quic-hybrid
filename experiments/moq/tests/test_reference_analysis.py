import unittest

from literature.analyze_reference import switching_row, timeout_row


class ReferenceAnalysisTests(unittest.TestCase):
    def test_timeout_denominator_includes_missing_frames_and_descriptions(self):
        data = dict(
            offered_frames=3,
            descriptions_per_frame=2,
            produced_s=[0, 0.1, 0.2],
            group_frames=5,
            bottleneck=dict(forwarded_bytes=100, queue_drops=0),
            events=[],
        )
        for frame, tile, t in [(0, 0, 0.05), (0, 1, 0.06), (1, 0, 0.8)]:
            data["events"].append(
                dict(
                    event="description",
                    frame=frame,
                    tile=tile,
                    group=0,
                    payload_valid=True,
                    received_s=t,
                )
            )
        result = timeout_row(data, {})
        self.assertEqual(result["complete_frames"], 1)
        self.assertEqual(result["zero_description_frames"], 1)
        self.assertEqual(result["spatial_coverage_fraction"], 0.5)
        self.assertEqual(result["coverage_within_500ms"], 2 / 6)
        self.assertEqual(result["complete_frames_within_500ms"], 1)

    def test_switch_duplicates_are_not_extra_successes(self):
        data = dict(
            offered_frames=3,
            produced_s=[0, 0.1, 0.2],
            group_frames=5,
            old_alias=1,
            new_alias=2,
            switch_request_s=0.15,
            objects=[],
        )
        for obj, alias, t, size in [(0, 1, 0.1, 100), (0, 2, 0.2, 50), (2, 2, 0.5, 100)]:
            data["objects"].append(
                dict(group=0, object=obj, alias=alias, t=t, bytes=size, payload_valid=True)
            )
        result = switching_row(data, {})
        self.assertEqual(result["unique_frames"], 2)
        self.assertEqual(result["missing_frames"], 1)
        self.assertEqual(result["duplicate_objects"], 1)
        self.assertEqual(result["duplicate_complete_payload_bytes"], 50)
        self.assertEqual(result["frames_within_500ms"], 2)
        data["objects"][0]["payload_valid"] = False
        with self.assertRaises(AssertionError):
            switching_row(data, {})
