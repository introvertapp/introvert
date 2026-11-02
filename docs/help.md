# Settings and help

[← Documentation](../README.md#table-of-contents)

Settings save automatically. Use these sections for everyday preferences and maintenance.

## Display and cache

In **Settings → Application**, choose your **Timestamp format** and **TV Show
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

1. Open **Settings → Application** and click **Check for Updates**.
2. If an update is available, click the green **Latest** version download badge
   for Introvert or the Detector.
3. Follow the status message. For an application update, click **Restart to Update**
   when the download is ready.

Detector updates take effect for new scans without restarting. Scans already
running keep their current detector version.

Close the editor and let active detection and submission work finish before
restarting. On Linux, application installation may require system authorization.
You can also install a newer [release package](installation.md) manually.

Checks are **Manual** by default. **Automatic** checks at startup and periodically
while Introvert is open; it still waits for you to download and install updates.
App updates keep your settings and saved timestamps.

## Back up your data

Quit Introvert completely, then copy its application-data folder to a safe location.
Restore it while Introvert is closed.

| System | Default folder |
| --- | --- |
| macOS | `~/Library/Application Support/Introvert` |
| Windows | `%LOCALAPPDATA%\Introvert` |
| Linux | `~/.local/share/Introvert` |

Linux uses `$XDG_DATA_HOME/Introvert` when that variable is set. A custom
`--data-dir` launch option overrides the default folder.

The folder includes `data/app.db` (settings, API keys, timestamps, queue, and
history) and `data/media-image-cache` (downloaded artwork). Keep backups private:
the database contains your saved credentials.

On macOS, closing the window keeps Introvert running. Use **Cmd + Q** to quit
before a backup. On Windows and Linux, closing the main window exits the app.

## Troubleshooting

| Problem | What to check |
| --- | --- |
| Library will not load | Check **Settings → Media**, the server URL and key, or local folder permissions. Mount network storage and allow local-network access if prompted. |
| New local episodes are missing | Use **Rescan for changes**. Use filenames with season/episode numbers such as `S01E01`. |
| Local show has the wrong title or no artwork | Check metadata credentials, use **Fetch metadata**, or correct the match with **Identify**. |
| Video will not open | Check file access and original-file download permissions on the server. Introvert does not use server transcoding. |
| No Intro or Credits found | Check that the season has at least two usable episodes and a recurring sequence within the [search areas](detection.md#existing-timestamps-and-missed-matches). Add the range manually if needed. |
| Re-running detection leaves a timestamp unchanged | Existing timestamps, edits, and intentional deletions are protected. [Edit the range](editor.md) to correct it. |
| Submission fails | Click **FAILED** on the episode; check the endpoint key, show IDs, and episode mapping. |
| Update cannot install | Finish active jobs and close the editor. On macOS, move the app to a writable Applications folder; on Linux, use an installed DEB/RPM package. |

For more detail, open **Settings → Logs**, search the message, and filter by
level. Logs cover the current session and are cleared when the app exits.
When [reporting an issue](https://github.com/introvertapp/introvert/issues), include
your OS, app and detector versions, the steps you took, and the relevant message.
Leave out API keys, private server URLs, and your database.
