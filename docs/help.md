# Settings and help

[← Documentation](../README.md#table-of-contents)

Settings save automatically. Use these sections for everyday preferences and maintenance.

Press **⌘K** on macOS or **Ctrl+K** on Windows/Linux to focus the current
screen's search field. The shortcut hint changes to a clear button while editing
a nonempty search. In **Settings → Segments**, focus the **TV Show** column and
type a show's starting letters to jump to it. Pause briefly to start a new prefix.

## Display and cache

In **Settings → Application**, choose your **Timestamp format** and **Library
listing type**. **Basic (no images)** uses a text list instead of posters.

**Image cache → Clear cache** removes downloaded artwork. **Settings → Media →
Rebuild Cache** refreshes a server source; Local media uses **Rescan for changes**.
**Clear local cache** removes metadata for the selected local folder. These
actions keep your media files, saved timestamp edits, and credentials.

## Notifications

Open **Settings → Notifications** to choose in-app alerts for detection
completion, errors, and application/detector updates. Update alerts require
automatic update checks.

For a webhook, enable **Webhook notifications**, enter its **URL** and optional
**Key**, then click **Test**. Discord webhook URLs are supported. Webhooks report
successful detection completion; they do not report submission completion.

## Updates

The app and detector are updated independently. The detector is
[installed separately](installation.md#install-the-detector) and is required
only for automated detection. Its installed version shows **Not installed**
until you download it through the same controls below.

1. Open **Settings → Application** and click **Check for Updates**.
2. When a download is available, click the green **Latest** version badge for
   INTROvert or the Detector. Use the Detector badge for first installation as
   well as updates.
3. Follow the status message. For an application update, click **Restart to Update**
   when the download is ready.

Click the **ⓘ** inside the right edge of an available update button to read its
release notes before downloading. Application and Detector updates each show
their own notes.

Detector updates take effect for new scans without restarting. Scans already
running keep their current detector version.

Close the editor and let active detection and submission work finish before
restarting. On Linux, application installation may require system authorization.
You can also install a newer [release package](installation.md) manually.

Checks are **Manual** by default. **Automatic** checks at startup and periodically
while INTROvert is open; it still waits for you to download and install updates.
App updates keep your settings and saved timestamps.

## Back up or move your data

To move to another machine, including a different operating system:

1. Quit INTROvert completely on both machines. On macOS, use **Cmd + Q**;
   closing the window keeps the app running.
2. Copy `data/app.db` from the source application-data folder into `data/` in
   the destination application-data folder, replacing its existing database.
   Keep a backup of the destination database first if it contains data you need.
3. Open the same or a newer version of INTROvert on the destination machine.

The database carries settings, API keys, library metadata, saved timestamps and
intentional deletions, detection records, submission queue, and history. No
conversion is needed between macOS, Windows, and Linux. Copy after a clean quit;
a database copied while the app is running may be missing recent changes.

| System | Default folder |
| --- | --- |
| macOS | `~/Library/Application Support/Introvert` |
| Windows | `%LOCALAPPDATA%\Introvert` |
| Linux | `~/.local/share/Introvert` |

Linux uses `$XDG_DATA_HOME/Introvert` when that variable is set. A custom
`--data-dir` launch option overrides the default folder.

Downloaded artwork lives separately in `data/media-image-cache` and can be
fetched again. To back up artwork too, copy the whole application-data folder
while INTROvert is closed. Keep backups private: the database contains your
saved credentials.

Local media files are separate from the database. If their paths differ on the
new machine, choose the destination media folder in **Settings → Media**.
Server sources must remain reachable from the new machine. Install the app
package for the destination OS and CPU, then [install the detector](installation.md#install-the-detector)
through Settings before running detection there.

## Troubleshooting

| Problem | What to check |
| --- | --- |
| Library will not load | Check **Settings → Media**, the server URL and key, or local folder permissions. Mount network storage and allow local-network access if prompted. |
| New local episodes are missing | Use **Rescan for changes**. Use filenames with season/episode numbers such as `S01E01`. |
| Local show has the wrong title or no artwork | Check metadata credentials, use **Fetch metadata**, or correct the match with **Identify**. |
| Video will not open | Check file access and original-file download permissions on the server. INTROvert does not use server transcoding. |
| Detector unavailable | [Install the detector](installation.md#install-the-detector) through **Settings → Application → Check for Updates**, then start detection again. |
| No Intro or Credits found | Check that the season has at least two usable episodes and a recurring sequence within the [search areas](detection.md#existing-timestamps-and-missed-matches). Add the range manually if needed. |
| Re-running detection leaves a timestamp unchanged | Existing timestamps, edits, and intentional deletions are protected. [Edit the range](editor.md) to correct it. |
| Submission fails | Click **FAILED** on the episode; check the endpoint key, show IDs, and episode mapping. |
| Update cannot install | Finish active jobs and close the editor. On macOS, move the app to a writable Applications folder; on Linux, use an installed DEB/RPM package. |

For more detail, open **Settings → Logs**, search the message, and filter by
level. Logs cover the current session and are cleared when the app exits.
When [reporting an issue](https://github.com/introvertapp/introvert/issues), include
your OS, app and detector versions, the steps you took, and the relevant message.
Leave out API keys, private server URLs, and your database.
