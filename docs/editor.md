# Review and edit timestamps

[← Documentation](../README.md#table-of-contents)

Introvert supports **Recap**, **Intro**, **Credits**, and **Preview** ranges.
Edits are saved locally; they do not rewrite videos or server markers.

## Edit without opening video

1. Open a show and season.
2. Click an existing timestamp, or **No intro**, **No credits**, etc. to add one.
3. Enter **Start** and **End**, then click **Save**.

You can enter seconds (`83.5`), minutes and seconds (`01:23.5`), or hours,
minutes, and seconds (`00:01:23.5`). The start must come before the end, and
the range must fit within the episode. Click **Delete** to remove an existing range.

## Use the video editor

1. Click the episode's **clapperboard icon** to open the video editor.
2. Play or seek to the sequence you want to check.
3. Drag a segment's edges on the timeline, or type its **Start** and **End**
   in the segment drawer. Drag the body of a segment to move the whole range.
4. To add a missing segment, move the playhead to its start and use the drawer's
   insert icon, or press **R**, **I**, **C**, or **P**.
5. Check both boundaries and click the **Save icon**, or press **Cmd/Ctrl + S**.

Scroll over the timeline to zoom. Shift-scroll or scroll horizontally to pan
when zoomed in. Audio levels can help locate transitions. The playback controls
also offer frame-step size, playback speed, mute, auto-play, and embedded
subtitles when available. Hover over an icon to see its action.

Use the episode drawer or **J / L** to switch episodes. Introvert asks you to
save or discard unsaved changes before leaving. The **Reset icon** restores
the saved timeline and resets zoom.

## Useful shortcuts

Use **Cmd** on macOS and **Ctrl** on Windows/Linux. Press **H** or click the
keyboard icon for the full shortcut list.

| Key | Action |
| --- | --- |
| Space / K | Play or pause |
| Left / Right | Move by the selected frame-step size |
| Shift + Left / Right | Move one frame |
| R / I / C / P | Insert Recap / Intro / Credits / Preview |
| Shift + R / I / C / P | Move that segment's nearest edge to the playhead |
| Delete / Backspace | Delete selected segment drafts |
| Cmd/Ctrl + Z | Undo the last timeline edit |
| Cmd/Ctrl + S | Save changes |
| S / E | Show or hide the segment / episode drawer |
| J / L | Previous / next episode |

## Delete timestamps in bulk

1. Open **Settings → Segments**.
2. Select shows, or open a show and select seasons or episodes.
3. Use **Cmd/Ctrl-click** for individual selections or **Shift-click** for a range.
4. Click **Delete segment timestamps** and confirm the selection.

This removes all segment types for the selected items and cannot be undone.
Media files remain unchanged. Deletions are remembered so later imports or
detection do not restore them. To replace a deleted range, enter and save it manually.

Next: [Share selected timestamps](sharing.md).
