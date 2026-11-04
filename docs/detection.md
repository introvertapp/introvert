# Run detection

[← Documentation](../README.md#table-of-contents)

Detection finds **Intro** and **Credits** by comparing recurring audio within
a season. Each season needs at least two readable episodes with usable audio.
Recap and Preview can be [imported or edited manually](editor.md).

Before your first scan, [install the detector separately](installation.md#install-the-detector).
It is not bundled with the app. Open **Settings → Application**, click **Check
for Updates**, then click the green **Latest** version badge in the **Detector**
row. If it is missing, starting detection shows guidance to install it. Other
app features remain available. Detector updates are independent of app updates
and apply to new scans without restarting.

## One season

1. Open a show in **TV Shows** and click the season you want.
2. Choose **Intro**, **Credits**, or both.
3. Click **Detect Segments**.

The season button shows progress. When detection finishes, review the timestamps
in the episode list or video editor.

## Multiple seasons

1. Open the show and select your first season.
2. Hold **Shift** and click additional season buttons. The first Shift-click on
   another season includes the currently open season.
3. Shift-click a selected season again to remove it from the selection.
4. Choose **Intro**, **Credits**, or both, then click **Detect N Seasons**.

To scan a whole show, include all its seasons. Each season is processed separately.
Ordinary clicks browse seasons; Shift-clicks change the detection selection.

## Entire library

1. Check the selected source in **Settings → Media**. For Local media, rescan first
   if files have changed.
2. Open **Settings → Detection → Full-library scan**.
3. Choose **Intro**, **Credits**, or both, then click **Scan Library**.

The scan covers all shows and seasons with at least two episodes in the configured
source. Progress shows the current show/season, remaining shows, and an approximate
time remaining. If one show cannot load, the scan logs the error and continues.

## Progress and stopping

- Use **Queue** to check active work and **History** for completed jobs.
- You can browse other screens while detection runs. Keep INTROvert open;
  a large library may take a long time.
- Click **Stop Detection** to stop the show's queued detection group, or
  **Stop Scan** in Settings to stop the full-library scan.
- Completed seasons keep their results. A canceled or failed season does not
  save partial detections. Full-library scans do not resume automatically after quitting.

## Detection settings

Open **Settings → Detection** before starting a new scan.

| Setting | What it does |
| --- | --- |
| Effort: Light | Uses fewer system resources; useful while doing other work. |
| Effort: Medium | Balances resource use and processing speed. |
| Effort: High | Uses more resources for additional parallel work. |
| Visual refinement | Checks short video sections near detected boundaries. Turn off for audio-only detection. |

Effort changes resource use, **not detection quality or accuracy**. Visual
refinement is on by default and applies to new scans.

## Existing timestamps and missed matches

Detection skips segment types that already have timestamps. Saved manual edits
and deletion records are protected from later scans and imports. Changing Effort
or visual refinement does not replace existing timestamps.

Some episodes have no recurring sequence to match. Detection searches the first
five and last three minutes of episodes under 30 minutes, or the first and last
ten minutes of longer episodes. Sequences outside those areas may be missed.

For a missing or incorrect result, [add or adjust the range in the editor](editor.md).
Deleting a timestamp records an intentional deletion; it is not a reset for re-detection.
