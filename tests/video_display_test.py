import unittest

from video_display import analyze


class DisplayTests(unittest.TestCase):
    def check(self, rows, **kwargs):
        return analyze(rows, column=1, period=600, minimum_fps=59, **kwargs)

    def test_loop_and_faster_screen_sampling(self):
        result = self.check([[i / 120, (598 + i // 2) % 600] for i in range(121)])
        self.assertTrue(result['ok'])
        self.assertEqual(result['displayedFPS'], 60)
        self.assertEqual(result['missingFrames'], 0)
        self.assertEqual(result['repeatedSamples'], 60)

    def test_high_rate_does_not_hide_missing_or_reversed_frames(self):
        result = self.check([[0, 1], [.01, 3], [.02, 2], [.03, 4]])
        self.assertGreater(result['displayedFPS'], 60)
        self.assertEqual(result['missingFrames'], 2)
        self.assertEqual(result['backwardTransitions'], 1)
        self.assertFalse(result['ok'])

    def test_frozen_screen(self):
        result = self.check([[0, 10], [.5, 10], [1, 10]])
        self.assertEqual(result['displayedFPS'], 0)
        self.assertEqual(result['maxHoldMs'], 1000)
        self.assertFalse(result['ok'])

    def test_invalid_samples(self):
        for rows in ([], [[0, 1]], [[0, 1], [0, 2]], [[0, 1], [-1, 2]],
                     [[0, 1], [1, 4095]], [[0, 1], [float('nan'), 2]], [[0, 1], [1, 1.5]]):
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                self.check(rows)


if __name__ == '__main__':
    unittest.main()
