# introvert

**NOTE:** This is a personal tool developed to do things quickly and automate the process where possible. While there is error checking built in, app will try to correct as much as possible, but it is still your responsibility to check everything and make sure what you submit is correct.

Simple tool used to query TV show episode intro timestamps from Emby and prepare it for submission to IntroDB.

Right now it only works with Emby and requires Emby API key. Jellyfin support will come soon.

Account at IntroDB is also required to used the app.

Enter your details in admin panel when prompted on first launch.

App will import your Emby library and you will be able to browse it and submit to IntroDB.

NOTE: This is still very early and app is being worked on but should be ready enough to play around with.

## Docker compose
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
