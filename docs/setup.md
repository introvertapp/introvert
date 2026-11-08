# Connect your media

[← Documentation](../README.md#table-of-contents)

Open **Settings → Media** and choose **Plex**, **Emby**, **Jellyfin**, or **Local**.
Settings save automatically. Open **TV Shows** or **Movies** to load the selected source.

## Plex, Emby, or Jellyfin

1. Enter your server's **URL**, including its port when needed.
2. Enter the server credential in **API Key**.
3. Set **Import** to **TV shows**, **Movies**, or **Both**. Each server keeps its own choice.
4. Optionally enable **Import TV timestamps** for existing episode markers or
   **Import movie timestamps** for segments from named chapters.
5. Open **TV Shows** or click the film-strip button next to it for **Movies**.

| Server | Example URL | Credential |
| --- | --- | --- |
| Plex | `http://server:32400` | Your [Plex authentication token](https://support.plex.tv/articles/204059436-finding-an-authentication-token-x-plex-token/) (`X-Plex-Token`) |
| Emby | `http://server:8096` | An API key created in the server dashboard |
| Jellyfin | `http://server:8096` | An API key created in the server dashboard |

Use your server's actual address in place of `server`. Use a direct server URL
that does not redirect to another address. Your credential must allow access to
the selected library. Playback and TV detection need access to the original
media files.

Both timestamp import switches are **off by default** and saved separately for
each provider. Turning them off keeps timestamps already saved in INTROvert.
Use **Rebuild Cache** to refresh the selected server's library and artwork.

### Movie details and chapter imports

Open a movie to see its runtime, logo, and title links when available. Use its
movie card or video editor to review and edit timestamps.

With **Import movie timestamps** enabled, INTROvert converts named chapters to
Intro and Credits when you open a movie. For Intro, an explicit **Intro** chapter
takes priority, followed by labels such as **Studio Logo** or **Studio Logos**,
then **Main Titles** or **Opening Credits**. Chapters named **Credits** or
**End Credits** can supply the Credits segment.

The import uses an explicit Start/End pair or the chapter's end time when
available. For a chapter with a start time only, it uses the next chapter
start or the movie runtime. Saved edits and deletions take precedence. You can
[enter timestamps manually](editor.md) when the movie has no usable chapters.

## Local folders and network storage

1. Select **Local**, then click **Browse** or enter your TV library folder.
2. Mount any network drive first and make sure your account can read the files.
3. Return to **TV Shows** for the initial scan.

Use clear show folders and season/episode numbers, for example:

```text
TV Shows/
  Example Show (2024)/
    Season 01/
      Example Show - S01E01.mkv
      Example Show - S01E02.mkv
```

Supported embedded chapters named Recap, Intro, Credits, or Preview are imported
during discovery. Ordinary browsing uses the saved catalog. After adding,
moving, or removing files, use **Settings → Media → Rescan for changes**.

## Local show titles and artwork

Fetching online metadata is optional; local detection and editing work without it.

1. In **Settings → Media**, choose a **Metadata provider** and **Artwork provider**.
2. Enter the credentials shown for those providers.
3. Click **Fetch metadata**. Use **Stop fetch** to stop; completed shows are kept.

| Provider | Credential |
| --- | --- |
| TMDB | API key |
| TheTVDB | Application API key; subscriber PIN if required |
| OMDb | API key |
| TVmaze | None |
| fanart.tv | Personal API key; artwork only |

For TheTVDB, choose the **Episode order** matching your files. Artwork can use
the metadata provider, another provider, or **Off**. Rescan for new files before
fetching metadata for them.

If a show is matched incorrectly, open it in **TV Shows**, click **Identify**,
and enter its show ID from the selected metadata provider. Check the resulting
show and episode numbering before [sharing timestamps](sharing.md).

[Run TV detection](detection.md) or [review and edit timestamps](editor.md).
