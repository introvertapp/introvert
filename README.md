# introvert

Simple tool used to query TV show episode intro timestamps from Emby and prepare it for submission to IntroDB.

Right now it only works with Emby and requires Emby API key. Jellyfin support will come soon.

Account at IntroDB is also required to used the app.

Enter your details in admin panel when prompted on first launch.

App will import your Emby library and you will be able to browse it and submit to IntroDB.

NOTE: This is still very early and app is being worked on but should be ready enough to play around with.

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

```
docker compose up -d
```

```
http://YOUR_IP_OR_HOST:3001
```
