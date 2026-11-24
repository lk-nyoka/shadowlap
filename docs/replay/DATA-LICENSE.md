# Data on the replay page

`index.html` embeds two kinds of content:

- **Code** (the Shadow Lap dashboard and the replay shim): MIT, like the rest of the repository.
- **Data**: the lap (speed and track positions) comes from [OpenF1](https://openf1.org), an independent
  project not affiliated with Formula 1, and the recorded run's `compare.csv` is derived from that lap.
  This data is shared under [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/):
  attribution to OpenF1, non-commercial use only, and anything built from it must use the same licence.

The run was recorded on QNX SDP 8.0 in QEMU (nested in WSL2 on a laptop) in November 2026, with
candidate v2 on a hot day with worn tyres (the same run as the demo video). Rebuild with `tools/make_replay.py`.
