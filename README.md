# introvert

Introvert is a self-hosted web app for managing and submitting TV show intro timestamps.  
It integrates with media servers like Emby, Jellyfin and Plex, and submits intro data to [IntroDB](https://introdb.app).

## Current media server support
- [Emby](https://emby.media)
  * Full metadata, image and intro timestamp support
- [Jellyfin](https://jellyfin.org)
  * Metadata and image support only. Jellyfin doesn't have native support for intro timestamps. Support may come in the future. In the meantime, timestamps can be manually entered.
- [Plex](https://www.plex.tv))
  * Metadata and image support only. Intro timestamps support may come in the future. In the meantime, timestamps can be manually entered.
 
## Features

- Browse shows from your media server
- View seasons and episodes
- Edit intro timestamps per episode
- Submit intros to [IntroDB](https://introdb.app)
- Avoid duplicate submissions via lookup checks
- Optional local database tracking of submitted episodes
- Visual indicator for already submitted episodes
- Multi-provider support (Emby + Jellyfin + Plex)
- Docker-ready for easy deployment

## Getting Started (Docker)
```
services:
  introvert:
    container_name: introvert
    image: ghcr.io/introvertapp/introvert:latest
    restart: unless-stopped
    user: 1000:1000
    ports:
      - "3001:3001"
    environment:
      PORT: 3001
      SETTINGS_DB_PATH: /data/app.db
    volumes:
      - YOUR_PATH_HERE:/data
```

## Launch container
```
docker compose up -d
```

## Launch the app
```
http://YOUR_IP_OR_HOST:3001
```

## Initial Setup

On first launch:

1. You will be redirected to the **Admin Panel**
2. Configure (all fields are required):

### Provider
- Select provider (Emby / Jellyfin / Plex)
- Enter:
  - Base URL (e.g. `http://10.0.1.12:8096`)
  - API Key

### IntroDB
- Submit URL (default prefilled)
- Lookup URL (default prefilled)
- API Key

### API Timeout
- How long will the app wait for IntroDB response (ex. 10000)

### API Delay
- Delay between each submission to IntroDB. Keep this at a reasonably
  high number so you don't spam the API

### Optional
- Enable **Save submissions in local DB**

---

## Local Submission Tracking

When enabled:

- Successful submissions are stored in SQLite
- Episodes display:
  - Pulsating dot next to title = already submitted

Stored data includes:
- provider ID + episode ID
- IMDb ID
- season/episode numbers
- intro start/end timestamps
- submission result + timestamp

---

## Submission Flow

For each episode:

1. Lookup IntroDB  
2. Compare:
   - Missing → submit
   - Different → submit
   - Duplicate → skip  
3. Handle rate limits gracefully  
4. Optionally store locally

---

## License

This project is licensed under the Creative Commons Attribution-NonCommercial 4.0 International License.

You are free to use, modify, and distribute this software for non-commercial purposes only.

Commercial use is prohibited without explicit permission.
