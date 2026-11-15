# Connect your media

[← Documentation](../README.md#table-of-contents)

Open **Settings → Media** and choose **Plex**, **Emby**, **Jellyfin**, **Silo**, or **Local**.
Settings save automatically. Open **TV Shows** or **Movies** to load the selected source.

## Plex, Emby, Jellyfin, or Silo

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
| Silo | `http://server:8090` | A key created in **Admin → Sign-in & Security → API Keys**, owned by the account whose libraries you want to access |

Use your server's actual address in place of `server`. Use a direct server URL
that does not redirect to another address. Your credential must allow access to
the selected library. Playback and TV detection need access to the original
media files.

Both timestamp import switches are **off by default** and saved separately for
each provider. Turning them off keeps timestamps already saved in INTROvert.
Use **Rebuild Cache** to refresh the selected server's library and artwork.

### Silo

INTROvert connects through Silo's official native API and automatically uses the
key owner's primary profile. In Silo, open **Admin → Sign-in & Security → API
Keys** and create an **unrestricted (unscoped)** key for the account whose
libraries you want to access. The key should show **Full access**; a key limited
to `libraries:read` cannot fetch the required metadata. The key owner does not
need to be an administrator.

In INTROvert, select **Silo** in **Settings → Media**, enter the normal Silo
server URL and key, then choose **TV shows**, **Movies**, or **Both**. Include
the port and any reverse-proxy path prefix in the URL, for example
`http://server:8090` or `https://example.com/silo`.

Browsing metadata and reading the original video require different permissions.
For editor playback and TV detection, check these settings in Silo:

1. In **Admin → Downloads → Settings**, turn on **Allow downloads**.
2. In **Admin → Users**, edit the account that owns the API key. Under
   **Access & limits**, ensure **Downloads → Offline downloads** is allowed and
   the **Playback & streaming → Max quality** limit permits the original file's
   resolution.

An unrestricted API key still follows these download permissions and quality
limits. If video loading reports **HTTP 403**, check both the server and account
settings, then close and reopen the video editor. Silo describes these checks in
its [original-download documentation](https://github.com/Silo-Server/silo-server/blob/main/docs/downloads-api.md#410-direct-download).

TV shows use Silo's season and episode numbering, including specials in season
0. With **Import TV timestamps** off, episodes load when you open a show. Turn
it on to import available Recap, Intro, Credits, and Preview markers; previously
saved edits and intentional deletions keep their priority. Turning imports on
later fetches the markers without a cache reset. For movies, enable **Import
movie timestamps** to use the [chapter imports](#movie-details-and-chapter-imports)
below.

Silo metadata requests automatically wait and retry when the server rate-limits
them. Let the refresh finish before starting another rebuild. Use **Settings →
Logs** to check a failed request, and **Rebuild Cache** after changes to the
server's library. Posters and logos use INTROvert's normal artwork cache.

### Movie details and chapter imports

Open a movie to see its runtime, logo, and title links when available. Use its
movie card or video editor to review and edit timestamps.

With **Import movie timestamps** enabled, INTROvert converts recognized chapter
names when you open a movie. **Studio Logos** and **Opening Titles** can become
separate Intro ranges, and **End Credits** becomes Credits. Chapters labeled
**Recap** or **Preview** can supply those ranges. Separate ranges of the same
type are imported when they meet the [movie timing rules](editor.md#edit-without-opening-video).
If Intro chapters conflict, explicit Intro labels take priority over studio
logos, followed by opening titles.

Use matching labels such as **Intro Start** and **Intro End** to mark an explicit
pair. For a standalone chapter, the import uses its end time, the start of the
next chapter, or the movie runtime, in that order. Saved edits and deletions
take precedence. You can [enter timestamps manually](editor.md) when the movie
has no usable chapters.

## Local folders and network storage

1. Select **Local**, then use **Local media folders** inside **TV Shows** or **Movies**.
2. Click **Add folder** in that section, then **Browse** or enter its path.
   The section supplies its media type. Use the dropdown to move a folder to the
   other section. Choose folders that do not overlap.
3. Mount any network drive first and make sure your account can read the files.
4. Open **TV Shows** or **Movies** for that library's initial scan.

Existing Local paths become **TV Shows** folders automatically.

Use clear show folders and season/episode numbers, for example:

```text
TV Shows/
  Example Show (2024)/
    Season 01/
      Example Show - S01E01.mkv
      Example Show - S01E02.mkv
```

TV discovery imports supported embedded Recap, Intro, Credits, or Preview chapters.
Local movie scans exclude disc internals (`BDMV` and `VIDEO_TS`), standard
extras folders, and marked extras such as `-trailer`, `-interview`, and
`-deletedscene`. Numeric movie titles and special editions are included.
Use filenames such as `Example Film (2024).mkv` for a readable title and year. Open a movie to view
its playable item. To convert supported chapters such as Studio Logos, Main
Titles, and End Credits into segments, enable **Import movie timestamps** in
**Settings → Media**. This setting defaults to off.

Browsing uses the saved catalog. After adding, moving, or removing files, use
**Rescan for changes** in the corresponding Local section. A movie rescan also
removes previously listed disc clips and recognised extras from the catalog.
If a folder is unavailable during the
rescan, Introvert keeps its saved listings and reports the folder that failed.

## Local metadata and artwork

Local has separate **TV Shows** and **Movies** configuration sections. Each
section contains its own folders, metadata provider, artwork provider, API keys,
and actions to rescan, fetch metadata, and clear its cache.
Movies match by filename title and release year. Matching fills in titles and
available external IDs, posters, and logos. TV matches also include episode names
and numbering.

1. In **Settings → Media**, choose a **Metadata provider** and **Artwork provider**
   in each Local section.
   Artwork defaults to **Same as metadata**. Choose **Off** to hide and skip artwork.
2. Enter the credentials shown for that section's selected providers.
3. Click **Fetch metadata**. Titles and artwork update in the library as each item
   is saved. Use **Stop fetch** to stop and keep completed items.

Successful matches and their cached images are reused for up to 24 hours. Fetching
again retries failed lookups and restores missing cached images.

Local movie names recognise release years followed by edition or quality tags,
such as `Example Film.1996.Remastered.1080p.mkv`. TMDB movie matching follows
Jellyfin's ranking of titles and original titles, nearby release years, and
TMDB's search relevance. Existing unmatched movies use the updated filename
parsing on the next fetch; clearing the library or rescanning is unnecessary.

| Provider | Credential |
| --- | --- |
| TMDB | API key |
| TheTVDB | Application API key |
| OMDb | API key |
| TVmaze | None |
| fanart.tv | Personal API key; artwork only |

TVmaze is available in the TV Shows section. The provider supplies the available
image types. For TheTVDB, choose the **Episode
order** matching your TV files. Rescan after adding or moving files in a previously
scanned folder.

If a show is matched incorrectly, open it in **TV Shows**, click **Identify**,
and enter its show ID from the selected metadata provider. Check the resulting
show and episode numbering before [sharing timestamps](sharing.md).

[Run TV detection](detection.md) or [review and edit timestamps](editor.md).
