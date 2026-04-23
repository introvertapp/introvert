# introvert

Right now it requires Emby.

Account at IntroDB is also required to used the app.

Enter your details in admin panel when prompted on first launch.

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
