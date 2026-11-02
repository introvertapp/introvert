# INTROvert

INTROvert is a desktop application for detecting, reviewing, and editing TV
episode segment timestamps. It combines automated intro and credits detection
with a video editor for manually matching segment boundaries to the exact frames
of an episode.

<p align="center">
  <a href="assets/screenshots/tv_shows.png"><img src="assets/screenshots/tv_shows.png" alt="INTROvert TV show library" width="32%"></a>
  <a href="assets/screenshots/episode_details.png"><img src="assets/screenshots/episode_details.png" alt="INTROvert episode details and segment timestamps" width="32%"></a>
  <a href="assets/screenshots/video_editor.png"><img src="assets/screenshots/video_editor.png" alt="INTROvert video editor and segment timeline" width="32%"></a>
</p>

Automated detection compares recurring audio across episodes in a season to find
intros and credits on macOS, Windows, and Linux. Season-wide audio agreement helps
recover partial matches while retaining supported alternate sequences. Optional refinement uses short,
low-resolution video scans near the detected boundaries on every platform.
It looks for supported black-frame transitions and scene cuts without reading
credit text. Resolved audio boundaries skip video refinement. Uncertain intro
starts can expand to a preceding black-frame transition while retaining the matched
audio; unsupported transitions retain their audio boundaries. Detected
timestamps can be reviewed and adjusted against the original video before sharing.

The video editor supports frame-by-frame navigation, a zoomable timeline, audio
levels, and precise timestamp entry. These controls make it possible to check
exactly where a segment starts and ends, correct a detection result, or create
timestamps entirely by hand. Recap, Intro, Credits, and Preview segments can all
be edited, with keyboard shortcuts and episode navigation keeping the review
workflow close to the video.

INTROvert also brings the surrounding timestamp workflow into one application:

- **Media libraries:** Browse episodes from Plex, Emby, Jellyfin, or local media
  folders, including network storage.
- **Existing markers:** Import supported server markers and embedded chapter
  timestamps as a starting point for review.
- **Metadata and artwork:** Identify local shows and fetch their metadata and
  images from selectable providers in the background.
- **Community submissions:** Review and share selected timestamps with
  [IntroDB](https://introdb.app) and [TheIntroDB](https://theintrodb.org), with
  duplicate checks, queued processing, and submission history.
- **Timestamp management:** Organize and remove segments by show, season, or
  episode, and retain saved edits independently of later marker imports.

Timestamps and edits are stored locally. Reviewing and editing segments does not
rewrite the media files or change the markers on a connected media server.

## Detection performance

**Synthetic baseline for a complete 10-episode season**, detecting both Intro
and Credits at the default **Medium** effort setting. Media is stored on a local
SSD; times below are for the whole season.

| Season | Resolution | Audio only | With visual refinement |
| --- | --- | ---: | ---: |
| 10 × 30 min | 720p | ≈5.5 s | ≈6.3 s |
| 10 × 30 min | 1080p | ≈5.6 s | ≈7.1 s |
| 10 × 30 min | 4K | ≈7.7 s | ≈13.3 s |
| 10 × 60 min | 720p | ≈5.5 s | ≈6.3 s |
| 10 × 60 min | 1080p | ≈5.6 s | ≈7.2 s |
| 10 × 60 min | 4K | ≈7.7 s | ≈13.0 s |

Measured on a **Mac mini · Apple M6 · 12-core CPU · 16 GB RAM**, running
**macOS 27.2** (Introvert 0.1.17, detector 0.51.2). Each value is the rounded
median of three fresh-analysis runs using synthetic 24 fps H.264/AAC files with
repeated moving test-pattern video, generated music and quiet gaps. File generation
and library import are excluded; operating-system file caches were not flushed.

Thirty- and sixty-minute episodes both analyze the first and last ten minutes
of audio, so doubling episode length does not necessarily double detection time.

**Estimates only:** these simple synthetic files can be much faster to analyze
than real media. Results may vary drastically with hardware, codecs, bitrate,
frame rate, available memory, background load and network/server performance.
Visual refinement scanned one six-second boundary window per episode here;
other sequences may require more or less work.

## OS and CPU compatibility

| Operating system | CPU architecture | Testing status |
| --- | --- | --- |
| macOS | ARM64 (Apple Silicon) | Tested |
| macOS | x86_64 (Intel) | Tested |
| Windows | x86_64 | Not yet tested |
| Linux | x86_64, ARM64 | Not yet tested |

Only macOS has been tested at runtime. All internal tests pass, including build,
packaging, architecture and dependency checks for Windows and Linux. Native
runtime and installation testing on those platforms is still pending.
