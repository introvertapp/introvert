# Introvert

Introvert is a desktop app for finding, reviewing, editing, and sharing TV episode
segment timestamps. Connect a Plex, Emby, or Jellyfin server, or browse a local
media folder, and manage your library in one place.

## What it does

- **Browse your TV library.** View shows, seasons, episodes, posters, and external
  metadata links. Metadata and artwork load in the background while you navigate.
- **Import existing timestamps.** Read supported markers from media servers,
  including Jellyfin media segments. For local files, read embedded chapters named
  Recap, Intro, Credits, or Preview, including explicit Start/End chapter pairs.
  Ordinary chapter names are left alone. Availability depends on the server or
  metadata embedded in the file.
- **Detect intros and credits.** Compare recurring audio across episodes in a
  season. At least two usable episodes are required. Run detection again to
  replace imported timestamps, then review the results before sharing them.
- **Edit with video playback.** Inspect an episode, adjust segment boundaries on
  the timeline or enter precise timestamps, and manage Recap, Intro, Credits, and
  Preview segments. Keyboard shortcuts, audio levels, and playback controls help
  with review.
- **Submit selected segments.** Review submissions to [IntroDB](https://introdb.app)
  and [TheIntroDB](https://theintrodb.org), with a persistent queue, history,
  duplicate checks, and endpoint rate limits.
- **Manage your data.** Delete timestamps by show, season, or episode; refresh
  metadata and artwork caches; and optionally receive detection completion
  notifications through generic or Discord webhooks.

Imported timestamps provide a starting point. Your saved edits and deletions take
priority over later imports. Segment changes stay in Introvert's local database;
importing and editing do not rewrite your media files or your server's markers.
Submission to a community service requires your review and confirmation.

## Getting started

1. Open Introvert and go to **Settings → Media**.
2. Select Plex, Emby, Jellyfin, or Local. Enter the server URL and token/API key,
   or choose your local TV media folder.
3. Open **TV Shows**, select a show and season, and review any existing timestamps.
4. Use **Detect Segments** for intros and credits, or open the video editor to
   create and adjust timestamps yourself.
5. Configure your preferred submission service in Settings, select the segments
   you want to share, and review the submission before confirming it.

Local library metadata can optionally be enriched through TVmaze and TMDB.
TMDB artwork requires a TMDB API key. Remote playback uses the original media
stream, so file access, server permissions, and codec support affect playback and
detection.

## Storage and privacy

Introvert stores settings, credentials, library metadata, timestamps, detection
jobs, and submission history locally in `data/app.db` beneath its application data
root. Downloaded artwork is stored separately in `data/media-image-cache`.
On macOS, the default application data root is
`~/Library/Application Support/Introvert`.

Server and service credentials are stored in the local database. Keep your
application data private. Network requests go to the providers and services you
configure, including optional metadata services and webhooks. Queue and history
dates are displayed using your system locale and time zone.

## Source and distribution

Introvert uses C++17, Qt Quick/QML, Qt Multimedia, SQLite, FFmpeg, and Chromaprint.
This repository publishes the application and detector source, shared UI,
production assets, and dependency license notices. Local build definitions,
tests, fixtures, packaging tools, and internal documentation are maintained
separately and are not included in this source checkout. It is not a standalone
CMake build kit.

Packaged downloads, when published, are available from
[GitHub Releases](https://github.com/introvertapp/introvert/releases). Packaged
applications include the runtimes they require; end users do not need a compiler
or development tools.

## License

Introvert is available under the [ISC license](LICENSE). Third-party components
retain their own licenses; see [`licenses/`](licenses/) and
[`third_party/`](third_party/).
