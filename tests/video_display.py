"""Validate screen-sampled frame numbers from a looping numbered video fixture."""
import argparse
import csv
import json
import math
from pathlib import Path


def analyze(rows, *, column, period, minimum_fps, max_missing=0):
    if column < 1 or period < 3 or not math.isfinite(minimum_fps) or minimum_fps <= 0 or max_missing < 0:
        raise ValueError('Invalid display validation parameters')
    samples = []
    for row in rows:
        timestamp, identity = float(row[0]), float(row[column])
        if not math.isfinite(timestamp) or not math.isfinite(identity) or not identity.is_integer() or not 0 <= identity < period:
            raise ValueError('Invalid screen timestamp or frame number')
        if samples and timestamp <= samples[-1][0]:
            raise ValueError('Screen timestamps must increase')
        samples.append((timestamp, int(identity)))
    if len(samples) < 2:
        raise ValueError('At least two screen samples are required')
    changes = missing = backwards = repeats = 0
    last_change = samples[0][0]
    max_hold = 0.0
    for previous, current in zip(samples, samples[1:]):
        step = (current[1] - previous[1]) % period
        if not step:
            repeats += 1
            continue
        changes += 1
        max_hold = max(max_hold, current[0] - last_change)
        last_change = current[0]
        if step >= period / 2:
            backwards += 1
        else:
            missing += step - 1
    max_hold = max(max_hold, samples[-1][0] - last_change)
    seconds = samples[-1][0] - samples[0][0]
    fps = changes / seconds
    return {
        'scope': 'screen-sampled-presentation', 'samples': len(samples),
        'seconds': seconds, 'frameChanges': changes, 'displayedFPS': fps,
        'missingFrames': missing, 'backwardTransitions': backwards,
        'repeatedSamples': repeats, 'maxHoldMs': max_hold * 1000,
        'minimumFPS': minimum_fps, 'maximumMissingFrames': max_missing,
        'ok': fps >= minimum_fps and missing <= max_missing and backwards == 0,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samples', type=Path, help='Screen sampler CSV or JSON containing samples')
    parser.add_argument('--column', type=int, required=True, help='Frame-number column, with timestamp at column 0')
    parser.add_argument('--period', type=int, required=True, help='Number of consecutive IDs before the fixture loops')
    parser.add_argument('--minimum-fps', type=float, required=True)
    parser.add_argument('--max-missing', type=int, default=0)
    args = parser.parse_args()
    try:
        with args.samples.open(encoding='utf-8-sig', newline='') as source:
            rows = json.load(source)['samples'] if args.samples.suffix == '.json' else list(csv.reader(source))
        result = analyze(rows, column=args.column, period=args.period,
                         minimum_fps=args.minimum_fps, max_missing=args.max_missing)
    except (OSError, ValueError, TypeError, KeyError, IndexError) as error:
        result = {'scope': 'screen-sampled-presentation', 'ok': False, 'error': str(error)}
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['ok'] else 1)


if __name__ == '__main__':
    main()
