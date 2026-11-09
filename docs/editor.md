# Review and edit timestamps

[← Documentation](../README.md#table-of-contents)

INTROvert supports **Recap**, **Intro**, **Credits**, and **Preview** ranges for
TV episodes and movies. Edits are saved locally, separate from your videos and
server markers.

## Edit without opening video

1. Open a movie in **Movies**, or open a show and season in **TV Shows**.
2. Click an existing timestamp, or **No intro**, **No credits**, etc. to add one.
3. Enter **Start** and **End**, then click **Save**.

You can enter seconds (`83.5`), minutes and seconds (`01:23.5`), or hours,
minutes, and seconds (`00:01:23.5`). The start must come before the end, and
the range must fit within the video. Click **Delete** to remove an existing range
from the movie or episode card.

TV episode ranges can be 5 seconds to 3 minutes long. Movie ranges have the same
5-second minimum and can be up to 5 minutes for Intro, 20 minutes for Recap, or
30 minutes for Credits and Preview. Movie Credits must start at least 5 seconds
into the video. Movie ranges cannot overlap. Leave at least 5 seconds between
ranges of the same type.

## Use the video editor

1. Click the **clapperboard icon** on the movie or episode card.
2. Play or seek to the sequence you want to check.
3. Drag a segment's edges on the timeline, or type its **Start** and **End**
   in the segment drawer. Drag the body of a segment to move the whole range.
4. To add a segment, move the playhead to its start and use the drawer's insert
   icon, or press **R**, **I**, **C**, or **P**. In a movie, this can add a second
   range of a type you already have.
5. Check both boundaries and click the **Save icon**, or press **Cmd/Ctrl + S**.

To split movie Credits around a mid-credits scene, add one range before the scene
and another after it. Once saved, each range has its own row on the movie card
where you can edit it or select it for sharing.

Scroll over the timeline to zoom. Shift-scroll or scroll horizontally to pan
when zoomed in. Audio levels can help locate transitions. The playback controls
also offer frame-step size, playback speed, mute, auto-play, and embedded text
subtitles when available. The editor skips image-based subtitles such as PGS.
Hover over an icon to see its action.

When editing TV, use the episode drawer or **J / L** to switch episodes. The
drawer shows the current movie's ranges when editing a movie. INTROvert asks
you to save or discard unsaved changes before leaving. The **Reset icon**
restores the saved timeline and resets zoom.

## Useful shortcuts

Use **Cmd** on macOS and **Ctrl** on Windows/Linux. Press **H** or click the
keyboard icon for the full shortcut list.

| Key | Action |
| --- | --- |
| Space / K | Play or pause |
| Left / Right | Move by the selected frame-step size |
| Shift + Left / Right | Move one frame |
| R / I / C / P | Insert Recap / Intro / Credits / Preview |
| Shift + R / I / C / P | Move the closest edge of that type to the playhead |
| Delete / Backspace | Delete selected segment drafts |
| Cmd/Ctrl + Z | Undo the last timeline edit |
| Cmd/Ctrl + S | Save changes |
| S / E | Show or hide the segment / episode drawer |
| J / L | Previous / next episode |

## Delete timestamps in bulk

1. For TV episodes, open **Settings → Segments**.
2. Select shows, or open a show and select seasons or episodes.
3. Use **Cmd/Ctrl-click** for individual selections or **Shift-click** for a range.
4. Click **Delete segment timestamps** and confirm the selection.

This removes all segment types for the selected items and cannot be undone.
Media files remain unchanged. Deletions are remembered so later imports or
detection do not restore them. To replace a deleted range, enter and save it manually.

[Share selected timestamps](sharing.md).
