# Share timestamps

[← Documentation](../README.md#table-of-contents)

Sharing is optional. Review timestamps against your copy of the movie or episode
before sending them to [IntroDB](https://introdb.app) or [TheIntroDB](https://theintrodb.org).
These are separate community services with separate API keys.

## Configure a service

1. Obtain an API key from the service you want to use.
2. Open **Settings → Endpoints**.
3. Enable that service and enter its **API Key**. Saved keys are masked. Click
   the eye icon to reveal or hide a key. Keep the default URLs unless you
   intentionally use a different endpoint.

IntroDB uses the title's IMDb ID. TheIntroDB uses TVDB identification and can
resolve supported TMDB or IMDb IDs. For a local TV show, [fetch metadata or
identify the show](setup.md#local-show-titles-and-artwork) first. Correct episode
numbering is especially important for specials and alternative episode orders.

## Submit selected timestamps

1. Open a movie, or open a TV show and select a season.
2. Select the ranges on the movie or episode cards. The switches below the cards
   select every available range of that type for the movie or selected seasons.
   For TV, Shift-click season buttons to include several seasons.
3. Click **IntroDB** or **TheIntroDB**. Shift-click the other service to select
   both. For movies, check the [submission limits](#movie-submission-limits).
4. Click **Submit**.
5. In **Review Submission**, check the title, IDs, selected segments, and
   endpoints. For TV, check the selected season and episodes. For TheIntroDB,
   check the resolved TVDB movie or episode.
6. Click **Confirm** to add the submission to the queue.

Submissions go to the selected community databases. Your media server's
markers remain separate.

### Movie submission limits

TheIntroDB accepts all four movie segment types and queues each selected range
separately. Ranges cannot overlap, and ranges of the same type must be at least
5 seconds apart. See the [movie editor's range limits](editor.md#edit-without-opening-video).

IntroDB accepts one movie Credits range per submission. It must be between
5 seconds and 15 minutes long. For example, if you select an Intro and one
eligible Credits range with both services selected, TheIntroDB receives both
ranges and IntroDB receives Credits only. With both services selected and no
Credits selected, the review includes only TheIntroDB. IntroDB alone requires
Credits to be selected.

If you select two Credits ranges while IntroDB is selected, review stops.
Select a single Credits row on the movie card, or choose TheIntroDB alone to
submit both Credits ranges together.

In **Review Submission**, check the endpoint list and the timestamps shown
for TheIntroDB. The **Segments** and **Types** totals count deliveries across
endpoints, so a Credits range sent to both services counts twice.

## Queue and history

**Queue** shows pending and active detection/submission work. New submissions
can be added while earlier ones are processing. **History** shows completed
work and submission totals. The **Endpoint** column identifies the service for
submission rows. Use search or click a column heading to sort. Drag a pending
row by an area outside its title link to change its position in the queue. Click
a movie or show title to return to it.

The submission queue is saved across restarts. Detection scans must be started
again after quitting.

Movie and episode cards show submission status by endpoint. Use the info button
beside **ERROR** to see why a submission failed. **Duplicate** in History means
the service already had a matching timestamp.

INTROvert checks for duplicates and observes service rate limits. The two
services process submissions independently. When one shows **API Limit**, the
other can continue. The countdown appears on the movie or episode card and in
the queue for the item that triggered the limit. Let the queue wait for that
service to retry. For failures, correct the reported key, identification, or
connection problem before reviewing and submitting again.
