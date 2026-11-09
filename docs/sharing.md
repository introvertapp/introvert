# Share timestamps

[← Documentation](../README.md#table-of-contents)

Sharing is optional. Review timestamps against your copy of the movie or episode
before sending them to [IntroDB](https://introdb.app) or [TheIntroDB](https://theintrodb.org).
These are separate community services with separate API keys.

## Configure a service

1. Obtain an API key from the service you want to use.
2. Open **Settings → Endpoints**.
3. Enable that service and enter its **API Key**. Keep the default URLs unless
   you intentionally use a different endpoint.

IntroDB uses the title's IMDb ID. TheIntroDB uses TVDB identification and can
resolve supported TMDB or IMDb IDs. For a local TV show, [fetch metadata or
identify the show](setup.md#local-show-titles-and-artwork) first. Correct episode
numbering is especially important for specials and alternative episode orders.

## Submit selected timestamps

1. Open a movie, or open a TV show and select a season.
2. Select the ranges on the movie or episode cards. The switches below the cards
   select every available range of that type for the movie or selected seasons.
   For TV, Shift-click season buttons to include several seasons.
3. Click **IntroDB** or **TheIntroDB**. Shift-click to select both for ranges
   that both services accept.
4. Click **Submit**.
5. In **Review Submission**, check the title, IDs, selected segments, and
   endpoints. For TV, check the selected season and episodes. For TheIntroDB,
   check the resolved TVDB movie or episode.
6. Click **Confirm** to add the submission to the queue.

Submissions go to the selected community databases. Your media server's
markers remain separate.

### Movie submission limits

Use TheIntroDB for movie Recap, Intro, Credits, and Preview submissions. You can
select multiple ranges of one type when they are at least 5 seconds apart.
Ranges of any type must avoid overlapping your other submissions for that movie.
For IntroDB, select one Credits range between 5 seconds and 15 minutes long.
See the [movie editor's range limits](editor.md#edit-without-opening-video).

## Queue and history

**Queue** shows pending and active detection/submission work. New submissions
can be added while earlier ones are processing. **History** shows completed
work and submission totals. Use search or click a column heading to sort.
Click a movie or show title to return to it.

The submission queue is saved across restarts. Detection scans must be started
again after quitting.

Movie and episode cards show submission status by endpoint. Use the info button
beside **ERROR** to see why a submission failed. **Duplicate** in History means
the service already had a matching timestamp.

INTROvert checks for duplicates and observes service rate limits. If a service
limits requests, let the queue wait rather than repeatedly submitting the same
items. For failures, correct the reported key, identification, or connection
problem before reviewing and submitting again.
